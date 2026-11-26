package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.pm.ComponentInfo;
import android.content.pm.PackageManager;
import android.system.ErrnoException;
import android.system.Os;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Map;
import java.util.Properties;

/** Persistent PackageManager overrides for the components declared by this APK. */
final class ArtComponentSettings {
    private final String packageName;
    private final Map<String, ComponentInfo> components;
    private final File file;
    private final Properties states = new Properties();

    ArtComponentSettings(String packageName, File data, Map<String, ComponentInfo> components) throws IOException {
        this.packageName = packageName;
        this.components = components;
        file = new File(data, ".stone-components.properties");
        try (FileInputStream input = new FileInputStream(file)) {
            states.load(input);
        } catch (FileNotFoundException absent) {
            if (file.exists()) throw absent;
        }
    }

    private int state(String name) {
        String value = states.getProperty(name);
        if ("1".equals(value)) return PackageManager.COMPONENT_ENABLED_STATE_ENABLED;
        if ("2".equals(value)) return PackageManager.COMPONENT_ENABLED_STATE_DISABLED;
        return PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    synchronized boolean enabled(ComponentInfo info) {
        int state = state(info.name);
        return state == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT ? info.enabled
            : state == PackageManager.COMPONENT_ENABLED_STATE_ENABLED;
    }

    ComponentInfo find(ComponentName name) {
        return packageName.equals(name.getPackageName()) ? components.get(name.getClassName()) : null;
    }

    synchronized int get(ComponentName name) {
        return find(name) != null ? state(name.getClassName()) : PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;
    }

    synchronized void set(ComponentName name, int next) {
        if (find(name) == null) throw new IllegalArgumentException("Unknown component: " + name);
        if (next < PackageManager.COMPONENT_ENABLED_STATE_DEFAULT || next > PackageManager.COMPONENT_ENABLED_STATE_DISABLED)
            throw new IllegalArgumentException("Invalid component enabled state: " + next);
        String key = name.getClassName();
        if (state(key) == next) return;
        Properties updated = new Properties();
        updated.putAll(states);
        if (next == PackageManager.COMPONENT_ENABLED_STATE_DEFAULT) updated.remove(key);
        else updated.setProperty(key, Integer.toString(next));
        File pending = new File(file.getParentFile(), file.getName() + ".tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(pending)) {
                updated.store(output, null);
                output.getFD().sync();
            }
            Os.rename(pending.getPath(), file.getPath());
        } catch (IOException | ErrnoException error) {
            throw new IllegalStateException("Could not store component enabled state", error);
        } finally {
            pending.delete();
        }
        states.clear();
        states.putAll(updated);
    }
}
