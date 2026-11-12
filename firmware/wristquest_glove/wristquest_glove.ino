/*
  WristQuest glove firmware
  Board: ESP32 DevKit (appears to the computer as a Bluetooth keyboard)

  WRIST MOVE            GAME ACTION      KEY SENT
  Wrist up              Jump             Up arrow
  Wrist down            Duck             Down arrow
  Tilt left             Move left        Left arrow
  Tilt right            Move right       Right arrow
  Close hand            Shield           x
  Bend too far          Game pauses      p, plus a long buzz

  SAFETY ALERTS (vibration motor)
  - Wrist bent past the safe angle: long buzz, game pauses until the wrist
    returns to neutral.
  - Too many wrist moves in one minute: three buzzes, game pauses for a rest.
  - Fingertips pressed too hard for too long (FSR): long buzz.

  BUTTON
  - Short press: start / pause.
  - Hold 1.5 s: calibrate. Keep the hand flat, open and relaxed until the
    double buzz.

  WIRING
  MPU-6050        VCC -> 3V3, GND -> GND, SDA -> GPIO 21, SCL -> GPIO 22
  Flex sensors    3V3 -> sensor -> GPIO pin, and a 10 k resistor from that
                  pin to GND.  Index 32, middle 33, ring 34.
  FSR 402         3V3 -> sensor -> GPIO pin, and a 10 k resistor from that
                  pin to GND.  Thumb 35, index fingertip 36 (marked VP).
  Motor           GPIO 26 -> 1 k resistor -> transistor base.
                  Emitter -> GND. Collector -> motor (-). Motor (+) -> 3V3.
                  1N4148 across the motor, stripe side to motor (+).
  Push button     GPIO 27 -> button -> GND (no resistor needed).

  LIBRARY
  "ESP32 BLE Keyboard" by T-vK. It needs the esp32 board package version
  2.0.17 (Tools > Board > Boards Manager > esp32 > pick 2.0.17).
  See the Acknowledgements section of the README.
*/

#include <Wire.h>
#include <BleKeyboard.h>

// ===================== 1. PINS =====================
const int PIN_FLEX[3] = {32, 33, 34};   // index, middle, ring finger
const int PIN_FSR[2]  = {35, 36};       // thumb tip, index fingertip
const int PIN_MOTOR   = 26;
const int PIN_BUTTON  = 27;
const int PIN_SDA     = 21;
const int PIN_SCL     = 22;

// ===================== 2. KEYS =====================
// Change these to match the game you are playing.
const uint8_t KEY_FOR_JUMP   = KEY_UP_ARROW;
const uint8_t KEY_FOR_DUCK   = KEY_DOWN_ARROW;
const uint8_t KEY_FOR_LEFT   = KEY_LEFT_ARROW;
const uint8_t KEY_FOR_RIGHT  = KEY_RIGHT_ARROW;
const uint8_t KEY_FOR_SHIELD = 'x';
const uint8_t KEY_FOR_PAUSE  = 'p';
const bool    SEND_PAUSE_KEY = true;    // false if the game has no pause key

// ===================== 3. SETTINGS TO TUNE =====================
// Angles are in degrees, measured from the calibrated neutral position.
const float MOVE_ANGLE    = 20;   // bend this far to trigger a move
const float RELEASE_ANGLE = 12;   // come back inside this to end the move
const float TOO_FAR_ANGLE = 45;   // past this the game pauses and the glove buzzes

// Flex sensors: change in reading (0-4095) from the open hand.
const int FLEX_BEND    = 300;     // at least two fingers past this = hand closed
const int FLEX_RELEASE = 180;

// Overuse: a rest is forced when this many moves happen within one minute.
const int           MAX_REPS_PER_MIN = 30;
const unsigned long REST_MS          = 20000;   // length of the rest
const int           REWARD_EVERY     = 10;      // short buzz every N moves

// FSR: warn when a fingertip presses harder than this for this long.
const int           FSR_LIMIT   = 3000;         // reading, 0-4095
const unsigned long FSR_HOLD_MS = 2000;

// If a direction is reversed on your glove, change its sign to -1.
// If up/down and left/right are swapped, set SWAP_AXES to true.
const float UP_DOWN_SIGN = 1;
const float TILT_SIGN    = 1;
const bool  SWAP_AXES    = false;

