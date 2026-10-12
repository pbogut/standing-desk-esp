# Standing desk ESP8266 height monitor

First-stage firmware for a Wemos D1 Mini wired to the desk's RX/TX interface.
It receives height/status frames at 9600 baud, 8N1 and serves a diagnostics page
over its own password-protected Wi-Fi access point.

## Wiring

These are the owner's **zero-based contact numbers**, using the plug orientation
in `standing-desk-esp8266-handoff.md`.

| Desk contact | Wemos D1 Mini |
|---|---|
| 0, controller status output | RX / GPIO3 |
| 1, controller command input | TX / GPIO1 |
| 4, ground | GND |
| 7, 5 V supply | 5V |

The firmware uses hardware UART0 in `SERIAL_RX_ONLY` mode, disables serial debug
output, and makes GPIO1 an input. It does not send idle packets or commands.
The ESP8266 ROM still sends boot output on GPIO1 before the application starts.
The onboard USB-to-serial adapter also shares GPIO3. Receive-only firmware cannot
resolve electrical contention if that adapter drives the line.

## Build

For a new checkout, run these commands from the project directory:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install platformio==6.2.0
cp include/secrets.example.h include/secrets.h
```

Edit `include/secrets.h` to choose an access-point password of 8 to 63 ASCII
characters, then run `.venv/bin/pio run`. This local configuration file is ignored
by Git. PlatformIO keeps its toolchains and packages in `.pio-core` inside this
project. Build outputs are in `.pio`.

The password, Python environment, toolchains, and compiled image are not committed.
Create them with the steps above on the laptop too.

## Flash and connect

1. Unplug the desk's modular cable from the Wemos assembly.
2. Connect the Wemos to your computer through USB.
3. Find the board's USB serial port:

   ```sh
   .venv/bin/pio device list
   ```

   Look for the USB serial adapter. This board appeared as a CH340 adapter with
   VID:PID `1A86:7523` at `/dev/ttyUSB0` on the original computer. If unsure, compare
   the list with USB unplugged and plugged in. Upload using the port that appears:

   ```sh
   .venv/bin/pio run -t upload --upload-port /dev/ttyUSB0
   ```

4. Disconnect USB, then reconnect the desk cable. The desk now supplies power.
5. Join the Wi-Fi network named `standing-desk-<chip-id>`. Its password is the
   `apPassword` value in `include/secrets.h`. Accept staying connected to this
   network without internet access if your phone asks.
6. Open **http://192.168.4.1/**.

Do not open a USB serial monitor for this firmware. GPIO3 is the desk receive
pin, and GPIO1 is connected to the desk command input. All application diagnostics
use HTTP. Disconnect the desk cable for subsequent USB uploads as well.

The compiled image is `.pio/build/d1_mini/firmware.bin`.

### Upload status on the original computer

The firmware builds and decoder tests pass, but flashing has not succeeded.
The uploader detects the ESP8266 and starts its RAM helper, then times out before
writing flash blocks. Attempts at 115200 and 57600 baud, the ROM-only path, and
esptool 4.11.0 also failed. The desk cable was confirmed disconnected. A flash ID
could not be obtained; this does not establish whether the fault is in the USB
connection, power, or board. The next test is uploading from the laptop.

`platformio.ini` currently sets the upload speed to 57600 baud.

## Diagnostics

The page polls `GET /api/status` twice per second. The endpoint also works directly:

```sh
curl http://192.168.4.1/api/status
```

- `state` is `waiting_for_frames`, `fresh`, `stale`, or `unsupported_display`.
- `height_cm` is a number only while fresh, otherwise `null`.
- `last_known_height_cm` retains the last decoded number for troubleshooting.
  It is historical whenever `fresh` is false.
- `height_age_ms` measures time since the last accepted numeric reading. A reading
  becomes stale at 1,000 ms. A checksum-valid, unrecognized display invalidates
  the current reading immediately.
- `last_frame` is the latest complete six-byte candidate as hexadecimal text,
  including candidates with bad checksums. `last_frame_age_ms` is its age.
- `bytes_received` counts received bytes. `valid_frames` counts checksum-valid
  frames, including unrecognized displays. `invalid_displays` counts those
  unrecognized digit codes or invalid decimal layouts.
- `checksum_failures` and `partial_timeouts` help diagnose framing problems.
- `uart_overruns` and `uart_errors` count observed UART error flags. They are
  indicators, not exact counts of lost or damaged bytes.
- `commands_enabled` is always `false` in this firmware.

Frames must start with `5A`, contain a valid checksum, and use recognized
seven-segment codes. Digit decoding respects the decimal-point bit, so both
`78.6` and `105` work. Multiple decimal points and a trailing decimal point are
rejected. Unknown status bytes are preserved in the raw frame, without assigning
them a meaning. After a bad checksum the parser looks for another header inside
the rejected frame. Incomplete frames are discarded after a 30 ms receive gap.

If the page cannot reach the monitor, it clears the displayed live height and
shows `Monitor unreachable`.

## Decoder tests

Run these from the project directory. The build above creates `.pio`:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -Iinclude \
  tests/desk_protocol_test.cpp -o .pio/desk_protocol_test
.pio/desk_protocol_test
```

Tests cover all seven captured examples, checksum rejection, invalid display
codes, decimal placement, stale readings, recovery after corrupt/truncated frames,
header-valued payload bytes, partial-frame timeouts, and timer wraparound.

## First desk test

Leave the desk stationary and keep the page open for at least 30 seconds. Report:

- Whether the access point and page appear and remain available.
- Whether the height matches the desk's known position.
- The JSON diagnostics, especially byte/frame counters and the last frame.
- Whether the desk behaves differently during power-up or reset.

Increasing valid-frame counts with a stable height establish the first receive
test. Zero received bytes alone cannot distinguish an absent heartbeat from a
shared-RX electrical problem. Increasing bytes with few valid frames suggests a
signal or framing problem. Live UART reliability, desk-powered supply stability,
and keypad-free behavior still require this hardware test.

Movement controls come after those results and a separate idle-packet test.
