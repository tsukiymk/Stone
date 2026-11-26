package com.trebledot.stone.runtime;

import java.lang.reflect.Method;
import java.util.Collections;

/** A discoverable LocationManager with no location providers on this host. */
final class ArtLocation {
    private ArtLocation() {}

    static Object call(Object instance, Method method, Object[] arguments) {
        switch (method.getName()) {
            case "getAllProviders":
            case "getProviders": return Collections.emptyList();
            case "getBestProvider": return null;
            case "hasProvider":
            case "isProviderEnabledForUser":
            case "isLocationEnabledForUser": return false;
            case "getProviderProperties":
                throw new IllegalArgumentException("Unknown location provider: " + arguments[0]);
            default: throw new UnsupportedOperationException("ART location service: " + method.getName());
        }
    }
}
