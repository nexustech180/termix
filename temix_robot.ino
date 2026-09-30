/*
  Temix Robot - Arduino firmware
  ------------------------------
  Receives a command string from the Temix app (over Bluetooth or USB) and drives the robot.

    F = forward 1 metre        B = backward 1 metre
    R = turn right 90 degrees  L = turn left 90 degrees
    X = emergency stop (can be sent while the robot is moving)

  Repeated letters are merged, so "FFFRFFFLLBB" runs as:
    forward 3 m -> right 90 -> forward 3 m -> left 180 -> backward 2 m

  Every command string must end with a newline ('\n'); the app does this for you.

  Hardware (defaults - change the pin numbers below if you wire it differently):
    - Arduino Uno / Nano
    - L298N dual motor driver + 2 DC gear motors (left and right wheels)
    - HC-05 or HC-06 Bluetooth module (for phone / PC wireless)
    - USB cable also works (for PC), no Bluetooth module needed.

  Wiring:
    L298N ENA -> D5     L298N IN1 -> D7     L298N IN2 -> D8     (left motor)
    L298N ENB -> D6     L298N IN3 -> D9     L298N IN4 -> D4     (right motor)
    L298N GND -> Arduino GND (common ground is required!)
    HC-05/06 VCC -> 5V      HC-05/06 GND -> GND
    HC-05/06 TXD -> D10
    HC-05/06 RXD <- D11 through a voltage divider, because the module's RX pin is 3.3 V:
                   D11 --[1k]--+-- RXD
                               |
                              [2k]
                               |
                              GND
    (HC-05 only: leave the EN/KEY pin unconnected so it starts in normal data mode.)

  The HC-05/06 talks at 9600 baud by default. If you changed it with AT commands,
  change BT_BAUD below to match.

  Pairing: in your phone/PC Bluetooth settings, pair with "HC-05" or "HC-06"
  (PIN 1234 or 0000). Then press Bluetooth in the Temix app and pick it.

  REMOTE CONTROL: lines starting with '!' drive the robot directly while a button is held:
    !F200  forward at speed 200 (0-255)   !B200  backward
    !L200  spin left                      !R200  spin right
    !S     stop
  The app repeats the command every 200 ms while the button is held. If nothing arrives
  for REMOTE_TIMEOUT_MS (e.g. Bluetooth dropped), the robot stops by itself.

  CALIBRATION: the robot has no wheel sensors, so distance and angle are timed.
  Send "F" and measure how far it went, then adjust MS_PER_METER. Send "RRRR" and
  adjust MS_PER_90_DEG until it ends facing the way it started.
*/

#include <SoftwareSerial.h>

// ---------------- Calibration (tune these for your robot) ----------------
const unsigned long MS_PER_METER  = 2500;  // time to drive 1 m at DRIVE_SPEED
const unsigned long MS_PER_90_DEG = 600;   // time to spin 90 degrees at TURN_SPEED
const uint8_t DRIVE_SPEED = 200;           // 0-255 motor power when driving
const uint8_t TURN_SPEED  = 180;           // 0-255 motor power when turning
const unsigned long PAUSE_MS = 200;        // short stop between steps (reduces skidding)
const unsigned long REMOTE_TIMEOUT_MS = 600;  // remote control: stop if the app goes quiet

// If a wheel spins the wrong way, flip its setting here instead of rewiring.
const bool INVERT_LEFT  = false;
const bool INVERT_RIGHT = false;

// ---------------- Pins ----------------
const uint8_t LEFT_EN = 5, LEFT_IN1 = 7, LEFT_IN2 = 8;
const uint8_t RIGHT_EN = 6, RIGHT_IN1 = 9, RIGHT_IN2 = 4;
const uint8_t BT_RX = 10, BT_TX = 11;   // Arduino pins wired to the module's TXD / RXD
const long BT_BAUD = 9600;

SoftwareSerial bt(BT_RX, BT_TX);

const uint8_t MAX_CMD_LEN = 64;
char cmd[MAX_CMD_LEN + 1];
uint8_t cmdLen = 0;
bool cmdOverflow = false;

bool remoteActive = false;          // true while a remote-control button is held
unsigned long lastRemoteMs = 0;

// ---------------- Messages back to the app ----------------
// Replies go to both USB and Bluetooth so whichever one the app uses sees them.
void reply(const String &msg) {
  Serial.println(msg);
  bt.println(msg);
}

// ---------------- Motors ----------------
// dir: +1 forward, -1 backward, 0 stop
void setMotor(uint8_t en, uint8_t in1, uint8_t in2, int dir, uint8_t speed, bool invert) {
  if (invert) dir = -dir;
  digitalWrite(in1, dir > 0 ? HIGH : LOW);
  digitalWrite(in2, dir < 0 ? HIGH : LOW);
  analogWrite(en, dir == 0 ? 0 : speed);
}

