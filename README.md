# Play/Replay

![Version](https://img.shields.io/badge/version-1.0.0-blue)
![Status](https://img.shields.io/badge/status-stable-brightgreen)
![Platform](https://img.shields.io/badge/platform-RP2040-orange)
![Python](https://img.shields.io/badge/python-3.8%2B-yellow)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

**Play/Replay** turns a Raspberry Pi Pico into a hardware-backed macro recorder for keyboard and mouse input. Press a button, do things on your PC, press it again and the exact sequence of keystrokes and mouse movement, down to the MILLISECOND, is stored on the Pico's own flash. Press the second button and it plays back on any USB host, looping forever until you stop it. No host PC or driver required.



## Why this exists

Most macro recorder tools live entirely in software on one machine: they hook the OS, store the macro in a config file, and replay it through the same OS's input APIs. That's bull-dookie (breaks across reboots, requires the software running, tied to one OS) and it can't target a *different* machine than the one you recorded on.

Play/Replay splits the job in two:

- **The Capture** happens at the OS level on a source PC because that's the only place you can reliably see input from *any* keyboard, including a laptop's built-in one, which never appears on an external USB bus.
- **Storage and playback** happen entirely on the Pico, as a real USB HID device. Once a macro is recorded the Pico can be unplugged from the source PC and plugged into any other USB host, it will type and click exactly like a human, **indistinguishable** at the protocol level from a real keyboard/mouse.

## Features

- Millisecond-accurate timing between every event (key press/release, mouse move, click, scroll)
- Two-button workflow: button 1 toggles recording, button 2 toggles looping playback (repeats forever until pressed again, even mid-sequence)
- Macros persist in flash (LittleFS) can survive power loss, no host PC needed for playback
- Works with **any** keyboard, including laptop-integrated ones (capture is OS-level, not USB-level)
- Relative mouse movement, compatible with `Mouse.h`'s HID model
- Onboard RGB LED status feedback (solid red = recording, solid green = playing, pulsing blue = idle)
- No visible window, no interruption of full-screen applications while capturing, safe to use with games
- Automatic serial reconnection: unplugging/replugging the Pico never freezes the PC-side script
- Interactive COM-port picker, or fully silent/headless operation for background use
- One Pico, one USB cable, composite USB device (Serial CDC + Keyboard + Mouse HID simultaneously)!


## Hardware requirements

- Raspberry Pi Pico (RP2040), any variant with enough flash for the macros you plan to record. Tested on a **YD-RP2040** board with onboard NeoPixel and 16MB of flash memory.
- 2 momentary push buttons, wired between a GPIO pin and GND (internal pull-ups are used, no external resistors needed)
- **Avoid GPIO26-29 for the buttons.** Those double as ADC0-3 and on several RP2040 boards are also tied to onboard analog dividers. That combination fights the internal pull-up and can read as noisy/floating instead of a clean HIGH when idle causing random "button pressed" detections. Any other GPIO (like GP14, GP15, GP22) works fine.
- If your board has an onboard NeoPixel (like the YD-RP2040's, on GPIO23) check whether the solder-bridge connecting the data line to the LED is actually closed on your revision, on some boards it isn't by default.
- A single USB cable to the source PC for recording: unplug and move to the target machine for playback

## Software requirements

- [Arduino IDE](https://www.arduino.cc/en/software) with the [arduino-pico](https://github.com/earlephilhower/arduino-pico) board package (Earle Philhower's RP2040 core), provides 'Keyboard.h', 'Mouse.h', and 'LittleFS.h'
- [Adafruit NeoPixel library](https://github.com/adafruit/Adafruit_NeoPixel) (via Library Manager), for the status LED
- Python 3.8+ on the source PC, with: pip install pynput pyserial
  

## Installation

1. Install the arduino-pico board package via Boards Manager in Arduino IDE, and the Adafruit NeoPixel library via Library Manager.
2. In **Tools -> Flash Size**, reserve a filesystem partition for LittleFS (**required!!!** The sketch will fail to mount storage otherwise). I'm using 15MB for the FS and 1MB for the code.
3. Open 'pico_macro_recorder.ino', adjust 'RECORD_BUTTON_PIN' / 'PLAY_BUTTON_PIN' and 'LED_PIN' to match your wiring (avoid GP26-29, see [Hardware requirements](#hardware-requirements)), and upload it to the Pico.
4. On the source PC, install the Python dependencies and run:
   
   python input_forwarder.py            # interactive port picker
   python input_forwarder.py --port COM5             # or specify directly (Windows)
   python input_forwarder.py --port /dev/ttyACM0      # or (Linux/macOS)
   

## Usage

1. Plug the Pico into your source PC. Make sure 'input_forwarder.py' is running (see [Running headless](#running-headless) to have it start automatically and silently).
2. Press button 1 to start recording (LED turns solid red). Do whatever you want captured.
3. Press button 1 again to stop (LED returns to the idle pulse). The macro is now saved on the Pico's flash.
4. Unplug the Pico and plug it into the target machine (or leave it on the same one).
5. Press button 2 to start looping playback (LED turns solid green), it repeats the macro continuously.
6. Press button 2 again at any point to stop it immediately (LED returns to the idle pulse).

## Running headless

Because 'input_forwarder.py' has no GUI and creates no window running it in the background never steals focus or interrupts whatever is on screen, including full-screen applications and games. To have it start automatically without any visible window:

- **Windows**: run it with 'pythonw.exe' instead of 'python.exe' (no console window at all), passing '--port COMx' explicitly (this also skips the interactive menu, which needs a console to display).
- **macOS**: use a [LaunchAgent](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html) ('~/Library/LaunchAgents/*.plist') to start it at login.
- **Linux**: use a systemd user service, or simply 'nohup python3 input_forwarder.py --port /dev/ttyACM0 &'.

## Storage capacity

Each event is stored as a short text line (roughly 8–20 bytes depending on the event type). On a 16 MB flash Pico, that translates to **on the order of a million events**, which in practice means several hours of typical usage (mouse movement is the dominant factor since keyboards generate comparatively few events). Durations shrink significantly if you record continuous high-polling-rate mouse movement and can be extended further with delta compression if needed.

## Known limitations

- **Terms of service**: replaying recorded input in online multiplayer games very likely violates the game's ToS independent of whether it's technically detectable. Use responsibly and at your own risk, lmao.
- **Text playback is timing-based, not text-based**: this is not an autotype tool with instant text injection, it reproduces the original timing down to the last minute detail (heh) including any hesitations or corrections that were part of the original recording.
- **No absolute mouse positioning**: 'Mouse.h' only supports relative movement so playback accuracy depends on the target screen resolution/cursor state matching the recording session closely.
- **ADC-capable GPIOs (26-29) are unreliable for buttons!!!** on several RP2040 boards, see [Hardware requirements](#hardware-requirements).

## Contributing

Issues and pull requests are welcome in particular around delta/varint compression for longer recordings, dual-role USB host+device wiring for on-device (non-PC-assisted) capture and cross-platform packaging of the Python forwarder. My gf contributed to this project by making me not crashout when I foolishly soldered the record button to pin 29 and it was bugging out.
