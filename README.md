# Temix

An app that drives an Arduino robot. The home screen has two modes. Pick one, and use ← to go back:

- **Terminal**: type a route and the robot drives it.
- **Remote control**: hold arrow buttons to drive live.

In Terminal mode, type a command string and the robot runs it:

| Letter | Action |
|---|---|
| `F` | forward 1 m |
| `B` | backward 1 m |
| `R` | turn right 90° |
| `L` | turn left 90° |

Example: `FFFRFFFLLBB` means forward 3 m → right 90° → forward 3 m → left 180° → backward 2 m.
The app shows a path preview before sending. **Stop** halts the robot immediately.


**Remote control:** hold the D-pad (or arrow keys / WASD on a PC) to drive; let go to stop. The red **STOP**
button stops everything, and **− / +** change the speed. If the connection drops, the robot stops by itself within 0.6 s.

## Robot hardware

- **ESP8266** board (NodeMCU / Wemos D1 / "ESP8266MOD"), which has WiFi built in, so no Bluetooth module is needed
- L298N motor driver + 2 DC gear motors
- Wiring (labels printed on the board): ENA → D1, IN1 → D5, IN2 → D6, ENB → D2, IN3 → D7, IN4 → D0, plus a common GND
- Using an ESP32 instead? Use [`temix_esp32.ino`](temix_esp32.ino) (ENA 14, IN1 27, IN2 26, ENB 32, IN3 25, IN4 33).

**Connect over WiFi:** the robot makes its own WiFi network. On your phone or PC, join **`Temix-Robot`**
(password `temix1234`). If it says "no internet", stay connected. Then tap **WiFi** in Temix.
If it can't connect, turn off mobile data. WiFi works in the **Android app (APK)** and **Windows app (EXE)**;
browsers block it on the https website.

**Upload the code:** open [`temix_robot.ino`](temix_robot.ino) in the Arduino IDE. You need two things, once:
1. The board package **esp8266 by ESP8266 Community**: add
   `http://arduino.esp8266.com/stable/package_esp8266com_index.json` under File → Preferences →
   Additional boards manager URLs, then install it from Tools → Board → Boards Manager.
2. The library **WebSockets** by Markus Sattler (Sketch → Include Library → Manage Libraries).

Then choose Tools → Board → **NodeMCU 1.0 (ESP-12E Module)** and upload.
(For the ESP32 file, install **esp32 by Espressif Systems** and choose **ESP32 Dev Module**; hold **BOOT** if it hangs on `Connecting...`.)

**Calibrate:** the robot has no wheel sensors, so distance is timed. Run `F`, measure how far it went in cm, and set
`MS_PER_REV_EMPTY` = old value × 100 ÷ cm. Run `RRRR` and adjust `MS_PER_90_DEG` until it ends facing the same way.

## Files

Everything is in one folder, except the two GitHub workflow files, which GitHub requires in `.github/workflows/`.

| File | Purpose |
|---|---|
| `index.html`, `styles.css`, `app.js` | The app: home screen, terminal, path preview, remote controller |
| `robot.js` | Command parser + Bluetooth/USB connection to the robot |
| `manifest.webmanifest`, `sw.js` | Makes the web version installable and work offline |
| `*.png` | App icons |
| `temix_robot.ino` | Robot code for ESP8266 boards (WiFi) |
| `temix_esp32.ino` | Same robot code for ESP32 boards |
| `TemixNativePlugin.java`, `MainActivity.java` | Android app: Bluetooth and landscape lock |
| `electron-main.js` | Windows app: window + port picker |
| `package.json`, `capacitor.config.json`, `prepare-web.js`, `patch-android.js`, `.gitignore` | Build settings for the APK/EXE |
| `.nojekyll` | Tells GitHub Pages to serve the files as they are |
| `.github/workflows/deploy.yml` | Publishes the web version to GitHub Pages on every upload |
| `.github/workflows/build-apps.yml` | Builds the Android APK and Windows EXE on every upload |

## Put it on GitHub (one time)

1. Create a new **public** repository on github.com (e.g. `temix`).
2. Click **"uploading an existing file"**, select **all the files** in the `temix` folder, drag them in, and commit.
3. Add the two workflow files: **Add file → Create new file**, type the name `.github/workflows/deploy.yml`
   (typing the `/` makes the folders), paste in that file's contents, and commit.
   Do the same for `.github/workflows/build-apps.yml`.
4. Go to **Settings → Pages**, and under **Source** choose **GitHub Actions**.
5. Open the **Actions** tab. Two jobs run:
   - **Deploy to GitHub Pages** (~1 min): the web version at `https://<your-username>.github.io/<repo-name>/`.
   - **Build apps** (~10 min): the real apps.

## Download the apps

When **Build apps** has a green check, open your repo's **Releases** page (right side of the repo page, or
`https://github.com/<your-username>/<repo-name>/releases/latest`). It has:

| File | For | How to install |
|---|---|---|
| `Temix.apk` | Android phone | Download it on the phone and tap it. Android asks to allow installing from your browser/Files app: allow it once. |
| `Temix-Setup.exe` | Windows PC | Run it. Windows SmartScreen may say "unrecognized app": click **More info → Run anyway** (the app isn't code-signed). |
| `Temix-Portable.exe` | Windows PC | Runs directly, no install. |

The first time you tap **Bluetooth** in the Android app, it asks for **Nearby devices** permission. Allow it.

### How the apps are built

Everything is built on GitHub's servers, so nothing needs installing on your PC:
- **Android:** [Capacitor](https://capacitorjs.com) wraps the web app, and our own Java code
  ([`TemixNativePlugin.java`](TemixNativePlugin.java)) talks to the HC-05 over Bluetooth
  and locks the controller to landscape. The APK is debug-signed, which is fine for installing yourself,
  but not for the Play Store.
- **Windows:** [Electron](https://www.electronjs.org) wraps the web app; [`electron-main.js`](electron-main.js)
  adds the port picker. A paired HC-05 appears as a "Standard Serial over Bluetooth link" COM port.

## Install the web version instead (no download)

| Device | How |
|---|---|
| **Android** (Chrome) | Open the link, tap **Install app** (or ⋮ menu → *Install app*) |
| **iPhone / iPad** (Safari) | Open the link, tap **Share → Add to Home Screen** |
| **Windows / Mac / Linux** (Chrome or Edge) | Open the link, click **Install app** (or the install icon in the address bar) |

## Publishing updates

1. Edit your files.
2. In `sw.js`, bump `VERSION` (e.g. `0.1.0` → `0.1.1`). **If you skip this, installed web copies won't update.**
3. If you added new files, add them to the `ASSETS` list in `sw.js` so they work offline.
4. Push/upload to GitHub.
   - Web version: installed copies show an **Update** bar the next time they're opened.
   - APK / EXE: wait for **Build apps** to finish, then download and install the new file from Releases over the old one.
     If Android says **"App not installed"**, uninstall the old Temix first. This happens when no build ran
     for 7+ days, because GitHub then forgets the signing key.
