#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ==================== CONFIGURATION ==================== */

#define HEARTBEAT_INTERVAL_SEC       1
#define WATCHDOG_TIMEOUT_SEC         3
#define SIMULATED_FAULT_AFTER_SEC    8
#define MAX_RECOVERY_ATTEMPTS        3
#define MONITOR_INTERVAL_MS          100
#define STOP_GRACE_PERIOD_MS         1000

static volatile sig_atomic_t keep_running = 1;

/* ==================== LOGGING ==================== */

static void log_event(const char *level, const char *format, ...)
{
    time_t now = time(NULL);
    struct tm local_time;
    char timestamp[32] = "time-unavailable";
    va_list args;

    if (localtime_r(&now, &local_time) != NULL) {
        (void)strftime(timestamp, sizeof(timestamp),
                       "%Y-%m-%d %H:%M:%S", &local_time);
    }

    printf("[%s] [%s] ", timestamp, level);

    va_start(args, format);
    vprintf(format, args);
    va_end(args);

    printf("\n");
    fflush(stdout);
}

/* ==================== SIGNAL HANDLING ==================== */

static void handle_signal(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

/* ==================== TIME UTILITIES ==================== */

static long long monotonic_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (long long)time(NULL);
    }

    return (long long)ts.tv_sec;
}

static void sleep_milliseconds(unsigned int milliseconds)
{
    struct timespec req;
    struct timespec remaining;

    req.tv_sec = (time_t)(milliseconds / 1000U);
    req.tv_nsec = (long)((milliseconds % 1000U) * 1000000U);

    while (nanosleep(&req, &remaining) == -1) {
        if (errno != EINTR) {
            break;
        }

        req = remaining;
    }
}

static void sleep_seconds(unsigned int seconds)
{
    sleep_milliseconds(seconds * 1000U);
}

/* ==================== ECU WORKER ==================== */

/*
 * The child process simulates an ECU.
 *
 * It sends one heartbeat byte every second.
 * After eight seconds, it stops sending heartbeats but
 * remains alive so the watchdog can detect a timeout.
 */
static void ecu_worker(int write_fd, int generation)
{
    long long started_at = monotonic_seconds();
    unsigned long heartbeat_count = 0;

    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGTERM, SIG_DFL);
    (void)signal(SIGPIPE, SIG_DFL);

    log_event("ECU",
              "Generation %d started. PID=%ld",
              generation, (long)getpid());

    for (;;) {
        long long elapsed = monotonic_seconds() - started_at;

        if (elapsed >= SIMULATED_FAULT_AFTER_SEC) {
            log_event("ECU",
                      "SIMULATED FAULT: generation %d stopped heartbeats",
                      generation);

            /*
             * Keep the process alive to simulate a hung ECU.
             * The watchdog must detect the missing heartbeat.
             */
            for (;;) {
                sleep_seconds(1);
            }
        }

        {
            const char heartbeat = 'H';
            ssize_t written;

            do {
                written = write(write_fd, &heartbeat, 1);
            } while (written == -1 && errno == EINTR);

            if (written == 1) {
                heartbeat_count++;

                log_event("ECU",
                          "Generation %d heartbeat #%lu sent",
                          generation, heartbeat_count);
            } else {
                log_event("ECU",
                          "Heartbeat write failed: %s",
                          written == -1 ? strerror(errno)
                                        : "unexpected short write");

                close(write_fd);
                _exit(EXIT_FAILURE);
            }
        }

        sleep_seconds(HEARTBEAT_INTERVAL_SEC);
    }
}

/* ==================== ECU PROCESS MANAGEMENT ==================== */

/*
 * Launch a new ECU worker.
 *
 * Returns:
 *   Child PID on success
 *   -1 on failure
 *
 * read_fd receives the non-blocking heartbeat pipe descriptor.
 */
