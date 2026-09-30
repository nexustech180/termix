package com.temix.app;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.content.pm.ActivityInfo;
import android.os.Build;

import com.getcapacitor.JSObject;
import com.getcapacitor.PermissionState;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.getcapacitor.annotation.Permission;
import com.getcapacitor.annotation.PermissionCallback;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/**
 * Native side of the Temix Android app.
 *  - Bluetooth Classic serial (SPP) to an HC-05 / HC-06 module.
 *  - Locking the screen to landscape for the remote controller.
 *
 * JavaScript: see robot.js (the "nativeAndroid" transport).
 *   connect()             -> shows a list of paired devices, connects, resolves { name }
 *   write({ data })       -> sends text
 *   disconnect()
 *   setLandscape({ on })
 *   events: "data" { data }, "disconnected"
 */
@CapacitorPlugin(
    name = "TemixNative",
    permissions = { @Permission(alias = "bluetooth", strings = { Manifest.permission.BLUETOOTH_CONNECT }) }
)
public class TemixNativePlugin extends Plugin {
    // Standard Serial Port Profile UUID - what HC-05 / HC-06 modules use.
    private static final UUID SPP_UUID = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");

    private BluetoothSocket socket;
    private OutputStream out;

    // ---------------- Connecting ----------------

    @PluginMethod
    public void connect(PluginCall call) {
        // Android 12+ asks the user for "Nearby devices" permission.
        if (Build.VERSION.SDK_INT >= 31 && getPermissionState("bluetooth") != PermissionState.GRANTED) {
            requestPermissionForAlias("bluetooth", call, "bluetoothPermissionResult");
            return;
        }
        pickDevice(call);
    }

    @PermissionCallback
    private void bluetoothPermissionResult(PluginCall call) {
        if (getPermissionState("bluetooth") == PermissionState.GRANTED) pickDevice(call);
        else call.reject("Bluetooth permission was denied. Allow \"Nearby devices\" for Temix in phone Settings.");
    }

    @SuppressLint("MissingPermission")
    private void pickDevice(PluginCall call) {
        BluetoothManager manager = (BluetoothManager) getContext().getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
        if (adapter == null) { call.reject("This phone has no Bluetooth."); return; }
        if (!adapter.isEnabled()) { call.reject("Bluetooth is off. Turn it on and try again."); return; }

        List<BluetoothDevice> devices = new ArrayList<>(adapter.getBondedDevices());
        if (devices.isEmpty()) {
            call.reject("No paired devices. Pair the HC-05/HC-06 in your phone's Bluetooth settings first (PIN 1234 or 0000).");
            return;
        }

        String[] labels = new String[devices.size()];
        for (int i = 0; i < devices.size(); i++) {
            String name = devices.get(i).getName();
            labels[i] = (name == null ? "Unknown device" : name) + "\n" + devices.get(i).getAddress();
        }

        getActivity().runOnUiThread(() -> new AlertDialog.Builder(getActivity())
            .setTitle("Choose your robot")
            .setItems(labels, (dialog, which) -> openSocket(call, devices.get(which)))
            .setOnCancelListener(dialog -> call.reject("No device selected."))
            .show());
    }

    @SuppressLint("MissingPermission")
    private void openSocket(PluginCall call, BluetoothDevice device) {
        new Thread(() -> {
            closeQuietly();
            BluetoothSocket s = null;
            try {
                s = device.createRfcommSocketToServiceRecord(SPP_UUID);
                s.connect();
            } catch (IOException first) {
                // Some HC-05 modules only accept the "channel 1" fallback socket.
                try { if (s != null) s.close(); } catch (IOException ignored) {}
                try {
                    s = (BluetoothSocket) device.getClass()
                        .getMethod("createRfcommSocket", int.class)
                        .invoke(device, 1);
                    s.connect();
                } catch (Exception second) {
                    try { if (s != null) s.close(); } catch (IOException ignored) {}
                    call.reject("Could not connect to " + device.getName() + ". Is the robot switched on and in range?");
                    return;
                }
            }

            try {
                synchronized (this) {
                    socket = s;
                    out = s.getOutputStream();
                }
                startReader(s, s.getInputStream());
                JSObject ret = new JSObject();
                ret.put("name", device.getName());
                call.resolve(ret);
            } catch (IOException e) {
                closeQuietly();
                call.reject("Connection failed: " + e.getMessage());
            }
        }).start();
    }

    // Passes everything the robot sends to JavaScript as "data" events.
    private void startReader(BluetoothSocket s, InputStream in) {
        new Thread(() -> {
            byte[] buf = new byte[256];
            try {
                int n;
                while ((n = in.read(buf)) > 0) {
                    JSObject data = new JSObject();
                    data.put("data", new String(buf, 0, n, StandardCharsets.US_ASCII));
                    notifyListeners("data", data);
                }
            } catch (IOException ignored) {
                // Link lost or closed.
            }
            boolean unexpected;
            synchronized (this) { unexpected = socket == s; }
            if (unexpected) {
                closeQuietly();
                notifyListeners("disconnected", new JSObject());
            }
        }).start();
    }

    // ---------------- Sending / closing ----------------

    @PluginMethod
    public void write(PluginCall call) {
        String data = call.getString("data", "");
        OutputStream o;
        synchronized (this) { o = out; }
        if (o == null) { call.reject("Not connected."); return; }
        try {
            o.write(data.getBytes(StandardCharsets.US_ASCII));
            o.flush();
            call.resolve();
        } catch (IOException e) {
            call.reject("Send failed: " + e.getMessage());
        }
    }

    @PluginMethod
    public void disconnect(PluginCall call) {
        closeQuietly();
        call.resolve();
    }

    private synchronized void closeQuietly() {
        BluetoothSocket s = socket;
        socket = null;
        out = null;
        if (s != null) { try { s.close(); } catch (IOException ignored) {} }
    }

    @Override
    protected void handleOnDestroy() {
        closeQuietly();
    }

    // ---------------- Screen orientation ----------------

    @PluginMethod
    public void setLandscape(PluginCall call) {
        boolean on = Boolean.TRUE.equals(call.getBoolean("on", false));
        getActivity().runOnUiThread(() -> getActivity().setRequestedOrientation(
            on ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE : ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED));
        call.resolve();
    }
}
