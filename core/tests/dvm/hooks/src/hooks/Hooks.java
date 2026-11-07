package hooks;

import de.robv.android.xposed.XposedBridge;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.HashMap;

/**
 * The runtime's hooks (core/src/dvm/mg_dvm_hook.c) as Aliuhook's XposedBridge uses them. core/tests/mg_dvm_tests_hooks.c
 * calls these and checks what they report; what each scenario returns is said above it.
 */
public final class Hooks {
    private static final HashMap<String, Hook> named = new HashMap<>();

    private Hooks() {}

    static Member member(String which) throws NoSuchMethodException {
        switch (which) {
        case "add": return Target.class.getDeclaredMethod("add", int.class);
        case "getValue": return Target.class.getDeclaredMethod("getValue");
        case "getValue2": return Target.class.getDeclaredMethod("getValue2");
        case "twice": return Target.class.getDeclaredMethod("twice", int.class);
        case "mix":
            return Target.class.getDeclaredMethod("mix", boolean.class, byte.class, char.class, short.class, int.class, long.class, float.class, double.class,
                                                  String.class);
        case "init": return Target.class.getDeclaredConstructor(int.class);
        default: throw new NoSuchMethodException(which);
        }
    }

    /** Hooks put on and taken off by name, for the tests in C, which read their calls and what they saw. */
    public static void on(String which, int mode) throws NoSuchMethodException {
        named.put(which, Hook.on(member(which), mode));
    }

    public static boolean off(String which) {
        Hook h = named.get(which);
        return h != null && h.off();
    }

    public static int calls(String which) {
        Hook h = named.get(which);
        return h == null ? -1 : h.calls;
    }

    public static String seen(String which) {
        Hook h = named.get(which);
        return h == null ? "" : h.seen.toString();
    }

    /** A hook nothing keeps but the runtime: its context and its backup's Method are the collector's roots. */
    public static void orphan(String which) throws NoSuchMethodException {
        Hook.on(member(which), Hook.COUNT);
    }

    public static boolean unhook(String which) throws NoSuchMethodException {
        return XposedBridge.unhook0(member(which));
    }

    public static int counted() {
        return Hook.counted;
    }

    /**
     * An instance method called from bytecode, through reflection, on a subclass that inherits it and by an override's super
     * call (the override itself is not hooked), then its backup, which the hook does not see, and the method unhooked:
     * "8 9 3 50 15 true 4 (LT5,I3)(LT5,I4)(LT1,I2)(LT2,I3) true false false 8 4".
     */
    public static String instance() throws Throwable {
        Target t = new Target(5);
        Method add = Target.class.getDeclaredMethod("add", int.class);
        Hook h = Hook.on(add, Hook.CALL);
        StringBuilder r = new StringBuilder();
        r.append(t.add(3));
        r.append(' ').append(add.invoke(t, 4));
        r.append(' ').append(new Sub(1).add(2));
        r.append(' ').append(new Over(2).add(3));
        r.append(' ').append(h.backup.invoke(t, 10));
        r.append(' ').append(XposedBridge.isHooked0(add));
        r.append(' ').append(h.calls).append(' ').append(h.seen);
        r.append(' ').append(h.off()).append(' ').append(XposedBridge.isHooked0(add)).append(' ').append(h.off());
        r.append(' ').append(t.add(3)).append(' ').append(h.calls);
        return r.toString();
    }

    /**
     * A static method given an argument of each kind, which the hook sees boxed as each is, and one whose result the hook
     * changes, then unhooked: "1099511697579 (Ztrue,B-2,Cc,S-300,I70000,J1099511627776,F25,D-15,Lfour) 1008 8 1".
     */
    public static String statics() throws Throwable {
        Hook h = Hook.on(member("mix"), Hook.CALL);
        long a = Target.mix(true, (byte) -2, 'c', (short) -300, 70000, 1L << 40, 2.5f, -1.5, "four");
        h.off();
        Hook t = Hook.on(member("twice"), Hook.RESULT);
        int b = Target.twice(4);
        t.off();
        int c = Target.twice(4);
        return a + " " + h.seen + " " + b + " " + c + " " + t.calls;
    }

    /**
     * A constructor: called by new, by Constructor.newInstance, by a subclass's constructor and by invokeConstructor0 on an
     * object allocateInstance0 made, all of them hooked; run on such an object by its backup, not hooked; and hooked so that
     * its body does not run: "7 8 9 0 11 true 12 0 5 4 1 (LT0,I7)(LT0,I8)(LT0,I9)(LT0,I12)".
     */
    public static String constructors() throws Throwable {
        Constructor<Target> ctor = Target.class.getDeclaredConstructor(int.class);
        int before = Target.made;
        Hook h = Hook.on(ctor, Hook.CALL);
        Target a = new Target(7);
        Target b = ctor.newInstance(8);
        Target c = new Sub(9);
        Target e = (Target) XposedBridge.allocateInstance0(Target.class);
        int e0 = e.value;
        h.backup.invoke(e, 11);
        Target f = (Target) XposedBridge.allocateInstance0(Target.class);
        boolean made = XposedBridge.invokeConstructor0(f, ctor, new Object[] { 12 });
        h.off();
        Hook skip = Hook.on(ctor, Hook.SKIP);
        Target g = new Target(13);
        skip.off();
        return a.value + " " + b.value + " " + c.value + " " + e0 + " " + e.value + " " + made + " " + f.value + " " + g.value + " " + (Target.made - before) + " "
            + h.calls + " " + skip.calls + " " + h.seen;
    }

