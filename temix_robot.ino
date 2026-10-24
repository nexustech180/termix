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

  Hardware (change the pin numbers below if you wire it differently):
    - Arduino Uno / Nano
    - L298N dual motor driver + 2 DC gear motors (left and right wheels)
    - HC-05 or HC-06 Bluetooth module (for phone / PC wireless)
    - HC-SR04 ultrasonic sensor on the front (obstacle stop)
    - USB cable also works (for PC), no Bluetooth module needed.

  Wiring:
    L298N ENA -> D3     L298N IN1 -> D4     L298N IN2 -> D5     (left motor)
    L298N ENB -> D9     L298N IN3 -> D6     L298N IN4 -> D7     (right motor)
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
    HC-SR04 VCC -> 5V   HC-SR04 GND -> GND   HC-SR04 Trig -> A1   HC-SR04 Echo -> A2
    Brick switch: wire A0 to GND while carrying the brick (see CARRYING A BRICK).

  The HC-05/06 talks at 9600 baud by default. If you changed it with AT commands,
  change BT_BAUD below to match.

  Pairing: in your phone/PC Bluetooth settings, pair with "HC-05" or "HC-06"
  (PIN 1234 or 0000). Then press Bluetooth in the Temix app and pick it.

  REMOTE CONTROL: lines starting with '!' drive the robot directly while a button is held:
    !F200  forward at speed 200 (0-255)   !B200  backward
    !L200  turn left                      !R200  turn right
    !S     stop
  The app repeats the command every 200 ms while the button is held. If nothing arrives
  for REMOTE_TIMEOUT_MS (e.g. Bluetooth dropped), the robot stops by itself.

  OBSTACLE STOP: while driving forward (terminal or remote), the ultrasonic sensor checks the
  way ahead about 20 times a second. If something is closer than OBSTACLE_CM the robot brakes
  and reports "OBSTACLE 23 cm". Backward and turns still work, so you can steer away.
  Soft things (cushions, clothes) absorb sound and may not be seen.

  TURNS pivot on one wheel: L = left wheel held still + right wheel forward,
                            R = right wheel held still + left wheel forward.

  DISTANCE is worked out from wheel revolutions:
    one F / B = STEP_CM of travel = STEP_CM / wheel circumference revolutions
    time      = revolutions x MS_PER_REV (time for one wheel revolution)
  With a 10 cm wheel radius, one revolution = 62.8 cm, so 1 m = 1.59 revolutions.

  CARRYING A BRICK: a load slows the motors, so there is a second revolution time for it.
    Put a wire (or a small switch) from pin A0 to GND when the brick is on the car.
    No wire = empty. The robot says which one it is using when it starts ("READY ... empty").

  CALIBRATION: the robot has no wheel sensors, so distance and angle are timed.
    1. Straight: send "F". If it drifts sideways, adjust LEFT_TRIM / RIGHT_TRIM.
    2. Distance (empty): send "F", measure how far it went in cm, then
         new MS_PER_REV_EMPTY = old MS_PER_REV_EMPTY x 100 / cm travelled
    3. Distance (with the brick, A0 wired to GND): same, for MS_PER_REV_LOADED.
    4. Turns: send "RRRR" and adjust MS_PER_90_DEG until it ends facing the way it started
       (turned too far -> smaller number, not far enough -> bigger).
*/

#include <SoftwareSerial.h>

// ---------------- Car measurements ----------------
const float WHEEL_RADIUS_CM = 10.0;        // car: 37 cm long, 13 cm wide, 10 cm wheel radius
const float STEP_CM = 100.0;               // distance of one F or B (the app's grid assumes 1 m)

// ---------------- Calibration (tune these for your robot) ----------------
const unsigned long MS_PER_REV_EMPTY  = 655;  // one wheel revolution at DRIVE_SPEED, no load
                                              // (measured: 1 m = 1.59 rev in 1042 ms -> 655)
const unsigned long MS_PER_REV_LOADED = 655;  // same, carrying the brick - measure and change
const uint8_t LOAD_PIN = A0;               // wire A0 to GND when carrying the brick
const unsigned long MS_PER_90_DEG = 1000;  // time to turn 90 degrees at TURN_SPEED (one wheel
                                           // stopped, the other driving forward)
