package com.trebledot.stone.runtime;

import android.app.Activity;
import android.app.LocaleConfig;
import android.app.LocaleManager;
import android.content.Context;
import android.content.res.Configuration;
import android.os.Bundle;
import android.os.Handler;
import android.os.LocaleList;
import android.os.Looper;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.CheckedTextView;
import android.text.Editable;
import android.text.TextWatcher;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.text.style.RelativeSizeSpan;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.Locale;

/** Per-application language choice using the APK's declared image LocaleConfig. */
public final class ArtLocaleActivity extends Activity {
    private Choices choices;
    private Button save;
    @Override protected void onCreate(Bundle state) {
        ArtSettingsUi.prepare(this);
        super.onCreate(state);
        if (!ArtLocaleSettings.accepts(getPackageName(),getIntent())) { finish(); return; }
        Object retained = getLastNonConfigurationInstance();
        choices = retained instanceof Choices ? (Choices) retained : new Choices(getApplicationContext());
        choices.owner = this;
        render();
    }
    private void render() {
        ArtSettingsUi ui = new ArtSettingsUi(this,ArtStrings.locale_title,false);
        ui.navigationEnabled(!choices.saving);
        ui.application(getApplicationInfo().loadLabel(getPackageManager()),getPackageName(),null);
        boolean retry = choices.labels == null && choices.error;
        save = ui.primary(retry ? ArtStrings.retry : choices.saving ? ArtStrings.locale_saving : ArtStrings.save,
            view -> { if (choices.labels == null) choices.load(); else choices.save(); });
        save.setEnabled(retry || choices.labels != null && !choices.saving && !choices.selected.equals(choices.original));
        if (choices.labels == null) {
            if (choices.error) ui.message(ArtText.get(this,ArtStrings.locale_read_error));
            else ui.waiting(ArtText.get(this,ArtStrings.locale_loading));
        } else {
            EditText search = new EditText(this);
            search.setId(android.R.id.input); search.setSingleLine(true); search.setTextSize(16); search.setTextColor(ui.onSurface);
            search.setHintTextColor(ui.secondary); search.setHint(ArtText.get(this,ArtStrings.locale_search));
            search.setInputType(android.text.InputType.TYPE_CLASS_TEXT);
            search.setMinHeight(ui.dp(56)); search.setPadding(ui.dp(16),ui.dp(12),ui.dp(16),ui.dp(12));
            search.setBackground(ui.shape(ui.surface,28)); search.setEnabled(!choices.saving);
            LinearLayout.LayoutParams searchParams = new LinearLayout.LayoutParams(-1,-2); searchParams.bottomMargin = ui.dp(8);
            ui.content.addView(search,searchParams);
            ListView list = new ListView(this); list.setId(android.R.id.list); list.setChoiceMode(ListView.CHOICE_MODE_SINGLE);
            list.setDivider(null); list.setEnabled(!choices.saving);
            TextView empty = ui.text(ArtText.get(this,ArtStrings.locale_no_results),16,ui.secondary);
            empty.setPadding(ui.dp(16),ui.dp(24),ui.dp(16),ui.dp(24));
            empty.setVisibility(android.view.View.GONE); ui.content.addView(empty);
            ArrayList<Integer> visible = new ArrayList<>();
            ArrayAdapter<CharSequence> adapter = new ArrayAdapter<CharSequence>(this,android.R.layout.simple_list_item_single_choice,new ArrayList<CharSequence>()) {
                @Override public android.view.View getView(int position, android.view.View recycled, android.view.ViewGroup parent) {
                    CheckedTextView row = (CheckedTextView) super.getView(position,recycled,parent);
                    row.setTextColor(ui.onSurface); row.setTextSize(16); row.setMinHeight(ui.dp(72));
                    row.setSingleLine(false);
                    row.setPaddingRelative(ui.dp(16),ui.dp(12),ui.dp(16),ui.dp(12));
                    row.setCheckMarkTintList(new android.content.res.ColorStateList(
                        new int[][]{new int[]{android.R.attr.state_checked},new int[0]},new int[]{ui.primary,ui.secondary}));
                    android.graphics.drawable.StateListDrawable background = new android.graphics.drawable.StateListDrawable();
                    background.addState(new int[]{android.R.attr.state_activated},ui.shape(ui.container,12));
                    background.addState(new int[0],ui.shape(0x00000000,12));
                    row.setBackground(new android.graphics.drawable.RippleDrawable(
                        android.content.res.ColorStateList.valueOf(ui.container),background,ui.shape(0xffffffff,12)));
                    return row;
                }
            };
            list.setAdapter(adapter); list.setEmptyView(empty);
            Runnable filter = () -> {
                String query = choices.query.trim().toLowerCase(Locale.ROOT);
                adapter.setNotifyOnChange(false); adapter.clear(); visible.clear(); list.clearChoices();
                Locale display = getResources().getConfiguration().getLocales().get(0);
                for (int index = 0; index < choices.tags.size(); index++) {
                    String tag = choices.tags.get(index);
                    String name = tag.isEmpty() ? ArtText.get(this,ArtStrings.locale_follow_system) : choices.labels.get(index);
                    String translated = tag.isEmpty() ? "" : Locale.forLanguageTag(tag).getDisplayName(display);
                    String detail = tag.isEmpty() ? choices.manager.getSystemLocales().toLanguageTags()
                        : translated.equals(name) ? tag : translated + " · " + tag;
                    if (!(name + " " + detail).toLowerCase(Locale.ROOT).contains(query)) continue;
                    SpannableString label = new SpannableString(name + "\n" + detail);
                    label.setSpan(new RelativeSizeSpan(0.8f),name.length()+1,label.length(),Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                    label.setSpan(new ForegroundColorSpan(ui.secondary),name.length()+1,label.length(),Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
                    visible.add(index); adapter.add(label);
                    if (choices.selected.isEmpty() ? tag.isEmpty() : tag.equals(choices.selected.get(0).toLanguageTag()))
                        list.setItemChecked(visible.size()-1,true);
                }
                adapter.notifyDataSetChanged();
            };
            list.setOnItemClickListener((parent,view,position,id) -> {
                if (choices.saving) return;
                choices.selected = LocaleList.forLanguageTags(choices.tags.get(visible.get(position)));
                save.setEnabled(!choices.selected.equals(choices.original));
            });
            search.setText(choices.query); search.setSelection(search.length());
            search.addTextChangedListener(new TextWatcher() {
                @Override public void beforeTextChanged(CharSequence text, int start, int count, int after) { }
                @Override public void onTextChanged(CharSequence text, int start, int before, int count) { choices.query = text.toString(); filter.run(); }
                @Override public void afterTextChanged(Editable text) { }
            });
            ui.content.addView(list,new LinearLayout.LayoutParams(-1,0,1)); filter.run();
            if (choices.error) {
                TextView error = ui.message(ArtText.get(this,ArtStrings.locale_save_error));
                error.setAccessibilityLiveRegion(android.view.View.ACCESSIBILITY_LIVE_REGION_POLITE);
            }
        }
        ui.show();
    }
    @Override public void onConfigurationChanged(Configuration config) { super.onConfigurationChanged(config); render(); }
    @Override public void onBackPressed() { if (choices == null || !choices.saving) super.onBackPressed(); }
    @Override public Object onRetainNonConfigurationInstance() { return choices; }
    @Override protected void onDestroy() {
        if (choices != null) { choices.owner = null; if (!isChangingConfigurations()) choices.closed = true; }
        super.onDestroy();
    }
    private static final class Choices {
        final Context context;
        final LocaleManager manager;
        final LocaleList original;
        final Handler main = new Handler(Looper.getMainLooper());
        LocaleList selected;
        ArtLocaleActivity owner;
        ArrayList<String> labels, tags;
        boolean saving, error;
        String query = "";
        volatile boolean closed;
        Choices(Context context) {
            this.context = context; manager = context.getSystemService(LocaleManager.class);
            original = manager.getApplicationLocales(); selected = original;
            load();
        }
        void load() {
            if (closed || saving) return;
            error = false;
            if (owner != null) owner.render();
            Thread worker = new Thread(() -> {
                try {
                    LocaleConfig config = new LocaleConfig(context);
                    LocaleList locales = config.getSupportedLocales();
                    if (config.getStatus() != LocaleConfig.STATUS_SUCCESS || locales == null || locales.isEmpty() || locales.size() > 512)
                        throw new IllegalStateException("APK has no usable declared locale configuration: " + config.getStatus());
                    ArrayList<Locale> all = new ArrayList<>();
                    for (int index = 0; index < locales.size(); index++) all.add(locales.get(index));
                    all.sort(java.util.Comparator.comparing(locale -> locale.getDisplayName(locale)));
                    LinkedHashSet<Locale> ordered = new LinkedHashSet<>();
                    for (LocaleList preferred : new LocaleList[]{original,manager.getSystemLocales()}) for (int index = 0; index < preferred.size(); index++) {
                        Locale match = locales.getFirstMatch(new String[]{preferred.get(index).toLanguageTag()});
                        if (match != null && match.getLanguage().equals(preferred.get(index).getLanguage())) ordered.add(match);
                    }
                    ordered.addAll(all);
                    ArrayList<String> names = new ArrayList<>(), values = new ArrayList<>();
                    names.add(""); values.add("");
                    for (Locale locale : ordered) { values.add(locale.toLanguageTag()); names.add(locale.getDisplayName(locale)); }
                    main.post(() -> { if (!closed) { labels = names; tags = values; if (owner != null) owner.render(); } });
                } catch (RuntimeException | LinkageError failure) {
                    android.util.Log.w("StoneLocales","Cannot read APK language choices",failure);
                    main.post(() -> { if (!closed) { error = true; if (owner != null) owner.render(); } });
                }
            },"Stone application language choices");
            worker.setDaemon(true); worker.start();
        }
        void save() {
            if (closed || saving || labels == null || selected.equals(original)) return;
            saving = true; error = false; if (owner != null) owner.render();
            LocaleList value = selected;
            Thread worker = new Thread(() -> {
                try {
                    manager.setApplicationLocales(value);
                    main.post(() -> { if (!closed && owner != null) { owner.setResult(RESULT_OK); owner.finish(); } });
                } catch (RuntimeException | LinkageError failure) {
                    android.util.Log.w("StoneLocales","Cannot save application language",failure);
                    main.post(() -> { if (!closed) { saving = false; error = true; if (owner != null) owner.render(); } });
                } finally { ArtConfiguration.endSave(); }
            },"Stone application language save");
            worker.setDaemon(true); ArtConfiguration.beginSave();
            try { worker.start(); }
            catch (RuntimeException | Error failure) { ArtConfiguration.endSave(); throw failure; }
        }
    }
}