const unsigned long LONG_PRESS_MS = 1500;

// ===================== 4. STATE =====================
BleKeyboard keyboard("WristQuest Glove", "WristQuest", 100);

enum { MOVE_UP, MOVE_DOWN, MOVE_LEFT, MOVE_RIGHT, MOVE_FIST, MOVE_COUNT };
const uint8_t MOVE_KEY[MOVE_COUNT] = {
  KEY_FOR_JUMP, KEY_FOR_DUCK, KEY_FOR_LEFT, KEY_FOR_RIGHT, KEY_FOR_SHIELD
};
const char *MOVE_NAME[MOVE_COUNT] = {
  "Wrist up -> jump", "Wrist down -> duck", "Tilt left -> move left",
  "Tilt right -> move right", "Close hand -> shield"
};
bool moveOn[MOVE_COUNT];

enum { PAUSED, ACTIVE, TOO_FAR, RESTING };
const char *MODE_NAME[] = {"paused", "active", "too far", "resting"};
int mode = PAUSED;
bool gamePaused = false;

float pitch = 0, roll = 0;          // filtered hand angles
float pitch0 = 0, roll0 = 0;        // neutral position from calibration
float wristUpDown = 0, wristTilt = 0;
int   flexBase[3] = {0, 0, 0};

float accPitch = 0, accRoll = 0, gyroPitchRate = 0, gyroRollRate = 0;
unsigned long lastUs = 0;

unsigned long repTimes[MAX_REPS_PER_MIN];
int repIndex = 0, repsStored = 0;
unsigned long totalReps = 0;

unsigned long restStart = 0, lastWarnAt = 0, lastPrintAt = 0;
unsigned long fsrSince = 0;
bool fsrHigh = false, fsrWarned = false;

// ===================== 5. VIBRATION MOTOR =====================
int buzzLeft = 0;
bool buzzIsOn = false;
unsigned long buzzChangeAt = 0, buzzOnMs = 0, buzzOffMs = 0;

// Start a pattern of `count` buzzes without stopping the rest of the code.
void buzz(int count, unsigned long onMs, unsigned long offMs) {
  buzzLeft = count;
  buzzOnMs = onMs;
  buzzOffMs = offMs;
  buzzIsOn = true;
  digitalWrite(PIN_MOTOR, HIGH);
  buzzChangeAt = millis() + onMs;
}

void updateBuzz() {
  if (buzzLeft == 0) return;
  if ((long)(millis() - buzzChangeAt) < 0) return;
  if (buzzIsOn) {
    digitalWrite(PIN_MOTOR, LOW);
    buzzIsOn = false;
    buzzLeft--;
    buzzChangeAt = millis() + buzzOffMs;
  } else {
    digitalWrite(PIN_MOTOR, HIGH);
    buzzIsOn = true;
    buzzChangeAt = millis() + buzzOnMs;
  }
}

void buzzShort()   { buzz(1, 80, 0); }
void buzzDouble()  { buzz(2, 80, 120); }
void buzzWarning() { buzz(1, 1000, 0); }
void buzzRest()    { buzz(3, 400, 200); }

// ===================== 6. KEYBOARD =====================
void pressKey(uint8_t key)   { if (keyboard.isConnected()) keyboard.press(key); }
void releaseKey(uint8_t key) { if (keyboard.isConnected()) keyboard.release(key); }

void releaseAllMoves() {
  for (int i = 0; i < MOVE_COUNT; i++) moveOn[i] = false;
  if (keyboard.isConnected()) keyboard.releaseAll();
}

// Taps the pause key only when the game's pause state needs to change.
void setGamePaused(bool paused) {
  if (paused == gamePaused) return;
  gamePaused = paused;
  if (SEND_PAUSE_KEY && keyboard.isConnected()) keyboard.write(KEY_FOR_PAUSE);
}

// ===================== 7. SENSORS =====================
const int MPU_ADDR = 0x68;

