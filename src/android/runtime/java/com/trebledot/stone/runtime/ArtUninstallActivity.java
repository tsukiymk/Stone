package com.trebledot.stone.runtime;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Button;
import android.widget.TextView;

/** Local confirmation and Activity result for ACTION_DELETE package: requests. */
public final class ArtUninstallActivity extends Activity {
    private ArtUninstaller.Request request;
    private TextView title, details;
    private Button uninstall;
    private ArtSettingsUi ui;
    private boolean decided;

    @Override protected void onCreate(Bundle state) {
        ArtSettingsUi.prepare(this);
        super.onCreate(state);
        ui = new ArtSettingsUi(this,ArtStrings.uninstall_title);
        title = ui.text(ArtText.get(this,ArtStrings.uninstall_loading),24,ui.onSurface);
        title.setAccessibilityHeading(true); ui.card().addView(title);
        details = ui.message("");
        details.setAccessibilityLiveRegion(android.view.View.ACCESSIBILITY_LIVE_REGION_POLITE);
        uninstall = ui.primary(ArtStrings.uninstall,view -> request.approve());
        uninstall.setEnabled(false);
        if (ArtSettingsConfirmation.enabled()) uninstall.setVisibility(android.view.View.GONE);
        ui.show();
        Object retained = getLastNonConfigurationInstance();
        try {
            request = retained instanceof ArtUninstaller.Request ? (ArtUninstaller.Request) retained : ArtServices.uninstaller.begin(getIntent());
            request.attach(this);
        } catch (RuntimeException error) {
            android.util.Log.w("StoneUninstaller", "Invalid removal request", error);
            title.setText(ArtText.get(this,ArtStrings.uninstall_title));
            details.setText(ArtText.get(this,ArtStrings.uninstall_invalid));
        }
    }

    void render(ArtUninstaller.Request current) {
        if (current != request) return;
        if (current.cancelled) { cancel(); return; }
        if (current.completed) {
            decided = true;
            setResult(RESULT_OK, new Intent().putExtra("android.intent.extra.INSTALL_RESULT", 1));
            finish(); return;
        }
        boolean changed = current.error instanceof ArtPackageStore.Failure
            && ((ArtPackageStore.Failure) current.error).status == android.content.pm.PackageInstaller.STATUS_FAILURE_CONFLICT;
        title.setText(current.label == null ? current.packageName : current.label);
        String text = current.packageName;
        if (current.info != null) text += "\n" + ArtText.get(this,ArtStrings.version,current.info.versionName);
        text += "\n\n" + (current.busy ? ArtText.get(this, ArtStrings.uninstall_busy) : changed
            ? ArtText.get(this, ArtStrings.uninstall_changed) : current.error != null && current.info == null
            ? ArtText.get(this, ArtStrings.uninstall_missing) : current.error != null
            ? ArtText.get(this, ArtStrings.uninstall_error)
            : ArtSettingsConfirmation.enabled() ? ArtText.get(this, ArtStrings.uninstall_wait)
            : ArtText.get(this, ArtStrings.uninstall_message));
        details.setText(text);
        uninstall.setEnabled(current.info != null && !current.busy && !current.cancelled && !changed);
        ui.navigationEnabled(!current.busy);
        if (ArtSettingsConfirmation.enabled()) {
            uninstall.setVisibility(android.view.View.GONE);
            current.confirmInSettings();
        }
    }

    private void cancel() {
        if (request != null && !request.cancel()) return;
        decided = true;
        setResult(RESULT_CANCELED, new Intent().putExtra("android.intent.extra.INSTALL_RESULT", -5));
        finish();
    }
    @Override public void onBackPressed() { cancel(); }
    @Override public Object onRetainNonConfigurationInstance() { return request; }
    @Override protected void onDestroy() {
        if (request != null) {
            request.detach(this);
            if (!decided && !isChangingConfigurations()) request.cancel();
        }
        super.onDestroy();
    }
}
