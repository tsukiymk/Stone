package com.trebledot.stone.runtime;

import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcel$ReadWriteHelper;

/** A guard on this Parcel only; never serializes a live Binder capability. */
final class ArtSnapshotParcel extends Parcel$ReadWriteHelper {
    @Override public void writeStrongBinder(Parcel parcel, IBinder binder) {
        if (binder != null) throw new IllegalArgumentException("PendingIntent contains a live Binder");
        super.writeStrongBinder(parcel, null);
    }
    static void install(Parcel parcel) {
        try {
            // The helper's Binder hook is image-dependent. Older images keep
            // their live behavior rather than attempting an unsafe snapshot.
            Class<?> helper = Class.forName("android.os.Parcel$ReadWriteHelper");
            helper.getMethod("writeStrongBinder", Parcel.class, IBinder.class);
            Parcel.class.getMethod("setReadWriteHelper", helper).invoke(parcel, new ArtSnapshotParcel());
        } catch (ReflectiveOperationException error) { throw new IllegalArgumentException("Image lacks safe Binder-free Parcel snapshots", error); }
    }
}
