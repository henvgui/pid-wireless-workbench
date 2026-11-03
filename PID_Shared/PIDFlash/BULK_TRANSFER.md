# Firmware staging: USB batches and selective-repeat window

This mode accelerates **PC → host → slave staging RAM/PSRAM**. Target UART/SWD erase, write, readback and start algorithms retain their existing gates. It does not promise lossless radio delivery: failed packets are retried, and success requires a complete image and matching SHA256.

## Negotiation and compatibility

The existing Begin manifest still binds transfer ID, size, target profile, interface and SHA256. A new slave advertises flag `0x10` in its normal Reply capability byte. The host reports `bulk_window: 8` only when that flag is present. New PC software automatically selects bulk mode; otherwise it uses the previous 128/192-byte stop-and-wait mode. `--legacy` forces the old mode for comparison.

This is an extension of this project's application protocol, not a switch to ESP-NOW v2 large frames. Radio frames remain <=250 bytes. Both ESP32 firmware projects must be updated to use the new mode.

## USB framing and flow control

The PC negotiates `!fw bulk ID OFFSET [DROP_EVERY]` (Op 26), then sends COBS frames bracketed by zero bytes. The host's binary parser is separate from the ASCII command parser, including when bulk mode is inactive: bytes in a binary payload can never execute `!run`, `!fw flash`, or other text commands.

Decoded USB frame, little endian:

| Offset | Field |
|---|---|
| 0 | `BU` magic, version 1, kind (Data=1 / Exit=2 / Query=3) |
| 4 | transfer ID, uint32 |
| 8 | image offset, uint32 |
| 12 | payload length, uint16; reserved uint16 = 0 |
| 16 | up to 3072 raw image bytes |
| 16 + length | CRC32 of the header and payload |

Host cache is 6144 bytes (32 radio chunks). The PC may send at most `6144 - (sent - slave_confirmed)` more bytes. Host JSON reports distinguish `queued` (host RAM) from `received` (contiguous slave RAM). Progress advances only for the latter. A partial USB write is uncertain and is never blindly resent. While bulk mode is active, unframed text is ignored; a malformed frame quarantines input until a valid Exit, so corrupted binary cannot become a text command. Exit is preceded by an extra zero to terminate a truncated frame; normal ASCII commands resume after Exit confirmation. A timed-out initial read-only `!get` can issue the fixed framed Exit(0) to recover after a PC crash; this never requests a target operation. A legacy resume offset not aligned to 192 bytes falls back to the old ordered mode.

## Radio window and receiver

- BulkData=27: existing encrypted PF frame envelope, 192 image bytes maximum. Sequence is the BulkOpen request token; offset identifies the chunk. Sender/peer boot sessions and transfer ID must match.
- BulkAck=28: offset is the first missing byte; payload includes error, version, window size and a bitmap of already received chunks relative to that offset. Bit 0 is zero after shifting over contiguous chunks. Optional statistics count deliberately discarded ACKs.
- At most 8 chunks are awaiting application confirmation. Radio driver transmissions are serialized by send callbacks; heartbeat traffic and application ACKs have priority over bulk data.
- Receiver writes only correctly sized/aligned chunks in the current window. Duplicate bytes must match; conflicting duplicates fail. A bitmap records future chunks while a hole is repaired. `Staging.received` advances only across a complete contiguous prefix. Seal requires exact final length, SHA256 and valid vector bounds.
- ACKs are merged after four arrivals or 8 ms. Retransmission starts at 80 ms and adapts to 40–300 ms. A chunk queued locally has no retransmission timer yet: the radio dispatcher timestamps it only when the driver accepts the actual send. Queue wait must not produce false retries. At most eight retransmissions per chunk. Unchanged packet/session identity makes retries idempotent. No guessed ACKs or automatic target erase replay.
- New boot sessions invalidate active transfers. Resume is only possible while the same verified manifest and staging RAM remain available. Power loss clears staging.

## Tests and measurement

`--bulk-drop-every N` deliberately drops every Nth data transmission and every Nth application ACK. It is accepted only for staging tests without `--execute`, `--run` or `--legacy`; default 0 disables injection. Native tests also cover independent loss, reordering, duplication, corruption, malformed USB frames, credit limits, timeout, rollover, partial final chunks and full 1 MiB images.

Use the same image and conditions when comparing modes. Journal `transfer.seconds` measures the data-transfer interval, `kib_per_second` counts confirmed image bytes, and staging success requires an actual SHA256 on the slave. These numbers exclude target Flash erase/program/readback and are not total download speed.

```powershell
python PID_Shared/PIDFlash/tools/flash_stm32.py path/to/valid-f407.bin --part STM32F407VET6 --interface swd --port COM14 --journal bulk.json
python PID_Shared/PIDFlash/tools/flash_stm32.py path/to/valid-f407.bin --part STM32F407VET6 --interface swd --port COM14 --legacy --journal legacy.json
```

Without `--execute`, neither command erases or programs the target. A successful staging test retains the RAM image; explicitly abort that transfer when finished to release the programmer/H2 pause. GUI and Keil builds automatically bundle `bulk_stm32.py`; normal job invocation does not require new options.
