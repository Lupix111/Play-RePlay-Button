"""
Script made by Emanuele Carlino, gimme a dolla if you like this!
dependencies:
    pip install pynput pyserial
usage:
    python input_forwarder.py --port COM5        (Windows)
    python input_forwarder.py --port /dev/ttyACM0 (Linux/macOS)
permissions note:
    - macOS: Settings -> Privacy & Security -> Accessibility / Input
      Monitoring must include the terminal/IDE this script runs from.
      (I have yet to test this on MacOS since I don't own a Mac)
    - Linux: on Wayland, global keyboard capture is prolly restricted by
      the compositor, on X11 it SHOULD work normally.
    - Windows: no special permission needed but antivirus software
      may flag the script for the global-hook pattern (a common false
      positive for any tool like this)
"""
import argparse
import sys
import threading
import time

try:
    import serial
except ImportError:
    sys.exit("Missing pyserial, install with: pip install pyserial")

from serial.tools import list_ports

try:
    from pynput import keyboard, mouse
except ImportError:
    sys.exit("Missing pynpum, install with: pip install pynput")

'''
Keyboard.h constants (Arduino-Pico core / Arduino Leonardo family).
Printable keys (letters, digits, symbols) use their own ASCII byte
and don't appear here; this table only covers the "special" keys
that have a dedicated non-ASCII code in Keyboard.h
'''
SPECIAL_KEY_TO_HID = {
    "shift": 0x81, "shift_r": 0x85,
    "ctrl": 0x80, "ctrl_l": 0x80, "ctrl_r": 0x84,
    "alt": 0x82, "alt_l": 0x82, "alt_r": 0x86,
    "cmd": 0x83, "cmd_l": 0x83, "cmd_r": 0x87,  #Windows/Command key
    "caps_lock": 0xC1,
    "tab": 0xB3,
    "enter": 0xB0,
    "backspace": 0xB2,
    "esc": 0xB1,
    "space": 0x20,  #space is ASCII, but pynput names it "space"
    "up": 0xDA, "down": 0xD9, "left": 0xD8, "right": 0xD7,
    "delete": 0xD4,
    "insert": 0xD1,
    "home": 0xD2,
    "end": 0xD5,
    "page_up": 0xD3,
    "page_down": 0xD6,
    "f1": 0xC2, "f2": 0xC3, "f3": 0xC4, "f4": 0xC5,
    "f5": 0xC6, "f6": 0xC7, "f7": 0xC8, "f8": 0xC9,
    "f9": 0xCA, "f10": 0xCB, "f11": 0xCC, "f12": 0xCD,
}


def key_to_hid_code(name: str) -> str:
    """
    Converts a key name (as returned by pynput) into the code that
    Keyboard.h expects as a 2-digit hex string (e.g. 'a' -> '61',
    F1 -> 'c2'), unmapped keys fall back to '00_' + the original name
    so nothing is silently lost even if a table entry is missing.
    """
    key = name.lower()
    if key in SPECIAL_KEY_TO_HID:
        return f"{SPECIAL_KEY_TO_HID[key]:02x}"
    if len(key) == 1:
        return f"{ord(key):02x}"
    # Unrecognized key (special layouts, media keys): forward the
    # raw name as a fallback, handle it on the Pico side if needed.
    return "00_" + name