    /**
     * What the original throws, through the backup; what the hook throws, from bytecode and through reflection; null for an
     * int; a String for an int, an Integer for a String; and the method again, unhooked:
     * "ArithmeticException 3 from the hook java.lang.IllegalStateException NullPointerException ClassCastException
     * ClassCastException x6" (one line).
     */
    public static String exceptions() throws Throwable {
        Target t = new Target(6);
        StringBuilder r = new StringBuilder();
        Method add = Target.class.getDeclaredMethod("add", int.class), name = Target.class.getDeclaredMethod("name", String.class);
        Hook h = Hook.on(Target.class.getDeclaredMethod("divide", int.class), Hook.CALL);
        try {
            r.append(t.divide(0));
        } catch (ArithmeticException e) {
            r.append("ArithmeticException");
        }
        r.append(' ').append(t.divide(2));
        h.off();
        h = Hook.on(add, Hook.THROW);
        try {
            r.append(' ').append(t.add(1));
        } catch (IllegalStateException e) {
            r.append(e.getMessage());
        }
        try {
            add.invoke(t, 1);
        } catch (InvocationTargetException e) {
            r.append(' ').append(e.getCause().getClass().getName());
        }
        h.off();
        h = Hook.on(add, Hook.SKIP);
        try {
            int v = t.add(1);
            r.append(' ').append(v);
        } catch (NullPointerException e) {
            r.append(" NullPointerException");
        }
        h.off();
        h = Hook.on(add, Hook.WRONG);
        try {
            int v = t.add(1);
            r.append(' ').append(v);
        } catch (ClassCastException e) {
            r.append(" ClassCastException");
        }
        h.off();
        h = Hook.on(name, Hook.WRONG);
        try {
            String v = t.name("x");
            r.append(' ').append(v);
        } catch (ClassCastException e) {
            r.append(" ClassCastException");
        }
        h.off();
        r.append(' ').append(t.name("x"));
        return r.toString();
    }

    /**
     * An interface's default method; a static method whose class is initialised before its hook runs; a native method; a
     * void one, called and not; arguments changed by the hook; a second hook of a hooked method and an unhook of one that is
     * not, refused; an abstract method, refused; the natives that have nothing to do; a final class made inheritable:
     * "hello! hello 1042 init hook 1005 5 2 1 1 102 (LT2,I1) true false true truetruetrue truetruefalsetrue".
     */
    public static String more() throws Throwable {
        StringBuilder r = new StringBuilder();
        Hook g = Hook.on(Greeter.class.getDeclaredMethod("greet"), Hook.RESULT);
        r.append(new Greeting().greet());
        g.off();
        r.append(' ').append(new Greeting().greet());
        Hook l = Hook.on(new Peek(Late.class.getDeclaredMethod("answer"), Hook.RESULT));
        r.append(' ').append(Late.answer()).append(' ').append(Hook.events.toString().trim());
        l.off();
        Hook n = Hook.on(Target.class.getDeclaredMethod("nativeAdd", int.class, int.class), Hook.RESULT);
        r.append(' ').append(Target.nativeAdd(2, 3));
        n.off();
        r.append(' ').append(Target.nativeAdd(2, 3));
        Target t = new Target(1);
        Method bump = Target.class.getDeclaredMethod("bump");
        Hook b = Hook.on(bump, Hook.CALL);
        t.bump();
        b.off();
        Hook bs = Hook.on(bump, Hook.SKIP);
        t.bump();
        bs.off();
        r.append(' ').append(t.value).append(' ').append(b.calls).append(' ').append(bs.calls);
        Method add = Target.class.getDeclaredMethod("add", int.class);
        Hook a = Hook.on(add, Hook.ARGS);
        r.append(' ').append(t.add(1)).append(' ').append(a.seen);
        r.append(' ').append(XposedBridge.hook0(new Hook(add, Hook.CALL), add, Hook.CALLBACK) == null);
        a.off();
        r.append(' ').append(XposedBridge.unhook0(add));
        Method run = Runnable.class.getDeclaredMethod("run");
        r.append(' ').append(XposedBridge.hook0(new Hook(run, Hook.CALL), run, Hook.CALLBACK) == null);
        r.append(' ').append(XposedBridge.deoptimize0(add)).append(XposedBridge.disableProfileSaver()).append(XposedBridge.disableHiddenApiRestrictions());
        r.append(' ').append(Modifier.isFinal(Sealed.class.getModifiers()));
        r.append(XposedBridge.makeClassInheritable0(Sealed.class));
        r.append(Modifier.isFinal(Sealed.class.getModifiers()));
        r.append(Modifier.isProtected(Sealed.class.getDeclaredConstructor().getModifiers()));
        return r.toString();
    }

    /** Calls of a method on another thread. */
    private static final class Caller extends Thread {
        final Target t;
        int wrong;

        Caller(Target t) {
            this.t = t;
        }

        @Override
        public void run() {
            for (int i = 0; i < 3000; i++) {
                int v = t.add(i);
                if (v != 1 + i && v != 1 + i + 1000) wrong++;
            }
        }
    }

    /**
     * Two threads calling a method while it is hooked and unhooked again and again: each answer the original's or the hook's,
     * nothing thrown: "0 true".
     */
    public static String threads() throws Throwable {
        Target t = new Target(1);
        Method add = Target.class.getDeclaredMethod("add", int.class);
        Caller one = new Caller(t), two = new Caller(t);
        int hooks = 0;
        one.start();
        two.start();
        while ((one.isAlive() || two.isAlive()) && hooks < 200) {
            Hook h = Hook.on(add, Hook.RESULT);
            hooks++;
            h.off();
        }
        one.join();
        two.join();
        return (one.wrong + two.wrong) + " " + (hooks > 0);
    }
}
