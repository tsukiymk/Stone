package com.trebledot.stone.probe;

import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ProviderInfo;
import android.content.res.AssetManager;
import android.database.ContentObserver;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.net.Uri;
import com.trebledot.stone.runtime.ArtPackage;
import com.trebledot.stone.runtime.ArtApplication;

public final class Application {
    private static native boolean injectTouch(int x, int y, int width, int height, int cancel);
    private static native boolean injectText(int stage);
    private static native boolean clickTitleBack(boolean releaseOutside);
    private static native boolean isWindowMinimized();
    private static native boolean requestWindowClose();
    private static native boolean verifyMotion(android.view.MotionEvent event, int sdk);
    public static int run(AssetManager assets) throws Exception {
        verifyNativeInput();
        ArtPackage application = ArtPackage.open(assets, "/data/application.apk", "/data/data/com.trebledot.stone.cit");
        try {
            com.trebledot.stone.runtime.NativeLibrariesProbe.verify(new java.io.File(application.applicationInfo.dataDir));
            Resources.verifyNativeAssets(application.resources);
            BitmapFactoryProbe.verify(application.resources.getAssets());
            if (!"com.trebledot.stone.cit".equals(application.applicationInfo.packageName)
                    || !"com.trebledot.stone.cit.CitActivity".equals(application.activityClassName()))
                throw new AssertionError("APK launch component");
            if (application.applicationInfo.minSdkVersion != 23 || application.applicationInfo.targetSdkVersion != 32
                    || !application.applicationInfo.enabled || !application.activityInfo.exported)
                throw new AssertionError("APK application metadata");
            Intent launch = application.launchIntent();
            if (!Intent.ACTION_MAIN.equals(launch.getAction()) || !launch.hasCategory(Intent.CATEGORY_LAUNCHER)
                    || !application.activityInfo.name.equals(launch.getComponent().getClassName()))
                throw new AssertionError("APK launch intent");
            int label = application.resources.getIdentifier("app_name", "string", application.applicationInfo.packageName);
            if (!"Stone CIT".equals(application.resources.getString(label))) throw new AssertionError("APK host resources");
            if (!new java.io.File(application.applicationInfo.dataDir).isDirectory()) throw new AssertionError("application data directory");
            ArtApplication running = ArtApplication.start(application);
            if (Boolean.getBoolean("stone.probe.uncaught-worker")) {
                new Thread(() -> { throw new IllegalStateException("Stone expected uncaught worker failure"); },
                    "stone-uncaught-worker").start();
                Thread.sleep(10000);
                throw new AssertionError("Uncaught worker exception did not terminate the Client");
            }
            // Service teardown queues ActivityThread's idle resource purge even
            // when no Activity/window was created. Keep its image JNI registered.
            Class<?> threadType = Class.forName("android.app.ActivityThread");
            java.lang.reflect.Method purge = threadType.getDeclaredMethod("nPurgePendingResources");
            purge.setAccessible(true); purge.invoke(threadType.getMethod("currentActivityThread").invoke(null));
            verifyText();
            if (!application.applicationInfo.packageName.equals(running.context.getPackageName())
                    || running.application.getBaseContext() != running.context
                    || running.context.getApplicationContext() != running.application
                    || !"android.app.ContextImpl".equals(running.context.getClass().getName()))
                throw new AssertionError("AOSP application context");
            Class<?> custom = Class.forName("com.trebledot.stone.cit.components.ComponentApplication");
            java.lang.reflect.Field ready = custom.getDeclaredField("providerReady");
            ready.setAccessible(true);
            if (!custom.isInstance(running.application) || !ready.getBoolean(null))
                throw new AssertionError("APK providers must initialize before Application.onCreate");
            if (Boolean.getBoolean("stone.probe.only-filesystem")) {
                SymbolicLinkProbe.verify(running.context);
                System.out.println("PASS filesystem-only probe completed before network probes.");
                return 42;
            }
            if (Boolean.getBoolean("stone.probe.only-packages")) {
                if (System.getProperty("stone.probe.archives") == null)
                    throw new AssertionError("Package-only probe requires signed archive fixtures");
                if (Boolean.getBoolean("stone.probe.only-install-sources")) {
                    PackageStoreProbe.run(running);
                    System.out.println("PASS install-source-only query checks.");
                    return 42;
                }
                ArchiveProbe.run(running.context);
                PackageStoreProbe.run(running);
                System.out.println("PASS package-only signature, compatibility and publication checks.");
                return 42;
            }
            if (Boolean.getBoolean("stone.probe.only-uninstall")) {
                UninstallProbe.run(running);
                return 42;
            }
            if (Boolean.getBoolean("stone.probe.only-launchers")) {
                LauncherProbe.run(running);
                return 42;
            }
            if (application.providers.size() != 3
                    || !running.context.getPackageManager().getApplicationInfo(application.applicationInfo.packageName, 0)
                        .sourceDir.equals("/data/application.apk")
                    || running.context.checkSelfPermission("android.permission.INTERNAL_SYSTEM_WINDOW")
                        != android.content.pm.PackageManager.PERMISSION_DENIED)
                throw new AssertionError("local package metadata and permission boundaries");
            PackageManager packages = running.context.getPackageManager();
            String name = running.context.getPackageName();
            ProviderInfo local = packages.getProviderInfo(new ComponentName(name,
                "com.trebledot.stone.cit.components.ComponentProvider"), PackageManager.GET_META_DATA);
            ProviderInfo remote = packages.resolveContentProvider("com.trebledot.stone.cit.components.remote", 0);
            if (local.metaData.getInt("stone.label") != label || !local.metaData.getBoolean("stone.enabled")
                    || remote == null || !(name + ":remote").equals(remote.processName))
                throw new AssertionError("manifest provider process and metadata");
            verifyDatabase(running.context);
            AnimatedVectorProbe.verify(running.context);
            DocumentProbe.verify(running.context);
            verifyContentObserver(running.context.getContentResolver());
            android.content.pm.PackageInfo info = running.context.getPackageManager().getPackageInfo(application.applicationInfo.packageName, 0);
            if (info.activities.length < 2 || !java.util.Arrays.asList(info.requestedPermissions).contains("android.permission.INTERNET")
                    || java.util.Arrays.asList(info.requestedPermissions).contains("android.permission.BLUETOOTH"))
                throw new AssertionError("manifest components and version-limited permission requests");
            java.io.File directory = running.context.getFilesDir();
            if (!directory.getCanonicalPath().startsWith(application.applicationInfo.dataDir + "/"))
                throw new AssertionError("private context files directory");
            byte[] content = "ART 私有文件 📝".getBytes("UTF-8");
            try {
                try (java.io.FileOutputStream output = running.context.openFileOutput("art-context.data", 0)) { output.write(content); }
                byte[] actual = new byte[content.length];
                try (java.io.FileInputStream input = running.context.openFileInput("art-context.data")) {
                    if (input.available() != content.length || input.read(actual) != content.length
                            || input.available() != 0 || input.read() != -1 || !java.util.Arrays.equals(content, actual))
                        throw new AssertionError("private context file I/O");
                }
            } finally { running.context.deleteFile("art-context.data"); }
            try { ArtApplication.start(application); throw new AssertionError("duplicate application startup"); }
            catch (IllegalStateException expected) { }
            final java.util.List<String> lifecycle = new java.util.ArrayList<>();
            SymbolicLinkProbe.verify(running.context);
            UserProfileProbe.verify(running.context);
            AppOpsProbe.verify(running.context);
            DatagramProbe.verify();
            TcpServerProbe.verify();
            NetworkCallbackProbe.verify(running.context);
            WifiProbe.verify(running.context);
            NetworkInterfaceProbe.verify(running.context);
            ConfigurationProbe.run(running);
            CallerIdentityProbe.run(running);
            EarlyFinishProbe.run(running);
            ActivityIntentProbe.run();
            NotificationProbe.run(running.context);
            InstallerProbe.run(running.context);
            ArchiveProbe.run(running.context);
            CommitProbe.recover(running.context);
            PreapprovalProbe.recover(running.context);
            PackageStoreProbe.run(running);
            BroadcastProbe.run(running.context);
            ServiceProbe.run(running.context);
            InputProbe.map();
            ClipboardProbe.verify(running.context);
            WebLinksProbe.verify(running.context);
            ShareProbe.verify(running.context);
            android.os.UserManager users = running.context.getSystemService(android.os.UserManager.class);
            if (users == null || users.hasUserRestriction(android.os.UserManager.DISALLOW_INSTALL_UNKNOWN_SOURCES)
                    || users.hasUserRestriction(android.os.UserManager.DISALLOW_INSTALL_UNKNOWN_SOURCES_GLOBALLY))
                throw new AssertionError("Unmanaged Stone user has unexpected Android install restrictions");
            try {
                android.os.UserHandle other = (android.os.UserHandle) android.os.UserHandle.class.getMethod("of",int.class).invoke(null,1);
                android.os.UserManager.class.getMethod("hasUserRestrictionForUser",String.class,android.os.UserHandle.class)
                    .invoke(users,android.os.UserManager.DISALLOW_INSTALL_UNKNOWN_SOURCES,other);
                throw new AssertionError("Cross-user restriction query accepted");
            } catch (java.lang.reflect.InvocationTargetException denied) {
                if (!(denied.getCause() instanceof SecurityException)) throw denied;
            }
            System.out.println("PASS current unmanaged user restriction queries and cross-user denial.");
            CommitProbe.run(running);
            PreapprovalProbe.run(running);
            PackageEventsProbe.run(running);
            final android.app.Activity[] resumed = new android.app.Activity[1];
            running.application.registerActivityLifecycleCallbacks(new android.app.Application.ActivityLifecycleCallbacks() {
                @Override public void onActivityCreated(android.app.Activity activity, android.os.Bundle state) { lifecycle.add("create"); }
                @Override public void onActivityStarted(android.app.Activity activity) { lifecycle.add("start"); }
                @Override public void onActivityResumed(android.app.Activity activity) {
                    resumed[0] = activity;
                    lifecycle.add("resume");
                }
                @Override public void onActivityPaused(android.app.Activity activity) { lifecycle.add("pause"); }
                @Override public void onActivityStopped(android.app.Activity activity) { lifecycle.add("stop"); }
                @Override public void onActivitySaveInstanceState(android.app.Activity activity, android.os.Bundle state) { }
                @Override public void onActivityDestroyed(android.app.Activity activity) { lifecycle.add("destroy"); }
            });
            android.app.Activity activity = running.startActivity();
            ServiceProbe.process(running.context, android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND);
            if (activity.getApplication() != running.application || activity.getWindow() == null
                    || !activity.getComponentName().getClassName().equals(application.activityInfo.name))
                throw new AssertionError("AOSP Activity attachment");
            if (!lifecycle.equals(java.util.Arrays.asList("create", "start", "resume")))
                throw new AssertionError("AOSP Activity lifecycle callbacks: " + lifecycle);
            ToastProbe.verify(activity);
            SecureWindowProbe.verifyPolicy(activity);
            SoftwareFrameProbe.verify(activity);
            android.view.Display display = activity.getDisplay();
            if (display == null || display.getDisplayId() != 0 || display.getState() != android.view.Display.STATE_ON
                    || display.getRefreshRate() <= 0 || activity.getResources().getDisplayMetrics().densityDpi <= 0)
                throw new AssertionError("Windows display integration");
            if (android.provider.Settings.Global.getInt(running.context.getContentResolver(), "stone.unset.setting", 42) != 42)
                throw new AssertionError("unconfigured host settings use framework defaults");
            try (AutoCloseable window = com.trebledot.stone.runtime.ArtWindow.attach(activity)) {
                if (!activity.getWindow().getDecorView().isAttachedToWindow()
                        || activity.getWindow().getDecorView().getHandler() == null)
                    throw new AssertionError("ART view root attachment");
                try {
                    com.trebledot.stone.runtime.ArtWindow.attach(activity);
                    throw new AssertionError("duplicate decor attachment");
                } catch (IllegalStateException expected) { }
                drawDashboard(activity);
                InputProbe.menu(activity);
                verifyPointer(activity);
                if (Boolean.getBoolean("stone.probe.window")) verifyEditor(activity);
            } finally { com.trebledot.stone.runtime.ArtWindow.close(); }
            if (activity.getWindow().getDecorView().isAttachedToWindow()) throw new AssertionError("ART view root detachment");
            boolean liveWindow = Boolean.getBoolean("stone.probe.window");
            if (liveWindow) verifyWindowLoop(activity, resumed);
            running.destroyActivity();
            running.destroyActivity();
            ServiceProbe.process(running.context, android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_CACHED);
            if (liveWindow && android.os.AsyncTask.THREAD_POOL_EXECUTOR instanceof java.util.concurrent.ExecutorService) {
                // The probe tears down ART in-process. The standalone APK host exits
                // its process after the Activity lifecycle, including framework workers.
                ((java.util.concurrent.ExecutorService) android.os.AsyncTask.THREAD_POOL_EXECUTOR).shutdown();
            }
            java.util.List<String> expectedLifecycle = liveWindow
                ? java.util.Arrays.asList("create", "start", "resume", "pause", "create", "start", "resume", "stop",
                    "pause", "stop", "destroy", "start", "resume", "pause", "create", "start", "resume", "stop",
                    "pause", "stop", "destroy", "destroy")
                : java.util.Arrays.asList("create", "start", "resume", "pause", "stop", "destroy");
            if (!lifecycle.equals(expectedLifecycle))
                throw new AssertionError("AOSP Activity shutdown callbacks: " + lifecycle);
        } finally { application.close(); }
        application.close();
        try { application.launchIntent(); throw new AssertionError("closed package accepted launch"); }
        catch (IllegalStateException expected) { }
        return 42;
    }