class InputForwarder:
    def __init__(self, port: str, baud: int = 115200):
        self.port_name = port
        self.baud = baud
        self.ser = None            #none whenever the Pico is unplugged/unreachable
        self.recording = False
        self.last_ts = None        #perf_counter timestamp of the last forwarded event
        self.last_x = None
        self.last_y = None
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self._open_serial()        # first connection attempt; reconnect_loop retries afterwards


                                             #connection management 
    def _open_serial(self):
        """tries to (re)open the serial port, never raises: leaves self.ser as None on failure"""
        try:
            ''' 
            write_timeout is the important part here: without it, write() can
            block FOREVER if the device is physically unplugged, which is
            exactly what made the script originally freeze and require Task Manager
            to kill it. With a timeout, a dead write turns into a normal
            catchable SerialTimeoutException instead of a permanent hang
            '''
            self.ser = serial.Serial(self.port_name, self.baud, timeout=0.05, write_timeout=0.5)
            print(f"[forwarder] Connected to {self.port_name}")
        except (serial.SerialException, OSError):
            self.ser = None

    def _drop_connection(self):
        """closes and forgets the current connection so the reconnect loop can retry."""
        with self.lock:
            if self.ser is not None:
                try:
                    self.ser.close()
                except (serial.SerialException, OSError):
                    pass
                self.ser = None
                self.recording = False
        print("[forwarder] Pico disconnected, will keep retrying in the background...")

    def reconnect_loop(self):
        """Runs for the whole lifetime of the script; reopens the port whenever it's missing."""
        while not self.stop_event.is_set():
            with self.lock:
                needs_reconnect = self.ser is None
            if needs_reconnect:
                self._open_serial()
            self.stop_event.wait(1.0)

                            #handling incoming commands from the Pico 
    def serial_listener(self):
        """reads lines from the Pico ('REC:1', 'REC:0') and updates state"""
        buf = b""
        while not self.stop_event.is_set():
            with self.lock:
                ser = self.ser
            if ser is None:
                time.sleep(0.2)
                continue
            try:
                chunk = ser.read(64)
            except (serial.SerialException, OSError):
                self._drop_connection()
                buf = b""
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if line == b"REC:1":
                    with self.lock:
                        self.recording = True
                        self.last_ts = time.perf_counter()
                    print("[forwarder] Recording STARTED by the Pico: YOU ARE LIVE!")
                elif line == b"REC:0":
                    with self.lock:
                        self.recording = False
                    print("[forwarder] Recording STOPPED by the Pico: Recording Done")

                                #sending a single event
    def send(self, line: str):
        with self.lock:
            if not self.recording or self.ser is None:
                return
            now = time.perf_counter()
            delta_ms = 0 if self.last_ts is None else round((now - self.last_ts) * 1000)
            self.last_ts = now
            ser = self.ser
        try:
            ser.write(f"{line},{delta_ms}\n".encode("ascii", errors="ignore"))
        except (serial.SerialException, OSError):
            '''
            covers both a timed-out write (dead port) and a hard I/O error
            (device physically removed). either way drop the connection
            instead of leaving the script stuck retrying on a broken handle
            self._drop_connection()
            '''
                                        #keyboard callbacks 
    def on_key(self, key, pressed: bool):
        #normalize the key into a stable string like "a", "space", "shift_l", "f5"
        if isinstance(key, keyboard.KeyCode):
            name = key.char if key.char is not None else f"vk{key.vk}"
        else:
            name = str(key).replace("Key.", "")
        hid_code = key_to_hid_code(name)
        #the Pico receives the byte already ready for Keyboard.press()/release(),
        #encoded as 2 hex characters (e.g. "61" for 'a', "c2" for F1).
        self.send(f"K,{1 if pressed else 0},{hid_code}")

    def on_press(self, key):
        self.on_key(key, True)

    def on_release(self, key):
        self.on_key(key, False)

                                    #mouse callbacks 
    def on_move(self, x, y):
        with self.lock:
            if self.last_x is None:
                dx, dy = 0, 0
            else:
                dx, dy = x - self.last_x, y - self.last_y
            self.last_x, self.last_y = x, y
        #mouse.h moves the cursor RELATIVELY: send dx,dy, not absolute x,y
        self.send(f"M,MOVE,{dx},{dy}")

    def on_click(self, x, y, button, pressed):
        btn = str(button).replace("Button.", "")
        self.send(f"M,CLICK,{btn},{1 if pressed else 0}")

    def on_scroll(self, x, y, dx, dy):
        self.send(f"M,SCROLL,{dy}")

                                                #startup 
    def run(self):
        threading.Thread(target=self.reconnect_loop, daemon=True).start()
        threading.Thread(target=self.serial_listener, daemon=True).start()
        '''
        daemon=True is what  fixes the "kill this mf with Task Manager"
        symptom even if pynput's internal OS hooks are slow to release
        '''
        kb_listener = keyboard.Listener(on_press=self.on_press, on_release=self.on_release)
        kb_listener.daemon = True
        ms_listener = mouse.Listener(on_move=self.on_move, on_click=self.on_click, on_scroll=self.on_scroll)
        ms_listener.daemon = True

        kb_listener.start()
        ms_listener.start()

        print(f"[forwarder] Listening on {self.port_name}. Waiting for REC:1 from the Pico...")
        print("[forwarder] Press Ctrl+C to quit.")

        try:
            while kb_listener.is_alive() and ms_listener.is_alive():
                time.sleep(0.2)
        except KeyboardInterrupt:
            print("\n[forwarder] Shutting down...")
        finally:
            self.stop_event.set()
            kb_listener.stop()
            ms_listener.stop()
            with self.lock:
                if self.ser is not None:
                    try:
                        self.ser.close()
                    except (serial.SerialException, OSError):
                        pass
            sys.exit(0)


def choose_port() -> str:
    #lists available serial ports and lets the user pick one by number
    ports = list(list_ports.comports())
    if not ports:
        sys.exit("No serial ports found. Plug in the Pico and try again, dumbazz.")

    print("Available serial ports:")
    for i, p in enumerate(ports, start=1):
        print(f"  {i}. {p.device} - {p.description}")

    while True:
        choice = input(f"Select a port [1-{len(ports)}]: ").strip()
        if choice.isdigit() and 1 <= int(choice) <= len(ports):
            return ports[int(choice) - 1].device
        print("Invalid selection, try again.")


def main():
    parser = argparse.ArgumentParser(description="Forward keyboard/mouse events to the Pico over serial")
    parser.add_argument("--port", default=None, help="Pico serial port like COM5 or /dev/ttyACM0"
                                                       "Omit to pick interactively from a menu")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default 115200)")
    args = parser.parse_args()

    port = args.port if args.port else choose_port()

    fwd = InputForwarder(port, args.baud)
    fwd.run()


if __name__ == "__main__":
    main()
