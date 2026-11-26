package com.trebledot.stone.runtime;

import java.util.Locale;

/** Exercise the actual runtime JAR's strings on the build JDK, without loading Android. */
final class ArtStringTests {
    private static void expect(String expected, String actual) {
        if (!expected.equals(actual)) throw new AssertionError("Expected: " + expected + "; actual: " + actual);
    }
    private static String text(String tags, int key, Object... arguments) {
        String[] names = tags.isEmpty() ? new String[0] : tags.split(",");
        Locale[] locales = new Locale[names.length];
        for (int index = 0; index < names.length; index++) locales[index] = Locale.forLanguageTag(names[index]);
        return ArtStringTable.get(locales,key,arguments);
    }
    public static void main(String[] arguments) {
        expect("Install app",text("en-US",ArtStrings.install_title));
        expect("安装应用",text("zh-CN",ArtStrings.install_title));
        expect("アプリのインストール",text("ja-JP",ArtStrings.install_title));
        expect("安装应用",text("zh-SG",ArtStrings.install_title));
        expect("アプリの言語",text("fr-FR,ja-JP,en-US",ArtStrings.locale_title));
        expect("Application language",text("en-GB,ja-JP",ArtStrings.locale_title));
        expect("Application language",text("de-DE",ArtStrings.locale_title));
        expect("Application language",text("",ArtStrings.locale_title));
        expect("版本 1.0 → 2.0",text("zh-CN",ArtStrings.version,"1.0 → 2.0"));
        expect("Version 2.0",text("en-US",ArtStrings.version,"2.0"));
        expect("バージョン 2.0",text("ja-JP",ArtStrings.version,"2.0"));
        if (!text("en-US",ArtStrings.install_preapproval_message).contains("\n\n"))
            throw new AssertionError("Paragraph escapes were not decoded");
        if (!text("en-US",ArtStrings.storage_message).contains("storage\\emulated\\0"))
            throw new AssertionError("Windows path escapes were not decoded");
        // A later configuration must not reuse text cached for the previous language.
        expect("通知权限",text("zh-CN",ArtStrings.notification_title));
        expect("通知の許可",text("ja-JP",ArtStrings.notification_title));
        expect("Notification permission",text("en-US",ArtStrings.notification_title));
        for (String[] translations : ArtStrings.values) {
            if (translations.length != ArtStrings.values[0].length) throw new AssertionError("Resource table length mismatch");
            for (String value : translations) if (value == null || value.isEmpty()) throw new AssertionError("Incomplete shipped translation");
        }
        String original = ArtStrings.values[1][ArtStrings.locale_title];
        try {
            ArtStrings.values[1][ArtStrings.locale_title] = null;
            expect("Application language",text(ArtStrings.tags[1],ArtStrings.locale_title));
        } finally { ArtStrings.values[1][ArtStrings.locale_title] = original; }
        System.out.println("ART runtime string behavior checks passed");
    }
}