    private static void verifyDatabase(Context context) {
        String name = "art-sqlite-probe.db";
        context.deleteDatabase(name);
        try {
            try (SQLiteDatabase db = context.openOrCreateDatabase(name, Context.MODE_PRIVATE, null)) {
                if (!db.enableWriteAheadLogging() || !db.isWriteAheadLoggingEnabled())
                    throw new AssertionError("Image Java SQLite did not enable WAL");
                try (Cursor mode = db.rawQuery("PRAGMA journal_mode",null)) {
                    if (!mode.moveToFirst() || !"wal".equals(mode.getString(0)))
                        throw new AssertionError("Java connection pool flags do not match actual WAL mode");
                }
                db.execSQL("CREATE TABLE probe (value TEXT NOT NULL)");
                db.execSQL("INSERT INTO probe VALUES (?)", new Object[]{"SQLite 已保存"});
                try (SQLiteDatabase reader = SQLiteDatabase.openDatabase(context.getDatabasePath(name).getPath(),null,SQLiteDatabase.OPEN_READONLY)) {
                    reader.beginTransactionReadOnly();
                    try {
                        try (Cursor row = reader.rawQuery("SELECT value FROM probe",null)) {
                            if (!row.moveToFirst() || !"SQLite 已保存".equals(row.getString(0))) throw new AssertionError("Initial Java WAL snapshot");
                        }
                        db.execSQL("UPDATE probe SET value=?",new Object[]{"SQLite WAL 已保存"});
                        try (Cursor row = reader.rawQuery("SELECT value FROM probe",null)) {
                            if (!row.moveToFirst() || !"SQLite 已保存".equals(row.getString(0))) throw new AssertionError("Concurrent writer changed pinned Java WAL snapshot");
                        }
                        reader.setTransactionSuccessful();
                    } finally { reader.endTransaction(); }
                    try (Cursor row = reader.rawQuery("SELECT value FROM probe",null)) {
                        if (!row.moveToFirst() || !"SQLite WAL 已保存".equals(row.getString(0))) throw new AssertionError("Java WAL reader did not observe committed update");
                    }
                }
            }
            try (SQLiteDatabase db = SQLiteDatabase.openDatabase(context.getDatabasePath(name).getPath(), null, SQLiteDatabase.OPEN_READONLY);
                    Cursor cursor = db.rawQuery("SELECT value FROM probe", null)) {
                if (!cursor.moveToFirst() || !"SQLite WAL 已保存".equals(cursor.getString(0)) || cursor.moveToNext())
                    throw new AssertionError("image Java SQLite round trip and cursor");
            }
            System.out.println("PASS image Java SQLite actual WAL mode, pinned read-only snapshot, concurrent committed update and reopen persistence.");
        } finally {
            context.deleteDatabase(name);
        }
    }

