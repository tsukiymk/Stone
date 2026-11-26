package com.trebledot.stone.cit;

import android.app.Activity;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toolbar;

import static com.trebledot.stone.cit.CitResult.State.*;

public final class InputActivity extends Activity {
    private SharedPreferences preferences;
    private CitResult multitouch;
    private CitResult input;
    private TextView pointers;
    private int count;
    private int peak;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        preferences = getSharedPreferences("input", MODE_PRIVATE);
        count = preferences.getInt("count", 0);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        LinearLayout hoverButtons = new LinearLayout(this);
        Button hover = addHoverButton(hoverButtons, "Hover");
        hover.setBackground(new RippleDrawable(ColorStateList.valueOf(0xff000000),
                new ColorDrawable(0xffffffff), null));
        addHoverButton(hoverButtons, "Disabled").setEnabled(false);
        Button crosshair = addHoverButton(hoverButtons, "Crosshair");
        crosshair.setPointerIcon(PointerIcon.getSystemIcon(this, PointerIcon.TYPE_CROSSHAIR));
        float density = getResources().getDisplayMetrics().density;
        int rowHeight = Math.round(56 * density);
        content.addView(hoverButtons, new LinearLayout.LayoutParams(-1, rowHeight));
        Toolbar toolbar = new Toolbar(this);
        GradientDrawable navIcon = new GradientDrawable();
        int iconSize = Math.round(24 * density);
        navIcon.setSize(iconSize, iconSize);
        navIcon.setColor(0xff245c94);
        toolbar.setNavigationIcon(navIcon);
        toolbar.setNavigationContentDescription("Navigation action");
        toolbar.setNavigationOnClickListener(view -> Log.i("StoneCIT", "hover toolbar click"));
        content.addView(toolbar, new LinearLayout.LayoutParams(-1, rowHeight));
        getWindow().setNavigationBarColor(0xff000000);
        TextView instructions = new TextView(this);
        instructions.setText(getString(R.string.input_instructions));
        content.addView(instructions);
        input = new CitResult(this, "input-count");
        input.set(INFO, "Saved input count " + count);
        content.addView(input.view());
        multitouch = new CitResult(this, "multitouch");
        multitouch.set(READY, "Waiting for two simultaneous finger contacts");
        content.addView(multitouch.view());
        pointers = new TextView(this);
        pointers.setText("Active contacts 0 · peak 0");
        content.addView(pointers);
        content.addView(new TouchArea(), new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(content);
        Log.i("StoneCIT", "input READY restored=" + count);
    }

    private Button addHoverButton(LinearLayout row, String label) {
        Button button = new Button(this);
        button.setText(label);
        button.setOnClickListener(view -> Log.i("StoneCIT", "hover " + label + " click"));
        button.setOnHoverListener((view, event) -> {
            if (event.isFromSource(InputDevice.SOURCE_MOUSE)) {
                if (event.getActionMasked() == MotionEvent.ACTION_HOVER_ENTER) {
                    Log.i("StoneCIT", "hover " + label + " enter");
                } else if (event.getActionMasked() == MotionEvent.ACTION_HOVER_EXIT) {
                    Log.i("StoneCIT", "hover " + label + " exit");
                }
            }
            return false;
        });
        row.addView(button, new LinearLayout.LayoutParams(0, -1, 1));
        return button;
    }

    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_K) {
            if (event.getRepeatCount() == 0) increment("keyboard");
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    private void increment(String source) {
        int next = count + 1;
        if (preferences.edit().putInt("count", next).commit()) {
            count = next;
            input.set(PASS, source + "=" + count);
        } else {
            input.set(FAIL, "Could not save input count");
        }
    }

    private final class TouchArea extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private MotionEvent active;
        private boolean multiple;
        private boolean mouse;

        TouchArea() {
            super(InputActivity.this);
            setBackgroundColor(0xffd8eaff);
            setOnClickListener(view -> increment(mouse ? "mouse" : "touch"));
            setContentDescription("Touch test area");
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (active != null) active.recycle();
            active = null;
            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) {
                multiple = false;
                mouse = event.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE;
            }
            if (action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL) {
                active = MotionEvent.obtain(event);
            }
            int fingers = 0;
            for (int i = 0; i < event.getPointerCount(); i++) {
                if (action == MotionEvent.ACTION_POINTER_UP && i == event.getActionIndex()) continue;
                if (event.getToolType(i) == MotionEvent.TOOL_TYPE_FINGER) fingers++;
            }
            if (active == null) fingers = 0;
            if (fingers > peak && fingers >= 2) {
                multitouch.set(PASS, fingers + " simultaneous finger contacts");
            }
            peak = Math.max(peak, fingers);
            multiple |= event.getPointerCount() >= 2;
            pointers.setText("Active finger contacts " + fingers + " · peak " + peak);
            if (action == MotionEvent.ACTION_UP && !multiple) performClick();
            invalidate();
            return true;
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (active == null) return;
            paint.setColor(0xff245c94);
            float density = getResources().getDisplayMetrics().density;
            paint.setTextSize(16 * density);
            for (int i = 0; i < active.getPointerCount(); i++) {
                if (active.getActionMasked() == MotionEvent.ACTION_POINTER_UP && i == active.getActionIndex()) continue;
                float x = active.getX(i);
                float y = active.getY(i);
                canvas.drawCircle(x, y, 12 * density, paint);
                canvas.drawText(String.valueOf(active.getPointerId(i)), x + 16 * density, y, paint);
            }
        }

        @Override protected void onDetachedFromWindow() {
            if (active != null) active.recycle();
            active = null;
            super.onDetachedFromWindow();
        }
    }
}
