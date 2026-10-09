# AutoGuardECU - Watchdog & Recovery Framework

## 1. Project Overview

AutoGuardECU is a software-based watchdog and recovery
framework developed using C and QNX SDP 8.0.

The prototype monitors a simulated ECU worker using heartbeat
messages. If the worker stops sending heartbeats or exits
unexpectedly, the supervisor detects the fault and attempts
to restart the worker.

## 2. Problem Statement

Embedded software failures can cause applications to stop
responding. A monitoring mechanism is needed to detect such
failures and initiate an appropriate recovery action.

## 3. Proposed Solution

The project implements:

- Heartbeat-based health monitoring
- Watchdog timeout detection
- Simulated ECU fault injection
- Automatic process restart
- Unexpected process-exit detection
- Configurable recovery-attempt limit
- Timestamped diagnostic logging

## 4. Technologies Used

- Programming Language: C
- Operating System: QNX SDP 8.0
- Development Environment: QNX Momentics IDE
- Test Environment: VMware x86_64 virtual target

## 5. System Architecture

1. The simulated ECU sends heartbeat messages.
2. The watchdog receives heartbeat messages through a pipe.
3. The supervisor checks the heartbeat timeout.
4. A simulated fault stops heartbeat generation.
5. The watchdog detects the fault.
6. The recovery manager restarts the worker.
7. Repeated failures eventually trigger the recovery limit.

## 6. Testing

The prototype is intended to demonstrate:

- Normal heartbeat operation
- Missing-heartbeat detection
- Automatic process restart
- Unexpected process-exit detection
- Recovery-limit handling

Record actual test results after running the application.

## 7. Limitations

This is a software simulation running on a QNX virtual target.
It does not implement a physical hardware watchdog or provide
production automotive safety certification.

## 8. Future Scope

- Hardware watchdog integration
- Physical ECU testing
- More extensive fault-injection tests
- Recovery-time measurement
- Enhanced diagnostic reporting
