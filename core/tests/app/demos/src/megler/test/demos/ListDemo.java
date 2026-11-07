package megler.test.demos;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.AsyncTask;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.Toast;

/**
 * A ListView with an adapter; each row starts one thing an ordinary app does. The first row touched starts a run down the list
 * from there, one step after the last has reported, to "all done" at the end; after that a row touched does its own thing
 * alone. Two of the steps wait for a finger: the dialog's button and the second activity's.
 */
public class ListDemo extends Activity implements AdapterView.OnItemClickListener {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private ListView list;
    private int step = -1;
    private boolean shown, chain = true;
    /* Rows the adapter has been asked for (getView), from the start. */
    private int bound;
    /* A finger's moves on the list: the events they came in, and the samples in them (each event's history and itself). */
    private int moveEvents, moveSamples;

    private static void say(String what) {
        Log.i("DemoTest", what);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        list = new ListView(this);
        list.setAdapter(new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, getResources().getStringArray(R.array.rows)) {
            @Override
            public View getView(int position, View convert, ViewGroup parent) {
                bound++;
                return super.getView(position, convert, parent);
            }
        });
        list.setOnItemClickListener(this);
        // How a finger's moves come: Megler batches those that come between two frames into one event (its history).
        list.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent e) {
                if (e.getActionMasked() == MotionEvent.ACTION_DOWN) moveEvents = moveSamples = 0;
                if (e.getActionMasked() == MotionEvent.ACTION_MOVE) {
                    moveEvents++;
                    moveSamples += e.getHistorySize() + 1;
                    // The samples in order, the last the event's own place.
                    for (int h = 0; h < e.getHistorySize(); h++)
                        if (e.getHistoricalEventTime(h) > (h + 1 < e.getHistorySize() ? e.getHistoricalEventTime(h + 1) : e.getEventTime())) say("list: a move's samples out of order");
                }
                if (e.getActionMasked() == MotionEvent.ACTION_UP) say("list: a finger's moves came as " + moveEvents + " events of " + moveSamples + " samples");
                return false;
            }
        });
        setContentView(list);
        say("list: created with " + list.getAdapter().getCount() + " rows");
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus || shown || list.getChildCount() == 0) return;
        shown = true;
        View row = list.getChildAt(0);
        int[] at = new int[2];
        row.getLocationOnScreen(at);
        say("list: shown rows=" + list.getChildCount() + " row0=" + at[0] + "," + at[1] + "," + row.getWidth() + "x" + row.getHeight());
        // A layout with the data unchanged puts the rows back as they are: the adapter is not asked for any of them again.
        final int before = bound;
        list.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
            @Override
            public void onGlobalLayout() {
                list.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                say("list: laid out again, rows bound again " + (bound - before) + " rows=" + list.getChildCount());
            }
        });
        list.requestLayout();
    }

    @Override
    public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
        say("list: clicked row " + position + " \"" + parent.getItemAtPosition(position) + "\"");
        if (position == 7) {
            /* The screen with the text fields: the keyboard's part of the test. */
            startActivity(new Intent(this, Typing.class));
            return;
        }
        if (position == 8) {
            /* Dialogs with a field, over which the keyboard comes up. */
            startActivity(new Intent(this, KeyboardDialogs.class));
            return;
        }
        step = position;
        run();
    }

    private void next() {
        handler.post(() -> {
            if (!chain) {
                say("row " + step + " done");
                return;
            }
            step++;
            run();
        });
    }

    private static String place(View v) {
        int[] at = new int[2];
        v.getLocationOnScreen(at);
        return at[0] + "," + at[1] + "," + v.getWidth() + "x" + v.getHeight();
    }

    private void run() {
        try {
            switch (step) {
            case 0: dialog(); break;
            case 1: preferences(); break;
            case 2: delayed(); break;
            case 3: task(); break;
            case 4: second(); break;
            case 5: toast(); break;
            default:
                say("all done");
                chain = false;
                break;
            }
        } catch (Throwable t) {
            Log.e("DemoTest", "step " + step + " threw", t);
            say("step " + step + " failed: " + t);
            next();
        }
    }

    private void dialog() {
        final AlertDialog dialog = new AlertDialog.Builder(this).setTitle("A question").setMessage("Is this a dialog?")
                .setPositiveButton("Yes", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        say("dialog: answered " + (which == DialogInterface.BUTTON_POSITIVE ? "yes" : "something else"));
                        next();
                    }
                }).setNegativeButton("No", null).create();
        dialog.show();
        // Where it is and where its button is, once it has been laid out: the button waits for a finger.
        handler.postDelayed(() -> say("dialog: showing=" + dialog.isShowing() + " window=" + place(dialog.getWindow().getDecorView()) + " yes="
                + place(dialog.getButton(DialogInterface.BUTTON_POSITIVE))), 300);
    }

    private void preferences() {
        SharedPreferences prefs = getSharedPreferences("demo", MODE_PRIVATE);
        int runs = prefs.getInt("runs", 0);
        prefs.edit().putInt("runs", runs + 1).putString("name", "héllo").putBoolean("flag", true).putLong("big", 1L << 40).putFloat("ratio", 0.25f).apply();
        SharedPreferences again = getSharedPreferences("demo", MODE_PRIVATE);
        boolean committed = again.edit().putString("second", "two").commit();
        // What commit() left on disk: the file with every key in it, and no ".new" file it was written to first.
        java.io.File file = new java.io.File(getDataDir(), "shared_prefs/demo.xml");
        say("preferences: runs=" + again.getInt("runs", -1) + " name=" + again.getString("name", null) + " flag=" + again.getBoolean("flag", false) + " big=" + again.getLong("big", 0)
                + " ratio=" + again.getFloat("ratio", 0) + " missing=" + again.getString("missing", "default") + " contains=" + again.contains("second") + " committed=" + committed
                + " keys=" + again.getAll().size() + " file=" + (file.length() > 0) + " new=" + new java.io.File(file.getPath() + ".new").exists());
        next();
    }

    private void delayed() {
        final long asked = SystemClock.uptimeMillis();
        final StringBuilder order = new StringBuilder();
        handler.postDelayed(() -> order.append("c"), 150);
        handler.postDelayed(() -> order.append("a"), 50);
        handler.postDelayed(() -> order.append("b"), 100);
        Runnable never = () -> order.append("X");
        handler.postDelayed(never, 120);
        handler.removeCallbacks(never);
        handler.postDelayed(() -> {
            long took = SystemClock.uptimeMillis() - asked;
            say("delayed: order=" + order + " after " + (took >= 200 && took < 5000 ? "the delay" : took + " ms") + " on " + (Looper.myLooper() == Looper.getMainLooper() ? "the main thread" : "another"));
            next();
        }, 200);
    }

    private void task() {
        new AsyncTask<Integer, Integer, String>() {
            private final StringBuilder progress = new StringBuilder();

            @Override
            protected void onPreExecute() {
                progress.append("pre;");
            }

            @Override
            protected String doInBackground(Integer... n) {
                long sum = 0;
                boolean other = Looper.myLooper() != Looper.getMainLooper();
                for (int i = 1; i <= n[0]; i++) {
                    sum += i;
                    if (i % 500 == 0) publishProgress(i);
                }
                return (other ? "background" : "main") + " sum=" + sum;
            }

            @Override
            protected void onProgressUpdate(Integer... v) {
                progress.append(v[0]).append(";");
            }

            @Override
            protected void onPostExecute(String result) {
                say("task: " + result + " progress=" + progress + " delivered on " + (Looper.myLooper() == Looper.getMainLooper() ? "the main thread" : "another"));
                next();
            }
        }.execute(1000);
    }

    private void second() {
        startActivityForResult(new Intent(this, Second.class).putExtra("number", 21).putExtra("text", "from the list"), 7);
        say("second: asked for");
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        say("second: result request=" + request + " ok=" + (result == RESULT_OK) + " answer=" + (data == null ? "none" : String.valueOf(data.getIntExtra("answer", -1))));
        next();
    }

    private void toast() {
        Toast toast = Toast.makeText(this, "A toast", Toast.LENGTH_SHORT);
        toast.show();
        // (A toast is a window of its own, which comes when the toast's turn does: a moment later. Its view is not the app's to ask for.)
        handler.postDelayed(() -> {
            say("toast: shown");
            next();
        }, 400);
    }

    @Override
    protected void onPause() {
        super.onPause();
        say("list: paused");
    }

    @Override
    protected void onResume() {
        super.onResume();
        say("list: resumed");
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        say("list: destroyed");
    }
}
