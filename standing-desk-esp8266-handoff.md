# Standing desk Wi-Fi controller: ESP8266 implementation handoff

## Objective

Build a network-controlled replacement for the original standing desk keypad using an existing **Wemos D1 Mini (ESP8266)**. The original keypad plugs into an 8-contact modular connector (RJ45 form factor). It is acceptable to unplug the ESP8266 and reconnect the original keypad when changing saved presets.

Desired functionality:

- Read and expose the current desk height in centimetres (decimal point present below 100 cm in observed captures; no decimal point at 105 cm).
- Trigger memory presets 1, 2 and 3.
- Optionally support UP, DOWN and STOP.
- Expose controls via a small local HTTP interface or Home Assistant/MQTT, to be chosen during implementation.
- Do **not** implement memory programming or clock/time functions. These are rarely needed and can be done on the original keypad.

Use the **existing Wemos D1 Mini**, not an ESP32. Prioritize a reliable, safe minimal implementation over feature creep.

## Hardware and observed interface

The desk keypad has a five-pad test header marked `G`, unknown, `T`, `R`, `5`. Multimeter readings relative to the presumed ground were approximately:

| Pad | Measurement |
|---|---:|
| `5` | 5.0 V |
| `R` | 3.25 V |
| `T` | 2.98 V |

The owner used a generic 8-channel, 24 MHz USB logic analyzer (USB VID:PID `0925:3881`), with PulseView / sigrok `fx2lafw`. The analyzer was connected CH1 to `T`, CH2 to `R`, and ground to `G`. PulseView mapped these to D0 and D1 respectively. D0 carried `A5` keypad command frames; D1 carried `5A` display/status frames. UART decoding was successful at **9600 baud, 8 data bits, no parity, 1 stop bit (8N1), non-inverted**.

The owner mapped the modular plug by continuity. **Important: these are zero-based contact numbers in the owner's viewing orientation, NOT standard RJ45 numbering.** Orientation: looking at the plug's exposed metal contacts, plug pointing upward and cable exiting downward, left to right:

| Owner contact | Original keypad signal | Expected role |
|---|---|---|
| 0 | `R` / RX | Desk controller transmits height/status to keypad |
| 1 | `T` / TX | Keypad transmits button frames to desk controller |
| 2 | Not identified | Do not connect |
| 3 | Not identified | Do not connect |
| 4 | GND | Common ground |
| 5 | Not identified | Do not connect |
| 6 | Not identified | Do not connect |
| 7 | 5 V | Keypad supply, current capacity unknown |

**Before connecting hardware, independently verify the connector orientation and pin mapping.** RJ45 pin numbers and mirrored plug/jack views are easy to confuse. Check continuity from each plug contact to the appropriate test pad. Do not assume unused contacts are safe or unconnected.

## Captured keypad protocol

The keypad sends repeated five-byte frames on its TX line (D0 in the capture), approximately every 10 ms. Format inferred from captures:

```
A5 00 BUTTON 00 CHECKSUM
```

For observed button frames, `CHECKSUM = (byte1 + byte2 + byte3) & 0xff`, excluding the `A5` header. This matches the observed frames but should still be validated on additional messages.

| Key state | Full observed frame | Evidence |
|---|---|---|
| Idle / no button | `A5 00 00 00 00` | Captured |
| UP | `A5 00 20 00 20` | Captured |
| DOWN | `A5 00 40 00 40` | Captured |
| Preset 1 | `A5 00 02 00 02` | Captured |
| Preset 2 | `A5 00 04 00 04` | Captured |
| Preset 3 | `A5 00 08 00 08` | Captured |

The button byte behaves like a bitmask. Do not assume unobserved combinations are valid. The capture with an UP press contained roughly 268 UP frames over 2.66 seconds, consistent with periodic repetition at approximately 100 Hz. Releasing the button returned to idle frames.

