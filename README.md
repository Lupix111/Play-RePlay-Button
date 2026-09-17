# Play-RePlay-Button
Two buttons that allow you to record and replay what you just did with your mouse and keyboard while gaming. Meant to be used if you're doing the same stupid quest over and over again to farm levels. 


## Why this exists

I just wanted to do a fun little project with a pico and this is what I came up with. 

Play/Replay splits the job in two:

- **Capture** happens at the OS level on a source PC (via a lightweight Python background process), because that's the only place you can reliably see input from *any* keyboard, including a laptop's built-in one, which never appears on an external USB bus.
- **Storage and playback** happen entirely on the Pico, as a real USB HID device. Once a macro is recorded, the Pico can be unplugged from the source PC and plugged into any other USB host, it will type and click exactly like a human, indistinguishable at the protocol level from a real keyboard/mouse.

## Features

- Millisecond-accurate timing between every event (key press/release, mouse move, click, scroll)
- Two-button workflow: button 1 toggles recording, button 2 replays
- Macros persist in flash (LittleFS) and survive power loss, no host PC needed for playback
- Works with **any** keyboard, including laptop-integrated ones (capture is OS-level, not USB-level)
- Relative mouse movement, compatible with `Mouse.h`'s HID model
- No visible window, no interruption of full-screen applications while capturing
- Single Pico, single USB cable, composite USB device (Serial CDC + Keyboard + Mouse HID simultaneously)

## How it works

### 1. Capture (PC-side, Python)