    private static void verifyContentObserver(ContentResolver resolver) {
        Uri parent = Uri.parse("content://com.trebledot.stone.cit.components.primary/rows");
        Uri child = Uri.withAppendedPath(parent, "1");
        java.util.List<Uri> received = new java.util.ArrayList<>();
        ContentObserver observer = new ContentObserver(null) {
            @Override public void onChange(boolean self, Uri uri) { received.add(uri); }
        };
        resolver.registerContentObserver(parent, true, observer);
        resolver.notifyChange(child, null, 0);
        resolver.unregisterContentObserver(observer);
        resolver.notifyChange(child, null, 0);
        if (!received.equals(java.util.Collections.singletonList(child)))
            throw new AssertionError("local content observer lifecycle: " + received);
    }

    private static void drawDashboard(android.app.Activity activity) throws Exception {
        int width = 480, height = 800;
        android.view.View root = activity.getWindow().getDecorView();
        root.measure(android.view.View.MeasureSpec.makeMeasureSpec(width, android.view.View.MeasureSpec.EXACTLY),
            android.view.View.MeasureSpec.makeMeasureSpec(height, android.view.View.MeasureSpec.EXACTLY));
        root.layout(0, 0, width, height);
        android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(width, height, android.graphics.Bitmap.Config.ARGB_8888);
        android.graphics.Canvas canvas = new android.graphics.Canvas(bitmap);
        root.draw(canvas);
        canvas.setBitmap(null);
        int[] pixels = new int[width * height];
        bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
        int opaque = 0;
        java.util.HashSet<Integer> colors = new java.util.HashSet<>();
        for (int pixel : pixels) { if ((pixel >>> 24) == 255) opaque++; colors.add(pixel); }
        if (opaque < width * height / 2 || colors.size() < 16) throw new AssertionError("Activity dashboard rasterization");
        if (Boolean.getBoolean("stone.probe.window")) {
            if (!com.trebledot.stone.runtime.ArtWindow.present(pixels, width, height))
                throw new AssertionError("Win32 ART frame presentation");
            System.out.println("ART presented the original Activity frame in the Stone Win32 window.");
        }
        try (java.io.FileOutputStream output = new java.io.FileOutputStream("/data/art-dashboard.png")) {
            if (!bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, output)) throw new AssertionError("dashboard PNG encoding");
        } finally { bitmap.recycle(); }
        System.out.println("ART laid out and rendered the original CIT Activity dashboard.");
    }

    private static void verifyEditor(android.app.Activity activity) throws Exception {
        android.widget.FrameLayout content = activity.findViewById(android.R.id.content);
        final class Editor extends android.widget.EditText {
            int connections, closes, leftKeys, keyMeta;
            Editor() { super(activity); }
            @Override public android.view.inputmethod.InputConnection onCreateInputConnection(android.view.inputmethod.EditorInfo info) {
                android.view.inputmethod.InputConnection target = super.onCreateInputConnection(info);
                if (target == null) return null;
                connections++;
                return new android.view.inputmethod.InputConnectionWrapper(target,false) {
                    @Override public void closeConnection() { closes++; super.closeConnection(); }
                };
            }
            @Override public boolean onKeyDown(int code,android.view.KeyEvent event) {
                if (code == android.view.KeyEvent.KEYCODE_DPAD_LEFT) { leftKeys++; keyMeta = event.getMetaState(); }
                return super.onKeyDown(code,event);
            }
        }
        Editor editor = new Editor();
        java.util.List<String> changes = new java.util.ArrayList<>();
        editor.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence text,int start,int count,int after) { }
            @Override public void onTextChanged(CharSequence text,int start,int before,int count) { }
            @Override public void afterTextChanged(android.text.Editable text) { changes.add(text.toString()); }
        });
        content.addView(editor, new android.widget.FrameLayout.LayoutParams(160, 64));
        try {
            if (!editor.requestFocus() || !injectText(0)) throw new AssertionError("Win32 editor focus and injection");
            int opened = editor.connections, closed = editor.closes;
            com.trebledot.stone.runtime.ArtWindow.pollTouches(activity);
            String initial = "a\ud83e\udd16\u4e2d\u6587";
            if (!initial.contentEquals(editor.getText()))
                throw new AssertionError("Win32 UTF-16 text: " + editor.getText());
            if (!changes.equals(java.util.Arrays.asList("a", "a\ud83e\udd16", "a\ud83e\udd16\u4e2d", initial)))
                throw new AssertionError("Win32 text edit callbacks changed: " + changes);
            if (editor.connections - opened != 1 || editor.closes - closed != 1)
                throw new AssertionError("Win32 text burst reopened its input connection: " + (editor.connections-opened) + "/" + (editor.closes-closed));
            drawDashboard(activity);
            changes.clear();
            if (!injectText(1)) throw new AssertionError("Win32 ordered text/key injection");
            com.trebledot.stone.runtime.ArtWindow.pollTouches(activity);
            if (!(initial + "yx").contentEquals(editor.getText()) || changes.size() != 2)
                throw new AssertionError("Win32 text/key order: " + editor.getText() + " / " + changes
                    + " left=" + editor.leftKeys + " meta=" + editor.keyMeta + " layout=" + (editor.getLayout()!=null));
            editor.setSelection(editor.length()); changes.clear();
            if (!injectText(2)) throw new AssertionError("Win32 split high surrogate injection");
            com.trebledot.stone.runtime.ArtWindow.pollTouches(activity);
            if (!changes.isEmpty()) throw new AssertionError("High surrogate committed without its pair");
            if (!injectText(3)) throw new AssertionError("Win32 split low surrogate injection");
            com.trebledot.stone.runtime.ArtWindow.pollTouches(activity);
            String paired = initial + "yx\ud83e\udd16";
            if (!paired.contentEquals(editor.getText()) || !changes.equals(java.util.Collections.singletonList(paired)))
                throw new AssertionError("Win32 cross-poll surrogate pair: " + editor.getText());
            changes.clear();
            if (!injectText(2)) throw new AssertionError("Win32 pending surrogate injection");
            com.trebledot.stone.runtime.ArtWindow.pollTouches(activity);
            if (!injectText(4)) throw new AssertionError("Win32 focus barrier injection");
            com.trebledot.stone.runtime.ArtWindow.pollTouches(activity);
            if (!paired.contentEquals(editor.getText()) || !changes.isEmpty())
                throw new AssertionError("Win32 text crossed a focus boundary");
            Editor next = new Editor();
            content.addView(next, new android.widget.FrameLayout.LayoutParams(160,64));
            try {
                editor.addTextChangedListener(new android.text.TextWatcher() {
                    @Override public void beforeTextChanged(CharSequence text,int start,int count,int after) { }
                    @Override public void onTextChanged(CharSequence text,int start,int before,int count) { }
                    @Override public void afterTextChanged(android.text.Editable text) {
                        if (text.toString().endsWith("1") && !next.requestFocus())
                            throw new AssertionError("Editor could not redirect focus");
                    }
                });
                if (!injectText(5)) throw new AssertionError("Win32 focus redirect injection");
                com.trebledot.stone.runtime.ArtWindow.pollTouches(activity);
                if (!(paired + "1").contentEquals(editor.getText()) || !"23".contentEquals(next.getText()))
                    throw new AssertionError("Win32 text reused a connection after editor focus changed");
            } finally { content.removeView(next); }
            System.out.println("PASS Win32 text connection reuse, unchanged edits, Chinese/emoji, key order, split surrogates and focus changes.");
        } finally { content.removeView(editor); }
    }

    private static void verifyPointer(android.app.Activity activity) throws Exception {
        android.widget.FrameLayout content = activity.findViewById(android.R.id.content);
        android.widget.Button button = new android.widget.Button(activity);
        button.setText("ART pointer");
        final int[] clicks = {0};
        final int[] touchActions = {0, 0, 0};
        button.setOnClickListener(view -> { clicks[0]++; button.setText("Clicked " + clicks[0]); });
        button.setOnTouchListener((view, event) -> {
            if (Boolean.getBoolean("stone.probe.window")) {
                int action = event.getActionMasked();
                if (event.getSource() != android.view.InputDevice.SOURCE_TOUCHSCREEN
                        || event.getToolType(0) != android.view.MotionEvent.TOOL_TYPE_FINGER || event.getButtonState() != 0)
                    throw new AssertionError("Win32 touch gesture source, tool and button metadata");
                if (action == android.view.MotionEvent.ACTION_DOWN) touchActions[0]++;
                else if (action == android.view.MotionEvent.ACTION_UP) touchActions[1]++;
                else if (action == android.view.MotionEvent.ACTION_CANCEL) touchActions[2]++;
            }
            return false;
        });
        content.addView(button, new android.widget.FrameLayout.LayoutParams(160, 64, android.view.Gravity.TOP | android.view.Gravity.RIGHT));
        try {
            drawDashboard(activity);
            int[] location = new int[2];
            button.getLocationInWindow(location);
            int x = location[0] + button.getWidth() / 2, y = location[1] + button.getHeight() / 2;
            if (Boolean.getBoolean("stone.probe.window")) {
                if (!injectTouch(x, y, 480, 800, 0)
                        || com.trebledot.stone.runtime.ArtWindow.pollTouches(activity) != 2)
                    throw new AssertionError("Win32 mouse message dispatch");
            } else {
                long now = android.os.SystemClock.uptimeMillis();
                for (int action : new int[]{android.view.MotionEvent.ACTION_DOWN, android.view.MotionEvent.ACTION_UP}) {
                    android.view.MotionEvent event = android.view.MotionEvent.obtain(now, now, action, x, y, 0);
                    try { activity.dispatchTouchEvent(event); } finally { event.recycle(); }
                }
            }
            drainMainQueue();
            if (clicks[0] != 1 || !"Clicked 1".contentEquals(button.getText()))
                throw new AssertionError("AOSP pointer click callback: " + clicks[0]);
            if (Boolean.getBoolean("stone.probe.window")) {
                if (!injectTouch(x, y, 480, 800, 1)
                        || com.trebledot.stone.runtime.ArtWindow.pollTouches(activity) != 2)
                    throw new AssertionError("Win32 mouse capture cancellation");
            } else {
                long now = android.os.SystemClock.uptimeMillis();
                for (int action : new int[]{android.view.MotionEvent.ACTION_DOWN, android.view.MotionEvent.ACTION_CANCEL}) {
                    android.view.MotionEvent event = android.view.MotionEvent.obtain(now, now, action, x, y, 0);
                    try { activity.dispatchTouchEvent(event); } finally { event.recycle(); }
                }
            }
            drainMainQueue();
            if (clicks[0] != 1 || button.isPressed()) throw new AssertionError("cancelled pointer clicked or stayed pressed");
            if (Boolean.getBoolean("stone.probe.window") && !java.util.Arrays.equals(touchActions, new int[]{2, 1, 1}))
                throw new AssertionError("Win32 pointer action sequence: " + java.util.Arrays.toString(touchActions));
            if (Boolean.getBoolean("stone.probe.window")) verifyPointerDrag(activity, content);
            drawDashboard(activity);
            System.out.println("ART dispatched pointer input through the Activity and redrew the clicked Button.");
        } finally { content.removeView(button); }
    }

    private static void verifyPointerDrag(android.app.Activity activity, android.widget.FrameLayout content) throws Exception {
        android.widget.ScrollView scroll = new android.widget.ScrollView(activity);
        android.view.View tall = new android.view.View(activity);
        scroll.addView(tall, new android.widget.ScrollView.LayoutParams(160, 2000));
        content.addView(scroll, new android.widget.FrameLayout.LayoutParams(160, 240, android.view.Gravity.TOP | android.view.Gravity.LEFT));
        final int[] moves = {0};
        final long[] downTime = {0};
        scroll.setOnTouchListener((view, event) -> {
            if (event.getSource() != android.view.InputDevice.SOURCE_TOUCHSCREEN
                    || event.getToolType(0) != android.view.MotionEvent.TOOL_TYPE_FINGER || event.getButtonState() != 0)
                throw new AssertionError("Drag lost finger metadata");
            if (event.getActionMasked() == android.view.MotionEvent.ACTION_DOWN) downTime[0] = event.getDownTime();
            if (event.getDownTime() != downTime[0]) throw new AssertionError("Drag changed down time");
            if (event.getActionMasked() == android.view.MotionEvent.ACTION_MOVE) moves[0]++;
            return false;
        });
        try {
            drawDashboard(activity);
            int[] location = new int[2];
            scroll.getLocationInWindow(location);
            if (!injectTouch(location[0] + 80, location[1] + 180, 480, 800, 2)
                    || com.trebledot.stone.runtime.ArtWindow.pollTouches(activity) != 3)
                throw new AssertionError("Win32 drag message dispatch");
            if (moves[0] != 1 || scroll.getScrollY() <= 0)
                throw new AssertionError("Finger drag did not scroll: moves=" + moves[0] + ", scrollY=" + scroll.getScrollY());
            System.out.println("PASS Win32 left-button drag scrolled an Android ScrollView with stable finger metadata.");
        } finally { content.removeView(scroll); }
    }

    private static void drainMainQueue() throws Exception {
        final boolean[] drained = {false};
        new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> drained[0] = true);
        java.lang.reflect.Method once = android.os.Looper.class.getDeclaredMethod("loopOnce", android.os.Looper.class, long.class, int.class);
        once.setAccessible(true);
        long previousIdentity = android.os.Binder.clearCallingIdentity();
        long identity = android.os.Binder.clearCallingIdentity();
        try {
            while (!drained[0]) {
                if (!((Boolean) once.invoke(null, android.os.Looper.getMainLooper(), identity, 0)))
                    throw new AssertionError("ART main message queue stopped");
            }
        } finally { android.os.Binder.restoreCallingIdentity(previousIdentity); }
    }

    private static void verifyWindowLoop(android.app.Activity activity, android.app.Activity[] resumed) throws Exception {
        android.widget.FrameLayout content = activity.findViewById(android.R.id.content);
        android.widget.Button button = new android.widget.Button(activity);
        button.setText("Live ART window");
        android.graphics.drawable.RippleDrawable ripple = new android.graphics.drawable.RippleDrawable(
            android.content.res.ColorStateList.valueOf(android.graphics.Color.BLACK),
            new android.graphics.drawable.ColorDrawable(android.graphics.Color.WHITE), null);
        button.setBackground(ripple);
        button.setBackgroundTintList(null);
        android.graphics.Bitmap ripplePixels = android.graphics.Bitmap.createBitmap(180, 64, android.graphics.Bitmap.Config.ARGB_8888);
        android.graphics.Canvas rippleCanvas = new android.graphics.Canvas(ripplePixels);
        final int[] clicks = {0}, stages = {0}, draws = {0, 0, 0};
        content.addView(button, new android.widget.FrameLayout.LayoutParams(180, 64, android.view.Gravity.TOP | android.view.Gravity.RIGHT));
        button.setOnTouchListener((view, event) -> {
            if (event.getActionMasked() == android.view.MotionEvent.ACTION_DOWN) {
                android.view.VelocityTracker tracker = android.view.VelocityTracker.obtain();
                try { tracker.addMovement(event); }
                finally { tracker.recycle(); }
            }
            return false;
        });
        android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper());
        final WindowCleanup[] cleanup = new WindowCleanup[2];
        Runnable close = () -> { if (!requestWindowClose()) throw new AssertionError("live window close message"); };
        Runnable verifyBack = () -> {
            if (!isWindowMinimized()) throw new AssertionError("Back at the root did not minimize the window");
            stages[0]++;
            cleanup[1] = new WindowCleanup(activity);
            main.postDelayed(() -> {
                cleanup[1].verifyAttached();
                activity.startActivity(new Intent().setClassName(activity.getPackageName(),
                    "com.trebledot.stone.cit.notes.NotesActivity"));
                awaitPresentation(main, () -> resumed[0] != activity && resumed[0].getWindow().getDecorView().isAttachedToWindow()
                    && activity.getWindow().getDecorView().getWindowVisibility() == android.view.View.GONE, () -> {
                    cleanup[1].verifyHidden();
                    if (resumed[0] == activity || !resumed[0].getWindow().getDecorView().isAttachedToWindow())
                        throw new AssertionError("Missing foreground Activity before closing the entire window");
                    close.run();
                });
            }, 150);
        };
        Runnable clickBack = () -> {
            if (isWindowMinimized()) throw new AssertionError("Leaving the back button before releasing activated it");
            if (!clickTitleBack(false)) throw new AssertionError("live title-bar back click");
            main.postDelayed(verifyBack, 150);
        };
        Runnable back = () -> {
            if (!clickTitleBack(true)) throw new AssertionError("live title-bar back cancellation");
            main.postDelayed(clickBack, 150);
        };
        Runnable navigate = () -> {
            String name = "com.trebledot.stone.cit.notes.NotesActivity";
            cleanup[0] = new WindowCleanup(activity);
            main.postDelayed(() -> {
                cleanup[0].verifyAttached();
                activity.startActivity(new Intent().setClassName(activity.getPackageName(), name));
                awaitPresentation(main, () -> resumed[0] != activity && resumed[0].getWindow().getDecorView().isAttachedToWindow()
                    && activity.getWindow().getDecorView().getWindowVisibility() == android.view.View.GONE, () -> {
                    cleanup[0].verifyHidden();
                    android.app.Activity child = resumed[0];
                    if (!child.getClass().getName().equals(name) || !child.getWindow().getDecorView().isAttachedToWindow()
                            || child.getWindow().getDecorView().getWindowVisibility() != android.view.View.VISIBLE)
                        throw new AssertionError("Local Activity was not presented in the Win32 window");
                    child.finish();
                    awaitPresentation(main, () -> resumed[0] == activity
                        && activity.getWindow().getDecorView().getWindowVisibility() == android.view.View.VISIBLE
                        && !child.getWindow().getDecorView().isAttachedToWindow(), () -> {
                        if (resumed[0] != activity || !activity.getWindow().getDecorView().isAttachedToWindow()
                                || child.getWindow().getDecorView().isAttachedToWindow())
                            throw new AssertionError("Finishing local Activity did not restore its caller");
                        cleanup[0].verifyAttached();
                        cleanup[0].dismiss();
                        main.postDelayed(() -> { cleanup[0].verifyDetached(); back.run(); }, 100);
                    });
                });
            }, 100);
        };
        Runnable dialog = () -> {
            android.app.Dialog overlay = new android.app.Dialog(activity);
            android.widget.TextView label = new android.widget.TextView(activity);
            label.setText("Stone software dialog");
            overlay.setContentView(label);
            overlay.show();
            overlay.getWindow().setLayout(android.view.WindowManager.LayoutParams.WRAP_CONTENT,
                android.view.WindowManager.LayoutParams.WRAP_CONTENT);
            final boolean[] drawn = {false};
            overlay.getWindow().getDecorView().getViewTreeObserver().addOnDrawListener(() -> drawn[0] = true);
            main.postDelayed(() -> {
                if (!drawn[0] || !label.isAttachedToWindow() || label.getWidth() == 0)
                    throw new AssertionError("Software dialog was not attached and drawn");
                overlay.dismiss();
                main.postDelayed(() -> {
                    if (label.isAttachedToWindow()) throw new AssertionError("Dismissed dialog remained attached");
                    navigate.run();
                }, 150);
            }, 200);
        };
        Runnable changed = () -> {
            if (clicks[0] != 1) throw new AssertionError("live window did not dispatch its pointer queue");
            button.setText("Live repaint");
            stages[0]++;
            main.postDelayed(() -> android.view.Choreographer.getInstance().postFrameCallback(frameTime -> dialog.run()), 150);
        };
        android.view.Choreographer choreographer = android.view.Choreographer.getInstance();
        android.view.Choreographer.FrameCallback repaint = frameTime -> changed.run();
        android.view.Choreographer.FrameCallback afterClick = frameTime -> choreographer.postFrameCallback(repaint);
        button.setOnClickListener(view -> {
            clicks[0]++;
            button.setText("Live click " + clicks[0]);
        });
        Runnable input = () -> {
            int[] location = new int[2];
            button.getLocationInWindow(location);
            if (!injectTouch(location[0] + button.getWidth() / 2, location[1] + button.getHeight() / 2, 480, 800, 0))
                throw new AssertionError("live window pointer injection");
            stages[0]++;
        };
        Runnable failSafe = () -> { requestWindowClose(); };
        android.view.ViewTreeObserver.OnDrawListener drawing = () -> {
            if ("Live click 1".contentEquals(button.getText())) {
                if (draws[0] == 0) choreographer.postFrameCallback(afterClick);
                draws[0]++;
            }
            if ("Live repaint".contentEquals(button.getText())) draws[1]++;
            if (clicks[0] != 0 && draws[2] == 0) {
                ripplePixels.eraseColor(android.graphics.Color.WHITE);
                ripple.draw(rippleCanvas);
                if (ripplePixels.getPixel(90, 32) != android.graphics.Color.WHITE) draws[2]++;
            }
        };
        final int[] preDraws = {0};
        android.view.ViewTreeObserver.OnPreDrawListener preparing = () -> ++preDraws[0] > 2;
        activity.getWindow().getDecorView().getViewTreeObserver().addOnDrawListener(drawing);
        activity.getWindow().getDecorView().getViewTreeObserver().addOnPreDrawListener(preparing);
        main.postDelayed(input, 100);
        main.postDelayed(failSafe, 10000);
        try {
            com.trebledot.stone.runtime.ArtWindow.run(activity, 480, 800);
            for (WindowCleanup check : cleanup) {
                if (check == null) throw new AssertionError("Missing child window cleanup scenario");
                check.verifyDetached();
            }
            if (stages[0] != 3 || clicks[0] != 1 || draws[0] == 0 || draws[1] == 0 || draws[2] == 0 || preDraws[0] <= 2
                    || activity.getWindow().getDecorView().isAttachedToWindow()
                    || resumed[0].getWindow().getDecorView().isAttachedToWindow())
                throw new AssertionError("live window main loop and cleanup: stages=" + stages[0] + " clicks=" + clicks[0]
                    + " draws=" + java.util.Arrays.toString(draws));
        } finally {
            main.removeCallbacks(input);
            choreographer.removeFrameCallback(afterClick);
            choreographer.removeFrameCallback(repaint);
            main.removeCallbacks(back);
            main.removeCallbacks(clickBack);
            main.removeCallbacks(verifyBack);
            main.removeCallbacks(failSafe);
            activity.getWindow().getDecorView().getViewTreeObserver().removeOnDrawListener(drawing);
            activity.getWindow().getDecorView().getViewTreeObserver().removeOnPreDrawListener(preparing);
            content.removeView(button);
            ripplePixels.recycle();
        }
        System.out.println("ART handled pointer input and title-bar Back in the live Win32 window, and stopped on WM_CLOSE.");
        System.out.println("Activity switching retained hidden roots and popups without input; Back restored them, and WM_CLOSE detached the entire stack.");
    }

    private static void awaitPresentation(android.os.Handler main, java.util.function.BooleanSupplier ready, Runnable check) {
        long deadline = android.os.SystemClock.uptimeMillis()+5000;
        main.post(new Runnable() {
            @Override public void run() {
                if (ready.getAsBoolean()) { check.run(); return; }
                if (android.os.SystemClock.uptimeMillis() >= deadline) throw new AssertionError("Activity window presentation did not complete");
                main.postDelayed(this,16);
            }
        });
    }

    /** Compose PopupLayout also removes its WindowManager view when its owner detaches. */
    private static final class WindowCleanup implements android.view.View.OnAttachStateChangeListener {
        final android.view.View owner;
        final android.view.WindowManager manager;
        final android.widget.TextView popup, orphan;
        int disposed;

        WindowCleanup(android.app.Activity activity) {
            owner = activity.getWindow().getDecorView();
            manager = activity.getWindowManager();
            popup = add(activity, "Owner-disposed popup", 0);
            orphan = add(activity, "Host-disposed orphan", 70);
            owner.addOnAttachStateChangeListener(this);
        }

        private android.widget.TextView add(android.app.Activity activity, String text, int y) {
            android.widget.TextView view = new android.widget.TextView(activity);
            view.setText(text);
            android.view.WindowManager.LayoutParams attributes = new android.view.WindowManager.LayoutParams(
                180, 60, android.view.WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                y == 0 ? 0 : android.view.WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE,
                android.graphics.PixelFormat.TRANSLUCENT);
            attributes.token = owner.getWindowToken();
            attributes.flags |= android.view.WindowManager.LayoutParams.FLAG_SECURE;
            attributes.y = y;
            manager.addView(view, attributes);
            return view;
        }

        void verifyAttached() {
            SecureWindowProbe.requirePolicy(true);
            if (disposed != 0 || !popup.isAttachedToWindow() || !orphan.isAttachedToWindow())
                throw new AssertionError("Popup cleanup fixture was not attached");
            for (android.view.View view : new android.view.View[]{owner, popup, orphan}) {
                if (view.getWindowVisibility() != android.view.View.VISIBLE)
                    throw new AssertionError("Restored window was not visible: " + view);
                android.graphics.Rect visible = new android.graphics.Rect();
                view.getWindowVisibleDisplayFrame(visible);
                if (!visible.equals(new android.graphics.Rect(0, 0, 480, 800)))
                    throw new AssertionError("Software host visible frame: " + visible + " for " + view);
            }
            // Exercise a focused popup even when another test window is foreground.
            try {
                java.lang.reflect.Field field = Class.forName("com.trebledot.stone.runtime.ArtServices").getDeclaredField("windows");
                field.setAccessible(true);
                Object windows = field.get(null);
                java.lang.reflect.Method focus = windows.getClass().getDeclaredMethod("focusChanged", android.view.View.class, boolean.class);
                focus.setAccessible(true);
                focus.invoke(windows, owner, true);
            } catch (ReflectiveOperationException error) { throw new AssertionError("Popup focus setup", error); }
            if (!popup.hasWindowFocus()) throw new AssertionError("Popup did not receive window focus");
        }

        void verifyDetached() {
            SecureWindowProbe.requirePolicy(false);
            if (disposed != 1 || popup.isAttachedToWindow() || orphan.isAttachedToWindow()
                    || popup.getParent() != null || orphan.getParent() != null)
                throw new AssertionError("Owner popup and orphan cleanup: disposed=" + disposed);
        }

        void verifyHidden() {
            SecureWindowProbe.requirePolicy(false);
            if (disposed != 0) throw new AssertionError("Background Activity root was detached");
            for (android.view.View view : new android.view.View[]{owner, popup, orphan})
                if (!view.isAttachedToWindow() || view.getWindowVisibility() != android.view.View.GONE || view.hasWindowFocus())
                    throw new AssertionError("Background window was not retained and hidden: " + view);
            try {
                java.lang.reflect.Field field = Class.forName("com.trebledot.stone.runtime.ArtServices").getDeclaredField("windows");
                field.setAccessible(true); Object windows = field.get(null);
                java.lang.reflect.Method key = windows.getClass().getDeclaredMethod("keyTarget"); key.setAccessible(true);
                if (key.invoke(windows) == popup) throw new AssertionError("Hidden popup captured keyboard input");
                java.lang.reflect.Method touch = windows.getClass().getDeclaredMethod("dispatchTouch",android.view.MotionEvent.class);
                touch.setAccessible(true);
                long now = android.os.SystemClock.uptimeMillis();
                android.view.MotionEvent event = android.view.MotionEvent.obtain(now,now,android.view.MotionEvent.ACTION_DOWN,10,10,0);
                try { if ((Boolean) touch.invoke(windows,event)) throw new AssertionError("Hidden popup captured pointer input"); }
                finally { event.recycle(); }
            } catch (ReflectiveOperationException error) { throw new AssertionError("Hidden window input check",error); }
        }

        void dismiss() {
            owner.removeOnAttachStateChangeListener(this);
            manager.removeViewImmediate(popup);
            manager.removeViewImmediate(orphan);
            disposed++;
        }

        @Override public void onViewAttachedToWindow(android.view.View view) { }
        @Override public void onViewDetachedFromWindow(android.view.View view) {
            owner.removeOnAttachStateChangeListener(this);
            if (popup.hasWindowFocus()) throw new AssertionError("Popup retained focus during owner detachment");
            manager.removeViewImmediate(popup);
            disposed++;
        }
    }

    private static void verifyNativeInput() {
        android.view.MotionEvent.PointerProperties pointer = new android.view.MotionEvent.PointerProperties();
        pointer.id = 7;
        pointer.toolType = android.view.MotionEvent.TOOL_TYPE_MOUSE;
        android.view.MotionEvent.PointerCoords position = new android.view.MotionEvent.PointerCoords();
        position.x = 15.25f;
        position.y = 20.5f;
        position.pressure = 1;
        position.size = 0.5f;
        android.view.MotionEvent event = android.view.MotionEvent.obtain(10, 11, android.view.MotionEvent.ACTION_MOVE, 1,
            new android.view.MotionEvent.PointerProperties[]{pointer}, new android.view.MotionEvent.PointerCoords[]{position},
            0, android.view.MotionEvent.BUTTON_PRIMARY, 1, 1, 42, 0, android.view.InputDevice.SOURCE_MOUSE, 0);
        try {
            event.addBatch(12, 17, 22, 0.75f, 0.25f, android.view.KeyEvent.META_SHIFT_ON);
            event.offsetLocation(3, 4);
            if (!verifyMotion(event, android.os.Build.VERSION.SDK_INT)) throw new AssertionError("ART Java-to-NDK input event conversion");
        } finally { event.recycle(); }
        System.out.println("ART and NDK agree on the image API and MotionEvent metadata, axes, history and nanosecond timestamps.");
    }

    static void verifySharedMemory() throws Exception {
        try (android.os.SharedMemory memory = android.os.SharedMemory.create("stone-probe", 4096)) {
            if (memory.getSize() != 4096) throw new AssertionError("SharedMemory size");
            java.nio.ByteBuffer writer = memory.mapReadWrite();
            java.nio.ByteBuffer reader = memory.mapReadOnly();
            try {
                writer.putInt(0, 0x12345678);
                if (reader.getInt(0) != 0x12345678) throw new AssertionError("SharedMemory coherent mappings");
                if (!memory.setProtect(android.system.OsConstants.PROT_READ))
                    throw new AssertionError("SharedMemory seal writes");
                try {
                    java.nio.ByteBuffer rejected = memory.mapReadWrite();
                    android.os.SharedMemory.unmap(rejected);
                    throw new AssertionError("SharedMemory allowed a new writable mapping after sealing");
                } catch (android.system.ErrnoException expected) { }
            } finally {
                android.os.SharedMemory.unmap(reader);
                android.os.SharedMemory.unmap(writer);
            }
        }
        System.out.println("PASS SharedMemory JNI creation, size, coherent mappings and write protection.");
    }

    private static void verifyText() throws Exception {
        android.graphics.Bitmap bitmap = android.graphics.Bitmap.createBitmap(300, 64, android.graphics.Bitmap.Config.ARGB_8888);
        android.graphics.Canvas canvas = new android.graphics.Canvas(bitmap);
        android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        paint.setColor(0xff18283e);
        paint.setTextSize(28);
        String text = "Stone 中文 📝";
        if (!paint.hasGlyph("中") || !paint.hasGlyph("📝") || paint.measureText(text) < 100)
            throw new AssertionError("image font fallback and shaping");
        canvas.drawText(text, 8, 44, paint);
        int[] pixels = new int[300 * 64];
        bitmap.getPixels(pixels, 0, 300, 0, 0, 300, 64);
        int covered = 0;
        for (int pixel : pixels) if ((pixel >>> 24) != 0) covered++;
        if (covered < 100) throw new AssertionError("image text rasterization");
        try (java.io.FileOutputStream output = new java.io.FileOutputStream("/data/art-fonts.png")) {
            if (!bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, output)) throw new AssertionError("font PNG encoding");
        } finally { canvas.setBitmap(null); bitmap.recycle(); }
        System.out.println("ART rendered Latin, Chinese and emoji with image fonts.");
    }
}
