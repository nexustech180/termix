package com.temix.app;

import android.os.Bundle;

import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        // Our own plugin (Bluetooth + screen orientation) must be registered before super.onCreate.
        registerPlugin(TemixNativePlugin.class);
        super.onCreate(savedInstanceState);
    }
}
