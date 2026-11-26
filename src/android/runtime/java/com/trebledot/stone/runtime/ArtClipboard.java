package com.trebledot.stone.runtime;

import android.content.ClipData;
import android.content.ClipDescription;
import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.util.Log;
import android.util.Base64;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.lang.reflect.Method;
import java.util.LinkedHashMap;
import java.util.Map;

/** Android objects/listeners stay in ART; managed Clients use the Service clipboard. */
final class ArtClipboard {
    private static native boolean available();
    private static native String[] exchange(String owner, String[] fields) throws IOException;
    private final String owner;
    private final int user;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Map<IBinder,IInterface> listeners = new LinkedHashMap<>();
    private ClipData primary;
    private final boolean remote = available();
    private Snapshot host = new Snapshot(new String[]{"","0","0","","",""});
    private long revision;
    private boolean warned;
    private static final class Snapshot {
        final String sequence, label, extras;
        final boolean present, text;
        Snapshot(String[] row) {
            if (row.length != 6 || row[3].length() > 1024 || row[5].length() > 8192
                    || !(row[1].equals("0") || row[1].equals("1")) || !(row[2].equals("0") || row[2].equals("1")))
                throw new IllegalArgumentException("Invalid Service clipboard snapshot");
            sequence = row[0]; present = row[1].equals("1"); text = row[2].equals("1"); label = row[3]; extras = row[5];
        }
    }

