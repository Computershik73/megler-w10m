package megler.test.procs;

/** The activity in the process ":far" (and its alias FarAlias, which names no process). */
public class Far extends Shown {
    @Override String who() { return "far"; }
    @Override Class<Main> other() { return Main.class; }
}