bool readMpu() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);                       // first accelerometer register
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, 14) != 14) return false;

  int16_t raw[7];                         // ax, ay, az, temperature, gx, gy, gz
  for (int i = 0; i < 7; i++) {
    int high = Wire.read();
    int low  = Wire.read();
    raw[i] = (int16_t)((high << 8) | low);
  }
  float ax = raw[0] / 16384.0f, ay = raw[1] / 16384.0f, az = raw[2] / 16384.0f;
  accRoll  = atan2(ay, az) * RAD_TO_DEG;
  accPitch = atan2(-ax, sqrt(ay * ay + az * az)) * RAD_TO_DEG;
  gyroRollRate  = raw[4] / 131.0f;        // degrees per second
  gyroPitchRate = raw[5] / 131.0f;
  return true;
}

// Blends the gyroscope (smooth) with the accelerometer (no drift).
void updateAngles(float dt) {
  if (!readMpu()) return;
  pitch = 0.96f * (pitch + gyroPitchRate * dt) + 0.04f * accPitch;
  roll  = 0.96f * (roll  + gyroRollRate  * dt) + 0.04f * accRoll;

  float p = pitch - pitch0, r = roll - roll0;
  if (SWAP_AXES) { float t = p; p = r; r = t; }
  wristUpDown = p * UP_DOWN_SIGN;
  wristTilt   = r * TILT_SIGN;
}

// How far the hand is closed: the middle value of the three finger bends,
// so at least two fingers must bend before the hand counts as closed.
int handClose() {
  int d[3];
  for (int i = 0; i < 3; i++) d[i] = abs((int)analogRead(PIN_FLEX[i]) - flexBase[i]);
  if (d[0] > d[1]) { int t = d[0]; d[0] = d[1]; d[1] = t; }
  if (d[1] > d[2]) { int t = d[1]; d[1] = d[2]; d[2] = t; }
  if (d[0] > d[1]) { int t = d[0]; d[0] = d[1]; d[1] = t; }
  return d[1];
}

int fsrMax() {
  int a = analogRead(PIN_FSR[0]), b = analogRead(PIN_FSR[1]);
  return a > b ? a : b;
}

// Records the neutral wrist position and the open-hand flex readings.
void calibrate() {
  releaseAllMoves();
  buzzLeft = 0;
  Serial.println("Calibrating: keep the hand flat, open and relaxed...");
  digitalWrite(PIN_MOTOR, HIGH); delay(150);
  digitalWrite(PIN_MOTOR, LOW);  delay(850);

  float sumPitch = 0, sumRoll = 0;
  long sumFlex[3] = {0, 0, 0};
  int good = 0;
  const int samples = 50;
  for (int i = 0; i < samples; i++) {
    if (readMpu()) { sumPitch += accPitch; sumRoll += accRoll; good++; }
    for (int f = 0; f < 3; f++) sumFlex[f] += analogRead(PIN_FLEX[f]);
    delay(10);
  }
  if (good > 0) {
    pitch0 = sumPitch / good; roll0 = sumRoll / good;
    pitch = pitch0; roll = roll0;
  } else {
    Serial.println("MPU-6050 not found. Check the SDA/SCL wiring.");
  }
  for (int f = 0; f < 3; f++) flexBase[f] = sumFlex[f] / samples;

  buzzDouble();
  lastUs = micros();
  Serial.println("Calibration done.");
}

// ===================== 8. GAME LOGIC =====================
// Returns true when the user has made too many moves within one minute.
bool recordRep() {
  unsigned long now = millis();
  totalReps++;
  repTimes[repIndex] = now;
  repIndex = (repIndex + 1) % MAX_REPS_PER_MIN;
  if (repsStored < MAX_REPS_PER_MIN) repsStored++;
  if (repsStored < MAX_REPS_PER_MIN) return false;
  return now - repTimes[repIndex] < 60000UL;    // repIndex now points at the oldest
}

void startRest() {
  releaseAllMoves();
  setGamePaused(true);
  repsStored = 0; repIndex = 0;
  restStart = millis();
  mode = RESTING;
  buzzRest();
  Serial.println("Too many wrist moves. Rest for a while.");
}

// Presses the key when the move starts and releases it when the move ends.
void updateMove(int i, float value, float startAt, float endAt) {
  if (mode != ACTIVE) return;
  if (!moveOn[i] && value > startAt) {
    moveOn[i] = true;
    pressKey(MOVE_KEY[i]);
    Serial.println(MOVE_NAME[i]);
    if (recordRep()) startRest();
    else if (totalReps % REWARD_EVERY == 0) buzzShort();
  } else if (moveOn[i] && value < endAt) {
    moveOn[i] = false;
    releaseKey(MOVE_KEY[i]);
  }
}

