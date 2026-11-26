package com.trebledot.stone.runtime;

import android.content.Context;
import android.os.LocaleList;
import java.util.Locale;

/** Resolve runtime-owned UI text without touching the application's resource IDs. */
final class ArtText {
    private ArtText() { }

    static String get(Context context, int key, Object... arguments) {
        return get(context.getResources().getConfiguration().getLocales(), key, arguments);
    }

    static String get(int key, Object... arguments) {
        return get(LocaleList.getDefault(), key, arguments);
    }

    private static String get(LocaleList preferences, int key, Object... arguments) {
        Locale[] locales = new Locale[preferences.size()];
        for (int index = 0; index < locales.length; index++) locales[index] = preferences.get(index);
        return ArtStringTable.get(locales, key, arguments);
    }
}
