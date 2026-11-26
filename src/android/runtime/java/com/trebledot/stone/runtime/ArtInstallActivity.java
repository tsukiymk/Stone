package com.trebledot.stone.runtime;

import android.app.Activity;
import android.os.Bundle;

/** Runtime-owned confirmation for a preapproval request or verified install candidate. */
public final class ArtInstallActivity extends Activity {
    private ArtPackageInstaller.Confirmation confirmation;
    private boolean decided;
    private ArtSettingsConfirmation.Session nativeConfirmation;

    @Override protected void onCreate(Bundle state) {
        ArtSettingsUi.prepare(this);
        super.onCreate(state);
        confirmation = ArtServices.installer.confirmation(getIntent());
        if (confirmation == null) { finish(); return; }
        boolean replacing = !confirmation.preapproval && confirmation.previousVersion != null;
        ArtSettingsUi ui = new ArtSettingsUi(this,confirmation.preapproval ? ArtStrings.install_approve_title
            : replacing ? ArtStrings.install_update_title : ArtStrings.install_title);
        String version = confirmation.preapproval ? null : ArtText.get(this,ArtStrings.version,
            (replacing ? confirmation.previousVersion + " → " : "") + confirmation.version);
        ui.application(confirmation.label,confirmation.packageName,version);
        ui.message(ArtText.get(this,confirmation.preapproval ? ArtStrings.install_preapproval_message
            : replacing ? ArtStrings.install_update_message : ArtStrings.install_message));
        if (ArtSettingsConfirmation.enabled()) {
            ui.waiting(ArtText.get(this,replacing ? ArtStrings.install_wait_update : ArtStrings.install_wait));
            ui.show();
            try {
                Object retained = getLastNonConfigurationInstance();
                nativeConfirmation = retained instanceof ArtSettingsConfirmation.Session ? (ArtSettingsConfirmation.Session) retained
                    : ArtSettingsConfirmation.create(confirmation.preapproval ? "preapprove" : "install", confirmation.label,
                        confirmation.packageName, confirmation.version, getApplicationInfo().loadLabel(getPackageManager()).toString(), !confirmation.preapproval, confirmation.previousVersion, confirmation.icon, confirmation.permissions);
                nativeConfirmation.attach(this::decide);
            } catch (Exception error) {
                android.util.Log.w("StoneSettings", "Could not request native install confirmation", error);
                decide(false);
            }
            return;
        }
        ui.primary(confirmation.preapproval ? ArtStrings.allow : replacing ? ArtStrings.update : ArtStrings.install,
            view -> { view.setEnabled(false); decide(true); });
        ui.show();
    }

    private void decide(boolean accepted) {
        if (decided || confirmation == null) return;
        decided = true;
        ArtServices.installer.confirm(confirmation.id, confirmation.nonce, accepted);
        finish();
    }

    @Override protected void onDestroy() {
        if (nativeConfirmation != null) {
            nativeConfirmation.detach();
            if (!isChangingConfigurations()) nativeConfirmation.cancel();
        }
        if (!decided && confirmation != null && !isChangingConfigurations())
            ArtServices.installer.confirm(confirmation.id, confirmation.nonce, false);
        super.onDestroy();
    }
    @Override public void onBackPressed() { decide(false); }
    @Override public Object onRetainNonConfigurationInstance() { return nativeConfirmation; }
}
