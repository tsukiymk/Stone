package com.trebledot.stone.probe;

import android.content.Context;
import android.os.UserManager;
import android.os.UserHandle;
import android.graphics.drawable.ColorDrawable;

/** Full local user queries through the image UserManager and PackageManager. */
final class UserProfileProbe {
    static void verify(Context context) throws Exception {
        UserManager users = context.getSystemService(UserManager.class);
        java.lang.reflect.Method isProfile = UserManager.class.getMethod("isProfile",int.class);
        int local = (Integer)UserHandle.class.getMethod("myUserId").invoke(null);
        if ((Boolean)isProfile.invoke(users,local) || users.isManagedProfile())
            throw new AssertionError("Stone local user was classified as an Android profile");
        ColorDrawable icon = new ColorDrawable(0xff345678);
        if (context.getPackageManager().getUserBadgedIcon(icon,android.os.Process.myUserHandle()) != icon)
            throw new AssertionError("Full user icon acquired a profile badge");
        try {
            isProfile.invoke(users,local+1);
            throw new AssertionError("Cross-user profile query accepted");
        } catch (java.lang.reflect.InvocationTargetException denied) {
            if (!(denied.getCause() instanceof SecurityException)) throw denied;
        }
        System.out.println("PASS image full-user profile queries, unchanged unbadged icon and cross-user denial.");
    }
}
