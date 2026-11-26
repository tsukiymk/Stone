package com.trebledot.stone.runtime;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

/** Android special-app-access UI; the grant covers only this profile's emulated storage. */
public final class ArtStorageAccessActivity extends Activity {
    private Choice choice;
    @Override protected void onCreate(Bundle state) {
        ArtSettingsUi.prepare(this);
        super.onCreate(state);
        if (!ArtStorageAccess.accepts(getPackageName(),getIntent())) { finish(); return; }
        Object retained=getLastNonConfigurationInstance();
        choice=retained instanceof Choice ? (Choice)retained : new Choice();
        choice.owner=this; render();
    }
    private void render() {
        ArtSettingsUi ui = new ArtSettingsUi(this,ArtStrings.storage_title);
        ui.navigationEnabled(!choice.saving);
        ui.application(getApplicationInfo().loadLabel(getPackageManager()),getPackageName(),null);
        LinearLayout card = ui.card();
        Switch allow = new Switch(this); allow.setId(android.R.id.checkbox);
        allow.setText(ArtText.get(this,ArtStrings.storage_allow)); allow.setTextColor(ui.onSurface);
        allow.setTextSize(16); allow.setPadding(0,ui.dp(8),0,ui.dp(8)); allow.setMinHeight(ui.dp(48));
        allow.setThumbTintList(new android.content.res.ColorStateList(
            new int[][]{new int[]{android.R.attr.state_checked},new int[0]},new int[]{ui.primary,ui.secondary}));
        allow.setChecked(ArtServices.storage.allFiles()); allow.setEnabled(ArtServices.storage.declared() && !choice.saving);
        allow.setOnCheckedChangeListener((button,checked) -> choice.save(checked)); card.addView(allow);
        ui.message(ArtText.get(this,ArtStrings.storage_message));
        if (choice.error) {
            TextView error = ui.message(ArtText.get(this,ArtStrings.storage_save_error));
            error.setAccessibilityLiveRegion(android.view.View.ACCESSIBILITY_LIVE_REGION_POLITE);
        }
        ui.show();
    }
    @Override public void onBackPressed() { if (choice==null || !choice.saving) finish(); }
    @Override public Object onRetainNonConfigurationInstance() { return choice; }
    @Override protected void onDestroy() { if (choice!=null) choice.owner=null; super.onDestroy(); }
    private static final class Choice {
        final Handler main=new Handler(Looper.getMainLooper());
        ArtStorageAccessActivity owner;
        boolean saving,error;
        void save(boolean allowed) {
            if (saving) return;
            saving=true; error=false; if (owner!=null) owner.render();
            Thread worker=new Thread(()->{
                boolean failed=false;
                try { ArtServices.storage.setAllFiles(allowed); }
                catch (java.io.IOException | RuntimeException failure) { failed=true; android.util.Log.w("StoneStorage","Cannot save all-files access",failure); }
                final boolean result=failed;
                main.post(()->{ saving=false; error=result; if (owner!=null) { owner.setResult(RESULT_OK); owner.render(); } });
                ArtConfiguration.endSave();
            },"Stone shared-storage access");
            worker.setDaemon(true); ArtConfiguration.beginSave();
            try { worker.start(); } catch (RuntimeException | Error failure) { ArtConfiguration.endSave(); saving=false; throw failure; }
        }
    }
}
