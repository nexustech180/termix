// App entry point. Put your app's features below the "App logic" section.

// ---------- Install button (Android, Windows, Mac, ChromeOS via Chrome/Edge) ----------
let deferredPrompt = null;
const installBtn = document.getElementById('installBtn');

window.addEventListener('beforeinstallprompt', (e) => {
  e.preventDefault();          // keep the prompt so we can show it from our own button
  deferredPrompt = e;
  installBtn.hidden = false;
});

installBtn.addEventListener('click', async () => {
  if (!deferredPrompt) return;
  deferredPrompt.prompt();
  await deferredPrompt.userChoice;
  deferredPrompt = null;
  installBtn.hidden = true;
});

window.addEventListener('appinstalled', () => { installBtn.hidden = true; });

// ---------- iPhone/iPad: no install prompt exists, so show instructions ----------
const isIOS = /iphone|ipad|ipod/i.test(navigator.userAgent) ||
  (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
const isStandalone = window.matchMedia('(display-mode: standalone)').matches ||
  window.navigator.standalone === true;
if (isIOS && !isStandalone) document.getElementById('iosHint').hidden = false;

// ---------- Service worker: offline support + update notifications ----------
// Not used in the APK / EXE: those carry their files inside and update by installing a new version.
if (Robot.isNativeApp) document.querySelector('.footer').hidden = true;
if ('serviceWorker' in navigator && !Robot.isNativeApp) {
  window.addEventListener('load', async () => {
    try {
      const reg = await navigator.serviceWorker.register('sw.js');

      const showUpdate = (worker) => {
        document.getElementById('updateBar').hidden = false;
        document.getElementById('updateBtn').onclick = () => worker.postMessage('skipWaiting');
      };

      if (reg.waiting && navigator.serviceWorker.controller) showUpdate(reg.waiting);
      reg.addEventListener('updatefound', () => {
        const worker = reg.installing;
        worker.addEventListener('statechange', () => {
          if (worker.state === 'installed' && navigator.serviceWorker.controller) showUpdate(worker);
        });
      });

      // Reload once the new version takes over.
      let reloading = false;
      navigator.serviceWorker.addEventListener('controllerchange', () => {
        if (!reloading) { reloading = true; location.reload(); }
      });

      navigator.serviceWorker.addEventListener('message', (e) => {
        if (e.data && e.data.version) document.getElementById('appVersion').textContent = e.data.version;
      });
      (reg.active || reg.waiting || reg.installing).postMessage('getVersion');
      navigator.serviceWorker.ready.then((r) => r.active.postMessage('getVersion'));
    } catch (err) {
      console.error('Service worker registration failed:', err);
    }
  });
}

// ---------- Terminal ----------
const logEl = document.getElementById('log');
const form = document.getElementById('prompt');
const input = document.getElementById('cmdInput');
const connStatus = document.getElementById('connStatus');
const btConnect = document.getElementById('btConnect');
const usbConnect = document.getElementById('usbConnect');
const disconnectBtn = document.getElementById('disconnectBtn');

const history = [];
let historyPos = 0;
let busy = false;   // true while the robot is running a program

function print(text, kind = '') {
  const line = document.createElement('div');
  line.className = `line ${kind}`;
  line.textContent = text;
  logEl.appendChild(line);
  logEl.scrollTop = logEl.scrollHeight;
}

const HELP = [
  'Commands:',
  '  F = forward 1 m     B = backward 1 m',
  '  R = turn right 90°  L = turn left 90°',
  '  Example: FFFRFFFLLBB',
  '',
  '  stop        stop the robot now (or press Stop)',
  '  bluetooth   connect over Bluetooth',
  '  usb         connect over USB cable',
  '  disconnect  close the connection',
  '  clear       clear the screen',
  '  help        show this help',
];

async function runLine(raw) {
  const text = raw.trim();
  if (!text) return;
  print(`> ${text}`, 'echo');

  switch (text.toLowerCase()) {
    case 'help': HELP.forEach((l) => print(l, 'muted')); return;
    case 'clear': logEl.innerHTML = ''; return;
    case 'stop': return stopRobot();
    case 'bluetooth': return connect('bluetooth');
    case 'usb': return connect('usb');
    case 'disconnect': return Robot.disconnect();
  }

  const result = Robot.parse(text);
  if (result.error) { print(result.error, 'error'); return; }

  print('Plan: ' + result.steps.map(Robot.describeStep).join(' → '), 'info');
  drawPath(result.steps);

  if (!Robot.isConnected()) {
    print(`Not connected - preview only. Connect with the ${isMobile ? 'Bluetooth' : 'Bluetooth or USB'} button.`, 'warn');
    return;
  }
  if (busy) { print('Robot is still moving. Wait for DONE or press Stop.', 'warn'); return; }

  try {
    busy = true;
    await Robot.send(result.program);
  } catch (err) {
    busy = false;
    print(`Send failed: ${err.message}`, 'error');
  }
}

async function stopRobot() {
  try { await Robot.stop(); print('Stop sent.', 'warn'); }
  catch (err) { print(err.message, 'error'); }
}

async function connect(kind) {
  try {
    print(`Connecting via ${kind === 'usb' ? 'USB' : 'Bluetooth'}…`, 'muted');
    if (kind === 'usb') await Robot.connectUsb();
    else await Robot.connectBluetooth();
  } catch (err) {
    // Closing the device picker is not an error worth shouting about.
    print(err.name === 'NotFoundError' ? 'No device selected.' : `Connection failed: ${err.message}`, 'error');
  }
}

// Messages from the Arduino
Robot.onLine((line) => {
  const kind = line.startsWith('ERR') ? 'error'
    : line === 'DONE' ? 'ok'
    : line === 'STOPPED' ? 'warn' : 'robot';
  print(`robot: ${line}`, kind);
  if (line === 'DONE' || line === 'STOPPED' || line.startsWith('ERR')) busy = false;
});

Robot.onStatus((name) => {
  busy = false;
  remoteStop(false);
  connStatus.textContent = name ? `Connected: ${name}` : 'Not connected';
  connStatus.className = `conn ${name ? 'on' : 'off'}`;
  btConnect.hidden = !!name;
  usbConnect.hidden = !!name || isMobile;
  disconnectBtn.hidden = !name;
  document.getElementById('linkLed').classList.toggle('on', !!name);
  print(name ? `Connected via ${name}.` : 'Disconnected.', name ? 'ok' : 'warn');
});

form.addEventListener('submit', (e) => {
  e.preventDefault();
  const text = input.value;
  if (text.trim()) { history.push(text); historyPos = history.length; }
  input.value = '';
  runLine(text);
});

// Redraw the preview as you type (only while no program is running).
input.addEventListener('input', () => {
  if (busy) return;
  const result = Robot.parse(input.value);
  if (!result.error) drawPath(result.steps);
});

// Up/down arrows scroll through previous commands, like a real terminal.
input.addEventListener('keydown', (e) => {
  if (e.key === 'ArrowUp' && historyPos > 0) { input.value = history[--historyPos]; e.preventDefault(); }
  if (e.key === 'ArrowDown') {
    historyPos = Math.min(historyPos + 1, history.length);
    input.value = history[historyPos] || '';
    e.preventDefault();
  }
});

document.getElementById('stopBtn').addEventListener('click', stopRobot);
btConnect.addEventListener('click', () => connect('bluetooth'));
usbConnect.addEventListener('click', () => connect('usb'));
disconnectBtn.addEventListener('click', () => Robot.disconnect());
const isMobile = /android|iphone|ipad|ipod/i.test(navigator.userAgent);
if (isMobile) usbConnect.hidden = true;   // phones connect over Bluetooth only
if (!Robot.supportsSerial()) btConnect.title = usbConnect.title = 'Not supported in this browser - use Chrome or Edge';

// ---------- Remote control ----------
// While a direction is held we resend it every 200 ms. The robot stops by itself if it
// hears nothing for 600 ms, so a dropped connection can't leave it driving away.
const pads = document.querySelectorAll('.controller .pad');
const remoteMsg = document.getElementById('remoteMsg');
let remoteDir = null;
let remoteTimer = null;

// Messages shown under the controller (the terminal isn't visible in remote mode).
let remoteMsgTimer = null;
function remoteNotice(text) {
  remoteMsg.textContent = text;
  clearTimeout(remoteMsgTimer);
  remoteMsgTimer = setTimeout(() => { remoteMsg.textContent = ''; }, 3000);
}

// Speed: 5 levels shown on the controller's speed lights, changed with − / +.
const SPEEDS = [110, 145, 180, 215, 255];
let speedLevel = 3;
const speedLeds = document.querySelectorAll('#speedLeds rect');
function setSpeedLevel(level) {
  speedLevel = Math.max(0, Math.min(SPEEDS.length - 1, level));
  speedLeds.forEach((led, i) => led.classList.toggle('on', i <= speedLevel));
}
setSpeedLevel(speedLevel);

document.querySelectorAll('.controller .speed-btn').forEach((btn) => {
  btn.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    btn.classList.add('active');
    setSpeedLevel(speedLevel + Number(btn.dataset.step));
  });
  ['pointerup', 'pointerleave', 'pointercancel'].forEach((ev) =>
    btn.addEventListener(ev, () => btn.classList.remove('active')));
});

