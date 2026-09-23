//Made with love by Emanuele Carlino
#include <Keyboard.h>
#include <Mouse.h>
#include <LittleFS.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>

const int LED_PIN = 23;
const int LED_COUNT = 1;
Adafruit_NeoPixel pixel(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

const uint8_t IDLE_MAX_BRIGHTNESS = 255;   // 0-255, kept low so idle breathing isn't blinding
const unsigned long IDLE_PULSE_PERIOD_MS = 1000;  // full breathe in/out cycle

// ---------------- Pin configuration ----------------
const int RECORD_BUTTON_PIN = 5;   // Button 1: start/stop recording
const int PLAY_BUTTON_PIN   = 22;   // Button 2: play last recording

const unsigned long DEBOUNCE_MS = 50;
const char* MACRO_FILE_PATH = "/macro.txt";

// ---------------- State machine ----------------
enum SystemState { STATE_IDLE, STATE_RECORDING, STATE_PLAYING };
SystemState currentState = STATE_IDLE;

File macroFile;

// ---------------- Debounced button helper ----------------
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

// ---------------- Setup ----------------
void setup() {
  pinMode(RECORD_BUTTON_PIN, INPUT_PULLUP);
  pinMode(PLAY_BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);

  Serial.println("[boot] Serial up");

  Keyboard.begin();
  Serial.println("[boot] Keyboard.begin() done");
  Mouse.begin();
  Serial.println("[boot] Mouse.begin() done");

  pixel.begin();
  Serial.println("[boot] pixel.begin() done");
  pixel.show();  // off until the first loop() iteration sets the idle pulse
  Serial.println("[boot] pixel.show() done");

  Serial.println("[boot] mounting LittleFS...");
  if (!LittleFS.begin()) {
    // First boot / corrupted filesystem: format once and retry.
    Serial.println("[boot] mount failed, formatting (can take a while on large flash)...");
    LittleFS.format();
    Serial.println("[boot] format complete, remounting...");
    LittleFS.begin();
  }
  Serial.println("[boot] setup() complete, entering loop()");
}

// ---------------- LED feedback ----------------
void setSolidColor(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r, g, b));
  pixel.show();
}

// Non-blocking breathing blue pulse, driven entirely off millis() so it
// never interferes with button polling or the recording/playback timing
void updateIdlePulse() {
  float phase = (millis() % IDLE_PULSE_PERIOD_MS) / (float)IDLE_PULSE_PERIOD_MS;
  float wave = (sin(phase * 2.0 * PI) + 1.0) / 2.0;  // 0.0 .. 1.0
  uint8_t brightness = (uint8_t)(wave * IDLE_MAX_BRIGHTNESS);
  pixel.setPixelColor(0, pixel.Color(0, 0, brightness));
  pixel.show();
}


void startRecording() {
  if (LittleFS.exists(MACRO_FILE_PATH)) {
    LittleFS.remove(MACRO_FILE_PATH);
  }
  macroFile = LittleFS.open(MACRO_FILE_PATH, "w");
  currentState = STATE_RECORDING;
  setSolidColor(255, 0, 0);  // solid red while recording
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

// Reads whatever is available on Serial and appends complete lines
// to the macro file while a recording is in progress
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

// ---------------- Playback ----------------
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

// Splits a comma-separated line into at most maxFields tokens.
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

void playRecordingOnce(bool &stopRequested) {
  File f = LittleFS.open(MACRO_FILE_PATH, "r");
  setSolidColor(0,255,0);
  if (!f) {
    stopRequested = true;  // nothing to play; behave as if stop was requested
    return;
  }

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      playbackEvent(line);
    }

    // Checked after EVERY event, not just after a full pass, so pressing
    // the play button stops playback immediately instead of waiting for
    // the whole (possibly looping) sequence to finish.
    if (buttonPressedEdge(playButton)) {
      stopRequested = true;
      break;
    }
  }

  f.close();
}

void stopPlayback() {
  // Release everything, in case playback was interrupted mid-press
  // (avoids "stuck" keys or held mouse buttons)
  Keyboard.releaseAll();
  Mouse.release(MOUSE_LEFT);
  Mouse.release(MOUSE_RIGHT);
  Mouse.release(MOUSE_MIDDLE);
  currentState = STATE_IDLE;
  updateIdlePulse();
}

void handlePlayButton() {
  // Only ever called from loop() while currentState == STATE_IDLE.
  if (!buttonPressedEdge(playButton)) return;
  currentState = STATE_PLAYING;
  setSolidColor(0, 255, 0);  // solid blue while playing
}

// ---------------- Main loop ----------------
void loop() {
  handleRecordButton();

  if (currentState == STATE_RECORDING) {
    captureSerialToFile();

  } else if (currentState == STATE_PLAYING) {
    bool stopRequested = false;
    playRecordingOnce(stopRequested);

    if (stopRequested) {
      stopPlayback();
    }

  } else {
    handlePlayButton();
    updateIdlePulse();
  }
}
