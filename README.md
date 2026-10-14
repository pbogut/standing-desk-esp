# Standing desk ESP8266 panel bridge

Firmware for a Wemos D1 Mini placed between the desk controller and original
panel. It forwards panel commands at 9600 baud, 8N1, reads height/status on a
separate input, and serves a diagnostics page over its password-protected Wi-Fi
access point. D5/D6/D7 handle desk traffic; RX/TX remain available for USB.

The web page controls presets 1/2/3 and hold-to-move UP/DOWN. Any physical panel
button takes priority and cancels the active ESP button press.

## Wiring

These are the owner's **zero-based contact numbers**, not standard RJ45 numbers.
Look at the plug's exposed metal contacts with the plug pointing upward and cable
exiting downward, then count 0 through 7 from left to right. Identify both adapter
ends by continuity; looking into a socket can mirror this view.

Build a removable inline adapter with a controller side and a panel side:

| Contact | Controller-to-panel connection | Wemos D1 Mini connection |
|---|---|---|
| 0, controller status output | Straight through to panel contact 0 | Tap to D5 / GPIO14 |
| 1, panel command output | **Split, no direct connection between the two sides** | Panel side to D6 / GPIO12; controller side to D7 / GPIO13 |
| 4, ground | Straight through | Tap to GND |
| 7, 5 V supply | Straight through so the original panel stays powered | Leave untapped for the first USB-powered test |
| 2, 3, 5, 6 | Preserve each contact straight through | No ESP connection |

```text
Controller contact 0 --------+-------- Panel contact 0
                            |
                            +-------- D5, status input

Controller contact 1 <--------------- D7, command output
                                      ESP forwards bytes
Panel contact 1 --------------------> D6, panel input

Controller contact 4 --------+-------- Panel contact 4
                            +-------- GND

Controller contact 7 ---------------- Panel contact 7
```

Move the previous GPIO3/RX status connection to D5. Move the previous GPIO1/TX
controller-command connection to D7, and add the panel-command connection to D6.
Leave the board's RX and TX pins disconnected from the desk. Never join D7 to
panel TX; those are two separate outputs.

The UART signals must be 3.3 V compatible. The measured averages in the handoff
are not confirmation of peak signal voltage; use level shifting if needed.
An external 10 kOhm pull-up from D7 to 3V3 is recommended to hold the command wire
at UART idle while the ESP is in reset. Firmware holds it high during application
startup and starts forwarding after Wi-Fi setup, without queuing startup presses.

For the first test, power the ESP by USB while contact 7 powers only the panel.
For subsequent desk-powered use, contact 7 can be tapped to the board's **5V** pin
after confirming the supply supports the ESP's Wi-Fi current peaks. Disconnect
USB when using that tap unless this board's power isolation has been verified.
Never connect contact 7 to 3V3.

Both software UART receivers remain active during transmission. The bridge
buffers five-byte panel frames and forwards idle, movement, preset, and
programming traffic. While an ESP button is active, panel idle frames are
suppressed and ESP button frames are sent roughly every 10 ms. Any non-idle panel
frame cancels ESP control and passes through. Unknown and corrupt traffic also
passes through; corrupt traffic cancels ESP control. Forwarding adds latency and
exact packet gaps are not preserved.

Panel buttons require working ESP firmware and are unavailable during boot,
reset, or flashing. The panel display receives status directly. If the ESP fails,
remove the whole adapter and reconnect the panel directly to the desk. There is
no automatic bypass.

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

1. Disconnect the adapter from both the desk and original panel.
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

4. For the first test, keep USB power, leave the ESP's 5V desk tap disconnected,
   and connect the adapter between the desk and original panel. If later using
   desk power, disconnect USB before connecting the desk's 5 V tap.
5. Join the Wi-Fi network named `standing-desk-<chip-id>`. Its password is the
   `apPassword` value in `include/secrets.h`. Accept staying connected to this
   network without internet access if your phone asks.
6. Open **http://192.168.4.1/**.

USB serial is now independent of desk traffic. The optional serial monitor runs
at 115200 baud and prints a startup message:

```sh
.venv/bin/pio device monitor --port /dev/ttyUSB0
```