**Unverified:** startup/handshake requirements, whether the desk responds when the original panel is completely absent, whether a brief preset command starts an autonomous movement, whether holding a preset command is necessary, and what the desk does when UART messages cease. Do not implement unattended movement based on guesses.

## Captured height/display protocol

The desk controller sends six-byte frames on the other line (D1):

```
5A DISPLAY_DIGIT_1 DISPLAY_DIGIT_2 DISPLAY_DIGIT_3 STATUS CHECKSUM
```

The three digit bytes are **seven-segment display codes**, not a binary height counter. `0x80` marks the decimal point on a digit. In the observed 70.x to 78.x cm range, the second digit byte has its decimal-point bit set (e.g. `7 8. 6`). A new capture at **105 cm** is `5A 06 3F 6D 10 C2`, representing `1 0 5` with **no decimal point**. Thus, do not assume the display always has one decimal place. Decode the digits and decimal-point positions to construct the displayed number. If no decimal point is present, interpret the three digits as an integer; validate other layouts as encountered. The status byte was `0x10` in observed frames; its meaning is unknown. The last byte equals the low eight bits of the sum of bytes 1 through 4, excluding header `5A`.

Seven-segment mapping (after masking off `0x80`):

| Digit | Hex |
|---|---|
| 0 | `3F` |
| 1 | `06` |
| 2 | `5B` |
| 3 | `4F` |
| 4 | `66` |
| 5 | `6D` |
| 6 | `7D` |
| 7 | `07` |
| 8 | `7F` |
| 9 | `6F` |

Examples, corroborated by the keypad display:

| Displayed height | Status frame |
|---|---|
| 70.0 cm | `5A 07 BF 3F 10 15` |
| 71.0 cm | `5A 07 86 3F 10 DC` |
| 72.0 cm | `5A 07 DB 3F 10 31` |
| 73.0 cm | `5A 07 CF 3F 10 25` |
| 73.5 cm | `5A 07 CF 6D 10 53` |
| 78.6 cm | `5A 07 FF 7D 10 93` |
| 105 cm (no decimal point) | `5A 06 3F 6D 10 C2` |

Example checksum for 105 cm: `(0x06 + 0x3F + 0x6D + 0x10) & 0xff = 0xC2`.

Example checksum for 78.6: `(0x07 + 0xFF + 0x7D + 0x10) & 0xff = 0x93`.

The 70.0-to-78.6 movement capture lasted about 7.63 s; the UP command was present for about 2.66 s. Periodic status frames were observed at approximately 100 Hz. Do not assume all heights, status modes, errors or power-on sequences have the same frame structure without testing. Reject malformed frames and stale data.

## Proposed Wemos D1 Mini interface

For the first **read-only** test, use software UART RX on **D5 / GPIO14** so USB serial remains free for programming and logs:

| Desk contact (owner numbering) | Wemos | First test |
|---|---|---|
| 0, controller TX/status | D5 / GPIO14 via suitable input protection or level shifting if required | Connect after electrical verification |
| 4, ground | GND | Connect after confirming ground |
| 1, controller RX/commands | No connection initially | Leave disconnected |
| 7, desk 5 V | No connection initially | Leave disconnected; power Wemos via USB |

The Wemos D1 Mini's GPIO pins are **3.3 V only**. Multimeter averages of 3.25 V and 2.98 V are not definitive measurements of peak logic voltage. Verify UART high levels and the input's safety before direct connection. A scope or appropriate voltage-rated analyzer may help. Do not connect a 5 V UART signal directly to a Wemos GPIO.

Later, if implementing replacement-panel TX, choose an appropriate GPIO and software UART implementation, or hardware UART if justified. Test serial reliability with Wi-Fi running. On the D1 Mini, GPIO1/TX and GPIO3/RX share the USB serial interface, so attaching the desk there can interfere with flashing and serial logging. Avoid ESP8266 boot-strap pins or account explicitly for their required boot states.

