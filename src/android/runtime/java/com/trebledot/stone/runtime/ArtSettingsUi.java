package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

/** Material 3 spacing, shapes and tonal surfaces using the image's platform widgets and palette. */
final class ArtSettingsUi {
    final Activity activity;
    final LinearLayout content;
    final int primary, onPrimary, surface, onSurface, secondary, outline, container;
    private final LinearLayout root;
    private final ImageButton back;

    static void prepare(Activity activity) {
        boolean dark = (activity.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
            == Configuration.UI_MODE_NIGHT_YES;
        activity.setTheme(dark ? android.R.style.Theme_DeviceDefault_NoActionBar : android.R.style.Theme_DeviceDefault_Light_NoActionBar);
    }

    ArtSettingsUi(Activity activity, int title) { this(activity, title, true); }
    ArtSettingsUi(Activity activity, int title, boolean scroll) {
        this.activity = activity;
        boolean dark = (activity.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
            == Configuration.UI_MODE_NIGHT_YES;
        primary = color(dark ? "system_accent1_200" : "system_accent1_600", dark ? 0xffd0bcff : 0xff6750a4);
        onPrimary = color(dark ? "system_accent1_800" : "system_accent1_0", dark ? 0xff381e72 : 0xffffffff);
        surface = color(dark ? "system_neutral1_800" : "system_neutral1_50", dark ? 0xff211f26 : 0xfff3edf7);
        onSurface = color(dark ? "system_neutral1_50" : "system_neutral1_900", dark ? 0xffe6e0e9 : 0xff1d1b20);
        secondary = color(dark ? "system_neutral2_200" : "system_neutral2_600", dark ? 0xffcac4d0 : 0xff49454f);
        outline = color(dark ? "system_neutral2_600" : "system_neutral2_200", dark ? 0xff49454f : 0xffcac4d0);
        container = color(dark ? "system_accent1_700" : "system_accent1_100", dark ? 0xff4f378b : 0xffeaddff);
        root = new LinearLayout(activity); root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(color(dark ? "system_neutral1_900" : "system_neutral1_10", dark ? 0xff141218 : 0xfffff7ff));
        root.setPadding(dp(16),dp(8),dp(16),dp(16));
        LinearLayout bar = new LinearLayout(activity); bar.setGravity(Gravity.CENTER_VERTICAL);
        back = new ImageButton(activity); back.setId(android.R.id.home); back.setImageDrawable(new BackIcon());
        back.setScaleType(android.widget.ImageView.ScaleType.CENTER);
        back.setContentDescription(ArtText.get(activity,ArtStrings.back));
        back.setBackground(new RippleDrawable(ColorStateList.valueOf(container),null,shape(0xffffffff,24)));
        back.setOnClickListener(view -> activity.onBackPressed());
        bar.addView(back,new LinearLayout.LayoutParams(dp(48),dp(48)));
        TextView heading = text(ArtText.get(activity,title),22,onSurface);
        heading.setAccessibilityHeading(true);
        LinearLayout.LayoutParams headingParams = new LinearLayout.LayoutParams(0,-2,1);
        headingParams.setMarginStart(dp(8)); bar.addView(heading,headingParams);
        root.addView(bar,new LinearLayout.LayoutParams(-1,-2));
        activity.setTitle(ArtText.get(activity,title));
        content = new LinearLayout(activity); content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(8),dp(20),dp(8),dp(8));
        if (scroll) {
            ScrollView viewport = new ScrollView(activity); viewport.setFillViewport(true);
            viewport.addView(content,new ScrollView.LayoutParams(-1,-2));
            root.addView(viewport,new LinearLayout.LayoutParams(-1,0,1));
        } else root.addView(content,new LinearLayout.LayoutParams(-1,0,1));
    }

    int dp(int value) { return Math.round(value * activity.getResources().getDisplayMetrics().density); }
    private int color(String name, int fallback) {
        int id = activity.getResources().getIdentifier(name,"color","android");
        if (id == 0) return fallback;
        try { return activity.getResources().getColor(id,activity.getTheme()); }
        catch (android.content.res.Resources.NotFoundException missing) { return fallback; }
    }
    GradientDrawable shape(int color, int radius) {
        GradientDrawable background = new GradientDrawable(); background.setColor(color); background.setCornerRadius(dp(radius));
        return background;
    }
    TextView text(CharSequence value, int size, int color) {
        TextView view = new TextView(activity); view.setText(value); view.setTextSize(size); view.setTextColor(color);
        view.setTextDirection(View.TEXT_DIRECTION_FIRST_STRONG);
        view.setLineSpacing(dp(3),1); return view;
    }
    LinearLayout card() {
        LinearLayout card = new LinearLayout(activity); card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(20),dp(20),dp(20),dp(20)); card.setBackground(shape(surface,16));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1,-2); params.bottomMargin = dp(16);
        content.addView(card,params); return card;
    }
    void application(CharSequence label, String packageName, String version) {
        LinearLayout card = card();
        TextView name = text(label,24,onSurface); name.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));
        name.setAccessibilityHeading(true); card.addView(name);
        TextView identity = text(packageName,14,secondary); identity.setPadding(0,dp(8),0,0); card.addView(identity);
        if (version != null) { TextView detail = text(version,14,secondary); detail.setPadding(0,dp(8),0,0); card.addView(detail); }
    }
    TextView message(String value) {
        TextView message = text(value,16,secondary); message.setPadding(0,0,0,dp(16)); content.addView(message); return message;
    }
    void waiting(String value) {
        LinearLayout row = card(); row.setOrientation(LinearLayout.HORIZONTAL); row.setGravity(Gravity.CENTER_VERTICAL);
        ProgressBar progress = new ProgressBar(activity); progress.setIndeterminateTintList(ColorStateList.valueOf(primary));
        progress.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        row.addView(progress,new LinearLayout.LayoutParams(dp(24),dp(24)));
        TextView status = text(value,16,onSurface); status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0,-2,1); params.setMarginStart(dp(16)); row.addView(status,params);
    }
    Button primary(int label, View.OnClickListener action) {
        Button button = new Button(activity); button.setId(android.R.id.button1); button.setText(ArtText.get(activity,label));
        button.setAllCaps(false); button.setTextSize(14); button.setMinHeight(dp(48)); button.setMinimumHeight(dp(48));
        button.setPadding(dp(24),dp(12),dp(24),dp(12));
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(container),shape(primary,24),null));
        button.setBackgroundTintList(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[0]},new int[]{outline,primary}));
        button.setTextColor(new ColorStateList(new int[][]{new int[]{-android.R.attr.state_enabled},new int[0]},new int[]{secondary,onPrimary}));
        button.setOnClickListener(action);
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1,-2); params.setMargins(dp(8),dp(8),dp(8),0);
        root.addView(button,params); return button;
    }
    void show() { activity.setContentView(root); }
    void navigationEnabled(boolean enabled) { back.setEnabled(enabled); }

    private final class BackIcon extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        BackIcon() { paint.setColor(onSurface); paint.setStrokeWidth(2); paint.setStyle(Paint.Style.STROKE); }
        @Override public void draw(Canvas canvas) {
            canvas.save(); canvas.translate(getBounds().left,getBounds().top);
            canvas.scale(getBounds().width()/24f,getBounds().height()/24f);
            if (activity.getResources().getConfiguration().getLayoutDirection() == View.LAYOUT_DIRECTION_RTL) { canvas.translate(24,0); canvas.scale(-1,1); }
            canvas.drawLine(20,12,4,12,paint); canvas.drawLine(4,12,11,5,paint); canvas.drawLine(4,12,11,19,paint); canvas.restore();
        }
        @Override public int getIntrinsicWidth() { return dp(24); }
        @Override public int getIntrinsicHeight() { return dp(24); }
        @Override public void setAlpha(int alpha) { paint.setAlpha(alpha); }
        @Override public void setColorFilter(ColorFilter filter) { paint.setColorFilter(filter); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }
}
