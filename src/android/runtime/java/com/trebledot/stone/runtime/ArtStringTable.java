package com.trebledot.stone.runtime;

import java.util.Locale;

/** String-only language/region selection for the runtime's generated resource table. */
final class ArtStringTable {
    private ArtStringTable() { }

    static String get(Locale[] locales, int key, Object... arguments) {
        int selected = 0;
        Locale formatting = locales.length == 0 ? Locale.ENGLISH : locales[0];
        for (Locale locale : locales) {
            int match = -1;
            for (int index = 0; index < ArtStrings.tags.length; index++) {
                Locale candidate = Locale.forLanguageTag(ArtStrings.tags[index]);
                if (!candidate.getLanguage().equals(locale.getLanguage())) continue;
                // An exact region wins; otherwise use that language's available
                // translation (including the existing Simplified Chinese UI).
                if (match == -1 || candidate.getCountry().isEmpty()) match = index;
                if (candidate.getCountry().equals(locale.getCountry())) { match = index; break; }
            }
            if (match >= 0) { selected = match; formatting = locale; break; }
        }
        String value = ArtStrings.values[selected][key];
        if (value == null) value = ArtStrings.values[0][key];
        return arguments.length == 0 ? value : String.format(formatting, value, arguments);
    }
}