The Wemos D1 Mini usually accepts regulated 5 V on its `5V` pin through its onboard 3.3 V regulator, **but the desk's 5 V supply capacity is unknown**. ESP8266 Wi-Fi current peaks can be several hundred milliamps. Begin with USB power; assess voltage droop, regulator behavior, common ground, backfeeding, and current budget before using desk power. Never put 5 V on the Wemos `3V3` pin. Avoid simultaneously powering through USB and an external 5 V rail unless the specific board's power path has been verified.

## Firmware requirements

Use PlatformIO with the Arduino framework and board `d1_mini` (or another well-supported toolchain if the local environment prefers it).

Suggested `platformio.ini`:

```ini
[env:d1_mini]
platform = espressif8266
board = d1_mini
framework = arduino
monitor_speed = 115200
```

Implementation stages:

1. **Read-only UART monitor.** Receive on D5/GPIO14 at 9600 8N1. Synchronize on `0x5A`, accumulate six-byte frames, verify checksum and seven-segment codes, interpret decimal-point placement (including no decimal point for 105 cm), decode height, and log changes over USB. Implement resynchronization after corruption and a stale-data timeout. Confirm height readings while the original keypad is unplugged. Do not drive any desk input in this stage.
2. **Wi-Fi height reporting.** Publish validated height and connection/status information through a small local HTTP endpoint or MQTT/Home Assistant integration. Never claim a fresh height after frames stop arriving. Keep Wi-Fi credentials out of committed source.
3. **Idle-only replacement test.** After verifying signal direction and voltage, attach ESP8266 TX to desk controller RX. Send only the known idle frame at roughly 10 ms intervals. Verify desk remains stationary and continues reporting height. Check behavior on boot, reset, cable unplug and Wi-Fi disconnect.
4. **Bounded manual movement.** Only after explicit validation, add UP/DOWN with strict time limits, immediate STOP/idle, mutually exclusive commands, startup default idle, and a hardware or firmware watchdog that fails to a safe state. Never automatically resume movement after reset. Test with clear space and physical ability to disconnect power.
5. **Memory presets.** Add buttons for presets 1/2/3. Determine empirically whether the desk requires a press duration or sustained packets and whether preset motion continues after release. Avoid unattended movement until stop semantics and obstacle/collision protection are verified.
6. **Optional desk-powered operation.** Only after current budget and supply stability are established.

For a simple web UI, show the latest valid height, stale/offline state, and three preset buttons; optionally add UP/DOWN/STOP. Keep endpoints on the local network, require suitable access control, and do not expose them directly to the internet. Rate-limit movement requests and guard against duplicate/retried HTTP requests.

## Open questions / test checklist

- [ ] Verify RJ45 plug orientation and all four mapped contacts with continuity testing.
- [ ] Confirm the UART signal high levels and electrical compatibility with 3.3 V ESP8266 GPIO.
- [ ] Confirm controller transmits `5A` status frames when original panel is disconnected.
- [ ] Confirm whether controller requires continuous `A5` idle heartbeat.
- [ ] Verify startup behavior, timeout behavior, and response to missing or corrupted frames.
- [ ] Verify preset press/release behavior, including whether presets are autonomous or hold-to-run.
- [ ] Verify the `0x10` status field under idle, moving, and error states.
- [ ] Determine whether height display has special codes for errors, calibration, or out-of-range values.
- [ ] Verify desk 5 V rail can power the Wemos safely if that feature is desired.
- [ ] Confirm chosen network integration (HTTP, MQTT, ESPHome/Home Assistant).

## Instructions to the local coding agent

Work incrementally. Start by inspecting the local PlatformIO/toolchain environment and create a minimal project that **only monitors height**. Provide exact wiring and flash instructions. Do not assume untested protocol details are verified, do not send movement commands in the initial firmware, and do not connect the desk's 5 V rail to the Wemos during the first test. Ask for specific test results before advancing to command transmission. Preserve the original keypad as a plug-in fallback.
