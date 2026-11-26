package com.trebledot.stone.runtime;

/** Process initialization normally performed by Android's RuntimeInit. */
final class ArtRuntimeInit {
    private static boolean initialized;

    private ArtRuntimeInit() {}

    static synchronized void initialize() throws ReflectiveOperationException {
        if (initialized) return;
        // JNI_CreateJavaVM does not run RuntimeInit.commonInit. Use the image's
        // handlers so applications can wrap the Android crash handler, while
        // uncaught exceptions still get logged and terminate this Client.
        Class<?> loggingType = Class.forName("com.android.internal.os.RuntimeInit$LoggingHandler");
        java.lang.reflect.Constructor<?> loggingConstructor = loggingType.getDeclaredConstructor();
        loggingConstructor.setAccessible(true);
        Thread.UncaughtExceptionHandler logging = (Thread.UncaughtExceptionHandler) loggingConstructor.newInstance();
        Class<?> killType = Class.forName("com.android.internal.os.RuntimeInit$KillApplicationHandler");
        java.lang.reflect.Constructor<?> killConstructor = killType.getDeclaredConstructor(loggingType);
        killConstructor.setAccessible(true);
        Thread.UncaughtExceptionHandler handler = (Thread.UncaughtExceptionHandler) killConstructor.newInstance(logging);
        java.lang.reflect.Method preHandler = Class.forName("dalvik.system.RuntimeHooks")
            .getDeclaredMethod("setUncaughtExceptionPreHandler", Thread.UncaughtExceptionHandler.class);
        preHandler.setAccessible(true);
        preHandler.invoke(null, logging);
        Thread.setDefaultUncaughtExceptionHandler(handler);
        initialized = true;
    }
}
