/*
  Temix Robot - ESP32 firmware (WiFi)
  -----------------------------------
  Receives a command string from the Temix app over WiFi (or the USB cable) and drives the robot.


    F = forward 1 metre        B = backward 1 metre
    R = turn right 90 degrees  L = turn left 90 degrees
    X = emergency stop (can be sent while the robot is moving)


  Repeated letters are merged, so "FFFRFFFLLBB" runs as:
    forward 3 m -> right 90 -> forward 3 m -> left 180 -> backward 2 m


  WIFI: the ESP32 makes its own WiFi network - no router needed.
    1. On the phone / PC, join the WiFi network  "Temix-Robot"  (password  temix1234 ).
       The phone may say "no internet" - choose to stay connected.
       If the app can't connect, turn OFF mobile data while driving.
    2. In the Temix app, tap  WiFi . It connects to the robot at 192.168.4.1.
    WiFi works in the Temix Android app (APK) and Windows app (EXE). The website version
    can't use it (browsers block it on https pages).


  ONE-TIME SETUP in the Arduino IDE:
    - Boards: File -> Preferences -> Additional boards manager URLs, add
        https://espressif.github.io/arduino-esp32/package_esp32_index.json
      then Tools -> Board -> Boards Manager, install "esp32 by Espressif Systems".
    - Board: Tools -> Board -> esp32 -> "ESP32 Dev Module".
    - Library: Sketch -> Include Library -> Manage Libraries, install
        "WebSockets" by Markus Sattler.
    - Uploading: if it says "Connecting......" for a long time, hold the BOOT button
      on the board until the upload starts.


  Hardware:
    - ESP32 DevKit (ESP32-WROOM-32 / "ESP32 Dev Module")
    - L298N dual motor driver + 2 DC gear motors (left and right wheels)


  Wiring:
    L298N ENA -> GPIO 14    L298N IN1 -> GPIO 27    L298N IN2 -> GPIO 26   (left motor)
    L298N ENB -> GPIO 32    L298N IN3 -> GPIO 25    L298N IN4 -> GPIO 33   (right motor)
    L298N GND -> ESP32 GND (common ground is required!)
    Power the ESP32 from the L298N "5V" pin -> ESP32 "VIN" (or "5V") pin, or by USB.
    Brick switch: wire GPIO 4 to GND while carrying the brick (see CARRYING A BRICK).
    (The ESP32's 3.3 V signals are fine for the L298N inputs - no level shifting needed.)


  If the ESP32 keeps restarting when the motors start ("Brownout detector was triggered" in the
  Serial Monitor), the battery is too weak: use fresh / stronger cells (2-3 x 18650 is best) and
  put a 470-1000 uF capacitor across the battery terminals.


  REMOTE CONTROL: lines starting with '!' drive the robot directly while a button is held:
    !F200  forward at speed 200 (0-255)   !B200  backward
    !L200  turn left                      !R200  turn right
    !S     stop
  The app repeats the command every 200 ms while the button is held. If nothing arrives
  for REMOTE_TIMEOUT_MS (e.g. WiFi dropped), the robot stops by itself.


  TURNS pivot on one wheel: L = left wheel held still + right wheel forward,
                            R = right wheel held still + left wheel forward.


  DISTANCE is worked out from wheel revolutions:
    one F / B = STEP_CM of travel = STEP_CM / wheel circumference revolutions
    time      = revolutions x MS_PER_REV (time for one wheel revolution)
  With a 10 cm wheel radius, one revolution = 62.8 cm, so 1 m = 1.59 revolutions.


  CARRYING A BRICK: a load slows the motors, so there is a second revolution time for it.
    Put a wire (or a small switch) from GPIO 4 to GND when the brick is on the car.
    No wire = empty. The robot says which one it is using when it starts ("READY ... empty").


  CALIBRATION: the robot has no wheel sensors, so distance and angle are timed.
    1. Straight: send "F". If it drifts sideways, adjust LEFT_TRIM / RIGHT_TRIM.
    2. Distance (empty): send "F", measure how far it went in cm, then
         new MS_PER_REV_EMPTY = old MS_PER_REV_EMPTY x 100 / cm travelled
    3. Distance (with the brick, GPIO 4 wired to GND): same, for MS_PER_REV_LOADED.
    4. Turns: send "RRRR" and adjust MS_PER_90_DEG until it ends facing the way it started
       (turned too far -> smaller number, not far enough -> bigger).
*/