function remoteStart(dir) {
  if (remoteDir === dir) return;
  if (!Robot.isConnected()) { remoteNotice('Not connected - tap Bluetooth (or USB) at the top first.'); return; }
  if (busy) { remoteNotice('A terminal program is running - press STOP first.'); return; }

  clearInterval(remoteTimer);
  remoteDir = dir;
  pads.forEach((p) => p.classList.toggle('active', p.dataset.dir === dir));
  const tick = () => Robot.remote(dir, SPEEDS[speedLevel]).catch(() => remoteStop(false));
  tick();
  remoteTimer = setInterval(tick, 200);
}

function remoteStop(send = true) {
  clearInterval(remoteTimer);
  remoteTimer = null;
  remoteDir = null;
  pads.forEach((p) => p.classList.remove('active'));
  if (send && Robot.isConnected()) Robot.remote('S').catch(() => {});
}

pads.forEach((pad) => {
  const dir = pad.dataset.dir;
  pad.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    if (dir === 'S') {
      remoteStop();
      if (busy) stopRobot();
      pad.classList.add('active');   // show the button pressed in
      return;
    }
    pad.setPointerCapture(e.pointerId);
    remoteStart(dir);
  });
  if (dir === 'S') {
    ['pointerup', 'pointerleave', 'pointercancel'].forEach((ev) =>
      pad.addEventListener(ev, () => pad.classList.remove('active')));
  } else {
    ['pointerup', 'pointercancel', 'lostpointercapture'].forEach((ev) =>
      pad.addEventListener(ev, () => { if (remoteDir === dir) remoteStop(); }));
  }
  pad.addEventListener('contextmenu', (e) => e.preventDefault());   // long-press menu on phones
});

