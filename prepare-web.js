// Copies the web app files into www/ for the Android and Windows builds,
// and adds Capacitor's JavaScript so the Android app can reach the native Bluetooth plugin.
const fs = require('fs');
const path = require('path');

const root = __dirname;
const www = path.join(root, 'www');

const WEB_FILES = [
  'index.html', 'manifest.webmanifest', 'sw.js', 'styles.css', 'robot.js', 'app.js',
  'favicon-64.png', 'icon-192.png', 'icon-512.png', 'icon-maskable-512.png', 'apple-touch-icon.png',
];

fs.rmSync(www, { recursive: true, force: true });
fs.mkdirSync(www, { recursive: true });
for (const file of WEB_FILES) fs.copyFileSync(path.join(root, file), path.join(www, file));

// Capacitor runtime (only does anything inside the Android app).
const capJs = path.join(root, 'node_modules', '@capacitor', 'core', 'dist', 'capacitor.js');
if (fs.existsSync(capJs)) {
  fs.copyFileSync(capJs, path.join(www, 'capacitor.js'));
  const indexPath = path.join(www, 'index.html');
  const html = fs.readFileSync(indexPath, 'utf8')
    .replace('<script src="robot.js"></script>', '<script src="capacitor.js"></script>\n  <script src="robot.js"></script>');
  fs.writeFileSync(indexPath, html);
} else {
  console.warn('warning: @capacitor/core/dist/capacitor.js not found - run npm install first');
}

// Source image for the Android launcher icon (used by @capacitor/assets).
fs.mkdirSync(path.join(root, 'assets'), { recursive: true });
fs.copyFileSync(path.join(root, 'icon-maskable-512.png'), path.join(root, 'assets', 'icon-only.png'));

console.log('Web app copied to', www);