#include <WiFi.h>
#include <WebSocketsServer.h>   // "WebSockets" library by Markus Sattler


// ---------------- WiFi ----------------
const char *WIFI_NAME     = "Temix-Robot";   // the network the phone joins
const char *WIFI_PASSWORD = "temix1234";     // at least 8 characters
const uint16_t WS_PORT = 81;                 // the app connects to ws://192.168.4.1:81


// ---------------- Car measurements ----------------
const float WHEEL_RADIUS_CM = 10.0;        // car: 37 cm long, 13 cm wide, 10 cm wheel radius
const float STEP_CM = 100.0;               // distance of one F or B (the app's grid assumes 1 m)


// ---------------- Calibration (tune these for your robot) ----------------
const unsigned long MS_PER_REV_EMPTY  = 655;  // one wheel revolution at DRIVE_SPEED, no load
                                              // (measured: 1 m = 1.59 rev in 1042 ms -> 655)
const unsigned long MS_PER_REV_LOADED = 655;  // same, carrying the brick - measure and change
const uint8_t LOAD_PIN = 4;                // wire GPIO 4 to GND when carrying the brick
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


// If a wheel spins the wrong way, flip its setting here instead of rewiring.
const bool INVERT_LEFT  = false;
const bool INVERT_RIGHT = false;


// ---------------- Pins ----------------
const uint8_t LEFT_EN = 14, LEFT_IN1 = 27, LEFT_IN2 = 26;    // L298N ENA, IN1, IN2
const uint8_t RIGHT_EN = 32, RIGHT_IN1 = 25, RIGHT_IN2 = 33; // L298N ENB, IN3, IN4


WebSocketsServer ws(WS_PORT);


const uint8_t MAX_CMD_LEN = 64;
char cmd[MAX_CMD_LEN + 1];
uint8_t cmdLen = 0;
bool cmdOverflow = false;


// Text received over WiFi, waiting to be processed.
char inbox[256];
uint16_t inboxLen = 0;


bool remoteActive = false;          // true while a remote-control button is held
unsigned long lastRemoteMs = 0;
char remoteDir = 0;                 // direction currently being driven by the remote
int remoteLeft = 0, remoteRight = 0, remoteSpeed = 0;
unsigned long kickUntil = 0;        // remote: full power until this time, then remoteSpeed


// ---------------- Messages back to the app ----------------
// Replies go to USB and to every app connected over WiFi.
void reply(const String &msg) {
  Serial.println(msg);
  String m = msg;
  ws.broadcastTXT(m);
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
bool carryingLoad() { return digitalRead(LOAD_PIN) == LOW; }   // GPIO 4 wired to GND = brick on


// Milliseconds to drive one F / B step, from wheel revolutions.
unsigned long msPerStep() {
  const float circumferenceCm = 2.0 * PI * WHEEL_RADIUS_CM;     // 62.8 cm for a 10 cm radius
  const float revolutions = STEP_CM / circumferenceCm;         // 1.59 rev for 1 m
  unsigned long msPerRev = carryingLoad() ? MS_PER_REV_LOADED : MS_PER_REV_EMPTY;
  return (unsigned long)(revolutions * msPerRev + 0.5);
}


String readyMessage() {
  return String("READY Temix robot (") + (carryingLoad() ? "loaded" : "empty") +
         ", 1 step = " + msPerStep() + " ms)";
}


// ---------------- WiFi messages ----------------
// Called from ws.loop(). Only stores the text; it is processed in loop() / stopRequested().
void onWebSocket(uint8_t client, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      String hello = readyMessage();
      ws.sendTXT(client, hello);
      break;
    }
    case WStype_DISCONNECTED:
      stopMotors();                  // safety: app gone -> stop
      remoteActive = false;
      remoteDir = 0;
      break;
    case WStype_TEXT:
      for (size_t i = 0; i < length && inboxLen < sizeof(inbox); i++) inbox[inboxLen++] = payload[i];
      break;
    default:
      break;
  }
}


