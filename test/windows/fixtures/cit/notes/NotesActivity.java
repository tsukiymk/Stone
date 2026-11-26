package com.trebledot.stone.cit.notes;

import com.trebledot.stone.cit.R;
import android.app.Activity;
import android.content.ContentValues;
import android.database.Cursor;
import android.os.Bundle;
import android.util.Log;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class NotesActivity extends Activity {
    private static final String TAG = "WindowsNotes";
    private EditText editor;
    private TextView notes;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (getContentResolver().call(NotesProvider.NOTES, "verifyDatabase", null, null) == null) {
            throw new IllegalStateException("Notes ContentProvider is unavailable");
        }
        Log.i(TAG, "SQLite rollback and Unicode verified");

        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        Button save = new Button(this);
        save.setText(R.string.save);
        save.setOnClickListener(view -> saveNote());
        layout.addView(save, new LinearLayout.LayoutParams(-1, (int) (80 * getResources().getDisplayMetrics().density)));
        editor = new EditText(this);
        editor.setHint(R.string.hint);
        editor.setText(R.string.example);
        layout.addView(editor, new LinearLayout.LayoutParams(-1, -2));
        notes = new TextView(this);
        layout.addView(notes, new LinearLayout.LayoutParams(-1, -2));
        refresh("restored");
        Log.i(TAG, "densityDpi=" + getResources().getDisplayMetrics().densityDpi);
        setContentView(layout);
    }

    private void saveNote() {
        String body = editor.getText().toString();
        if (body.trim().isEmpty()) return;
        ContentValues values = new ContentValues();
        values.put(NotesProvider.BODY, body);
        if (getContentResolver().insert(NotesProvider.NOTES, values) == null) {
            throw new IllegalStateException("Could not save note");
        }
        editor.setText("");
        refresh("saved");
    }

    private void refresh(String action) {
        try (Cursor cursor = getContentResolver().query(NotesProvider.NOTES,
                new String[]{NotesProvider.BODY}, null, null, "_id DESC")) {
            if (cursor == null) throw new IllegalStateException("Could not query notes");
            StringBuilder content = new StringBuilder();
            boolean firstNoteMatchesExample = false;
            while (cursor.moveToNext()) {
                String body = cursor.getString(0);
                if (content.length() == 0) firstNoteMatchesExample = getString(R.string.example).equals(body);
                content.append(body).append('\n');
            }
            notes.setText(content);
            Log.i(TAG, action + "=" + cursor.getCount() + " unicode=" + firstNoteMatchesExample);
        }
    }
}
