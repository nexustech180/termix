// Runs after "npx cap add android": adds our native code and the Bluetooth permissions.
const fs = require('fs');
const path = require('path');

const root = __dirname;
const javaDir = path.join(root, 'android', 'app', 'src', 'main', 'java', 'com', 'temix', 'app');
const manifestPath = path.join(root, 'android', 'app', 'src', 'main', 'AndroidManifest.xml');

fs.mkdirSync(javaDir, { recursive: true });
for (const file of ['MainActivity.java', 'TemixNativePlugin.java']) {
  fs.copyFileSync(path.join(root, file), path.join(javaDir, file));
}

const permissions = [
  '<uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30" />',
  '<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30" />',
  '<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />',
  '<uses-feature android:name="android.hardware.bluetooth" android:required="true" />',
];

let manifest = fs.readFileSync(manifestPath, 'utf8');
const missing = permissions.filter((p) => !manifest.includes(p.match(/android:name="([^"]+)"/)[0]));
if (missing.length) {
  manifest = manifest.replace('<application', missing.map((p) => '    ' + p).join('\n') + '\n\n    <application');
  fs.writeFileSync(manifestPath, manifest);
}

console.log('Android project patched:', missing.length, 'permission lines added');