float furthestBend() {
  float a = fabs(wristUpDown), b = fabs(wristTilt);
  return a > b ? a : b;
}

void checkFingertipForce() {
  if (fsrMax() > FSR_LIMIT) {
    if (!fsrHigh) { fsrHigh = true; fsrSince = millis(); }
    else if (!fsrWarned && millis() - fsrSince > FSR_HOLD_MS) {
      fsrWarned = true;
      buzzWarning();
      Serial.println("Pressing too hard. Relax the fingers.");
    }
  } else {
    fsrHigh = false; fsrWarned = false;
  }
}

void togglePause() {
  if (mode == PAUSED) {
    mode = ACTIVE;
    setGamePaused(false);
    buzzShort();
    Serial.println("Started.");
  } else {
    releaseAllMoves();
    mode = PAUSED;
    setGamePaused(true);
    buzzDouble();
    Serial.println("Paused. Press the button to start.");
  }
}

bool btnWasDown = false, btnLongDone = false;
unsigned long btnDownAt = 0;

void updateButton() {
  bool down = digitalRead(PIN_BUTTON) == LOW;
  unsigned long now = millis();
  if (down && !btnWasDown) { btnDownAt = now; btnLongDone = false; }
  if (down && !btnLongDone && now - btnDownAt > LONG_PRESS_MS) {
    btnLongDone = true;
    calibrate();
  }
  if (!down && btnWasDown && !btnLongDone && now - btnDownAt > 30) togglePause();
  btnWasDown = down;
}

// ===================== 9. SETUP AND LOOP =====================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.beginTransmission(MPU_ADDR);       // wake the MPU-6050
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission(true);

  keyboard.begin();
  delay(1000);
  calibrate();
  Serial.println("Paused. Pair with \"WristQuest Glove\", then press the button to start.");
}

void loop() {
  unsigned long nowUs = micros();
  float dt = (nowUs - lastUs) / 1000000.0f;
  lastUs = nowUs;

  updateAngles(dt);
  updateButton();
  updateBuzz();

  if (mode == ACTIVE) {
    if (furthestBend() > TOO_FAR_ANGLE) {
      releaseAllMoves();
      setGamePaused(true);
      mode = TOO_FAR;
      lastWarnAt = millis();
      buzzWarning();
      Serial.println("Wrist bent too far. Return to neutral.");
    } else {
      updateMove(MOVE_UP,    wristUpDown, MOVE_ANGLE, RELEASE_ANGLE);
      updateMove(MOVE_DOWN, -wristUpDown, MOVE_ANGLE, RELEASE_ANGLE);
      updateMove(MOVE_RIGHT, wristTilt,   MOVE_ANGLE, RELEASE_ANGLE);
      updateMove(MOVE_LEFT, -wristTilt,   MOVE_ANGLE, RELEASE_ANGLE);
      updateMove(MOVE_FIST,  handClose(), FLEX_BEND,  FLEX_RELEASE);
      checkFingertipForce();
    }
  } else if (mode == TOO_FAR) {
    if (furthestBend() < RELEASE_ANGLE) {
      mode = ACTIVE;
      setGamePaused(false);
    } else if (furthestBend() > TOO_FAR_ANGLE && millis() - lastWarnAt > 2000) {
      lastWarnAt = millis();
      buzzWarning();                      // still too far: warn again
    }
  } else if (mode == RESTING) {
    if (millis() - restStart >= REST_MS) {
      mode = ACTIVE;
      setGamePaused(false);
      buzzDouble();
      Serial.println("Rest over. Carry on.");
    }
  }

  // Live readings for tuning: open Tools > Serial Monitor at 115200 baud.
  if (millis() - lastPrintAt > 250) {
    lastPrintAt = millis();
    Serial.printf("%s | up/down %4.0f | tilt %4.0f | hand close %4d | fsr %4d | moves %lu\n",
                  MODE_NAME[mode], wristUpDown, wristTilt, handClose(), fsrMax(), totalReps);
  }

  delay(10);
}