// ---------------- Stop handling ----------------
// Returns true if an 'X' (stop) arrived on USB or WiFi. Other text received while
// moving is discarded - the app waits for DONE before sending the next program.
bool stopRequested() {
  bool stop = false;
  while (Serial.available()) { char c = Serial.read(); if (c == 'X' || c == 'x') stop = true; }
  ws.loop();
  for (uint16_t i = 0; i < inboxLen; i++) if (inbox[i] == 'X' || inbox[i] == 'x') stop = true;
  inboxLen = 0;
  return stop;
}


// Waits ms milliseconds while watching for a stop. Returns false if stopped.
bool waitOrStop(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    if (stopRequested()) { stopMotors(); return false; }
    delay(1);
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
    drive(left, right, 255);
    bool ok = waitOrStop(min(KICK_MS, duration));
    if (ok && duration > KICK_MS) {
      drive(left, right, turning ? TURN_SPEED : DRIVE_SPEED);
      ok = waitOrStop(duration - KICK_MS);
    }
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


  if (dir != 'F' && dir != 'B' && dir != 'L' && dir != 'R') {   // 'S' or anything unknown
    stopMotors(); remoteActive = false; remoteDir = 0; return;
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
// One character from USB or WiFi. A full line is run as a program or a remote command.
void feed(char c) {
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


void setup() {
  pinMode(LEFT_EN, OUTPUT);  pinMode(LEFT_IN1, OUTPUT);  pinMode(LEFT_IN2, OUTPUT);
  pinMode(RIGHT_EN, OUTPUT); pinMode(RIGHT_IN1, OUTPUT); pinMode(RIGHT_IN2, OUTPUT);
  stopMotors();
  pinMode(LOAD_PIN, INPUT_PULLUP);   // unconnected = empty, wired to GND = carrying the brick


  Serial.begin(115200);              // USB (Serial Monitor at 115200)


  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_NAME, WIFI_PASSWORD);
  WiFi.setSleep(false);              // no WiFi power saving: faster, steadier replies
  ws.begin();
  ws.onEvent(onWebSocket);


  reply(readyMessage());
  Serial.print("WiFi: join \""); Serial.print(WIFI_NAME);
  Serial.print("\", then tap WiFi in the app (robot address ");
  Serial.print(WiFi.softAPIP()); Serial.println(")");
}


void loop() {
  while (Serial.available()) feed(Serial.read());


  ws.loop();
  // Process WiFi text. Copy first: a program run from feed() reuses the inbox for stops.
  if (inboxLen > 0) {
    char text[sizeof(inbox)];
    uint16_t len = inboxLen;
    memcpy(text, inbox, len);
    inboxLen = 0;
    for (uint16_t i = 0; i < len; i++) feed(text[i]);
  }


  // Safety: stop if the remote went quiet (button released without a stop, or link lost).
  if (remoteActive && millis() - lastRemoteMs > REMOTE_TIMEOUT_MS) {
    stopMotors();
    remoteActive = false;
    remoteDir = 0;
  }


  // Remote: end of the start-up kick - settle to the chosen speed.
  if (remoteActive && kickUntil && (long)(millis() - kickUntil) >= 0) {
    drive(remoteLeft, remoteRight, remoteSpeed);
    kickUntil = 0;
  }
}
