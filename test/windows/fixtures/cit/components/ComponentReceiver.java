package com.trebledot.stone.cit.components;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Looper;

public class ComponentReceiver extends BroadcastReceiver {
    static final String ORDERED = "com.trebledot.stone.cit.components.ORDERED";
    static int explicit;
    static boolean valid = true;

    @Override public void onReceive(Context context, Intent intent) {
        valid &= Looper.myLooper() == Looper.getMainLooper()
                && context.getApplicationContext() == context;
        if (ORDERED.equals(intent.getAction())) {
            valid &= isOrderedBroadcast() && getResultCode() == 10;
            setResultCode(20);
            setResultData("manifest");
        } else {
            explicit++;
        }
    }

    public static final class Disabled extends ComponentReceiver {
        @Override public void onReceive(Context context, Intent intent) { valid = false; }
    }
}
