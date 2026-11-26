package com.trebledot.stone.cit.components;

import com.trebledot.stone.cit.R;
import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.ContentProviderClient;
import android.content.ContentProviderOperation;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.ServiceConnection;
import android.database.ContentObserver;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.OperationCanceledException;
import android.util.Log;
import android.widget.TextView;
import java.util.ArrayList;

public final class ComponentActivity extends Activity {
    private static final String TAG = "WindowsComponents";
    private Handler main;
    private Intent service;
    private int observed;
    private int connected;
    private IBinder firstBinder;
    private int ordinary;
    private boolean abortedReceiverRan;

    static void check(boolean value, String operation) {
        if (!value) throw new AssertionError(operation);
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        main = new Handler(Looper.getMainLooper());
        TextView view = new TextView(this);
        view.setText(getString(R.string.components_title));
        setContentView(view);
        testProvider();
        testService();
        testBroadcast();
    }

    private void testProvider() {
        check(ComponentApplication.providerReady, "provider should be ready before Application.onCreate");
        check(ComponentProvider.creates == 1, "disabled provider initialized");
        testResolver();
        Log.i(TAG, "Provider startup, aliases, CRUD, batch, refresh and observers verified");
    }

    private void testResolver() {
        android.content.ContentResolver resolver = getContentResolver();
        ContentObserver listener = new ContentObserver(null) {
            @Override public void onChange(boolean self, Uri uri) { observed++; }
        };
        resolver.registerContentObserver(ComponentProvider.ROWS, true, listener);
        resolver.notifyChange(Uri.parse("content://" + ComponentProvider.PRIMARY + "/rowsOther"), null);
        check(observed == 0, "observer received a sibling path");
        try (ContentProviderClient client = resolver.acquireContentProviderClient(ComponentProvider.ALIAS)) {
            check(client != null, "alias lookup");
            check(ComponentProvider.ALIAS.equals(client.call("authority", null, null).getString("authority")),
                    "client call authority");
            check(ComponentProvider.PRIMARY.equals(resolver.call(ComponentProvider.ROWS, "authority", null, null)
                    .getString("authority")), "resolver call authority");
            check(resolver.getType(Uri.parse("https://" + ComponentProvider.PRIMARY + "/rows")) == null,
                    "non-content URI routed to provider");
            ContentValues values = new ContentValues();
            values.put("value", "first");
            check(client.insert(ComponentProvider.ALIAS_ROWS, values) != null, "insert");
            values.put("value", "batch");
            ArrayList<ContentProviderOperation> batch = new ArrayList<>();
            batch.add(ContentProviderOperation.newUpdate(ComponentProvider.ALIAS_ROWS).withValues(values)
                    .withExpectedCount(1).build());
            check(client.applyBatch(batch).length == 1, "batch");
            batch.clear();
            batch.add(ContentProviderOperation.newUpdate(Uri.parse("content://other.provider/rows"))
                    .withValues(values).build());
            try {
                client.applyBatch(batch);
                throw new AssertionError("foreign batch operation reached local provider");
            } catch (IllegalArgumentException | SecurityException expected) {}
            try (Cursor cursor = resolver.query(ComponentProvider.ROWS, null, null, null, null)) {
                check(cursor != null && cursor.moveToFirst() && "batch".equals(cursor.getString(0)), "alias query");
            }
            Bundle extras = new Bundle();
            extras.putBoolean("refresh", true);
            check(resolver.refresh(ComponentProvider.ROWS, extras, null), "refresh");
            CancellationSignal cancelled = new CancellationSignal();
            cancelled.cancel();
            try {
                client.refresh(ComponentProvider.ALIAS_ROWS, extras, cancelled);
                throw new AssertionError("cancelled refresh was delivered");
            } catch (OperationCanceledException expected) {}
            check(resolver.delete(ComponentProvider.ROWS, null, null) == 1, "delete");
            check(observed == 2, "observer descendant notifications");
        } catch (Exception e) {
            throw new AssertionError("provider test failed", e);
        } finally {
            resolver.unregisterContentObserver(listener);
        }
        resolver.notifyChange(ComponentProvider.ROWS, null);
        check(observed == 2, "unregistered content observer called");
    }

    private void testService() {
        service = new Intent(this, ComponentService.class);
        check(startService(new Intent(this, ComponentService.Disabled.class)) == null, "disabled service");
        check(!bindService(service, new Connection(), 0), "bind without auto-create");
        check(startService(service) != null && startService(service) != null, "service resolution");
        check(ComponentService.creates == 0, "service start ran inline");
        Connection first = new Connection();
        Connection second = new Connection();
        first.action = binder -> {
            firstBinder = binder;
            connected++;
        };
        second.action = binder -> {
            check(connected == 1 && binder == firstBinder && ComponentService.binds == 1, "shared binder");
            ComponentService instance = ((ComponentService.LocalBinder) binder).service();
            check(ComponentService.starts == 2 && ComponentService.firstStart != ComponentService.latestStart,
                    "start delivery");
            check(!instance.stopSelfResult(ComponentService.firstStart), "obsolete start ID stopped service");
            unbindService(first);
            unbindService(second);
            main.post(() -> {
                check(ComponentService.unbinds == 1 && ComponentService.destroys == 0, "started service ownership");
                Connection again = new Connection();
                again.action = rebound -> {
                    check(rebound == binder && ComponentService.rebinds == 1, "onRebind");
                    check(instance.stopSelfResult(ComponentService.latestStart), "latest start ID did not stop service");
                    check(ComponentService.destroys == 0, "stopSelf destroyed bound service");
                    unbindService(again);
                    main.post(() -> main.post(() -> {
                        check(ComponentService.unbinds == 2 && ComponentService.destroys == 1, "last unbind teardown");
                        testNullBinding();
                    }));
                };
                check(bindService(service, again, BIND_AUTO_CREATE), "rebind request");
            });
        };
        check(bindService(service, first, Context.BindServiceFlags.of(BIND_AUTO_CREATE))
                        && bindService(service, second, BIND_AUTO_CREATE),
                "two bindings");
    }

