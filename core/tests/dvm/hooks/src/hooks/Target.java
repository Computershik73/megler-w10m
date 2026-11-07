package hooks;

/** What the hooks are put on (Hooks; core/tests/mg_dvm_tests_hooks.c). */
public class Target {
    /** How many times the constructor's body has run. */
    public static int made;
    public int value;

    public Target(int value) {
        this.value = value;
        made++;
    }

    public int add(int x) {
        return value + x;
    }

    /** Getters: a call of one is done in place, by the interpreter and in compiled code. */
    public int getValue() {
        return value;
    }

    public int getValue2() {
        return value;
    }

    public static int twice(int x) {
        return x * 2;
    }

    public String name(String prefix) {
        return prefix + value;
    }

    public void bump() {
        value++;
    }

    public int divide(int by) {
        return value / by;
    }

    /** An argument of each kind, and a wide result. */
    public static long mix(boolean z, byte b, char c, short s, int i, long j, float f, double d, String t) {
        return (z ? 1 : 0) + b + c + s + i + j + (long) f + (long) d + t.length();
    }

    /** Answered by the tests in C. */
    public static native int nativeAdd(int a, int b);

    /** Loops that call the methods above, compiled once hot (the tests run them with the JIT's threshold at 1). */
    public static int sumAdds(Target t, int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s += t.add(i);
        return s;
    }

    public static int sumValues(Target t, int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s += t.getValue();
        return s;
    }

    public static int sumValues2(Target t, int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s += t.getValue2();
        return s;
    }

    public static int sumTwice(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s += twice(i);
        return s;
    }

    @Override
    public String toString() {
        return "T" + value;
    }
}

/** Inherits add; its constructor calls Target's. */
class Sub extends Target {
    Sub(int value) {
        super(value);
    }
}

/** Overrides add, and calls Target's from it. */
class Over extends Target {
    Over(int value) {
        super(value);
    }

    @Override
    public int add(int x) {
        return super.add(x) * 10;
    }
}

interface Greeter {
    default String greet() {
        return "hello";
    }
}

class Greeting implements Greeter {
}

/** A final class, made inheritable (XposedBridge.makeClassInheritable0). Not nested: Class.getModifiers reads a nested class's flags elsewhere. */
final class Sealed {
    private Sealed() {
    }
}

/** A class whose initialiser runs before the hook of its static method is called. */
class Late {
    static int ready;

    static {
        ready = 42;
        Hook.events.append("init ");
    }

    static int answer() {
        return ready;
    }
}