// Keyboard driving (PC): arrow keys or WASD, Space = stop. Ignored while typing in a field.
const KEY_DIRS = { ArrowUp: 'F', w: 'F', ArrowDown: 'B', s: 'B', ArrowLeft: 'L', a: 'L', ArrowRight: 'R', d: 'R' };
const isTyping = (e) => ['INPUT', 'TEXTAREA', 'SELECT'].includes(e.target.tagName);

document.addEventListener('keydown', (e) => {
  if (currentView !== 'remote' || isTyping(e) || e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.key === ' ') { e.preventDefault(); remoteStop(); return; }
  if (e.key === '+' || e.key === '=') { setSpeedLevel(speedLevel + 1); return; }
  if (e.key === '-') { setSpeedLevel(speedLevel - 1); return; }
  const dir = KEY_DIRS[e.key] || KEY_DIRS[e.key.toLowerCase()];
  if (dir) { e.preventDefault(); remoteStart(dir); }
});
document.addEventListener('keyup', (e) => {
  const dir = KEY_DIRS[e.key] || KEY_DIRS[e.key.toLowerCase()];
  if (dir && dir === remoteDir) remoteStop();
});

// Safety: stop if the app loses focus while driving (switching apps, locking the phone).
window.addEventListener('blur', () => { if (remoteDir) remoteStop(); });
document.addEventListener('visibilitychange', () => { if (document.hidden && remoteDir) remoteStop(); });

// ---------- Path preview ----------
const canvas = document.getElementById('pathCanvas');
const ctx = canvas.getContext('2d');

// Heading 0 = facing up. Returns the list of points visited, in metres.
function tracePath(steps) {
  let x = 0, y = 0, heading = 0;
  const points = [{ x, y }];
  for (const { cmd, count } of steps) {
    if (cmd === 'R') heading += 90 * count;
    else if (cmd === 'L') heading -= 90 * count;
    else {
      const dist = (cmd === 'F' ? 1 : -1) * count;
      const rad = heading * Math.PI / 180;
      x += Math.round(Math.sin(rad)) * dist;
      y -= Math.round(Math.cos(rad)) * dist;
      points.push({ x, y });
    }
  }
  return { points, heading };
}