static pid_t start_ecu(int *read_fd, int generation)
{
    int pipe_fds[2];
    int flags;
    pid_t child_pid;

    *read_fd = -1;

    if (pipe(pipe_fds) == -1) {
        log_event("ERROR", "pipe() failed: %s", strerror(errno));
        return -1;
    }

    flags = fcntl(pipe_fds[0], F_GETFL, 0);

    if (flags == -1 ||
        fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        log_event("ERROR",
                  "Cannot configure heartbeat pipe: %s",
                  strerror(errno));

        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    child_pid = fork();

    if (child_pid == -1) {
        log_event("ERROR", "fork() failed: %s", strerror(errno));

        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }

    if (child_pid == 0) {
        close(pipe_fds[0]);

        ecu_worker(pipe_fds[1], generation);

        close(pipe_fds[1]);
        _exit(EXIT_SUCCESS);
    }

    close(pipe_fds[1]);

    *read_fd = pipe_fds[0];

    log_event("WATCHDOG",
              "Started ECU generation %d, PID=%ld",
              generation, (long)child_pid);

    return child_pid;
}

/*
 * Stop an ECU worker and reap its process.
 *
 * SIGTERM is attempted first. SIGKILL is used if the worker
 * does not exit during the grace period.
 */
static void stop_ecu(pid_t child_pid, int read_fd)
{
    int status;
    int waited_ms = 0;
    pid_t result;

    if (child_pid > 0) {
        if (kill(child_pid, SIGTERM) == -1 && errno != ESRCH) {
            log_event("WARNING",
                      "SIGTERM failed for PID %ld: %s",
                      (long)child_pid, strerror(errno));
        }

        while (waited_ms < STOP_GRACE_PERIOD_MS) {
            result = waitpid(child_pid, &status, WNOHANG);

            if (result == child_pid) {
                child_pid = -1;
                break;
            }

            if (result == -1) {
                if (errno == EINTR) {
                    continue;
                }

                if (errno == ECHILD) {
                    child_pid = -1;
                    break;
                }

                log_event("WARNING",
                          "waitpid() failed: %s",
                          strerror(errno));
                break;
            }

            sleep_milliseconds(100);
            waited_ms += 100;
        }

        if (child_pid > 0) {
            if (kill(child_pid, SIGKILL) == -1 && errno != ESRCH) {
                log_event("WARNING",
                          "SIGKILL failed for PID %ld: %s",
                          (long)child_pid, strerror(errno));
            }

            do {
                result = waitpid(child_pid, &status, 0);
            } while (result == -1 && errno == EINTR);
        }
    }

    if (read_fd >= 0) {
        close(read_fd);
    }
}

/* ==================== MAIN WATCHDOG ==================== */

int main(void)
{
    pid_t ecu_pid = -1;
    int heartbeat_fd = -1;

    int generation = 1;
    int recovery_attempts = 0;
    int timeout_events = 0;
    int unexpected_exits = 0;
    int exit_code = EXIT_SUCCESS;

    unsigned long total_heartbeats = 0;

    long long last_heartbeat_at;

    const char *shutdown_reason = "Normal shutdown";

    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    (void)sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) == -1 ||
        sigaction(SIGTERM, &action, NULL) == -1) {
        fprintf(stderr,
                "ERROR: Cannot install signal handlers: %s\n",
                strerror(errno));
        return EXIT_FAILURE;
    }

    printf("\n");
    printf("============================================================\n");
    printf("       AutoGuardECU - Watchdog & Recovery Framework\n");
    printf("============================================================\n");
    printf(" Platform       : QNX SDP 8.0 / VMware x86_64\n");
    printf(" Operating mode : Simulated ECU demonstration\n");
    printf(" Heartbeat      : Every %d second(s)\n",
           HEARTBEAT_INTERVAL_SEC);
    printf(" Timeout        : %d second(s)\n",
           WATCHDOG_TIMEOUT_SEC);
    printf(" Fault injection: After %d seconds per generation\n",
           SIMULATED_FAULT_AFTER_SEC);
    printf(" Max recoveries : %d\n", MAX_RECOVERY_ATTEMPTS);
    printf(" Monitor period : %d milliseconds\n",
           MONITOR_INTERVAL_MS);
    printf("============================================================\n\n");
    fflush(stdout);

    /* Start the initial ECU process. */
    ecu_pid = start_ecu(&heartbeat_fd, generation);

    if (ecu_pid == -1) {
        log_event("FATAL", "Initial ECU startup failed");
        return EXIT_FAILURE;
    }

    last_heartbeat_at = monotonic_seconds();

    log_event("WATCHDOG", "Heartbeat monitoring started");

    while (keep_running) {
        char buffer[64];
        ssize_t bytes_read;
        ssize_t i;
        int received_heartbeat = 0;
        int fault_detected = 0;
        int process_status = 0;
        pid_t wait_result;

        /* 1. Read available heartbeat bytes without blocking. */
        if (heartbeat_fd >= 0) {
            bytes_read = read(heartbeat_fd, buffer, sizeof(buffer));

            if (bytes_read > 0) {
                for (i = 0; i < bytes_read; i++) {
                    if (buffer[i] == 'H') {
                        received_heartbeat = 1;
                        total_heartbeats++;
                    }
                }
            } else if (bytes_read == -1 &&
                       errno != EAGAIN &&
                       errno != EWOULDBLOCK &&
                       errno != EINTR) {
                log_event("ERROR",
                          "Heartbeat pipe read failed: %s",
                          strerror(errno));
            }
        }

        /* 2. Refresh heartbeat deadline when data arrives. */
        if (received_heartbeat) {
            last_heartbeat_at = monotonic_seconds();

            log_event("WATCHDOG",
                      "Heartbeat received. Total=%lu",
                      total_heartbeats);
        }

        /* 3. Detect an ECU process that exited unexpectedly. */
        if (ecu_pid > 0) {
            wait_result = waitpid(ecu_pid, &process_status, WNOHANG);

            if (wait_result == ecu_pid) {
                log_event("FAULT",
                          "ECU process exited unexpectedly. PID=%ld",
                          (long)ecu_pid);

                unexpected_exits++;

                /*
                 * The process has already been reaped.
                 * Do not signal this PID again.
                 */
                ecu_pid = -1;

                if (heartbeat_fd >= 0) {
                    close(heartbeat_fd);
                    heartbeat_fd = -1;
                }

                fault_detected = 1;
            } else if (wait_result == -1 && errno == ECHILD) {
                log_event("FAULT",
                          "ECU process is no longer a child process");

                ecu_pid = -1;

                if (heartbeat_fd >= 0) {
                    close(heartbeat_fd);
                    heartbeat_fd = -1;
                }

                unexpected_exits++;
                fault_detected = 1;
            } else if (wait_result == -1 && errno != EINTR) {
                log_event("ERROR",
                          "waitpid() failed: %s",
                          strerror(errno));
            }
        }

        /* 4. Detect missing heartbeats. */
        if (!fault_detected &&
            monotonic_seconds() - last_heartbeat_at >=
                WATCHDOG_TIMEOUT_SEC) {

            log_event("FAULT",
                      "HEARTBEAT TIMEOUT: no heartbeat for %d seconds",
                      WATCHDOG_TIMEOUT_SEC);

            timeout_events++;
            fault_detected = 1;
        }

        /* 5. Recover from a detected fault. */
        if (fault_detected) {
            log_event("WATCHDOG",
                      "Fault detected in ECU generation %d",
                      generation);

            if (ecu_pid > 0 || heartbeat_fd >= 0) {
                log_event("RECOVERY",
                          "Stopping failed ECU worker");

                stop_ecu(ecu_pid, heartbeat_fd);

                ecu_pid = -1;
                heartbeat_fd = -1;
            }

            /*
             * MAX_RECOVERY_ATTEMPTS counts restart attempts.
             * Three restarts are allowed; the next fault causes
             * the recovery-limit shutdown.
             */
            if (recovery_attempts >= MAX_RECOVERY_ATTEMPTS) {
                log_event("SAFETY",
                          "Recovery limit reached (%d attempts)",
                          MAX_RECOVERY_ATTEMPTS);

                log_event("SAFETY",
                          "Safe shutdown: refusing further restarts");

                shutdown_reason = "Recovery limit reached";
                exit_code = EXIT_FAILURE;
                keep_running = 0;
                break;
            }

            recovery_attempts++;
            generation++;

            log_event("RECOVERY",
                      "Attempt %d of %d",
                      recovery_attempts, MAX_RECOVERY_ATTEMPTS);

            log_event("RECOVERY",
                      "Restarting ECU as generation %d",
                      generation);

            ecu_pid = start_ecu(&heartbeat_fd, generation);

            if (ecu_pid == -1) {
                log_event("FATAL",
                          "ECU restart failed; entering shutdown");

                shutdown_reason = "ECU restart failed";
                exit_code = EXIT_FAILURE;
                keep_running = 0;
                break;
            }

            /*
             * Reset the heartbeat deadline for the new worker.
             * A missing first heartbeat will still cause a timeout.
             */
            last_heartbeat_at = monotonic_seconds();

            log_event("RECOVERY",
                      "Restart successful; monitoring resumed");
        }

        /* 6. Responsive watchdog polling. */
        sleep_milliseconds(MONITOR_INTERVAL_MS);
    }

    /* ==================== CLEAN SHUTDOWN ==================== */

    log_event("WATCHDOG", "Shutdown requested");

    if (ecu_pid > 0 || heartbeat_fd >= 0) {
        stop_ecu(ecu_pid, heartbeat_fd);
    }

    printf("\n");
    printf("============================================================\n");
    printf("                  AutoGuardECU SUMMARY\n");
    printf("============================================================\n");
    printf(" Final generation       : %d\n", generation);
    printf(" Total heartbeats       : %lu\n", total_heartbeats);
    printf(" Heartbeat timeouts     : %d\n", timeout_events);
    printf(" Unexpected ECU exits   : %d\n", unexpected_exits);
    printf(" Recovery attempts      : %d\n", recovery_attempts);
    printf(" Shutdown reason        : %s\n", shutdown_reason);
    printf(" Final result           : %s\n",
           exit_code == EXIT_SUCCESS
               ? "Clean shutdown"
               : "Shutdown after failure");
    printf("============================================================\n");

    log_event("WATCHDOG", "AutoGuardECU demonstration stopped");

    return exit_code;
}  63
