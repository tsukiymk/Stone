package com.trebledot.stone.cit.webview;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.webkit.CookieManager;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.ValueCallback;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.LinearLayout;
import com.trebledot.stone.cit.CitResult;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.concurrent.atomic.AtomicBoolean;

import static com.trebledot.stone.cit.CitResult.State.*;

public final class WebActivity extends Activity {
    private static final String TAG = "StoneWebViewSmoke";
    private static final String ASSETS = "file:///android_asset/";
    private WebView web;
    private int phase;
    private int starts;
    private int commits;
    private int progress;
    private String title;
    private String origin;
    private final AtomicBoolean postVerified = new AtomicBoolean();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Runnable timeout = () -> complete(FAIL, "Page or JavaScript callback timed out");
    private CitResult status;
    private ServerSocket fixture;

    public static final class PageBridge {
        @JavascriptInterface public String message(String source) {
            return Looper.myLooper() != Looper.getMainLooper() ? "Hello " + source : "Wrong thread";
        }
    }

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        status = new CitResult(this, "webview");
        content.addView(status.view());
        setContentView(content);
        status.set(RUNNING, "Testing HTML, JavaScript and local HTTP navigation");
        main.postDelayed(timeout, 20000);
        verify(() -> startCheck(content));
    }

    private void startCheck(LinearLayout content) {
        CookieManager cookies = CookieManager.getInstance();
        Log.i(TAG, "persistent cookie restored=" +
                "persist=ready".equals(cookies.getCookie("https://persistent-webview.invalid/")));
        cookies.setCookie("https://before-webview.invalid/", "startup=ready; Secure");
        check("startup=ready".equals(cookies.getCookie("https://before-webview.invalid/")), "cookie before WebView creation");
        PackageInfo provider = null;
        if (android.os.Build.VERSION.SDK_INT >= 26) {
            provider = WebView.getCurrentWebViewPackage();
            if (provider != null) {
                Log.i(TAG, "Android WebView provider=" + provider.packageName + " version=" + provider.versionName);
                if (Boolean.getBoolean("stone.windows.host")) checkProviderServices(provider);
            }
        }
        web = new WebView(this);
        if (provider != null) {
            PackageInfo loaded = WebView.getCurrentWebViewPackage();
            check(loaded != null && provider.packageName.equals(loaded.packageName)
                    && provider.versionName.equals(loaded.versionName), "WebView provider changed during initialization");
        }
        content.addView(web, new LinearLayout.LayoutParams(-1, 0, 1));
        web.getSettings().setJavaScriptEnabled(true);
        web.addJavascriptInterface(new PageBridge(), "Android");
        web.setWebChromeClient(new WebChromeClient() {
            @Override public void onReceivedTitle(WebView view, String value) { title = value; }
            @Override public void onProgressChanged(WebView view, int value) { progress = value; }
        });
        web.setWebViewClient(new WebViewClient() {
            @Override public void onPageStarted(WebView view, String url, android.graphics.Bitmap icon) { starts++; }
            @Override public void onPageCommitVisible(WebView view, String url) { commits++; }
            @Override public void onPageFinished(WebView view, String url) {
                verify(() -> {
                    check(Looper.myLooper() == Looper.getMainLooper(), "page callback thread");
                    check(progress == 100, "page progress");
                    if (phase == 0) {
                        phase++;
                        web.evaluateJavascript("document.querySelector('h1').textContent", step(result -> {
                            check("\"Inline page\"".equals(result), "inline HTML: " + result);
                            web.loadUrl(ASSETS + "index.html");
                        }));
                    } else if (phase == 1) {
                        phase++;
                        check((ASSETS + "index.html").equals(url), "asset URL: " + url);
                        check("Windows WebView sample".equals(title), "asset title: " + title);
                        web.evaluateJavascript("document.querySelector('#message').textContent", step(result -> {
                            check("\"Hello APK\"".equals(result), "APK script or Java bridge: " + result);
                            web.loadUrl(ASSETS + "next.html");
                        }));
                    } else if (phase == 2) {
                        phase++;
                        check((ASSETS + "next.html").equals(url) && web.canGoBack(), "history state");
                        web.goBack();
                    } else if (phase == 3) {
                        phase++;
                        check((ASSETS + "index.html").equals(url) && web.canGoForward(), "back navigation: " + url);
                        check(starts > 0 && commits > 0, "navigation callbacks");
                        loadLocalHttp();
                    } else if (phase == 4) {
                        phase++;
                        web.evaluateJavascript("document.querySelector('h1').textContent", step(result -> {
                            check("\"HTTP header received\"".equals(result), "HTTP request: " + result);
                            CookieManager cookies = CookieManager.getInstance();
                            String session = cookies.getCookie(origin + "/");
                            check(session != null && session.contains("web_session=ready"), "HTTP-only server cookie");
                            cookies.setCookie(origin + "/", "android_session=ready; Path=/", step(ok -> {
                                check(ok && Looper.myLooper() == Looper.getMainLooper(), "async cookie callback");
                                web.postUrl(origin + "/login", "sample=posted".getBytes(StandardCharsets.US_ASCII));
                            }));
                        }));
                    } else if (phase == 5) {
                        phase++;
                        web.evaluateJavascript("document.querySelector('h1').textContent", step(result -> {
                            check("\"POST and cookie received\"".equals(result) && postVerified.get(),
                                    "POST navigation or Android cookie: " + result);
                            verifyCookieRemoval();
                        }));
                    }
                });
            }
            @Override public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame()) complete(FAIL, "Navigation failed: " + error.getDescription());
            }
        });
        web.loadDataWithBaseURL(null, "<html><head><title>Inline</title></head><body><h1>Inline page</h1></body></html>",
                "text/html", "UTF-8", null);
    }

    private void checkProviderServices(PackageInfo provider) {
        PackageManager packages = getPackageManager();
        PackageInfo archive = packages.getPackageArchiveInfo(provider.applicationInfo.sourceDir, PackageManager.GET_SERVICES);
        check(archive != null && archive.services != null, "WebView provider service manifest");
        for (ServiceInfo service : archive.services) {
            if (!service.enabled || (service.flags & ServiceInfo.FLAG_ISOLATED_PROCESS) == 0) continue;
            ComponentName component = new ComponentName(provider.packageName, service.name);
            Intent request = new Intent().setComponent(component);
            ResolveInfo resolved = packages.resolveService(request, 0);
            check(resolved != null && resolved.serviceInfo != null, "resolve image service " + component);
            ServiceInfo match = resolved.serviceInfo;
            check(provider.packageName.equals(match.packageName) && service.name.equals(match.name)
                && service.flags == match.flags, "image service identity and flags " + component);
            check(packages.resolveService(request.setPackage(getPackageName()), 0) == null,
                "foreign service outside its intent package");
            Log.i(TAG, "resolved isolated WebView service " + component.flattenToShortString());
            return;
        }
        check(false, "WebView provider isolated service declaration");
    }

    private void loadLocalHttp() {
        try {
            ServerSocket server = new ServerSocket(0, 1, InetAddress.getByName("127.0.0.1"));
            fixture = server;
            origin = "http://127.0.0.1:" + server.getLocalPort();
            Thread thread = new Thread(() -> {
                try (ServerSocket listener = server) {
                    listener.setSoTimeout(8000);
                    for (int attempt = 0; attempt < 10; attempt++) {
                        try (Socket socket = listener.accept()) {
                            socket.setSoTimeout(1000);
                            if (serveRequest(socket)) break;
                        } catch (java.net.SocketTimeoutException timeout) {
                            // Chromium may open a connection speculatively and leave it idle.
                        }
                    }
                } catch (Exception error) {
                    main.post(() -> complete(FAIL, "HTTP fixture: " + error.getMessage()));
                }
            }, "WebView smoke HTTP server");
            thread.setDaemon(true);
            thread.start();
            web.loadUrl(origin + "/", Collections.singletonMap("X-Stone-Smoke", "passed"));
        } catch (Exception error) {
            throw new AssertionError("Could not create HTTP fixture", error);
        }
    }

    private boolean serveRequest(Socket socket) throws java.io.IOException {
        BufferedReader request = new BufferedReader(new InputStreamReader(socket.getInputStream(),
                StandardCharsets.US_ASCII));
        String first = request.readLine();
        if (first == null || first.isEmpty()) return false;
        boolean customHeader = false, androidCookie = false, webCookie = false, form = false;
        int length = 0;
        for (String line = request.readLine(); line != null && !line.isEmpty(); line = request.readLine()) {
            customHeader |= line.equalsIgnoreCase("X-Stone-Smoke: passed");
            String lower = line.toLowerCase(java.util.Locale.ROOT);
            if (lower.startsWith("cookie:")) {
                androidCookie |= line.contains("android_session=ready");
                webCookie |= line.contains("web_session=ready");
            }
            if (lower.startsWith("content-length:")) length = Integer.parseInt(line.substring(15).trim());
            if (lower.startsWith("content-type: application/x-www-form-urlencoded")) form = true;
        }
        boolean post = first.startsWith("POST /login ");
        String message = "Missing header";
        if (post) {
            StringBuilder formBody = new StringBuilder();
            for (int i = 0; i < length; i++) formBody.append((char) request.read());
            postVerified.set(androidCookie && webCookie && form && "sample=posted".contentEquals(formBody));
            message = postVerified.get() ? "POST and cookie received" : "POST or cookie mismatch";
        } else if (customHeader) message = "HTTP header received";
        String body = "<!doctype html><style>html { background: #2168a0; }</style><h1>" + message + "</h1>";
        String response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n"
                + (customHeader ? "Set-Cookie: web_session=ready; Path=/; HttpOnly\r\n" : "")
                + "Content-Length: " + body.length() + "\r\n\r\n" + body;
        socket.getOutputStream().write(response.getBytes(StandardCharsets.US_ASCII));
        socket.getOutputStream().flush();
        return post;
    }

    private void verifyCookieRemoval() {
        CookieManager cookies = CookieManager.getInstance();
        cookies.setCookie(origin + "/private/page", "kept=ready; Max-Age=3600");
        String rootCookie = cookies.getCookie(origin + "/");
        check(rootCookie == null || !rootCookie.contains("kept=ready"), "default cookie path");
        cookies.removeSessionCookies(step(removed -> {
            check(removed, "session removal callback");
            String result = cookies.getCookie(origin + "/private/page");
            check("kept=ready".equals(result), "session-only removal: " + result);
            cookies.setCookie(origin + "/private/page", "kept=gone; Max-Age=0");
            String afterExpiry = cookies.getCookie(origin + "/private/page");
            check(afterExpiry == null || !afterExpiry.contains("kept="), "Max-Age=0 did not delete the target cookie");
            cookies.setCookie(origin + "/", "clear=ready");
            cookies.removeAllCookies(step(cleared -> {
                check(cleared && !cookies.hasCookies(), "removeAllCookies");
                cookies.setCookie("https://persistent-webview.invalid/", "persist=ready; Secure; Max-Age=3600");
                cookies.flush();
                complete(PASS, "HTML, assets, JavaScript, history, HTTP, shared cookies and POST verified");
            }));
        }));
    }

    private static void check(boolean valid, String message) {
        if (!valid) throw new AssertionError(message);
    }

    private <T> ValueCallback<T> step(ValueCallback<T> callback) {
        return value -> verify(() -> callback.onReceiveValue(value));
    }

    private void verify(Runnable action) {
        if (!status.running()) return;
        try {
            action.run();
        } catch (RuntimeException | LinkageError | AssertionError error) {
            main.removeCallbacks(timeout);
            closeFixture();
            status.error(error);
        }
    }

    private void complete(CitResult.State state, String detail) {
        if (!status.running()) return;
        main.removeCallbacks(timeout);
        closeFixture();
        status.set(state, detail);
    }

    private void closeFixture() {
        if (fixture != null) {
            try { fixture.close(); } catch (java.io.IOException ignored) {}
            fixture = null;
        }
    }

    @Override protected void onDestroy() {
        complete(CANCELLED, "Left the WebView test");
        if (web != null && web.getParent() instanceof LinearLayout) {
            ((LinearLayout) web.getParent()).removeView(web);
        }
        if (web != null) web.destroy();
        super.onDestroy();
    }
}