const uint8_t DRIVE_SPEED = 200;           // 0-255 motor power when driving
const uint8_t TURN_SPEED  = 255;           // 0-255 motor power for the moving wheel when turning

// Straight-line trim. Two "identical" motors never run at exactly the same speed, so the
// car drifts. If it drifts RIGHT, the left wheel is faster: lower LEFT_TRIM (e.g. 0.92).
// If it drifts LEFT, lower RIGHT_TRIM. Change in small steps (0.03-0.05) until it goes straight.
const float LEFT_TRIM  = 1.00;
const float RIGHT_TRIM = 1.00;
const unsigned long KICK_MS = 150;         // full power for this long at the start of each move,
                                           // to break the motors free from standstill
const unsigned long PAUSE_MS = 200;        // short stop between steps (reduces skidding)
const unsigned long REMOTE_TIMEOUT_MS = 600;  // remote control: stop if the app goes quiet

// ---------------- Obstacle sensor (HC-SR04) ----------------
const bool USE_ULTRASONIC = true;          // set to false to drive without the sensor
const uint8_t US_TRIG = A1, US_ECHO = A2;
const unsigned int OBSTACLE_CM = 40;       // stop when something is closer than this. The car
                                           // covers ~1 m per second, so it needs room to brake.
const unsigned long US_INTERVAL_MS = 50;   // time between distance readings

// If a wheel spins the wrong way, flip its setting here instead of rewiring.
const bool INVERT_LEFT  = false;
const bool INVERT_RIGHT = false;

// ---------------- Pins ----------------
const uint8_t LEFT_EN = 3, LEFT_IN1 = 4, LEFT_IN2 = 5;    // ENA must be a PWM pin (3 is)
const uint8_t RIGHT_EN = 9, RIGHT_IN1 = 6, RIGHT_IN2 = 7;  // ENB must be a PWM pin (9 is)
const uint8_t BT_RX = 10, BT_TX = 11;   // Arduino pins wired to the module's TXD / RXD
const long BT_BAUD = 9600;

SoftwareSerial bt(BT_RX, BT_TX);

const uint8_t MAX_CMD_LEN = 64;
char cmd[MAX_CMD_LEN + 1];
uint8_t cmdLen = 0;
bool cmdOverflow = false;

bool remoteActive = false;          // true while a remote-control button is held
unsigned long lastRemoteMs = 0;
char remoteDir = 0;                 // direction currently being driven by the remote
int remoteLeft = 0, remoteRight = 0, remoteSpeed = 0;
unsigned long kickUntil = 0;        // remote: full power until this time, then remoteSpeed

unsigned int distanceCm = 0;        // last ultrasonic reading (0 = nothing in range)
uint8_t closeReadings = 0;          // consecutive readings closer than OBSTACLE_CM
unsigned long lastPingMs = 0;
unsigned long lastObstacleReplyMs = 0;

// ---------------- Messages back to the app ----------------
// Replies go to both USB and Bluetooth so whichever one the app uses sees them.
void reply(const String &msg) {
  Serial.println(msg);
  bt.println(msg);
}

// ---------------- Motors ----------------
// dir: +1 forward, -1 backward, 0 = brake (wheel held still, so it can't be dragged along)
void setMotor(uint8_t en, uint8_t in1, uint8_t in2, int dir, uint8_t speed, bool invert, float trim) {
  if (invert) dir = -dir;
  if (dir == 0) {
    // L298N brake: both inputs the same with the enable pin on.
    digitalWrite(in1, LOW);
    digitalWrite(in2, LOW);
    analogWrite(en, 255);
    return;
  }
  digitalWrite(in1, dir > 0 ? HIGH : LOW);
  digitalWrite(in2, dir < 0 ? HIGH : LOW);
  analogWrite(en, (uint8_t)constrain(speed * trim, 0, 255));
}

void drive(int left, int right, uint8_t speed) {
  setMotor(LEFT_EN, LEFT_IN1, LEFT_IN2, left, speed, INVERT_LEFT, LEFT_TRIM);
  setMotor(RIGHT_EN, RIGHT_IN1, RIGHT_IN2, right, speed, INVERT_RIGHT, RIGHT_TRIM);
}

