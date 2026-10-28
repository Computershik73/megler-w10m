package megler.test.data;

/** The step delegate-last-scope: a class of the module alone, whose Twin is the one its loader finds first. */
public final class ModuleProbe {
    private ModuleProbe() {}
    public static String twin() { return Twin.who(); }
}
