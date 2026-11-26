package com.trebledot.stone.runtime;

import android.content.ContentResolver;
import android.net.Uri;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/** Delivers content change notifications within the host's Android user. */
final class ArtContentObservers {
    private final int user;
    private final Method onChange;
    private final List<Registration> registrations = new ArrayList<>();

    private static final class Registration {
        final Object observer;
        final Uri uri;
        final boolean descendants;

        Registration(Object observer, Uri uri, boolean descendants) {
            this.observer = observer;
            this.uri = uri;
            this.descendants = descendants;
        }

        boolean matches(Uri change, int flags) {
            if (!uri.getAuthority().equals(change.getAuthority())) return false;
            List<String> watched = uri.getPathSegments();
            List<String> changed = change.getPathSegments();
            int shared = Math.min(watched.size(), changed.size());
            if (!watched.subList(0, shared).equals(changed.subList(0, shared))) return false;
            if (watched.size() < changed.size()) return descendants;
            return !descendants || (flags & ContentResolver.NOTIFY_SKIP_NOTIFY_FOR_DESCENDANTS) == 0;
        }
    }

    ArtContentObservers(int user) throws ReflectiveOperationException {
        this.user = user;
        onChange = Class.forName("android.database.IContentObserver").getMethod("onChangeEtc",
            boolean.class, Uri[].class, int.class, int.class);
    }

    Object call(Object instance, Method method, Object[] arguments) throws ReflectiveOperationException {
        switch (method.getName()) {
            case "registerContentObserver":
                requireLocalUser((Integer) arguments[3]);
                Uri uri = (Uri) arguments[0];
                if (!"content".equals(uri.getScheme()) || uri.getAuthority() == null)
                    throw new IllegalArgumentException("Invalid content observer URI: " + uri);
                synchronized (registrations) {
                    registrations.add(new Registration(arguments[2], uri, (Boolean) arguments[1]));
                }
                return null;
            case "unregisterContentObserver":
                synchronized (registrations) {
                    registrations.removeIf(registration -> registration.observer == arguments[0]);
                }
                return null;
            case "notifyChange":
                requireLocalUser((Integer) arguments[4]);
                notifyChange((Uri[]) arguments[0], arguments[1], (Boolean) arguments[2], (Integer) arguments[3]);
                return null;
            default: throw new UnsupportedOperationException("ART content service: " + method.getName());
        }
    }

    private void requireLocalUser(int requested) {
        if (requested != user) throw new SecurityException("Content observers require the local Android user");
    }

    private void notifyChange(Uri[] uris, Object sender, boolean deliverSelf, int flags) throws ReflectiveOperationException {
        List<Registration> snapshot;
        synchronized (registrations) { snapshot = new ArrayList<>(registrations); }
        for (Registration registration : snapshot) {
            boolean self = registration.observer == sender;
            if (self && !deliverSelf) continue;
            List<Uri> changes = new ArrayList<>();
            for (Uri uri : uris) if (registration.matches(uri, flags)) changes.add(uri);
            if (!changes.isEmpty()) onChange.invoke(registration.observer, self, changes.toArray(new Uri[0]), flags, user);
        }
    }
}