Opening a monitor may reset the ESP through USB control lines and briefly stop
panel forwarding. Disconnect the adapter from the desk and panel for uploads.

The compiled image is `.pio/build/d1_mini/firmware.bin`.

### Previous board upload history

The previous board could not be flashed on the original computer.
The uploader detected the ESP8266 and started its RAM helper, then timed out before
writing flash blocks. Attempts at 115200 and 57600 baud, the ROM-only path, and
esptool 4.11.0 also failed. The desk cable was confirmed disconnected. A flash ID
could not be obtained; this does not establish whether the fault is in the USB
connection, power, or board. This history does not establish a fault in the
replacement D1 Mini. The bridge firmware builds and its host tests pass; upload
and physical forwarding still need verification on the replacement.

`platformio.ini` currently sets the upload speed to 57600 baud.

## Diagnostics

The page polls `GET /api/status` twice per second. The endpoint also works directly:

```sh
curl http://192.168.4.1/api/status
```

- `state` is `waiting_for_frames`, `fresh`, `stale`, or `unsupported_display`.
- `height_cm` is a number only while fresh, otherwise `null`.
- `last_known_height_cm` retains the last decoded number until the ESP restarts.
  When `fresh` is false, the page shows this value with a `Last known height` label.
  Before the first height reading, the page shows `No reading`.
- `height_age_ms` measures time since the last accepted numeric reading. A reading
  becomes stale at 1,000 ms. A checksum-valid, unrecognized display invalidates
  the current reading immediately.
- `last_frame` is the latest complete six-byte candidate as hexadecimal text,
  including candidates with bad checksums. `last_frame_age_ms` is its age.
- `bytes_received` counts received bytes. `valid_frames` counts checksum-valid
  frames, including unrecognized displays. `invalid_displays` counts those
  unrecognized digit codes or invalid decimal layouts.
- `checksum_failures` and `partial_timeouts` help diagnose framing problems.
- `uart_overruns` counts observed software-UART overflow flags on D5.
- `panel_bytes_received` and `panel_bytes_forwarded` count bytes read on D6 and
  forwarded unchanged on D7. With no web command, they match once buffered bytes
  have been forwarded. During web commands, idle bytes are counted separately in
  `panel_bytes_suppressed`. These counts do not prove the desk received them.
- `esp_bytes_sent` counts generated button/release bytes successfully written on D7.
- `panel_invalid_frames` and `panel_partial_timeouts` count corrupt panel frames
  and incomplete frames flushed after a 30 ms receive gap.
- `physical_overrides` counts non-idle panel frames that cancelled an ESP press.
- `panel_write_failures` counts unsuccessful byte writes. Expect zero.
- `panel_uart_overruns` counts observed overflow flags on the panel input. Expect
  both overflow counters to stay zero. They are not exact counts of lost bytes;
  software serial does not expose the old hardware `uart_errors` field.
- `panel_age_ms` is time since the last panel byte was read. `panel_fresh` becomes
  false after 1,000 ms without bytes; this indicates traffic, not frame validity.
- `passthrough_enabled` and `commands_enabled` are `true`.
- `remote_button` is `idle`, `up`, `down`, or `preset_1`/`preset_2`/`preset_3`.
  It describes the ESP button press, not whether the desk is physically moving.
- `remote_command_id` identifies the last accepted ESP command. `remote_ready`
  means a new command can start: recent idle panel traffic, no active ESP press,
  and a completed release gap.
- `hold_lease_ms`, `max_hold_ms`, `preset_press_ms`, and `release_gap_ms` expose
  command timing settings.

Frames must start with `5A`, contain a valid checksum, and use recognized
seven-segment codes. Digit decoding respects the decimal-point bit, so both
`78.6` and `105` work. Multiple decimal points and a trailing decimal point are
rejected. Unknown status bytes are preserved in the raw frame, without assigning
them a meaning. After a bad checksum the parser looks for another header inside
the rejected frame. Incomplete frames are discarded after a 30 ms receive gap.

If the page cannot reach the monitor, it clears the displayed live height and
shows `Monitor unreachable`.

## Web controls and command API

Preset buttons simulate a 100 ms press, followed by release. The owner confirmed
that the desk completes preset travel without holding the button and that any
new button press interrupts preset travel, including the same preset again.
There must be at least 100 ms of release before another ESP press can start.
The press duration still needs checking on hardware.

