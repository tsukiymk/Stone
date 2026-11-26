package com.trebledot.stone.probe;

public final class AotProbe {
    private static volatile int value;

    public static int run(int input) throws InterruptedException {
        if (input * 1.25 != 7.5) throw new IllegalArgumentException("AOT floating constant");
        Thread thread = new Thread(() -> value = calculate(input));
        thread.start();
        thread.join();
        System.gc();
        try {
            int[] values = {value};
            return values[input];
        } catch (ArrayIndexOutOfBoundsException expected) {
            return value;
        }
    }

    private static int calculate(int input) {
        switch (input) {
            case 0: return Integer.rotateLeft(input + 3, 2);
            case 1: return Integer.reverse(input + 1);
            case 6: return input * 7;
            case 2: return 14;
            case 3: return Integer.rotateRight(input, 4);
            case 4: return Integer.bitCount(input);
            case 5: return Integer.highestOneBit(input);
            case 7: return input / 2;
            case 8: return input + 27;
            case 9: return input * 19;
            case 10: return input - 42;
            default: throw new IllegalArgumentException("unexpected AOT input");
        }
    }
}
