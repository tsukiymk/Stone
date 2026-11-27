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
    private static final String PAGE="<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>Stone WebView Rendered</title><style>html,body{margin:0;background:rgb(30,150,90);min-height:2400px}input{display:block;width:200px;height:48px}button{display:block;width:120px;height:60px;margin:12px}</style></head><body><input id='editor'><button id='click' onclick='this.textContent=Number(this.textContent)+1'>0</button><div id='proof'>0</div><script>document.getElementById('proof').textContent=6*7;window.ticks=0;requestAnimationFrame(function step(){ticks++;document.getElementById('proof').style.transform='translateX('+(ticks%2)+'px)';requestAnimationFrame(step)})</script></body></html>";
    public static final class RenderActivity extends Activity {
        WebView web;
        int loads, captures, lastAnimationTick;
        long repeatedDrawingStarted;
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
                        if(++loads==1) new Handler().postDelayed(() -> focusEditor(),500);
                        else if(loads==2) {
                            web.scrollTo(0,240);
                            web.reload();
                        } else {
                            web.scrollTo(0,240);
                            new Handler().postDelayed(() -> capture(),500);
                        }
                    });
                }
            });
            System.out.println("WebView render: loading original provider local HTML");
            web.loadUrl("http://127.0.0.1:"+server.getLocalPort()+"/");
            new Handler().postDelayed(() -> { if(!complete) throw new AssertionError("WebView interaction callback timeout"); },180000);
        }
        private void focusEditor() {
            try {
                // Exercise the software-window focus path without stealing desktop focus.
                java.lang.reflect.Field field=Class.forName("com.trebledot.stone.runtime.ArtServices").getDeclaredField("windows");
                field.setAccessible(true); Object windows=field.get(null);
                Method focus=windows.getClass().getDeclaredMethod("focusChanged",android.view.View.class,boolean.class);
                focus.setAccessible(true); focus.invoke(windows,getWindow().getDecorView(),true);
            } catch(Exception error) { throw new AssertionError(error); }
            touch(android.view.MotionEvent.ACTION_DOWN,60,24);
            new Handler().postDelayed(() -> {
                touch(android.view.MotionEvent.ACTION_UP,60,24);
                new Handler().postDelayed(() -> web.evaluateJavascript("document.activeElement.id",value -> {
                    if(!"\"editor\"".equals(value)) throw new AssertionError("DOM editor touch focus: "+value);
                    typeText();
                }),200);
            },50);
        }
        private long touchDown;
        private void touch(int action,float x,float y) {
            long now=android.os.SystemClock.uptimeMillis();
            if(action==android.view.MotionEvent.ACTION_DOWN) touchDown=now;
            android.view.MotionEvent.PointerProperties pointer=new android.view.MotionEvent.PointerProperties();
            pointer.id=0;pointer.toolType=android.view.MotionEvent.TOOL_TYPE_FINGER;
            android.view.MotionEvent.PointerCoords position=new android.view.MotionEvent.PointerCoords();
            position.x=x;position.y=y;position.size=1;
            position.pressure=action==android.view.MotionEvent.ACTION_UP?0:1;
            android.view.MotionEvent event=android.view.MotionEvent.obtain(touchDown,now,action,1,
                new android.view.MotionEvent.PointerProperties[]{pointer},new android.view.MotionEvent.PointerCoords[]{position},
                0,0,1,1,0,0,android.view.InputDevice.SOURCE_TOUCHSCREEN,0);
            try { dispatchTouchEvent(event); } finally { event.recycle(); }
        }
        private void typeText() {
            try {
                android.view.inputmethod.InputMethodManager manager=getSystemService(android.view.inputmethod.InputMethodManager.class);
                if(!manager.isActive(web)) throw new AssertionError("WebView editor not served by the image input focus controller");
                Class<?> session=Class.forName("com.trebledot.stone.runtime.ArtInputMethod$TextSession");
                java.lang.reflect.Constructor<?> constructor=session.getDeclaredConstructor();constructor.setAccessible(true);
                Method commit=session.getDeclaredMethod("commitText",android.view.View.class,CharSequence.class);commit.setAccessible(true);
                try(AutoCloseable connection=(AutoCloseable)constructor.newInstance()) {
                    if(!(Boolean)commit.invoke(connection,web,"Stone42")) throw new AssertionError("WebView rejected committed Windows text");
                }
                new Handler().postDelayed(() -> web.evaluateJavascript("document.getElementById('editor').value",value -> {
                    if(!"\"Stone42\"".equals(value)) throw new AssertionError("WebView text result: "+value);
                    clickButton();
                }),200);
            } catch(Exception error) { throw new AssertionError(error); }
        }
        private void clickButton() {
            long now=android.os.SystemClock.uptimeMillis();
            for(int action:new int[]{android.view.MotionEvent.ACTION_DOWN,android.view.MotionEvent.ACTION_UP}) {
                android.view.MotionEvent event=android.view.MotionEvent.obtain(now,now+40,action,60,90,0);
                event.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN);
                try { web.dispatchTouchEvent(event); } finally { event.recycle(); }
            }
            new Handler().postDelayed(() -> web.evaluateJavascript("document.getElementById('click').textContent",value -> {
                if(!"\"1\"".equals(value)) throw new AssertionError("WebView click result: "+value);
                web.setAlpha(0.5f);
                new Handler().postDelayed(() -> {
                    web.setAlpha(1f);
                    web.loadUrl("http://127.0.0.1:"+server.getLocalPort()+"/second");
                },300);
            }),300);
        }
        private void capture() {
            web.evaluateJavascript("window.ticks+':'+window.scrollY",value -> {
                String[] state=value.substring(1,value.length()-1).split(":");
                int tick=Integer.parseInt(state[0]);
                if(tick<=lastAnimationTick || Integer.parseInt(state[1])!=240)
                    throw new AssertionError("WebView animation or scroll stalled: "+value);
                lastAnimationTick=tick;
                captureFrame();
            });
        }
        private void captureFrame() {
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
                long now=android.os.SystemClock.uptimeMillis();
                if(repeatedDrawingStarted==0) repeatedDrawingStarted=now;
                long drawingMillis=now-repeatedDrawingStarted;
                if(++captures%50==0) System.out.println("WebView render: captures="+captures+" ticks="+lastAnimationTick+" drawingMs="+drawingMillis);
                if(captures<20 || drawingMillis<60000) {
                    web.invalidate();
                    new Handler().postDelayed(() -> capture(),100);
                    return;
                }
                File output=new File("/data/webview-render");
                if(!output.isDirectory() && !output.mkdirs()) throw new AssertionError("WebView render output directory");
                try(FileOutputStream file=new FileOutputStream(new File(output,"frame.png"))) {
                    if(!bitmap.compress(Bitmap.CompressFormat.PNG,100,file)) throw new AssertionError("WebView frame encoding");
                }
                complete=true;
                System.out.println("PASS original WebView HTTP, JavaScript, input focus, committed text, touch click, alpha drawing, navigation, reload, scroll and repeated software pixels: captures="+captures+" drawingMs="+drawingMillis);
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
