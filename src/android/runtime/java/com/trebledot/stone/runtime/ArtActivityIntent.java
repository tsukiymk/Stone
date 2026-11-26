package com.trebledot.stone.runtime;

import android.content.ComponentName;
import android.content.Intent;
import android.os.Bundle;
import android.util.Base64;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;

/** Bounded value-only transport: never unmarshals another APK's Parcelable or Serializable. */
final class ArtActivityIntent {
    // Only used on broker-authenticated routes between the same APK/UID.
    // The image Parcel rejects Binder references and descriptor marshalling;
    // application Serializable/Parcelable values use this APK's own loader.
    static String encodeLocal(Intent intent) throws IOException {
        if (intent == null) return "";
        if (intent.getSelector() != null || intent.getClipData() != null || intent.getSourceBounds() != null
                || intent.getIdentifier() != null || intent.getFlags() != 0)
            throw new UnsupportedOperationException("Private-process Activity task/grant flags are not connected yet");
        android.os.Parcel parcel = android.os.Parcel.obtain();
        try {
            parcel.writeInt(0x53544c31);
            intent.writeToParcel(parcel,0);
            if (parcel.hasFileDescriptors() || parcel.dataSize() > 12288) throw new IOException("Activity Parcel is too large or contains descriptors");
            return Base64.encodeToString(parcel.marshall(),Base64.NO_WRAP);
        } catch (RuntimeException error) { throw new IOException("Cannot marshal private-process Activity Parcel",error);
        } finally { parcel.recycle(); }
    }
    static Intent decodeLocal(String payload, ClassLoader loader) throws IOException {
        if (payload.isEmpty()) return null;
        if (payload.length() > 16384) throw new IOException("Activity Parcel is too large");
        final byte[] bytes;
        try { bytes = Base64.decode(payload,Base64.NO_WRAP); }
        catch (IllegalArgumentException error) { throw new IOException("Invalid Activity encoding",error); }
        android.os.Parcel parcel = android.os.Parcel.obtain();
        try {
            parcel.unmarshall(bytes,0,bytes.length); parcel.setDataPosition(0);
            if (bytes.length < 8 || parcel.readInt() != 0x53544c31) throw new IOException("Unknown private-process Activity Parcel version");
            Intent result = Intent.CREATOR.createFromParcel(parcel); result.setExtrasClassLoader(loader);
            if (parcel.dataAvail() != 0 || result.hasFileDescriptors() || result.getSelector() != null || result.getClipData() != null
                    || result.getFlags() != 0 || result.getSourceBounds() != null || result.getIdentifier() != null)
                throw new IOException("Invalid private-process Activity Parcel");
            return result;
        } catch (RuntimeException error) { throw new IOException("Invalid Activity Parcel",error); }
        finally { parcel.recycle(); }
    }
    private static void text(DataOutputStream output, String value) throws IOException {
        output.writeBoolean(value != null); if (value != null) output.writeUTF(value);
    }
    private static String text(DataInputStream input) throws IOException { return input.readBoolean() ? input.readUTF() : null; }
    static String encode(Intent intent) throws IOException {
        if (intent == null) return "";
        if (intent.getSelector() != null || intent.getClipData() != null || intent.getFlags() != 0
                || intent.getSourceBounds() != null || intent.getIdentifier() != null)
            throw new UnsupportedOperationException("Cross-process Activity selectors, ClipData and task/grant flags are not connected yet");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (DataOutputStream out = new DataOutputStream(bytes)) {
            out.writeInt(1);
            text(out,intent.getAction()); text(out,intent.getDataString()); text(out,intent.getType()); text(out,intent.getPackage());
            text(out,intent.getComponent() == null ? null : intent.getComponent().flattenToString());
            java.util.Set<String> categories = intent.getCategories();
            if (categories != null && categories.size() > 32) throw new IOException("Too many Activity categories");
            out.writeInt(categories == null ? 0 : categories.size());
            if (categories != null) for (String category : categories) out.writeUTF(category);
            Bundle extras = intent.getExtras();
            if (extras != null && extras.size() > 64) throw new IOException("Too many Activity extras");
            out.writeInt(extras == null ? 0 : extras.size());
            if (extras != null) for (String key : extras.keySet()) {
                out.writeUTF(key); Object value = extras.get(key);
                if (value == null) out.writeByte(0);
                else if (value instanceof String) { out.writeByte(1); out.writeUTF((String)value); }
                else if (value instanceof Integer) { out.writeByte(2); out.writeInt((Integer)value); }
                else if (value instanceof Long) { out.writeByte(3); out.writeLong((Long)value); }
                else if (value instanceof Boolean) { out.writeByte(4); out.writeBoolean((Boolean)value); }
                else if (value instanceof Float) { out.writeByte(5); out.writeFloat((Float)value); }
                else if (value instanceof Double) { out.writeByte(6); out.writeDouble((Double)value); }
                else if (value instanceof byte[]) {
                    byte[] array = (byte[])value;
                    if (array.length > 8192) throw new IOException("Activity byte array is too large");
                    out.writeByte(7); out.writeInt(array.length); out.write(array);
                } else throw new UnsupportedOperationException("Cross-process Activity extra type is not supported: "+value.getClass().getName());
            }
        }
        if (bytes.size() > 12288) throw new IOException("Activity Intent is too large");
        return Base64.encodeToString(bytes.toByteArray(),Base64.NO_WRAP);
    }
    private static int count(DataInputStream input, int maximum) throws IOException {
        int count = input.readInt(); if (count < 0 || count > maximum) throw new IOException("Invalid Activity field count"); return count;
    }
    static Intent decode(String payload) throws IOException {
        if (payload.isEmpty()) return null;
        if (payload.length() > 16384) throw new IOException("Activity Intent is too large");
        byte[] bytes;
        try { bytes = Base64.decode(payload,Base64.NO_WRAP); }
        catch (IllegalArgumentException error) { throw new IOException("Invalid Activity encoding",error); }
        try (DataInputStream in = new DataInputStream(new ByteArrayInputStream(bytes))) {
            if (in.readInt() != 1) throw new IOException("Unknown Activity Intent version");
            Intent intent = new Intent(); intent.setAction(text(in));
            String data = text(in), type = text(in), pkg = text(in), component = text(in);
            intent.setDataAndType(data == null ? null : android.net.Uri.parse(data),type); intent.setPackage(pkg);
            if (component != null) {
                ComponentName name = ComponentName.unflattenFromString(component);
                if (name == null) throw new IOException("Invalid Activity component"); intent.setComponent(name);
            }
            int categories = count(in,32);
            for (int index = 0; index < categories; ++index) intent.addCategory(in.readUTF());
            int extras = count(in,64);
            java.util.Set<String> keys = new java.util.HashSet<>();
            for (int index = 0; index < extras; ++index) {
                String key = in.readUTF(); if (!keys.add(key)) throw new IOException("Duplicate Activity extra");
                switch (in.readUnsignedByte()) {
                    case 0: intent.putExtra(key,(String)null); break;
                    case 1: intent.putExtra(key,in.readUTF()); break;
                    case 2: intent.putExtra(key,in.readInt()); break;
                    case 3: intent.putExtra(key,in.readLong()); break;
                    case 4: intent.putExtra(key,in.readBoolean()); break;
                    case 5: intent.putExtra(key,in.readFloat()); break;
                    case 6: intent.putExtra(key,in.readDouble()); break;
                    case 7:
                        byte[] array = new byte[count(in,8192)]; in.readFully(array); intent.putExtra(key,array); break;
                    default: throw new IOException("Unknown Activity extra type");
                }
            }
            if (in.available() != 0) throw new IOException("Trailing Activity Intent fields");
            return intent;
        }
    }
}
