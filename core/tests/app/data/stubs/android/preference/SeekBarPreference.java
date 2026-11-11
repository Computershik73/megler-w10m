package android.preference;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step seekbar-preference calls of the class Android hides. */
public class SeekBarPreference extends Preference implements android.widget.SeekBar.OnSeekBarChangeListener {
    public SeekBarPreference(android.content.Context context) { super(context); }
    public void setMax(int max) {}
    public void setProgress(int progress) {}
    public int getProgress() { return 0; }
    public void onProgressChanged(android.widget.SeekBar seekBar, int progress, boolean fromUser) {}
    public void onStartTrackingTouch(android.widget.SeekBar seekBar) {}
    public void onStopTrackingTouch(android.widget.SeekBar seekBar) {}
}
