// Temix for Windows (Electron). Loads the web app from www/ (made by prepare-web.js) and adds a port picker,
// because Electron has no built-in dialog for Web Serial (USB or Bluetooth COM ports).
const { app, BrowserWindow, dialog } = require('electron');
const path = require('path');

function createWindow() {
  const win = new BrowserWindow({
    width: 1100,
    height: 760,
    minWidth: 380,
    minHeight: 500,
    backgroundColor: '#0b1020',
    autoHideMenuBar: true,
    icon: path.join(__dirname, 'www', 'icon-512.png'),
    webPreferences: { contextIsolation: true, nodeIntegration: false },
  });

  const ses = win.webContents.session;
  ses.setPermissionCheckHandler((_wc, permission) => permission === 'serial' || permission === 'fullscreen');
  ses.setDevicePermissionHandler((details) => details.deviceType === 'serial');

  ses.on('select-serial-port', async (event, portList, _webContents, callback) => {
    event.preventDefault();

    if (portList.length === 0) {
      await dialog.showMessageBox(win, {
        type: 'info',
        title: 'Connect to robot',
        message: 'No ports found.',
        detail: 'Bluetooth: pair the HC-05/HC-06 in Windows Settings > Bluetooth first (PIN 1234 or 0000).\n' +
                'USB: plug the Arduino into this PC.',
      });
      callback('');
      return;
    }

    const labels = portList.map((p) => (p.displayName ? `${p.displayName} (${p.portName})` : p.portName));
    const { response } = await dialog.showMessageBox(win, {
      type: 'question',
      title: 'Connect to robot',
      message: 'Choose the robot\'s port',
      detail: 'Bluetooth HC-05/HC-06 shows up as "Standard Serial over Bluetooth link". ' +
              'Windows usually makes two of them - if the first one doesn\'t work, try the other.',
      buttons: [...labels, 'Cancel'],
      cancelId: labels.length,
      noLink: true,
    });
    callback(response < portList.length ? portList[response].portId : '');
  });

  win.loadFile(path.join(__dirname, 'www', 'index.html'));
}

app.whenReady().then(createWindow);
app.on('window-all-closed', () => app.quit());
