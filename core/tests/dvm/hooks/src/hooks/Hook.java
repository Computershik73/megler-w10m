package hooks;

import de.robv.android.xposed.XposedBridge;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

/**
 * A hook's context, as Aliuhook's XposedBridge.HookInfo is one: the runtime calls callback(args) for each call of the hooked
 * method -- args[0] the object, for an instance method -- and the backup hook0 handed back runs the original.
 */
public class Hook {
    /**
     * What the callback does: call the original; add 1000 to its result (or "!" to a string); call it with 100 for its first
     * argument; throw; return null without calling it; return something of the wrong type; count the call in `counted`.
     */
    public static final int CALL = 0, RESULT = 1, ARGS = 2, THROW = 3, SKIP = 4, WRONG = 5, COUNT = 6;
    static final Method CALLBACK;

    static {
        try {
            CALLBACK = Hook.class.getMethod("callback", Object[].class);
        } catch (NoSuchMethodException e) {
            throw new RuntimeException(e);
        }
    }

    /** Calls of hooks made with COUNT, whose contexts nothing but the runtime keeps. */
    static int counted;
    /** What happened in order, where the order is what is tested. */
    static final StringBuilder events = new StringBuilder();

    final Member method;
    final boolean isStatic;
    final int mode;
    Method backup;
    int calls;
    final StringBuilder seen = new StringBuilder();

    Hook(Member method, int mode) {
        this.method = method;
        this.mode = mode;
        isStatic = Modifier.isStatic(method.getModifiers());
    }

    static Hook on(Member method, int mode) {
        return on(new Hook(method, mode));
    }

    static Hook on(Hook h) {
        h.backup = XposedBridge.hook0(h, h.method, CALLBACK);
        if (h.backup == null) throw new IllegalStateException("not hooked: " + h.method);
        return h;
    }

    boolean off() {
        return XposedBridge.unhook0(method);
    }

    public Object callback(Object[] args) throws Throwable {
        int skip = isStatic ? 0 : 1;
        Object self = isStatic ? null : args[0];
        Object[] rest = new Object[args.length - skip];
        System.arraycopy(args, skip, rest, 0, rest.length);
        calls++;
        if (mode == COUNT) {
            counted++;
            return original(self, rest);
        }
        if (seen.length() < 600) {
            seen.append('(');
            for (int i = 0; i < args.length; i++) seen.append(i == 0 ? "" : ",").append(text(args[i]));
            seen.append(')');
        }
        switch (mode) {
        case RESULT: {
            Object r = original(self, rest);
            if (r instanceof Integer) return (Integer) r + 1000;
            if (r instanceof String) return r + "!";
            return r;
        }
        case ARGS:
            rest[0] = 100;
            return original(self, rest);
        case THROW:
            throw new IllegalStateException("from the hook");
        case SKIP:
            return null;
        case WRONG:
            return method instanceof Method && ((Method) method).getReturnType() == String.class ? (Object) 5 : "five";
        default:
            return original(self, rest);
        }
    }

    /** The original, through the backup, as Aliuhook's callback calls it: what it throws is thrown as it is. */
    Object original(Object self, Object[] rest) throws Throwable {
        try {
            return backup.invoke(self, rest);
        } catch (InvocationTargetException e) {
            throw e.getCause();
        }
    }

    /** An argument as the tests compare it: its box's kind and its value (a float's and a double's in tenths). */
    static String text(Object o) {
        if (o == null) return "null";
        if (o instanceof Boolean) return "Z" + o;
        if (o instanceof Byte) return "B" + o;
        if (o instanceof Character) return "C" + o;
        if (o instanceof Short) return "S" + o;
        if (o instanceof Integer) return "I" + o;
        if (o instanceof Long) return "J" + o;
        if (o instanceof Float) return "F" + (int) ((Float) o * 10);
        if (o instanceof Double) return "D" + (long) ((Double) o * 10);
        return "L" + o;
    }
}

/** A hook that notes when it is called, after what Late's initialiser noted: the class is initialised before the hook runs. */
final class Peek extends Hook {
    Peek(Member method, int mode) {
        super(method, mode);
    }

    @Override
    public Object callback(Object[] args) throws Throwable {
        events.append("hook ");
        return super.callback(args);
    }
}