    private void testNullBinding() {
        Intent request = new Intent(service).setAction("nullBinding").putExtra("null", true);
        Connection cancelled = new Connection();
        check(bindService(request, cancelled, BIND_AUTO_CREATE), "queued bind");
        unbindService(cancelled);
        Connection connection = new Connection() {
            @Override public void onNullBinding(ComponentName name) {
                check(ComponentService.class.getName().equals(name.getClassName()), "null binding component");
                unbindService(this);
                main.post(() -> main.post(() -> {
                    check(ComponentService.creates == 2 && ComponentService.binds == 2
                            && ComponentService.unbinds == 3 && ComponentService.destroys == 2
                            && ComponentService.allOnMain, "bind-only service lifecycle");
                    check(startService(service) != null && stopService(service), "queued start then stop");
                    main.post(() -> main.post(() -> {
                        check(ComponentService.starts == 3 && ComponentService.destroys == 3
                                && !stopService(service), "explicit stop teardown");
                        Log.i(TAG, "Service start, bind, rebind, stop and destroy verified");
                    }));
                }));
            }
        };
        check(bindService(request, connection, BIND_AUTO_CREATE), "null binder accepted");
    }

    private static class Connection implements ServiceConnection {
        java.util.function.Consumer<IBinder> action;
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            check(action != null, "unexpected service connection");
            action.accept(binder);
        }
        @Override public void onServiceDisconnected(ComponentName name) {
            throw new AssertionError("unexpected disconnection");
        }
        @Override public void onNullBinding(ComponentName name) {
            throw new AssertionError("unexpected null binding");
        }
    }

    private void testBroadcast() {
        String ordinaryAction = getPackageName() + ".ORDINARY";
        BroadcastReceiver dynamic = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                check(context == ComponentActivity.this && Looper.myLooper() == Looper.getMainLooper(),
                        "registered receiver context and thread");
                check(intent.getIntExtra("value", 0) == 7, "broadcast Intent was changed after send");
                ordinary++;
            }
        };
        BroadcastReceiver removed = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) { ordinary += 100; }
        };
        registerReceiver(dynamic, new IntentFilter(ordinaryAction));
        registerReceiver(removed, new IntentFilter(ordinaryAction));
        BroadcastReceiver guarded = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) { ordinary += 100; }
        };
        String missingPermission = getPackageName() + ".permission.MISSING";
        registerReceiver(guarded, new IntentFilter(ordinaryAction), missingPermission, main);
        Intent message = new Intent(ordinaryAction).putExtra("value", 7);
        sendBroadcast(message);
        message.putExtra("value", 99);
        unregisterReceiver(removed);
        sendBroadcast(new Intent(ordinaryAction), missingPermission);
        sendBroadcast(new Intent(ordinaryAction).setPackage("other.package"));
        sendBroadcast(new Intent(this, ComponentReceiver.class));
        sendBroadcast(new Intent(this, ComponentReceiver.Disabled.class));
        IntentFilter ordered = new IntentFilter(ComponentReceiver.ORDERED);
        ordered.setPriority(50);
        BroadcastReceiver asynchronous = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                check(isOrderedBroadcast() && getResultCode() == 20 && "manifest".equals(getResultData()),
                        "ordered broadcast did not carry prior result");
                PendingResult pending = goAsync();
                main.post(() -> {
                    pending.setResultCode(30);
                    pending.getResultExtras(true).putString("async", "finished");
                    pending.abortBroadcast();
                    pending.finish();
                });
            }
        };
        BroadcastReceiver skipped = new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) { abortedReceiverRan = true; }
        };
        registerReceiver(asynchronous, ordered);
        registerReceiver(skipped, new IntentFilter(ComponentReceiver.ORDERED));
        sendOrderedBroadcast(new Intent(ComponentReceiver.ORDERED).setPackage(getPackageName()), null,
                new BroadcastReceiver() {
                    @Override public void onReceive(Context context, Intent intent) {
                        check(ComponentReceiver.valid && ComponentReceiver.explicit == 1 && ordinary == 1,
                                "manifest or dynamic broadcast dispatch");
                        check(getResultCode() == 30 && "finished".equals(getResultExtras(false).getString("async"))
                                && !abortedReceiverRan, "ordered broadcast completion or abort");
                        unregisterReceiver(dynamic);
                        unregisterReceiver(guarded);
                        unregisterReceiver(asynchronous);
                        unregisterReceiver(skipped);
                        Log.i(TAG, "Manifest, dynamic and ordered broadcasts verified");
                    }
                }, main, 10, null, null);
    }
}
