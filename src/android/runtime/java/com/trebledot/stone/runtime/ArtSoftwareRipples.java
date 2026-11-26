package com.trebledot.stone.runtime;

import android.graphics.drawable.Drawable;
import android.graphics.drawable.DrawableWrapper;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.RippleDrawable;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CompoundButton;
import android.widget.ImageView;
import java.lang.reflect.Field;
import java.lang.reflect.Method;

/** Uses the image's solid ripple animation on Stone's software Canvas. */
final class ArtSoftwareRipples {
    private static final int SOLID = 0;
    private static final int PATTERNED = 1;

    private final Field state;
    private final Field style;
    private final Field active;
    private final Field pending;
    private final Method forceSoftware;
    private final Method enter;
    private final Method exit;

    private ArtSoftwareRipples(Field state, Field style) throws ReflectiveOperationException {
        this.state = state;
        this.style = style;
        active = field(RippleDrawable.class, "mRippleActive");
        pending = field(RippleDrawable.class, "mAddRipple");
        forceSoftware = RippleDrawable.class.getMethod("setForceSoftware", boolean.class);
        enter = RippleDrawable.class.getDeclaredMethod("tryRippleEnter");
        exit = RippleDrawable.class.getDeclaredMethod("tryRippleExit");
        enter.setAccessible(true);
        exit.setAccessible(true);
    }

    static ArtSoftwareRipples forImage() throws ReflectiveOperationException {
        Field state = field(RippleDrawable.class, "mState");
        Field style;
        try { style = field(state.getType(), "mRippleStyle"); }
        catch (NoSuchFieldException ignored) { return null; } // Older images only draw solid ripples.
        return new ArtSoftwareRipples(state, style);
    }

    void prepare(View view) {
        prepare(view.getBackground());
        prepare(view.getForeground());
        if (view instanceof ImageView) prepare(((ImageView) view).getDrawable());
        if (view instanceof CompoundButton) prepare(((CompoundButton) view).getButtonDrawable());
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            // Compose's ripple hosts are zero-sized children drawn by their owning composables.
            for (int index = 0; index < group.getChildCount(); index++) prepare(group.getChildAt(index));
        }
    }

    private void prepare(Drawable drawable) {
        if (drawable == null) return;
        if (drawable instanceof RippleDrawable) useSolid((RippleDrawable) drawable);
        if (drawable instanceof LayerDrawable) {
            LayerDrawable layers = (LayerDrawable) drawable;
            for (int index = 0; index < layers.getNumberOfLayers(); index++) prepare(layers.getDrawable(index));
        } else if (drawable instanceof DrawableWrapper) {
            prepare(((DrawableWrapper) drawable).getDrawable());
        } else {
            Drawable current = drawable.getCurrent();
            if (current != drawable) prepare(current);
        }
    }

    private void useSolid(RippleDrawable ripple) {
        try {
            if (style.getInt(state.get(ripple)) != PATTERNED) return;
            ripple.mutate();
            style.setInt(state.get(ripple), SOLID);
            forceSoftware.invoke(ripple, true);
            boolean pressed = active.getBoolean(ripple);
            // A newly attached drawable can receive a complete tap before its first draw.
            if (pressed || pending.getBoolean(ripple)) {
                enter.invoke(ripple);
                if (!pressed) exit.invoke(ripple);
            }
            pending.setBoolean(ripple, false);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("Could not prepare the image's software ripple", error);
        }
    }

    private static Field field(Class<?> type, String name) throws NoSuchFieldException {
        Field field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }
}
