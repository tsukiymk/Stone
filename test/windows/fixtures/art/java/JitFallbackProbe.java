package com.trebledot.stone.probe;

/** Kept in a separate DEX which is never passed to dex2oat. */
public final class JitFallbackProbe {
    public static int hot(int input) {
        if (input < 0) throw new IllegalArgumentException("negative JIT input");
        return input * 7;
    }
}
