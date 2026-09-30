// Robot link: command parsing + connection over Bluetooth (HC-05 / HC-06) or USB.
// Both use the Web Serial API. Must match the protocol in temix_robot.ino.

const Robot = (() => {
  const METERS_PER_STEP = 1;
  const DEG_PER_TURN = 90;

  // Standard Bluetooth Serial Port Profile - what HC-05 / HC-06 modules speak.
  const SPP_UUID = '00001101-0000-1000-8000-00805f9b34fb';
  const BAUD = 9600;

  // ---------- Parsing ----------
  // Returns { program: "FFFRFFFLLBB", steps: [{cmd:'F', count:3}, ...] } or { error }.
  function parse(input) {
    const program = input.toUpperCase().replace(/\s+/g, '');
    if (!program) return { error: 'Empty command.' };
    const bad = program.match(/[^FBLR]/);
    if (bad) return { error: `Unknown command '${bad[0]}'. Use F, B, L, R.` };
    if (program.length > 64) return { error: 'Too long (max 64 letters).' };

    const steps = [];
    for (const c of program) {
      const last = steps[steps.length - 1];
      if (last && last.cmd === c) last.count++;
      else steps.push({ cmd: c, count: 1 });
    }
    return { program, steps };
  }

  function describeStep({ cmd, count }) {
    switch (cmd) {
      case 'F': return `forward ${count * METERS_PER_STEP} m`;
      case 'B': return `backward ${count * METERS_PER_STEP} m`;
      case 'R': return `right ${count * DEG_PER_TURN}°`;
      case 'L': return `left ${count * DEG_PER_TURN}°`;
    }
  }

  // ---------- Connection ----------
  let transport = null;       // { name, write(text), close() }
  let lineHandler = () => {};
  let statusHandler = () => {};
  let rxBuffer = '';
  const encoder = new TextEncoder();
  const decoder = new TextDecoder();

  function receive(text) {
    rxBuffer += text;
    let i;
    while ((i = rxBuffer.search(/\r?\n/)) >= 0) {
      const line = rxBuffer.slice(0, i).trim();
      rxBuffer = rxBuffer.slice(rxBuffer[i] === '\r' ? i + 2 : i + 1);
      if (line) lineHandler(line);
    }
  }

  function setTransport(t) {
    transport = t;
    rxBuffer = '';
    statusHandler(t ? t.name : null);
  }

  // ---------- Which kind of app are we running in? ----------
  // Android app (APK): Bluetooth goes through our native plugin, TemixNativePlugin.java.
  const nativeAndroid = (() => {
    const cap = window.Capacitor;
    if (!cap || !cap.isNativePlatform || !cap.isNativePlatform()) return null;
    const register = cap.registerPlugin || (window.capacitorExports && window.capacitorExports.registerPlugin);
    return register ? register('TemixNative') : null;
  })();
  // Windows app (EXE): Web Serial with our own port picker (electron-main.js).
  const isElectron = /Electron/.test(navigator.userAgent);
  const isNativeApp = !!nativeAndroid || isElectron;

  const supportsSerial = () => !!nativeAndroid || !!navigator.serial;
  const NO_SERIAL = 'This browser cannot connect to the robot. Use Chrome or Edge on Android (version 137+), Windows, Mac or Linux. iPhones cannot connect to HC-05/HC-06 modules.';

  async function connectNative() {
    const { name } = await nativeAndroid.connect();   // native list of paired devices
    const subs = [];
    const drop = () => { subs.forEach((s) => s.remove()); subs.length = 0; };
    subs.push(await nativeAndroid.addListener('data', (e) => receive(e.data)));
    subs.push(await nativeAndroid.addListener('disconnected', () => { drop(); setTransport(null); }));

    setTransport({
      name: `Bluetooth (${name || 'robot'})`,
      async write(text) { await nativeAndroid.write({ data: text }); },
      async close() {
        drop();
        await nativeAndroid.disconnect();
        setTransport(null);
      },
    });
  }

  // HC-05 / HC-06: the module must be paired in the phone/PC Bluetooth settings first (PIN 1234 or 0000).
  function connectBluetooth() {
    if (nativeAndroid) return connectNative();
    // In the Windows app, paired HC-05s are plain COM ports - list them all in our picker.
    if (isElectron) return openSerial({}, 'Bluetooth');
    return openSerial({
      filters: [{ bluetoothServiceClassId: SPP_UUID }],
      allowedBluetoothServiceClassIds: [SPP_UUID],
    }, 'Bluetooth');
  }

  function connectUsb() {
    return openSerial({}, 'USB');
  }

  async function openSerial(options, name) {
    if (!supportsSerial()) throw new Error(NO_SERIAL);
    const port = await navigator.serial.requestPort(options);
    await port.open({ baudRate: BAUD });
    const writer = port.writable.getWriter();
    const reader = port.readable.getReader();
    let open = true;

    (async () => {
      try {
        while (open) {
          const { value, done } = await reader.read();
          if (done) break;
          receive(decoder.decode(value));
        }
      } catch (_) { /* unplugged */ }
      if (open) { open = false; setTransport(null); }
    })();

    setTransport({
      name,
      async write(text) { await writer.write(encoder.encode(text)); },
      async close() {
        open = false;
        try { await reader.cancel(); } catch (_) {}
        reader.releaseLock(); writer.releaseLock();
        await port.close();
        setTransport(null);
      },
    });
  }

  async function disconnect() { if (transport) await transport.close(); }

  async function send(program) {
    if (!transport) throw new Error('Not connected.');
    await transport.write(program + '\n');
  }

  async function stop() {
    if (!transport) throw new Error('Not connected.');
    await transport.write('X\n');
  }

  // Remote control: dir is 'F', 'B', 'L', 'R' or 'S' (stop); speed 0-255.
  async function remote(dir, speed) {
    if (!transport) throw new Error('Not connected.');
    await transport.write(dir === 'S' ? '!S\n' : `!${dir}${speed}\n`);
  }

  return {
    parse, describeStep,
    connectBluetooth, connectUsb, disconnect, send, stop, remote,
    supportsSerial, isNativeApp,
    // Android app only: lock/unlock landscape. Returns false when not available.
    async nativeLandscape(on) {
      if (!nativeAndroid) return false;
      try { await nativeAndroid.setLandscape({ on }); return true; } catch (_) { return false; }
    },
    isConnected: () => !!transport,
    onLine: (fn) => { lineHandler = fn; },
    onStatus: (fn) => { statusHandler = fn; },
  };
})();
