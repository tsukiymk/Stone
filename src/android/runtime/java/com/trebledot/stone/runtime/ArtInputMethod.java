package com.trebledot.stone.runtime;

import android.os.IInterface;
import android.os.IBinder;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.Map;

/** Local editor client registry; no Android IME engine is exposed. */
final class ArtInputMethod {
    private final Map<IBinder, Object[]> clients = new HashMap<>();
    private final int user;
    ArtInputMethod(int user) { this.user=user; }

    /** One contiguous Win32 text burst; each character retains its edit callback. */
    static final class TextSession implements AutoCloseable {
        private View editor;
        private InputConnection connection;

        boolean commitText(View target, CharSequence text) {
            if (target != editor || connection == null) {
                close();
                editor = target;
                if (target != null) connection = target.onCreateInputConnection(new EditorInfo());
            }
            if (connection == null) return false;
            boolean accepted = connection.commitText(text, 1);
            if (!accepted) close();
            return accepted;
        }

        @Override public void close() {
            InputConnection previous = connection;
            connection = null; editor = null;
            if (previous != null) previous.closeConnection();
        }
    }

    Object call(Object instance, Method method, Object[] arguments) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "getInputMethodList":
            case "getEnabledInputMethodList":
                if((Integer)arguments[0]!=user) throw new SecurityException("Input methods are restricted to the current user");
                return method.getReturnType()==java.util.List.class?java.util.Collections.emptyList():
                    method.getReturnType().getMethod("create",java.util.List.class).invoke(null,java.util.Collections.emptyList());
            case "getCurrentInputMethodInfoAsUser":
                if((Integer)arguments[0]!=user) throw new SecurityException("Input methods are restricted to the current user");
                return null;
            case "addClient":
                clients.put(((IInterface) arguments[0]).asBinder(), arguments.clone());
                return null;
            case "startInputOrWindowGainedFocus":
                Object result=Class.forName("com.android.internal.inputmethod.InputBindResult").getField("NO_IME").get(null);
                if(method.getReturnType()==void.class) {
                    IInterface client=(IInterface)arguments[1];
                    if(!clients.containsKey(client.asBinder())) throw new SecurityException("Unknown input method client");
                    Class.forName("com.android.internal.inputmethod.IInputMethodClient")
                        .getMethod("onStartInputResult",result.getClass(),int.class)
                        .invoke(client,result,(Integer)arguments[arguments.length-1]);
                    return null;
                }
                return result;
            case "getImeTrackerService": return null;
            case "showSoftInput":
            case "hideSoftInput": return false;
            default: throw new UnsupportedOperationException("ART input method service: " + method.getName());
        }
    }
}
