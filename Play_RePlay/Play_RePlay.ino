/*
 * pico_macro_recorder.ino
 * ------------------------
 * Board:  Raspberry Pi Pico (RP2040), Arduino-Pico core by Earle Philhower
 * Role:   USB composite device (Serial CDC + Keyboard + Mouse HID)
 *
 * Behavior:
 *   - Button 1 (RECORD_BUTTON_PIN): first press starts recording
 *       -> sends "REC:1" over Serial (tells the PC-side Python script to
 *          start forwarding captured keyboard/mouse events)
 *       -> every line received afterward on Serial is appended to a file
 *          on LittleFS, already timestamped by the PC script.
 *     Second press stops recording
 *       -> sends "REC:0" (Python script stops forwarding)
 *       -> closes the file.
 *
 *   - Button 2 (PLAY_BUTTON_PIN): replays the last recorded macro using
 *     Keyboard.h and Mouse.h, respecting the millisecond delays stored
 *     with each event.
 *
 * Expected line formats received over Serial while recording:
 *   K,<1|0>,<hex_keycode>,<delta_ms>          e.g. K,1,61,0
 *   M,MOVE,<dx>,<dy>,<delta_ms>                e.g. M,MOVE,12,-4,8
 *   M,CLICK,<left|right|middle>,<1|0>,<delta_ms>
 *   M,SCROLL,<dy>,<delta_ms>
 *
 * Notes:
 *   - Mouse.move() is RELATIVE, so dx/dy must already be relative deltas
 *     (the companion Python script computes these, not absolute coords).
 *   - Keyboard.press()/release() accept a single byte: either a plain
 *     ASCII character for printable keys, or one of the special HID
 *     codes defined in Keyboard.h (0x80+) for modifiers/function keys.
 *     The Python script already sends the correct byte as 2 hex chars.
 *   - Reserve enough flash for LittleFS via Tools > Flash Size in the
 *     Arduino IDE board menu (the default split is usually fine for
 *     macros of a few hundred thousand events; increase it if needed).
 */

#include <Keyboard.h>
#include <Mouse.h>
#include <LittleFS.h>

//pin configuration 
const int RECORD_BUTTON_PIN = 22;   // Button 1: start/stop recording
const int PLAY_BUTTON_PIN   = 23;   // Button 2: play last recording

const unsigned long DEBOUNCE_MS = 50;
const char* MACRO_FILE_PATH = "/macro.txt";

//state machine 
enum SystemState { STATE_IDLE, STATE_RECORDING, STATE_PLAYING };
SystemState currentState = STATE_IDLE;

File macroFile;

//debounced button helper 
struct DebouncedButton {
  int pin;
  int lastReading;
  int stableState;
  unsigned long lastChangeTime;
};

DebouncedButton recordButton = { RECORD_BUTTON_PIN, HIGH, HIGH, 0 };
DebouncedButton playButton   = { PLAY_BUTTON_PIN,   HIGH, HIGH, 0 };

// Returns true exactly once, on the transition from released to pressed
// (buttons are wired active-LOW using the internal pull-up).
bool buttonPressedEdge(DebouncedButton &btn) {
  int reading = digitalRead(btn.pin);
  if (reading != btn.lastReading) {
    btn.lastChangeTime = millis();
  }
  bool edge = false;
  if ((millis() - btn.lastChangeTime) > DEBOUNCE_MS) {
    if (reading != btn.stableState) {
      btn.stableState = reading;
      if (btn.stableState == LOW) {
        edge = true;  // just pressed
      }
    }
  }
  btn.lastReading = reading;
  return edge;
}