void drive(int left, int right, uint8_t speed) {
  setMotor(LEFT_EN, LEFT_IN1, LEFT_IN2, left, speed, INVERT_LEFT);
  setMotor(RIGHT_EN, RIGHT_IN1, RIGHT_IN2, right, speed, INVERT_RIGHT);
}

void stopMotors() { drive(0, 0, 0); }

// ---------------- Stop handling ----------------
// Returns true if an 'X' (stop) arrived on either port. Other bytes received while
// moving are discarded - the app waits for DONE before sending the next program.
bool stopRequested() {
  bool stop = false;
  while (Serial.available()) { char c = Serial.read(); if (c == 'X' || c == 'x') stop = true; }
  while (bt.available())     { char c = bt.read();     if (c == 'X' || c == 'x') stop = true; }
  return stop;
}

// Waits ms milliseconds while watching for a stop. Returns false if stopped.
bool waitOrStop(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (stopRequested()) { stopMotors(); return false; }
  }
  return true;
}

// ---------------- Running a program ----------------
bool isValidCommand(char c) { return c == 'F' || c == 'B' || c == 'L' || c == 'R'; }

void runProgram(char *p) {
  remoteActive = false;

  // Normalise: uppercase, drop spaces, and validate before moving anything.
  uint8_t n = 0;
  for (uint8_t i = 0; p[i]; i++) {
    char c = toupper(p[i]);
    if (c == ' ' || c == '\t') continue;
    if (c == 'X') { stopMotors(); reply("STOPPED"); return; }
    if (!isValidCommand(c)) {
      reply(String("ERR unknown command '") + p[i] + "' - use F, B, L, R");
      return;
    }
    p[n++] = c;
  }
  p[n] = '\0';
  if (n == 0) return;

  reply(String("RUN ") + p);

  uint8_t i = 0;
  while (i < n) {
    char c = p[i];
    uint8_t count = 0;
    while (i < n && p[i] == c) { count++; i++; }   // merge repeats: FFF -> 3 m

    switch (c) {
      case 'F': reply(String("STEP forward ") + count + " m");    drive( 1,  1, DRIVE_SPEED); break;
      case 'B': reply(String("STEP backward ") + count + " m");   drive(-1, -1, DRIVE_SPEED); break;
      case 'R': reply(String("STEP right ") + (count * 90) + " deg"); drive( 1, -1, TURN_SPEED); break;
      case 'L': reply(String("STEP left ") + (count * 90) + " deg");  drive(-1,  1, TURN_SPEED); break;
    }

    unsigned long duration = (c == 'F' || c == 'B') ? MS_PER_METER : MS_PER_90_DEG;
    bool ok = waitOrStop(duration * count);
    stopMotors();
    if (!ok || !waitOrStop(PAUSE_MS)) { reply("STOPPED"); return; }
  }

  reply("DONE");
}

// ---------------- Remote control ----------------
// c is the text after '!', e.g. "F200" or "S". No reply is sent - these arrive 5 times a second.
void handleRemote(const char *c) {
  char dir = toupper(c[0]);
  int speed = atoi(c + 1);
  if (speed <= 0 || speed > 255) speed = DRIVE_SPEED;

  switch (dir) {
    case 'F': drive( 1,  1, speed); break;
    case 'B': drive(-1, -1, speed); break;
    case 'L': drive(-1,  1, speed); break;
    case 'R': drive( 1, -1, speed); break;
    default:  stopMotors(); remoteActive = false; return;   // 'S' or anything unknown
  }
  remoteActive = true;
  lastRemoteMs = millis();
}

// ---------------- Reading commands ----------------
void readFrom(Stream &port) {
  while (port.available()) {
    char c = port.read();
    if (c == '\n' || c == '\r') {
      if (cmdOverflow) reply(String("ERR command too long (max ") + MAX_CMD_LEN + " letters)");
      else if (cmdLen > 0) {
        cmd[cmdLen] = '\0';
        if (cmd[0] == '!') handleRemote(cmd + 1);
        else runProgram(cmd);
      }
      cmdLen = 0;
      cmdOverflow = false;
    } else if (cmdLen < MAX_CMD_LEN) {
      cmd[cmdLen++] = c;
    } else {
      cmdOverflow = true;
    }
  }
}

void setup() {
  pinMode(LEFT_EN, OUTPUT);  pinMode(LEFT_IN1, OUTPUT);  pinMode(LEFT_IN2, OUTPUT);
  pinMode(RIGHT_EN, OUTPUT); pinMode(RIGHT_IN1, OUTPUT); pinMode(RIGHT_IN2, OUTPUT);
  stopMotors();

  Serial.begin(9600);   // USB
  bt.begin(BT_BAUD);    // HC-05 / HC-06
  reply("READY Temix robot");
}

void loop() {
  readFrom(Serial);
  readFrom(bt);

  // Safety: stop if the remote went quiet (button released without a stop, or link lost).
  if (remoteActive && millis() - lastRemoteMs > REMOTE_TIMEOUT_MS) {
    stopMotors();
    remoteActive = false;
  }
}