    ArtClipboard(String owner, int user) {
        this.owner = owner; this.user = user;
        if (remote) java.util.concurrent.Executors.newSingleThreadScheduledExecutor(action -> {
            Thread thread = new Thread(action,"Stone clipboard state"); thread.setDaemon(true); return thread;
        }).scheduleWithFixedDelay(this::poll,0,500,java.util.concurrent.TimeUnit.MILLISECONDS);
    }
    private boolean foreground() { return ArtServices.activities.importance() <= android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND; }
    private synchronized void accept(String[] row, boolean notify) {
        Snapshot next = new Snapshot(row); boolean different = !host.sequence.equals(next.sequence); host = next; revision++;
        if (notify && different) changed();
    }
    private String[] request(String... fields) {
        try { return exchange(owner,fields); }
        catch (IOException error) { throw new IllegalStateException("Clipboard service operation failed",error); }
    }
    private void poll() {
        if (!foreground()) return;
        try {
            long expected; String previous; synchronized (this) { expected = revision; previous = host.sequence; }
            String[] sequence = request("sequence");
            if (sequence.length != 1) throw new IOException("Invalid clipboard sequence");
            if (!sequence[0].equals(previous)) {
                String[] row = request("describe");
                synchronized (this) { if (revision == expected) accept(row,!previous.isEmpty()); }
            }
            warned = false;
        } catch (IOException | RuntimeException error) { if (!warned) Log.w("StoneClipboard","Windows clipboard unavailable",error); warned = true; }
    }
    private static String extras(ClipDescription description) throws IOException {
        if (description.getExtras() == null) return "";
        ByteArrayOutputStream output = new ByteArrayOutputStream(); description.getExtras().writeToStream(output);
        String encoded = Base64.encodeToString(output.toByteArray(),Base64.NO_WRAP);
        if (encoded.length() > 8192) throw new IllegalArgumentException("Clipboard metadata is too large"); return encoded;
    }
    private static ClipData from(String[] row) throws IOException, org.xmlpull.v1.XmlPullParserException {
        Snapshot state = new Snapshot(row); if (!state.present) return null;
        ClipData value = ClipData.newPlainText(state.label,row[4]);
        if (!state.extras.isEmpty()) {
            try {
                byte[] bytes = Base64.decode(state.extras,Base64.DEFAULT);
                org.xmlpull.v1.XmlPullParser parser = android.util.Xml.newPullParser(); parser.setInput(new ByteArrayInputStream(bytes),"UTF-8"); int count = 0, items = 0;
                for (int event = parser.nextToken(); event != org.xmlpull.v1.XmlPullParser.END_DOCUMENT; event = parser.nextToken()) {
                    if (event == org.xmlpull.v1.XmlPullParser.DOCDECL || parser.getDepth() > 16 || event == org.xmlpull.v1.XmlPullParser.START_TAG && ++count > 512)
                        throw new IOException("Invalid clipboard metadata structure");
                    if (event == org.xmlpull.v1.XmlPullParser.START_TAG) {
                        String number = parser.getAttributeValue(null,"num");
                        if (number != null) { long size = Long.parseLong(number); if (size < 0 || size > 512 || (items += (int) size) > 512) throw new IOException("Clipboard metadata array is too large"); }
                    }
                }
                value.getDescription().setExtras(android.os.PersistableBundle.readFromStream(new ByteArrayInputStream(bytes)));
            } catch (IOException | RuntimeException | org.xmlpull.v1.XmlPullParserException error) { Log.w("StoneClipboard","Ignored invalid Windows clipboard metadata"); }
        }
        return value;
    }
    private static ClipData copy(ClipData value) {
        if (value == null) return null;
        if (value.getItemCount() != 1 || value.getItemAt(0).getText() == null
                || value.getItemAt(0).getUri() != null || value.getItemAt(0).getIntent() != null || value.getItemAt(0).getHtmlText() != null)
            throw new UnsupportedOperationException("Only a single plain-text clipboard item is connected");
        String text = value.getItemAt(0).getText().toString();
        CharSequence sourceLabel = value.getDescription().getLabel();
        String label = sourceLabel == null ? null : sourceLabel.toString();
        if (text.length() > 8192 || label != null && label.length() > 1024)
            throw new IllegalArgumentException("Clipboard text exceeds the supported size");
        ClipData result = ClipData.newPlainText(label,text);
        if (value.getDescription().getExtras() != null)
            result.getDescription().setExtras(value.getDescription().getExtras().deepCopy());
        return result;
    }
    private void changed() {
        for (IInterface listener : listeners.values().toArray(new IInterface[0])) main.post(() -> {
            synchronized (ArtClipboard.this) { if (listeners.get(listener.asBinder()) != listener) return; }
            try { Class.forName("android.content.IOnPrimaryClipChangedListener").getMethod("dispatchPrimaryClipChanged").invoke(listener); }
            catch (ReflectiveOperationException error) { Log.w("StoneClipboard","Clipboard listener failed",error); }
        });
    }
    synchronized Object call(Object instance, Method method, Object[] args) throws Exception {
        String name = method.getName();
        if (name.equals("setPrimaryClipAsPackage") || name.equals("getPrimaryClipSource")
                || name.equals("areClipboardAccessNotificationsEnabledForUser") || name.equals("setClipboardAccessNotificationsEnabledForUser"))
            throw new SecurityException("Privileged clipboard policy is unavailable to applications");
        int offset = name.equals("setPrimaryClip") || name.equals("addPrimaryClipChangedListener") || name.equals("removePrimaryClipChangedListener") ? 1 : 0;
        if (args == null || args.length < offset + 4 || !owner.equals(args[offset]) || (Integer) args[offset+2] != user || (Integer) args[offset+3] != 0)
            throw new SecurityException("Clipboard caller, user or device does not match the application");
        if (remote) {
            if (name.equals("addPrimaryClipChangedListener") || name.equals("removePrimaryClipChangedListener")) { /* Binder listeners remain local below. */ }
            else if (!foreground()) {
                if (name.equals("hasPrimaryClip") || name.equals("hasClipboardText")) return false;
                if (name.equals("getPrimaryClip") || name.equals("getPrimaryClipDescription")) return null;
                throw new SecurityException("Clipboard writes require a foreground application");
            } else switch (name) {
                case "setPrimaryClip": {
                    if (args[0] == null) throw new IllegalArgumentException("Missing clipboard data");
                    ClipData value = copy((ClipData) args[0]);
                    accept(request("set",value.getDescription().getLabel() == null ? "" : value.getDescription().getLabel().toString(),value.getItemAt(0).getText().toString(),extras(value.getDescription())),true); return null;
                }
                case "clearPrimaryClip": accept(request("clear"),true); return null;
                case "getPrimaryClip": {
                    String[] row = request("get"); accept(row,!host.sequence.isEmpty()); return from(row);
                }
                case "getPrimaryClipDescription": return host.present ? from(new String[]{host.sequence,"1",host.text ? "1" : "0",host.label,"",host.extras}).getDescription() : null;
                case "hasPrimaryClip": return host.present;
                case "hasClipboardText": return host.text;
                default: throw new UnsupportedOperationException("ART clipboard service: " + name);
            }
        }
        switch (name) {
            case "setPrimaryClip":
                if (args[0] == null) throw new IllegalArgumentException("Missing clipboard data");
                primary = copy((ClipData) args[0]); changed(); return null;
            case "clearPrimaryClip": if (primary != null) { primary = null; changed(); } return null;
            case "getPrimaryClip": return copy(primary);
            case "getPrimaryClipDescription": return primary == null ? null : new ClipDescription(copy(primary).getDescription());
            case "hasPrimaryClip": return primary != null;
            case "hasClipboardText": return primary != null && primary.getItemAt(0).getText().length() != 0;
            case "addPrimaryClipChangedListener": {
                IInterface listener = (IInterface) args[0];
                if (!listeners.containsKey(listener.asBinder()) && listeners.size() >= 64) throw new IllegalStateException("Too many clipboard listeners");
                listeners.put(listener.asBinder(),listener); return null;
            }
            case "removePrimaryClipChangedListener": listeners.remove(((IInterface) args[0]).asBinder()); return null;
            default: throw new UnsupportedOperationException("ART clipboard service: " + name);
        }
    }
}
