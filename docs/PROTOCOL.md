# Relay Bluetooth protocol (version 1)

The tablet is the Bluetooth Low Energy **central**. The relay unit is the **peripheral** and advertises as `FStopRelay`.
Every frame is at most 20 bytes so it fits the default Bluetooth payload (ATT MTU 23) with no negotiation.
Multi-byte numbers are **little endian**. Times are in **milliseconds**.

## Service

| Name | UUID | Properties | Purpose |
|---|---|---|---|
| Service | `6f9c0001-3a5b-4c7d-9e21-8b4f1d2a7c10` | | The relay |
| Command | `6f9c0002-3a5b-4c7d-9e21-8b4f1d2a7c10` | write (with response) | Tablet to relay |
| Event | `6f9c0003-3a5b-4c7d-9e21-8b4f1d2a7c10` | notify | Relay to tablet |
| Info | `6f9c0004-3a5b-4c7d-9e21-8b4f1d2a7c10` | read | `[protocol, firmwareMajor, firmwareMinor, 0]` |

The tablet subscribes to the Event characteristic, then sends `PING` every 500 ms. The relay treats 1500 ms of silence, or a disconnect, as a lost link.

## Commands (tablet to relay)

| Byte 0 | Name | Rest of the frame |
|---|---|---|
| `0x01` | PING | none |
| `0x10` | ARM_BEGIN | `planId u8`, `kind u8` (0 single, 1 strip, 2 recipe), `steps u8` (1 to 12), `gapMs u32` |
| `0x11` | ARM_STEPS | `planId u8`, `first u8`, `count u8` (1 to 4), then `count` times `ms u32` |
| `0x12` | ARM_END | `planId u8` |
| `0x20` | START | none |
| `0x21` | PAUSE | none |
| `0x22` | RESUME | none |
| `0x23` | CANCEL | none |
| `0x30` | FOCUS | `on u8` |
| `0x31` | SAFELIGHT | `on u8` |
| `0x40` | CONFIG | `latencyMs i16`, `focusTimeoutSec u16` (0 = never), `volume u8` (0 to 100), `flags u8` (bit 0 mute, bit 1 safelight off while exposing) |

**Arming.** A plan is sent as `ARM_BEGIN`, one or more `ARM_STEPS` (four durations each), then `ARM_END`. The relay commits the plan only when every step has arrived, then answers with an `ARM_OK` event. A plan is rejected while an exposure is running. Each step is at most 30 minutes.

The relay keeps the last committed plan while it has power, so the foot pedal can start it even if the tablet is asleep.

Step durations already include any reciprocity compensation, which the tablet computes. The relay adds `latencyMs` to every lamp switch-on.

## Messages (relay to tablet)

### STATUS `0x81` (18 bytes, about ten per second)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `0x81` |
| 1 | 1 | state: 0 idle, 1 armed, 2 focus, 3 exposing, 4 gap, 5 paused, 6 fault |
| 2 | 1 | flags: bit 0 enlarger on, bit 1 safelight on, bit 2 safelight wanted on |
| 3 | 1 | step index (in a gap: the step that just finished) |
| 4 | 1 | number of steps in the armed plan |
| 5 | 4 | time remaining in the step or gap, ms |
| 9 | 4 | total length of the step or gap, ms |
| 13 | 2 | focus time left, seconds (`0xFFFF` = none) |
| 15 | 1 | kind of the armed plan (`0xFF` = none) |
| 16 | 1 | state before pausing (`0xFF` = not paused) |
| 17 | 1 | fault: 0 none, 1 link lost in focus mode, 2 lamp on too long |

### EVENT `0x82`

| Byte 1 | Event | Extra |
|---|---|---|
| 1 | DONE | `totalLampMs u32` |
| 2 | CANCELLED | none |
| 3 | PEDAL | `long u8` |
| 4 | FOCUS_TIMEOUT | none |
| 5 | FAULT | `fault u8` |
| 6 | STEP_DONE | `index u8` |
| 7 | ARM_OK | `planId u8` |
| 8 | ARM_ERR | `planId u8`, `code u8` (1 busy, 2 bad frame, 3 incomplete, 4 too long, 5 too many steps, 6 no plan in progress) |

DONE and FAULT events that could not be delivered (no tablet connected) are kept, up to four, and sent when the tablet next talks to the relay.

## Behaviour the relay guarantees

* Boot: enlarger off, safelight on.
* An exposure in progress finishes at the commanded length even if the tablet disconnects.
* Focus mode switches the enlarger off if the link is lost or the focus timeout expires.
* Pedal: short press starts the armed exposure, pauses and resumes it. A one second hold cancels.
* The enlarger is never on for more than one hour continuously.
