package com.trebledot.stone.runtime;

import android.os.Handler;
import android.os.IBinder;
import android.os.IInterface;
import android.os.Looper;
import android.os.PowerManager;
import android.util.Log;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;

/** Android's no-thermal-HAL state; this is not a Windows temperature measurement. */
final class ArtThermal {
    private final Map<IBinder,IInterface> listeners=new HashMap<>();
    private final Handler handler=new Handler(Looper.getMainLooper());

    synchronized Object call(Object instance,Method method,Object[] arguments) throws ReflectiveOperationException {
        switch(method.getName()) {
            case "getCurrentThermalStatus": return PowerManager.THERMAL_STATUS_NONE;
            case "getCurrentThermalStatusForDevice":
                device((Integer)arguments[0]); return PowerManager.THERMAL_STATUS_NONE;
            case "getThermalHeadroom": return Float.NaN;
            case "registerThermalStatusListenerForDevice": device((Integer)arguments[0]); // fall through
            case "registerThermalStatusListener": {
                IInterface listener=(IInterface)arguments[arguments.length-1];
                if(listener==null || listeners.containsKey(listener.asBinder())) return false;
                if(listeners.size()>=128) return false;
                listeners.put(listener.asBinder(),listener);
                if(!handler.post(() -> initial(listener))) { listeners.remove(listener.asBinder()); return false; }
                return true;
            }
            case "unregisterThermalStatusListener":
                return listeners.remove(((IInterface)arguments[0]).asBinder())!=null;
            default: throw new UnsupportedOperationException("Thermal HAL telemetry is unavailable: "+method.getName());
        }
    }

    private synchronized void initial(IInterface listener) {
        if(listeners.get(listener.asBinder())!=listener) return;
        try {
            Class.forName("android.os.IThermalStatusListener").getMethod("onStatusChange",int.class)
                .invoke(listener,PowerManager.THERMAL_STATUS_NONE);
        } catch(ReflectiveOperationException error) { Log.w("StoneThermal","Thermal status callback failed",error); }
    }

    private static void device(int id) {
        if(id!=android.content.Context.DEVICE_ID_DEFAULT) throw new SecurityException("Unknown thermal device");
    }
}
