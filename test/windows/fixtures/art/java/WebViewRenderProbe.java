package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Method;

/** Real provider creation, local HTML, JavaScript and image-owned software drawing. */
public final class WebViewRenderProbe {
    private static RenderActivity activity;
    private static boolean complete;
    private static java.net.ServerSocket server;
    private static volatile boolean requested;
    private static volatile Throwable serverError;
    private static final String PAGE="<!doctype html><html><head><title>Stone WebView Rendered</title><style>html,body{margin:0;background:rgb(30,150,90);height:100%}</style></head><body><h1>Android WebView</h1><div id='proof'>0</div><script>document.getElementById('proof').textContent=6*7</script></body></html>";
    public static final class RenderActivity extends Activity {
        WebView web;
        @Override public void onCreate(Bundle state) {
            super.onCreate(state); activity=this;
            System.out.println("WebView render: constructing original provider View");
            web=new WebView(this);
            if(web.getSettings().getBlockNetworkLoads()) throw new AssertionError("Declared INTERNET permission blocked WebView network loads");
            System.out.println("WebView render: original provider View constructed");
            web.getSettings().setJavaScriptEnabled(true);
            setContentView(web);
            web.setWebViewClient(new WebViewClient() {
                @Override public void onPageFinished(WebView view,String url) {
                    System.out.println("WebView render: onPageFinished "+url);
                    view.evaluateJavascript("document.title + ':' + document.getElementById('proof').textContent",value -> {
                        if(!"\"Stone WebView Rendered:42\"".equals(value)) throw new AssertionError("JavaScript result: "+value);
                        new Handler().postDelayed(() -> capture(),500);
                    });
                }
            });
            System.out.println("WebView render: loading original provider local HTML");
            web.loadUrl("http://127.0.0.1:"+server.getLocalPort()+"/");
            new Handler().postDelayed(() -> { if(!complete) throw new AssertionError("WebView render callback timeout"); },20000);
        }
        private void capture() {
            android.view.View decor=getWindow().getDecorView();
            android.view.WindowManager.LayoutParams params=new android.view.WindowManager.LayoutParams();
            params.copyFrom(getWindow().getAttributes());
            getWindowManager().updateViewLayout(decor,params);
            if(!requested || serverError!=null) throw new AssertionError("WebView did not complete the actual HTTP exchange",serverError);
            Bitmap bitmap=Bitmap.createBitmap(web.getWidth(),web.getHeight(),Bitmap.Config.ARGB_8888);
            try {
                bitmap.eraseColor(Color.MAGENTA); web.draw(new Canvas(bitmap));
                int pixel=bitmap.getPixel(web.getWidth()-10,web.getHeight()-10);
                if(pixel!=Color.rgb(30,150,90)) throw new AssertionError("WebView software background pixel: "+Integer.toHexString(pixel));
                File output=new File("/data/webview-render");
                if(!output.isDirectory() && !output.mkdirs()) throw new AssertionError("WebView render output directory");
                try(FileOutputStream file=new FileOutputStream(new File(output,"frame.png"))) {
                    if(!bitmap.compress(Bitmap.CompressFormat.PNG,100,file)) throw new AssertionError("WebView frame encoding");
                }
                complete=true;
                System.out.println("PASS original WebView loopback HTTP, JavaScript=42, onPageFinished, window layout update and software pixels");
                finish();
            } catch(java.io.IOException error) { throw new AssertionError(error); }
            finally { bitmap.recycle(); }
        }
        @Override protected void onDestroy() {
            if(web!=null) {
                if(web.getParent() instanceof android.view.ViewGroup) ((android.view.ViewGroup)web.getParent()).removeView(web);
                web.destroy();
            }
            super.onDestroy();
        }
    }
    static int run(ArtApplication app) throws Exception {
        server=new java.net.ServerSocket(0,8,java.net.InetAddress.getByAddress(new byte[]{127,0,0,1}));
        Thread responder=new Thread(() -> {
            try {
                while(!server.isClosed()) try(java.net.Socket client=server.accept()) {
                    client.setSoTimeout(3000);
                    java.io.BufferedReader input=new java.io.BufferedReader(new java.io.InputStreamReader(client.getInputStream(),java.nio.charset.StandardCharsets.US_ASCII));
                    String first=input.readLine();
                    if(first==null || !first.startsWith("GET /")) throw new java.io.IOException("Invalid loopback HTTP request");
                    int headers=0; String line;
                    while((line=input.readLine())!=null && !line.isEmpty()) if(++headers>64) throw new java.io.IOException("Too many HTTP headers");
                    byte[] body=PAGE.getBytes(java.nio.charset.StandardCharsets.UTF_8);
                    java.io.OutputStream output=client.getOutputStream();
                    output.write(("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "+body.length+"\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n").getBytes(java.nio.charset.StandardCharsets.US_ASCII));
                    output.write(body); output.flush(); requested=true;
                }
            } catch(java.io.IOException error) { if(!server.isClosed()) serverError=error; }
        },"WebView loopback HTTP");
        responder.setDaemon(true); responder.start();
        ActivityInfo info=new ActivityInfo(app.apk.activityInfo); info.name=RenderActivity.class.getName(); info.targetActivity=null;
        Method launch=ArtApplication.class.getDeclaredMethod("launch",ActivityInfo.class,Intent.class,android.os.IBinder.class,String.class,int.class);
        launch.setAccessible(true);
        launch.invoke(app,info,new Intent(Intent.ACTION_MAIN).setClassName(app.context.getPackageName(),info.name),null,null,-1);
        try { ArtWindow.run(activity,360,640); if(!complete) throw new AssertionError("WebView rendering did not complete"); }
        finally { app.destroyActivity(); server.close(); responder.join(3000); }
        return 42;
    }
}
