# STM32 motor PID extension (2026-10-19)

The STM32F407VET6 motor project at `C:/Users/twice/Desktop/STM32PRO/f407` now uses
`motor_protocol.h` for commands/status over USART1, 115200 8N1. The same header
is kept in the target project. This is distinct from the UART_ECHO protocol.

`motor_bridge.cpp` forwards the slave PIDLink mirror's P/I/D/revision/request/run
every 100 ms and continuously validates target telemetry. Hardware host knobs
thus reach the target without USB monitoring. Initial targets are 100 RPM per
wheel, with a 50% duty ceiling. Targets are RAM-only. Reconnect requires STOP
before RUN; the target independently times out new commands after 500 ms.

Host USB commands:

```
!motor status <nonzero hex operation id>
!motor target <nonzero hex operation id> <left RPM * 100> <right RPM * 100>
```

Target range is 0..40000 hundredths of RPM. Radio opcodes MotorStatus=14,
MotorReply=15, MotorTarget=16 are appended after UartRead=12/UartReply=13.
USB replies are JSON with type=motor, id, original op, ok, error, hex.

MotorStatus replies contain 104 bytes: LE32 cache age, LE32 bridge run inhibit,
then the original target 96-byte frame with its CRC32. Status requests fail if
no matching target application/session status is available within 500 ms.
MotorTarget replies echo the 8-byte targets and only acknowledge the slave;
clients must check subsequent target status before claiming target acceptance.
The project tool `tools/motor_pid.py` does so.

Normal bridging occupies the target UART converter. Firmware staging, raw UART
capture, echo and SWD diagnostics pause it; it resumes after release. Monitoring
does not interrupt bridging. The legacy workbench's ack_scope=slave is unchanged;
do not interpret that ACK alone as proof that the motor target applied a value.

Host and slave upload environments: `host_flash`, `slave_flash_psram`.
Both were built and uploaded for this change. Prior binaries/source and initial
host settings are preserved under the motor project's `pid_bridge_backup_20261019`.