function cssVar(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

function drawArrow(px, py, headingDeg, size, color) {
  ctx.save();
  ctx.translate(px, py);
  ctx.rotate(headingDeg * Math.PI / 180);
  ctx.fillStyle = color;
  ctx.beginPath();
  ctx.moveTo(0, -size); ctx.lineTo(size * 0.7, size * 0.7); ctx.lineTo(-size * 0.7, size * 0.7);
  ctx.closePath(); ctx.fill();
  ctx.restore();
}

function drawPath(steps = []) {
  const { points, heading } = tracePath(steps);
  const W = canvas.width, H = canvas.height;
  const xs = points.map((p) => p.x), ys = points.map((p) => p.y);
  const minX = Math.min(...xs, -2), maxX = Math.max(...xs, 2);
  const minY = Math.min(...ys, -2), maxY = Math.max(...ys, 2);
  const span = Math.max(maxX - minX, maxY - minY) + 2;
  const scale = Math.min(W, H) / span;
  const cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
  const toPx = (p) => ({ x: W / 2 + (p.x - cx) * scale, y: H / 2 + (p.y - cy) * scale });

  ctx.clearRect(0, 0, W, H);

  // 1 m grid
  ctx.strokeStyle = cssVar('--grid');
  ctx.lineWidth = 1;
  const half = span / 2 + 1;
  for (let g = Math.floor(cx - half); g <= Math.ceil(cx + half); g++) {
    const { x } = toPx({ x: g, y: 0 });
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, H); ctx.stroke();
  }
  for (let g = Math.floor(cy - half); g <= Math.ceil(cy + half); g++) {
    const { y } = toPx({ x: 0, y: g });
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(W, y); ctx.stroke();
  }

  // Path
  ctx.strokeStyle = cssVar('--accent');
  ctx.lineWidth = 4; ctx.lineCap = 'round'; ctx.lineJoin = 'round';
  ctx.beginPath();
  points.forEach((p, i) => { const q = toPx(p); i ? ctx.lineTo(q.x, q.y) : ctx.moveTo(q.x, q.y); });
  ctx.stroke();

  // Start (outline) and end (filled, pointing where the robot faces)
  const s = toPx(points[0]), e = toPx(points[points.length - 1]);
  drawArrow(s.x, s.y, 0, 12, cssVar('--muted'));
  if (steps.length) drawArrow(e.x, e.y, heading, 14, cssVar('--ok'));
}

drawPath();
print('Temix terminal ready. Type "help" for commands.', 'muted');
print('Try: FFFRFFFLLBB', 'muted');

// ---------- Modes: home picker, terminal, remote (one at a time) ----------
// The URL hash (#terminal / #remote) picks the view, so the phone's Back button returns home.
const VIEWS = {
  home:     { el: document.getElementById('homeView'),     title: 'Temix' },
  terminal: { el: document.getElementById('terminalView'), title: 'Terminal' },
  remote:   { el: document.getElementById('remoteView'),   title: 'Remote control' },
};
const backBtn = document.getElementById('backBtn');
const viewTitle = document.getElementById('viewTitle');
let currentView = 'home';

function showView() {
  const name = location.hash.slice(1);
  const next = VIEWS[name] ? name : 'home';
  if (currentView === 'remote' && next !== 'remote') remoteStop();   // never leave it driving

  currentView = next;
  for (const [key, view] of Object.entries(VIEWS)) view.el.hidden = key !== next;
  backBtn.hidden = next === 'home';
  viewTitle.textContent = VIEWS[next].title;
  window.scrollTo(0, 0);
  if (next === 'terminal' && !isMobile) input.focus();
  setLandscape(next === 'remote' && isMobile);
}

// ---------- Auto-landscape for the controller on phones ----------
// 1st choice: fullscreen + lock the screen to landscape (Android Chrome).
// Fallback (lock refused, e.g. iPhone or page opened without a tap): rotate the page
// ourselves with CSS whenever the phone is upright, so it still shows as landscape.
let wantLandscape = false;
let orientationLocked = false;

async function setLandscape(on) {
  wantLandscape = on;
  // Android app: the native plugin locks orientation directly (no fullscreen needed).
  if (await Robot.nativeLandscape(on)) {
    orientationLocked = on;
    updateRotation();
    return;
  }
  if (on) {
    try {
      if (!document.fullscreenElement && document.documentElement.requestFullscreen) {
        await document.documentElement.requestFullscreen({ navigationUI: 'hide' });
      }
      await screen.orientation.lock('landscape');
      orientationLocked = true;
    } catch (_) {
      orientationLocked = false;   // not allowed here - the CSS rotation below takes over
    }
  } else {
    if (orientationLocked) { try { screen.orientation.unlock(); } catch (_) {} }
    orientationLocked = false;
    if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
  }
  updateRotation();
}

function updateRotation() {
  const portrait = window.innerHeight > window.innerWidth;
  const rotate = wantLandscape && !orientationLocked && portrait;
  const root = document.documentElement;
  root.classList.toggle('rotated', rotate);
  if (rotate) {
    // After rotating, the page's width is the screen height and vice versa.
    root.style.setProperty('--rot-w', window.innerHeight + 'px');
    root.style.setProperty('--rot-h', window.innerWidth + 'px');
  }
}

window.addEventListener('resize', updateRotation);
document.addEventListener('fullscreenchange', () => {
  // Leaving fullscreen (e.g. the phone's Back gesture) releases the lock.
  if (!document.fullscreenElement && orientationLocked) { orientationLocked = false; updateRotation(); }
});

window.addEventListener('hashchange', showView);
showView();
