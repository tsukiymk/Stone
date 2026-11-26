package com.trebledot.stone.cit;

import android.content.Context;
import android.util.Log;
import android.widget.TextView;

public final class CitResult {
    public enum State {
        READY, INFO, RUNNING, PASS, FAIL, UNAVAILABLE, BLOCKED, CONFIRM, CANCELLED
    }

    private final String id;
    private final TextView view;
    private State state = State.READY;

    public CitResult(Context context, String id) {
        this.id = id;
        view = new TextView(context);
        view.setTextSize(14);
    }

    public TextView view() { return view; }
    public boolean running() { return state == State.RUNNING; }

    public void set(State status, String detail) {
        state = status;
        view.setTextColor(status == State.FAIL ? 0xffa31621
                : status == State.PASS ? 0xff176244 : 0xff34465d);
        view.setText(status + "  " + detail);
        Log.i("StoneCIT", id + " " + status + " " + detail.replace('\n', ' ').replace('\r', ' '));
    }

    public void error(Throwable error) {
        State status = error instanceof SecurityException ? State.BLOCKED
                : error instanceof UnsupportedOperationException ? State.UNAVAILABLE : State.FAIL;
        String message = error.getMessage();
        set(status, error.getClass().getSimpleName() + (message == null ? "" : ": " + message));
    }
}
