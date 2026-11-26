package com.trebledot.stone.runtime;

import android.os.Parcel;
import android.view.InputDevice;
import android.view.KeyCharacterMap;

/** The virtual keyboard used by Win32-generated KeyEvents and framework menu shortcuts. */
final class ArtInput {
    private InputDevice keyboard;

    synchronized InputDevice device(int id) throws ReflectiveOperationException {
        if (id != KeyCharacterMap.VIRTUAL_KEYBOARD) return null;
        if (keyboard != null) return keyboard;
        KeyCharacterMap map = loadVirtualMap();
        Class<?> builderType = Class.forName("android.view.InputDevice$Builder");
        Object builder = builderType.getConstructor().newInstance();
        set(builder, "setId", int.class, KeyCharacterMap.VIRTUAL_KEYBOARD);
        set(builder, "setName", String.class, "Stone virtual keyboard");
        set(builder, "setDescriptor", String.class, "stone:virtual-keyboard");
        set(builder, "setSources", int.class, InputDevice.SOURCE_KEYBOARD);
        set(builder, "setKeyboardType", int.class, InputDevice.KEYBOARD_TYPE_ALPHABETIC);
        set(builder, "setKeyCharacterMap", KeyCharacterMap.class, map);
        keyboard = (InputDevice) builderType.getMethod("build").invoke(builder);
        return keyboard;
    }

    private static void set(Object builder, String name, Class<?> type, Object value) throws ReflectiveOperationException {
        builder.getClass().getMethod(name, type).invoke(builder, value);
    }

    private static KeyCharacterMap loadVirtualMap() throws ReflectiveOperationException {
        // The image Java API only loads maps from InputDevice, which we are creating.
        // Rehydrate an owned native map with its base filename, then ask libinput to
        // parse that file. Key data and behavior remain entirely image-owned.
        // Android 37's KeyCharacterMap parcel: device, presence, String8 filename,
        // keyboard type, overlay flag, then key/remapping/scan/usage entry counts.
        Parcel parcel = Parcel.obtain();
        KeyCharacterMap map;
        try {
            parcel.writeInt(KeyCharacterMap.VIRTUAL_KEYBOARD);
            parcel.writeInt(1);
            Parcel.class.getMethod("writeString8", String.class).invoke(parcel, "/system/usr/keychars/Virtual.kcm");
            for (int i = 0; i < 6; i++) parcel.writeInt(0);
            parcel.setDataPosition(0);
            map = KeyCharacterMap.CREATOR.createFromParcel(parcel);
        } finally { parcel.recycle(); }
        int status = nativeReloadMap(map);
        if (status != 0) throw new IllegalStateException("Could not load the selected image's Virtual.kcm: " + status);
        return map;
    }

    private static native int nativeReloadMap(KeyCharacterMap map);
}
