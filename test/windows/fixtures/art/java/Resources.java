package com.trebledot.stone.probe;

import android.content.res.XmlResourceParser;
import android.content.res.AssetManager;
import android.content.res.loader.ResourcesLoader;
import android.content.res.loader.ResourcesProvider;
import android.os.ParcelFileDescriptor;
import java.io.File;
import org.xmlpull.v1.XmlPullParser;

public final class Resources {
    private static native boolean verifyAssets(AssetManager assets, byte[] expected, int density, int width, int height);
    public static void verifyNativeAssets(android.content.res.Resources resources) throws Exception {
        AssetManager assets = resources.getAssets();
        java.io.ByteArrayOutputStream data = new java.io.ByteArrayOutputStream();
        try (java.io.InputStream stream = assets.open("native-check.txt")) {
            byte[] buffer = new byte[128];
            for (int count; (count = stream.read(buffer)) != -1;) data.write(buffer, 0, count);
        }
        android.content.res.Configuration config = resources.getConfiguration();
        if (!verifyAssets(assets, data.toByteArray(), config.densityDpi, config.screenWidthDp, config.screenHeightDp))
            throw new AssertionError("image NDK assets, directory, seek, buffer, descriptor and configuration");
    }
    public static int run(AssetManager assets) throws Exception {
        android.content.res.Resources system = android.content.res.Resources.getSystem();
        if (!"OK".equals(system.getString(android.R.string.ok))) throw new AssertionError("framework string resource");
        if (system.getIdentifier("ok", "string", "android") != android.R.string.ok)
            throw new AssertionError("framework identifier lookup");
        XmlResourceParser parser = system.getXml(android.R.layout.simple_list_item_1);
        try {
            int event;
            do { event = parser.next(); } while (event != XmlPullParser.START_TAG && event != XmlPullParser.END_DOCUMENT);
            if (event != XmlPullParser.START_TAG || !"TextView".equals(parser.getName()))
                throw new AssertionError("framework binary XML");
        } finally { parser.close(); }
        ResourcesProvider provider;
        try (ParcelFileDescriptor file = ParcelFileDescriptor.open(new File("/data/application.apk"), ParcelFileDescriptor.MODE_READ_ONLY)) {
            provider = ResourcesProvider.loadFromApk(file);
        }
        ResourcesLoader loader = new ResourcesLoader();
        loader.addProvider(provider);
        android.content.res.Resources application = new android.content.res.Resources(assets,
            system.getDisplayMetrics(), system.getConfiguration());
        application.addLoaders(loader);
        try {
            verifyNativeAssets(application);
            int title = application.getIdentifier("app_name", "string", "com.trebledot.stone.cit");
            int unicode = application.getIdentifier("example", "string", "com.trebledot.stone.cit");
            if (title == 0 || !"Stone CIT".equals(application.getString(title))) throw new AssertionError("APK string resource");
            if (unicode == 0 || !"Café • 离线笔记 📝".equals(application.getString(unicode)))
                throw new AssertionError("APK Unicode resource");
            parser = assets.openXmlResourceParser("AndroidManifest.xml");
            try {
                int event;
                do { event = parser.next(); } while (event != XmlPullParser.START_TAG && event != XmlPullParser.END_DOCUMENT);
                if (event != XmlPullParser.START_TAG || !"manifest".equals(parser.getName())
                        || !"com.trebledot.stone.cit".equals(parser.getAttributeValue(null, "package")))
                    throw new AssertionError("APK manifest binary XML");
            } finally { parser.close(); }
        } finally {
            application.removeLoaders(loader);
            loader.clearProviders();
            provider.close();
            assets.close();
        }
        return 42;
    }
}
