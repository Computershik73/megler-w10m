package megler.test.procs;

/** The launcher activity, in the package's own process. */
public class Main extends Shown {
    @Override String who() { return "main"; }
    @Override Class<Far> other() { return Far.class; }
}