UP/DOWN buttons move while held. The page renews a command every 100 ms; the ESP
expires it after 300 ms without renewal. A single hold is limited to ten seconds,
then requires a new press. Releasing the button, hiding the page, or losing focus
requests release. A physical panel press cancels the ESP command, and later web
renewals for its old ID are rejected.

Commands do not depend on the display being a fresh height. New commands require
a checksum-valid idle panel frame within the last 250 ms. Missing panel traffic,
malformed traffic, UART overflow, or an observed TX failure cancels ESP control.
No ESP command resumes after reboot.

All control endpoints use `POST` with form fields. Successful responses contain
`command_id`; invalid arguments return HTTP 400 and conflicts return HTTP 409.
The page never automatically retries a preset request.

```sh
# Press preset 1. Change number to 2 or 3 for the other presets.
curl -d 'number=1' http://192.168.4.1/api/preset

# Start an UP hold. The response gives its command_id.
curl -d 'direction=up' http://192.168.4.1/api/move

# Renew that hold before its lease expires, using the returned ID.
curl -d 'direction=up&id=123' http://192.168.4.1/api/move

# Release that command. Replace 123 with the returned ID.
curl -d 'id=123' http://192.168.4.1/api/release
```

Use `direction=down` for DOWN. Renewals require the same direction and ID as the
active hold. IDs also prevent a delayed release from cancelling a newer command.
For manual API tests, a lone move request expires automatically after 300 ms.

`/api/release` ends the simulated button press. It does not stop autonomous preset
travel. Pressing a preset again can interrupt travel but can also start travel
when stationary, so it is not labelled as an unconditional Stop button.

## Host tests

Run these from the project directory. The build above creates `.pio`:

```sh
g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -Iinclude \
  tests/desk_protocol_test.cpp -o .pio/desk_protocol_test
.pio/desk_protocol_test
g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -Iinclude \
  tests/panel_bridge_test.cpp -o .pio/panel_bridge_test
.pio/panel_bridge_test
node tests/web_controls_test.js
```

Tests cover all seven captured examples, checksum rejection, invalid display
codes, decimal placement, stale readings, recovery after corrupt/truncated frames,
header-valued payload bytes, partial-frame timeouts, and timer wraparound.
Bridge tests cover transparent forwarding, malformed-frame recovery, command
timing, release gaps, physical-panel priority, stale renewal/release rejection,
lease and absolute hold expiry, failure cancellation, and timer wraparound.
Web tests cover preset submission, hold renewal/release, early pointer release,
physical override, connection failure, and cached-height display. These tests do
not verify electrical signaling or software-UART timing with Wi-Fi.

## First desk test

Start with the desk stationary and keep the page open for at least 30 seconds.
Check the original panel display and compare the web height with it. Then test a
short physical UP/DOWN press and release, followed by the original preset buttons.
Keep the page polling throughout, and report:

- Whether the access point and page appear and remain available.
- Whether the height matches the desk's known position.
- Whether panel buttons, release, and presets behave as with the direct cable.
- The JSON diagnostics, especially byte/frame counters and the last frame.
- Whether panel received/forwarded counters match while web commands are inactive,
  and both overflow counters and write failures remain zero.
- Whether the desk behaves differently during power-up or reset.

Increasing valid-frame counts with a stable height establish the first receive
test. Zero status bytes suggest checking the D5 tap, ground, signal levels, and
controller traffic. Zero panel bytes suggest checking the panel-side D6 wire and
panel power. Increasing bytes with few valid frames suggests a signal or framing
problem. Live forwarding latency, UART reliability with Wi-Fi, and desk-powered
supply stability still require hardware verification.

## First remote-control test

After flashing, check a brief web UP/DOWN hold and release, then a preset click.
Check that pressing a preset again interrupts travel. During a web hold, press
any physical panel button and confirm that its action passes through and web
movement does not resume. Also release a web hold before its start request
finishes, hide the page during a hold, and disconnect Wi-Fi during a hold.
Report behavior and `/api/status`, especially `panel_uart_overruns`,
`panel_write_failures`, `panel_invalid_frames`, and `physical_overrides`.