void stopMotors() { drive(0, 0, 0); }

// Direction of each wheel for a command letter. Turns pivot on one wheel:
//   L = left wheel stopped, right wheel forward      R = right wheel stopped, left wheel forward
void wheelsFor(char c, int &left, int &right) {
  switch (c) {
    case 'F': left =  1; right =  1; break;
    case 'B': left = -1; right = -1; break;
    case 'L': left =  0; right =  1; break;
    case 'R': left =  1; right =  0; break;
    default:  left =  0; right =  0; break;
  }
}

// ---------------- Distance ----------------
bool carryingLoad() { return digitalRead(LOAD_PIN) == LOW; }   // A0 wired to GND = brick on

// Milliseconds to drive one F / B step, from wheel revolutions.
unsigned long msPerStep() {
  const float circumferenceCm = 2.0 * PI * WHEEL_RADIUS_CM;     // 62.8 cm for a 10 cm radius
  const float revolutions = STEP_CM / circumferenceCm;         // 1.59 rev for 1 m
  unsigned long msPerRev = carryingLoad() ? MS_PER_REV_LOADED : MS_PER_REV_EMPTY;
  return (unsigned long)(revolutions * msPerRev + 0.5);
}

// ---------------- Obstacle sensor ----------------
// One HC-SR04 reading in cm. 0 = nothing within ~4 m (or no sensor connected).
unsigned int readDistanceCm() {
  digitalWrite(US_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(US_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(US_TRIG, LOW);
  unsigned long echoUs = pulseIn(US_ECHO, HIGH, 25000UL);   // gives up after ~4 m
  return echoUs == 0 ? 0 : (unsigned int)(echoUs / 58);
}

// Takes a new reading every US_INTERVAL_MS. Two close readings in a row are needed, so one
// stray echo doesn't stop the car.
void updateDistance() {
  if (!USE_ULTRASONIC || millis() - lastPingMs < US_INTERVAL_MS) return;
  lastPingMs = millis();
  distanceCm = readDistanceCm();
  if (distanceCm > 0 && distanceCm < OBSTACLE_CM) { if (closeReadings < 255) closeReadings++; }
  else closeReadings = 0;
}

bool obstacleAhead() {
  updateDistance();
  return USE_ULTRASONIC && closeReadings >= 2;
}

// Fresh check before starting to drive forward (doesn't wait for the next reading slot).
bool obstacleNow() {
  if (!USE_ULTRASONIC) return false;
  closeReadings = 0;
  for (uint8_t i = 0; i < 2; i++) {
    lastPingMs = 0;               // force a reading now
    updateDistance();
    delay(30);                    // let the last echo die away
  }
  return closeReadings >= 2;
}

void reportObstacle() {
  reply(String("OBSTACLE ") + distanceCm + " cm");
  lastObstacleReplyMs = millis();
}

// ---------------- Stop handling ----------------
// Returns true if an 'X' (stop) arrived on either port. Other bytes received while
// moving are discarded - the app waits for DONE before sending the next program.
bool stopRequested() {
  bool stop = false;
  while (Serial.available()) { char c = Serial.read(); if (c == 'X' || c == 'x') stop = true; }
  while (bt.available())     { char c = bt.read();     if (c == 'X' || c == 'x') stop = true; }
  return stop;
}

bool obstacleStopped = false;   // set when waitOrStop stopped because of an obstacle

// Waits ms milliseconds while watching for a stop (and, going forward, for obstacles).
// Returns false if stopped.
bool waitOrStop(unsigned long ms, bool watchAhead) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (stopRequested()) { stopMotors(); return false; }
    if (watchAhead && obstacleAhead()) { stopMotors(); obstacleStopped = true; return false; }
  }
  return true;
}

// ---------------- Running a program ----------------
bool isValidCommand(char c) { return c == 'F' || c == 'B' || c == 'L' || c == 'R'; }

