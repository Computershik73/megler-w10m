package bench;

/**
 * What the interpreter does in place of a call must be what the call does, at the edges too (core/tests/hk_dvm_tests_libcore.c
 * checks the text run() returns, which is the JVM's answer for each case): the intrinsics, where each declines and the
 * method itself runs and throws; and the trivial methods, where a getter's class must still be initialised first.
 */
public final class Edges {
    private Edges() {}

    static final class Box {
        int value;
        long wide;
        Object ref;
        byte small;
        char letter;
        Box() {}
        int value() { return value; }
        long wide() { return wide; }
        Object ref() { return ref; }
        byte small() { return small; }
        char letter() { return letter; }
        void setValue(int v) { value = v; }
        void setRef(Object o) { ref = o; }
        void setSmall(byte b) { small = b; }
        void setLetter(char c) { letter = c; }
        boolean no() { return false; }
        int seven() { return 7; }
        int minusThree() { return -3; }
        void nothing() {}
    }
    static class Late {
        static int initialised;
        static int counter = 41;
        static { initialised++; }
        static int counter() { return counter; }
    }
    static class Other {
        static int read() { return Late.counter; }
    }
    static final class Holder {
        static int seen;
        static int seen() { return seen; }
    }

    private static String name(RuntimeException e) { return e.getClass().getSimpleName() + ": " + e.getMessage(); }

    /* Each case that throws, in a method of its own (no lambdas: javac against android.jar makes none). */
    private static String charAtOf(String s, int i) {
        try { return String.valueOf(s.charAt(i)); } catch (RuntimeException e) { return name(e); }
    }
    private static String copy(Object src, int sp, Object dst, int dp, int n) {
        try { System.arraycopy(src, sp, dst, dp, n); return "none"; } catch (RuntimeException e) { return name(e); }
    }
    private static String valueOf(Box b) {
        try { return String.valueOf(b.value()); } catch (RuntimeException e) { return name(e); }
    }

    public static String run() {
        StringBuilder sb = new StringBuilder();
        String s = "abc";
        sb.append("charAt ").append(s.charAt(0)).append(s.charAt(2)).append(' ');
        sb.append(charAtOf(s, 3)).append(" | ").append(charAtOf(s, -1)).append('\n');
        sb.append("length ").append(s.length()).append(' ').append("".length()).append(' ').append("".isEmpty()).append(s.isEmpty()).append('\n');
        String t = new String(new char[] { 'a', 'b', 'c' });
        Object o = t;
        sb.append("equals ").append(s.equals(t)).append(s.equals(s)).append(s.equals(null)).append(s.equals(new StringBuilder("abc")))
          .append(s.equals("abd")).append(s.equals("ab")).append(o.equals(s)).append('\n');
        sb.append("hashCode ").append("".hashCode()).append(' ').append(s.hashCode()).append(' ').append(t.hashCode()).append(' ')
          .append("Ā￿".hashCode()).append('\n');
        int[] a = { 1, 2, 3, 4, 5 };
        System.arraycopy(a, 0, a, 1, 4);
        sb.append("arraycopy ").append(a[0]).append(a[1]).append(a[2]).append(a[3]).append(a[4]).append(' ');
        Object[] objs = new Object[2];
        String[] strs = { "x", "y" };
        System.arraycopy(strs, 0, objs, 0, 2);
        sb.append(objs[1]).append(' ');
        sb.append(copy(new Object[] { 1 }, 0, new String[1], 0, 1)).append(" | ");
        sb.append(copy(new int[2], 0, new long[2], 0, 1)).append(" | ");
        sb.append(copy(new int[2], 1, new int[2], 0, 2)).append(" | ");
        sb.append(copy(null, 0, new int[2], 0, 0)).append('\n');
        sb.append("math ").append(Math.min(-5, 3)).append(' ').append(Math.max(-5, 3)).append(' ').append(Math.abs(Integer.MIN_VALUE)).append(' ')
          .append(Math.abs(-9)).append(' ').append(Math.min(Long.MIN_VALUE, 0L)).append(' ').append(Math.max(1L << 40, 3L)).append(' ')
          .append(Math.abs(Long.MIN_VALUE)).append(' ').append(Math.sqrt(2.0)).append(' ').append(Math.sqrt(-1.0)).append('\n');
        sb.append("getClass ").append(s.getClass().getName()).append(' ').append(new Box().getClass().getSimpleName()).append(' ')
          .append(Thread.currentThread() == Thread.currentThread()).append('\n');
        Box b = new Box();
        b.setValue(-12);
        b.setRef("r");
        b.setSmall((byte) -2);
        b.setLetter('é');
        b.wide = 1L << 35;
        b.nothing();
        sb.append("trivial ").append(b.value()).append(' ').append(b.ref()).append(' ').append(b.small()).append(' ').append((int) b.letter())
          .append(' ').append(b.wide()).append(' ').append(b.no()).append(' ').append(b.seven()).append(' ').append(b.minusThree()).append(' ');
        // Late's static field read through Other's trivial getter while Late is not initialised yet: Late's initialiser runs first.
        sb.append(Other.read()).append(' ').append(Late.initialised).append(' ').append(Late.counter()).append(' ').append(Holder.seen()).append(' ');
        sb.append(valueOf(null)).append('\n');
        return sb.toString();
    }
}