`input_forwarder.py` runs continuously and quietly in the background (so no console window) and uses [`pynput`](https://pynput.readthedocs.io/) to install a global OS-level listener for keyboard and mouse events. It does **not** forward anything by default, it waits for a `REC:1` command from the Pico over serial before it starts sending events, and stops on `REC:0`. This means the script's own startup never needs to coincide with the recording itself: you can leave it running permanently (e.g. launched at login) and just press the Pico's button whenever you want to record.

Every event is translated into a compact line and sent over the serial (USB CDC) connection, with the millisecond delta since the previous event already computed on the PC side (using `time.perf_counter()` for sub-millisecond precision before rounding).

### 2. Storage (Pico-side, Arduino/RP2040)

While recording is active, the Pico appends every incoming line verbatim to a file on its internal flash filesystem (LittleFS). No translation happens at this stage, the PC has already encoded keycodes into the exact byte values `Keyboard.h` expects, so the Pico's job here is just "receive line, write line."

### 3. Playback (Pico-side, USB HID)

On the second button press, the Pico reads the stored file back line by line, `delay()`s for the recorded millisecond delta before each event, and calls `Keyboard.press()/release()` or `Mouse.move()/press()/release()` accordingly. Because this happens entirely through standard USB HID descriptors (via `Keyboard.h`/`Mouse.h`), the target machine sees a completely ordinary keyboard and mouse. No drivers, no special software, works on any OS that supports USB HID (which is effectively all of them, including BIOS/UEFI screens).

## Hardware requirements

- Raspberry Pi Pico (RP2040) any variant with 16MB flash for the macros you plan to record (see [Storage capacity](#storage-capacity))
- 2 momentary push buttons, wired between a GPIO pin and GND (internal pull-ups are used, no external resistors needed)
- A single USB cable to the source PC for recording, unplug and move to the target machine for playback

## Software requirements

- [Arduino IDE](https://www.arduino.cc/en/software) with the [arduino-pico](https://github.com/earlephilhower/arduino-pico) board package (Earle Philhower's RP2040 core), provides `Keyboard.h`, `Mouse.h`, and `LittleFS.h`
- Python 3.8+ on the source PC, with:
  ```bash
  pip install pynput pyserial
  ```

## Installation

1. Install the arduino-pico board package via Boards Manager in Arduino IDE.
2. In **Tools -> Flash Size**, reserve a filesystem partition for LittleFS (required, the sketch will fail to mount storage otherwise).
3. Open `pico_macro_recorder.ino`, adjust `RECORD_BUTTON_PIN` / `PLAY_BUTTON_PIN` to match your wiring, and upload it to the Pico.
4. On the source PC, install the Python dependencies and run:
   ```bash
   python input_forwarder.py --port COM5        # Windows
   python input_forwarder.py --port /dev/ttyACM0 # Linux/macOS
   ```

## Usage

1. Plug the Pico into your source PC. Make sure `input_forwarder.py` is running (see [Running headless](#running-headless) to have it start automatically and silently).
2. Press button 1 to start recording. Do whatever you want captured.
3. Press button 1 again to stop. The macro is now saved on the Pico's flash.
4. Unplug the Pico and plug it into the target machine (or leave it on the same one).
5. Press button 2 to replay the macro.

## Running headless

Because `input_forwarder.py` has no GUI and creates no window, running it in the background never steals focus or interrupts whatever is on screen, including full-screen applications and games. To have it start automatically without any visible window:

- **Windows**: run it with `pythonw.exe` instead of `python.exe` (no console window at all), or package it with `pyinstaller --noconsole --onefile`. Add it to Task Scheduler with an "At log on" trigger for full automation.
- **macOS**: use a [LaunchAgent](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html) (`~/Library/LaunchAgents/*.plist`) to start it at login.
- **Linux**: use a systemd user service, or simply `nohup python3 input_forwarder.py --port /dev/ttyACM0 &`.

## Serial protocol reference

Lines sent from the PC to the Pico while recording (`\n`-terminated):

| Event | Format | Example |
|---|---|---|
| Key down/up | `K,<1\|0>,<hex_keycode>,<delta_ms>` | `K,1,61,0` |
| Mouse move (relative) | `M,MOVE,<dx>,<dy>,<delta_ms>` | `M,MOVE,12,-4,8` |
| Mouse click | `M,CLICK,<left\|right\|middle>,<1\|0>,<delta_ms>` | `M,CLICK,left,1,340` |
| Mouse scroll | `M,SCROLL,<dy>,<delta_ms>` | `M,SCROLL,-1,15` |

Lines sent from the Pico to the PC:

| Command | Meaning |
|---|---|
| `REC:1` | Recording started, the PC should begin forwarding events |
| `REC:0` | Recording stopped, the PC should stop forwarding events |

Keycodes are pre-encoded on the PC side as 2 hex digits, matching the byte `Keyboard.h` expects: the ASCII value for printable characters, or one of the dedicated constants (`0x80`+) for modifiers and special keys (see `SPECIAL_KEY_TO_HID` in `input_forwarder.py`).

## Storage capacity

Each event is stored as a short text line (roughly 8–20 bytes depending on the event type). On a 16 MB flash Pico, that translates to **on the order of a million events**, which in practice means several hours of typical usage (mouse movement is the dominant factor, since keyboards generate comparatively few events). See the project discussion for the full back-of-envelope numbers, durations shrink significantly if you record continuous high-polling-rate mouse movement (e.g. gaming/drawing), and can be extended further with delta compression if needed.

## Known limitations

- **Anti-cheat and exclusive-mode games**: some titles with kernel-level anti-cheat (e.g. Vanguard, EasyAntiCheat, BattlEye) or exclusive-mode DirectInput deliberately block or don't report input to OS-level global hooks like the one this project uses, this is an intentional countermeasure against exactly this kind of capture/replay tool. Works reliably in single-player or non-protected contexts.
- **Terms of service**: replaying recorded input in online multiplayer games very likely violates the game's ToS, independent of whether it's technically detectable. Use responsibly and at your own risk.
- **Text playback is timing-based, not text-based**: this is not an "autotype" tool with instant text injection!!! It faithfully reproduces the original timing, including any hesitations or corrections that were part of the original recording.
- **No absolute mouse positioning**: `Mouse.h` only supports **relative movement**, so playback accuracy depends on the target screen resolution/cursor state matching the recording session reasonably closely.

## Contributing

Issues and pull requests are welcome, in particular around delta/varint compression for longer recordings, dual-role USB host+device wiring for on-device (non-PC-assisted) capture, and cross-platform packaging of the Python forwarder. I'm building this project with my gf so I guess there are two contributors to this project already. 