//Setup
void setup() {
  pinMode(RECORD_BUTTON_PIN, INPUT_PULLUP);
  pinMode(PLAY_BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  Keyboard.begin();
  Mouse.begin();

  if (!LittleFS.begin()) {
    // First boot / corrupted filesystem: format once and retry.
    LittleFS.format();
    LittleFS.begin();
  }
}

//#########################Recording controls
void startRecording() {
  if (LittleFS.exists(MACRO_FILE_PATH)) {
    LittleFS.remove(MACRO_FILE_PATH);
  }
  macroFile = LittleFS.open(MACRO_FILE_PATH, "w");
  currentState = STATE_RECORDING;
  Serial.println("REC:1");
}

void stopRecording() {
  if (macroFile) {
    macroFile.close();
  }
  currentState = STATE_IDLE;
  Serial.println("REC:0");
}

void handleRecordButton() {
  if (!buttonPressedEdge(recordButton)) return;

  if (currentState == STATE_IDLE) {
    startRecording();
  } else if (currentState == STATE_RECORDING) {
    stopRecording();
  }
  // Ignored if currently playing.
}

// reads whatever is available on Serial and appends complete lines to the macro file while a recording is in progress
void captureSerialToFile() {
  static String lineBuffer;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n') {
      lineBuffer.trim();
      if (lineBuffer.length() > 0 && macroFile) {
        macroFile.println(lineBuffer);
      }
      lineBuffer = "";
    } else if (c != '\r') {
      lineBuffer += c;
    }
  }
}

//playback 
uint8_t hexPairToByte(const String &hex) {
  auto hexDigit = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  if (hex.length() < 2) return 0;
  return (hexDigit(hex[0]) << 4) | hexDigit(hex[1]);
}

// splits a comma-separated line into at most maxFields tokens
int splitCSV(const String &line, String fields[], int maxFields) {
  int count = 0;
  int start = 0;
  while (count < maxFields) {
    int idx = line.indexOf(',', start);
    if (idx == -1) {
      fields[count++] = line.substring(start);
      break;
    }
    fields[count++] = line.substring(start, idx);
    start = idx + 1;
  }
  return count;
}

void playbackEvent(const String &line) {
  String fields[5];
  int n = splitCSV(line, fields, 5);
  if (n < 2) return;

  if (fields[0] == "K") {
    // K,<1|0>,<hex_keycode>,<delta_ms>
    if (n < 4) return;
    bool pressed = (fields[1] == "1");
    uint8_t code = hexPairToByte(fields[2]);
    unsigned long deltaMs = fields[3].toInt();

    delay(deltaMs);
    if (pressed) {
      Keyboard.press((char)code);
    } else {
      Keyboard.release((char)code);
    }

  } else if (fields[0] == "M") {
    if (fields[1] == "MOVE" && n >= 5) {
      // M,MOVE,<dx>,<dy>,<delta_ms>  (relative movement)
      int dx = fields[2].toInt();
      int dy = fields[3].toInt();
      unsigned long deltaMs = fields[4].toInt();

      delay(deltaMs);
      Mouse.move(dx, dy, 0);

    } else if (fields[1] == "CLICK" && n >= 4) {
      // M,CLICK,<left|right|middle>,<1|0>,<delta_ms>
      String buttonName = fields[2];
      bool pressed = (fields[3] == "1");
      unsigned long deltaMs = (n >= 5) ? fields[4].toInt() : 0;

      uint8_t mouseButton = MOUSE_LEFT;
      if (buttonName == "right") mouseButton = MOUSE_RIGHT;
      else if (buttonName == "middle") mouseButton = MOUSE_MIDDLE;

      delay(deltaMs);
      if (pressed) {
        Mouse.press(mouseButton);
      } else {
        Mouse.release(mouseButton);
      }

    } else if (fields[1] == "SCROLL" && n >= 4) {
      // M,SCROLL,<dy>,<delta_ms>
      int dy = fields[2].toInt();
      unsigned long deltaMs = fields[3].toInt();

      delay(deltaMs);
      Mouse.move(0, 0, dy);
    }
  }
}

void playRecording() {
  currentState = STATE_PLAYING;

  File f = LittleFS.open(MACRO_FILE_PATH, "r");
  if (!f) {
    currentState = STATE_IDLE;
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      playbackEvent(line);
    }
  }

  // Release everything at the end, in case the recording was cut off
  // mid-press (avoids "stuck" keys or held mouse buttons).
  Keyboard.releaseAll();
  Mouse.release(MOUSE_LEFT);
  Mouse.release(MOUSE_RIGHT);
  Mouse.release(MOUSE_MIDDLE);

  f.close();
  currentState = STATE_IDLE;
}

void handlePlayButton() {
  if (!buttonPressedEdge(playButton)) return;
  if (currentState == STATE_IDLE) {
    playRecording();
  }
  //ignored while recording or already playing
}

// main loop s
void loop() {
  handleRecordButton();
  handlePlayButton();

  if (currentState == STATE_RECORDING) {
    captureSerialToFile();
  }
}