void runProgram(char *p) {
  remoteActive = false;
  remoteDir = 0;
  kickUntil = 0;

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

    bool forward = (c == 'F');
    if (forward && obstacleNow()) {                 // don't even start into a wall
      reportObstacle();
      reply("STOPPED");
      return;
    }

    switch (c) {
      case 'F': reply(String("STEP forward ") + count + " m"); break;
      case 'B': reply(String("STEP backward ") + count + " m"); break;
      case 'R': reply(String("STEP right ") + (count * 90) + " deg"); break;
      case 'L': reply(String("STEP left ") + (count * 90) + " deg"); break;
    }
    int left, right;
    wheelsFor(c, left, right);
    bool turning = (c == 'L' || c == 'R');
    unsigned long duration = (turning ? MS_PER_90_DEG : msPerStep()) * count;

    // Kick: full power briefly so the wheels actually start turning on the floor.
    obstacleStopped = false;
    drive(left, right, 255);
    bool ok = waitOrStop(min(KICK_MS, duration), forward);
    if (ok && duration > KICK_MS) {
      drive(left, right, turning ? TURN_SPEED : DRIVE_SPEED);
      ok = waitOrStop(duration - KICK_MS, forward);
    }
    stopMotors();
    if (!ok && obstacleStopped) reportObstacle();
    if (!ok || !waitOrStop(PAUSE_MS, false)) { reply("STOPPED"); return; }
  }

  reply("DONE");
}

// ---------------- Remote control ----------------
// c is the text after '!', e.g. "F200" or "S". No reply is sent - these arrive 5 times a second.
void handleRemote(const char *c) {
  char dir = toupper(c[0]);
  int speed = atoi(c + 1);
  if (speed <= 0 || speed > 255) speed = DRIVE_SPEED;

  if (dir != 'F' && dir != 'B' && dir != 'L' && dir != 'R') {   // 'S' or anything unknown
    stopMotors(); remoteActive = false; remoteDir = 0; return;
  }

  // Blocked ahead: refuse to drive forward (backward and turns are still allowed).
  if (dir == 'F' && obstacleAhead()) {
    stopMotors();
    remoteActive = false;
    remoteDir = 0;
    if (millis() - lastObstacleReplyMs > 1000) reportObstacle();   // don't flood the app
    return;
  }

  if ((dir == 'L' || dir == 'R') && speed < TURN_SPEED) speed = TURN_SPEED;   // turning needs full power
  int left, right;
  wheelsFor(dir, left, right);

  remoteLeft = left; remoteRight = right; remoteSpeed = speed;
  if (!remoteActive || dir != remoteDir) {
    // New move: kick at full power first (loop() drops to remoteSpeed after KICK_MS).
    drive(left, right, 255);
    kickUntil = millis() + KICK_MS;
  } else if ((long)(millis() - kickUntil) >= 0) {
    drive(left, right, speed);   // same button still held: keep going (speed may have changed)
  }
  remoteDir = dir;
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
  pinMode(LOAD_PIN, INPUT_PULLUP);   // unconnected = empty, wired to GND = carrying the brick
  pinMode(US_TRIG, OUTPUT);
  pinMode(US_ECHO, INPUT);
  digitalWrite(US_TRIG, LOW);

  Serial.begin(9600);   // USB
  bt.begin(BT_BAUD);    // HC-05 / HC-06
  reply(String("READY Temix robot (") + (carryingLoad() ? "loaded" : "empty") +
        ", 1 step = " + msPerStep() + " ms)");
}

void loop() {
  readFrom(Serial);
  readFrom(bt);

  // Safety: stop if the remote went quiet (button released without a stop, or link lost).
  if (remoteActive && millis() - lastRemoteMs > REMOTE_TIMEOUT_MS) {
    stopMotors();
    remoteActive = false;
    remoteDir = 0;
  }

  // Remote driving forward into something: brake.
  if (remoteActive && remoteDir == 'F' && obstacleAhead()) {
    stopMotors();
    remoteActive = false;
    remoteDir = 0;
    reportObstacle();
  }

  // Remote: end of the start-up kick - settle to the chosen speed.
  if (remoteActive && kickUntil && (long)(millis() - kickUntil) >= 0) {
    drive(remoteLeft, remoteRight, remoteSpeed);
    kickUntil = 0;
  }
}
