package com.trebledot.stone.probe;

import android.app.Activity;
import android.os.Parcel;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.Window;
import java.lang.reflect.Proxy;

/** Image keyboard data and the PhoneWindow shortcut path that prepares panel menus. */
final class InputProbe {
    static void map() {
        KeyCharacterMap map = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
        if (map.getKeyboardType() != KeyCharacterMap.FULL
                || map.get(KeyEvent.KEYCODE_A, 0) != 'a'
                || map.get(KeyEvent.KEYCODE_A, KeyEvent.META_SHIFT_ON) != 'A'
                || map.get(KeyEvent.KEYCODE_1, KeyEvent.META_SHIFT_ON) != '!'
                || map.getDisplayLabel(KeyEvent.KEYCODE_A) != 'A'
                || map.getNumber(KeyEvent.KEYCODE_1) != '1')
            throw new AssertionError("Image Virtual.kcm characters, modifiers and labels");
        InputDevice device = InputDevice.getDevice(KeyCharacterMap.VIRTUAL_KEYBOARD);
        if (device == null || !device.isVirtual() || device.getKeyboardType() != InputDevice.KEYBOARD_TYPE_ALPHABETIC
                || !device.supportsSource(InputDevice.SOURCE_KEYBOARD) || InputDevice.getDevice(87654321) != null)
            throw new AssertionError("Virtual input device enumeration");
        if (KeyCharacterMap.load(87654321).get(KeyEvent.KEYCODE_A, 0) != 'a')
            throw new AssertionError("Unknown keyboard did not fall back to the virtual map");
        Parcel parcel = Parcel.obtain();
        try {
            map.writeToParcel(parcel, 0); parcel.setDataPosition(0);
            if (KeyCharacterMap.CREATOR.createFromParcel(parcel).get(KeyEvent.KEYCODE_A, KeyEvent.META_SHIFT_ON) != 'A')
                throw new AssertionError("Image key map parcel ownership");
        } finally { parcel.recycle(); }
        KeyEvent[] events = map.getEvents("aA1!".toCharArray());
        if (events == null || events.length < 8) throw new AssertionError("Image key event synthesis");
        System.out.println("ART loaded the selected image's full Virtual.kcm with modifiers, parcel round trip and native key event synthesis.");
    }

    static void menu(Activity activity) throws Exception {
        Window window = activity.getWindow();
        Window.Callback previous = window.getCallback();
        int[] selected = {0};
        Window.Callback callback = (Window.Callback) Proxy.newProxyInstance(Window.Callback.class.getClassLoader(),
            new Class<?>[]{Window.Callback.class}, (proxy, method, args) -> {
                if (method.getName().equals("onCreatePanelMenu")) {
                    ((Menu) args[1]).add(0, 9631, 0, "Keyboard shortcut").setAlphabeticShortcut('s', KeyEvent.META_CTRL_ON);
                    return true;
                }
                if (method.getName().equals("onPreparePanel")) return true;
                if (method.getName().equals("onMenuItemSelected") && ((android.view.MenuItem) args[1]).getItemId() == 9631) {
                    selected[0]++; return true;
                }
                try { return method.invoke(previous, args); }
                catch (java.lang.reflect.InvocationTargetException error) { throw error.getCause(); }
            });
        window.setCallback(callback);
        try {
            long time = SystemClock.uptimeMillis();
            KeyEvent event = new KeyEvent(time, time, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_S, 0,
                KeyEvent.META_CTRL_ON, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD);
            if (!window.performPanelShortcut(Window.FEATURE_OPTIONS_PANEL, KeyEvent.KEYCODE_S, event, Menu.FLAG_PERFORM_NO_CLOSE)
                    || selected[0] != 1) throw new AssertionError("PhoneWindow panel preparation and QWERTY shortcut dispatch");
        } finally { window.setCallback(previous); }
        System.out.println("ART prepared the real PhoneWindow panel menu and dispatched its virtual-keyboard shortcut.");
    }
}
