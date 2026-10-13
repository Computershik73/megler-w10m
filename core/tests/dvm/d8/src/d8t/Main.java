package d8t;

/*
 * A program for the Dalvik runtime's tests that a real compiler turns into DEX: javac, then d8
 * (tools/dvm-d8-fixture.sh). Each test is a static method that takes nothing and returns a string. The same
 * class files are run on the JVM, which writes "name=result" for each to expected.txt; the tests then call
 * each method in the interpreter and compare. So what is checked is not what this file's author thought the
 * answer was, but what Java's own implementation says it is.
 *
 * It uses only what the runtime's built-in java.lang has (core/dvm/boot): no java.util, no java.io, and
 * nothing printed that the two would format differently (a default hashCode, the message of an exception the
 * runtime itself throws, a float with many digits).
 */
public class Main {
    interface IntOp {
        int apply(int a, int b);

        default IntOp twice() {
            return (a, b) -> apply(apply(a, b), b);
        }

        static IntOp add() {
            return (a, b) -> a + b;
        }
    }

    interface Shape {
        int area();

        default String describe() {
            return name() + ":" + area();
        }

        String name();
    }

    static abstract class Base implements Shape {
        static int made;
        protected final String label;

        Base(String label) {
            this.label = label;
            made++;
        }

        public String name() {
            return label;
        }

        @Override
        public String toString() {
            return "<" + describe() + ">";
        }
    }

    static class Rect extends Base {
        final int w, h;

        Rect(int w, int h) {
            this("rect", w, h);
        }

        protected Rect(String label, int w, int h) {
            super(label);
            this.w = w;
            this.h = h;
        }

        public int area() {
            return w * h;
        }
    }

    static final class Square extends Rect implements Comparable<Square> {
        Square(int side) {
            super("square", side, side);
        }

        @Override
        public int area() {
            return super.area() + 1000;
        }

        @Override
        public int compareTo(Square o) {
            return w - o.w;
        }

        @Override
        public boolean equals(Object o) {
            return o instanceof Square && ((Square) o).w == w;
        }

        @Override
        public int hashCode() {
            return w * 31;
        }
    }

    enum Color {
        RED(1), GREEN(2), BLUE(4) {
            @Override
            String shout() {
                return "BLUE!";
            }
        };

        final int bit;

        Color(int bit) {
            this.bit = bit;
        }

        String shout() {
            return name().toLowerCase();
        }
    }

    static class Oops extends Exception {
        final int code;

        Oops(String message, int code) {
            super(message);
            this.code = code;
        }
    }

    static class Resource implements AutoCloseable {
        final StringBuilder log;
        final String name;
        final boolean failOnClose;

        Resource(StringBuilder log, String name, boolean failOnClose) {
            this.log = log;
            this.name = name;
            this.failOnClose = failOnClose;
            log.append("open ").append(name).append(';');
        }

        @Override
        public void close() {
            log.append("close ").append(name).append(';');
            if (failOnClose) throw new IllegalStateException("close " + name);
        }
    }

    /* A stack of anything, by hand: generics erase to Object and come back through casts. */
    static class Stack<T> {
        private Object[] items = new Object[2];
        private int count;

        void push(T item) {
            if (count == items.length) {
                Object[] grown = new Object[count * 2];
                System.arraycopy(items, 0, grown, 0, count);
                items = grown;
            }
            items[count++] = item;
        }

        @SuppressWarnings("unchecked")
        T pop() {
            if (count == 0) throw new IllegalStateException("empty");
            T item = (T) items[--count];
            items[count] = null;
            return item;
        }

        int size() {
            return count;
        }
    }

    /* An inner class: it reaches its outer object's private field through what the compiler adds. */
    private int secret = 7;

    class Inner {
        int peek() {
            return secret * 6;
        }
    }

    static int order;
    static final int FIRST = ++order;
    static final String CONSTANT = "const" + 1;
    static final long BIG = 1L << 40;

    public static String arith() {
        int a = 7, b = -3;
        long l = 0x123456789abcdefL;
        StringBuilder sb = new StringBuilder();
        sb.append(a / b).append(',').append(a % b).append(',').append(-a / b).append(',').append(-a % b);
        sb.append(',').append(a << 33).append(',').append(b >> 1).append(',').append(b >>> 28).append(',').append(a ^ b);
        sb.append(',').append(Integer.MIN_VALUE / -1).append(',').append(Integer.MAX_VALUE + 1).append(',').append(Integer.MIN_VALUE % -1);
        sb.append(',').append(l >> 4).append(',').append(l >>> 60).append(',').append(l << 8).append(',').append(l * l).append(',').append(l % 1000);
        sb.append(',').append((int) l).append(',').append((short) l).append(',').append((byte) l).append(',').append((int) (char) l);
        sb.append(',').append((int) 3.99).append(',').append((int) -3.99).append(',').append((long) 1e30).append(',').append((int) Double.NaN);
        sb.append(',').append((int) Float.MAX_VALUE).append(',').append((long) -1e30f).append(',').append((int) (0.1f + 0.2f == 0.3f ? 1 : 0));
        sb.append(',').append(5 / 2.0).append(',').append(1.0 / 4).append(',').append((float) 1 / 8).append(',').append(7.5 % 2);
        int i = 5;
        i += i++ + ++i;
        sb.append(',').append(i);
        char c = 'a';
        c += 2;
        c++;
        sb.append(',').append(c).append(',').append(c + 1).append(',').append((char) (c + 1));
        long big = Long.MAX_VALUE;
        big++;
        sb.append(',').append(big).append(',').append(big == Long.MIN_VALUE).append(',').append(Math.abs(-5) + Math.max(3, 9) + Math.min(-1L, 4L));
        return sb.toString();
    }

    public static String loops() {
        StringBuilder sb = new StringBuilder();
        int sum = 0;
        outer:
        for (int i = 0; i < 10; i++) {
            for (int j = 0; j < 10; j++) {
                if (j == i) continue outer;
                if (i * j > 40) break outer;
                sum += i * j;
            }
        }
        sb.append(sum);
        int n = 27, steps = 0;
        while (n != 1) {
            n = (n & 1) == 0 ? n / 2 : 3 * n + 1;
            steps++;
        }
        sb.append(',').append(steps);
        long f = 1;
        int k = 0;
        do {
            f *= ++k;
        } while (k < 20);
        sb.append(',').append(f);
        /* primes below 100, by a sieve */
        boolean[] composite = new boolean[100];
        int primes = 0;
        for (int p = 2; p < 100; p++) {
            if (composite[p]) continue;
            primes++;
            for (int m = p * p; m < 100; m += p) composite[m] = true;
        }
        sb.append(',').append(primes);
        return sb.toString();
    }

    static String kind(int n) {
        switch (n) {
        case 1:
            return "one";
        case 2:
        case 3:
            return "few";
        case 4:
            return "four";
        case 1000:
            return "thousand";
        case -50000:
            return "negative";
        default:
            return "many";
        }
    }

    static int word(String s) {
        switch (s) {
        case "alpha":
            return 1;
        case "beta":
            return 2;
        case "Aa":
            return 3;
        case "BB":
            return 4;     /* the same hash code as "Aa" */
        default:
            return 0;
        }
    }

    static String tint(Color c) {
        switch (c) {
        case RED:
            return "warm";
        case BLUE:
            return "cold";
        default:
            return "other";
        }
    }

    public static String switches() {
        StringBuilder sb = new StringBuilder();
        int[] ns = { 0, 1, 2, 3, 4, 5, 1000, -50000, Integer.MIN_VALUE };
        for (int n : ns) sb.append(kind(n)).append(' ');
        String[] words = { "alpha", "beta", "Aa", "BB", "gamma", "" };
        for (String w : words) sb.append(word(w));
        for (Color c : Color.values()) sb.append(' ').append(tint(c));
        return sb.toString();
    }

    public static String arrays() {
        StringBuilder sb = new StringBuilder();
        int[] a = { 5, 3, 9, 1, 7, 2, 8 };
        /* insertion sort */
        for (int i = 1; i < a.length; i++) {
            int v = a[i], j = i - 1;
            while (j >= 0 && a[j] > v) {
                a[j + 1] = a[j];
                j--;
            }
            a[j + 1] = v;
        }
        for (int v : a) sb.append(v);
        long[] ls = { 1L << 40, -1L, 3 };
        double[] ds = { 0.5, 0.25 };
        byte[] bs = { (byte) 200, 100, -1 };
        short[] ss = { (short) 40000, 7 };
        char[] cs = { 'x', 'y', 'z' };
        float[] fs = { 1.5f, 2.5f };
        boolean[] zs = new boolean[3];
        zs[1] = true;
        sb.append(',').append(ls[0] + ls[1] + ls[2]).append(',').append((long) ((ds[0] + ds[1]) * 100)).append(',').append(bs[0] + bs[1] + bs[2]);
        sb.append(',').append(ss[0] + ss[1]).append(',').append(new String(cs)).append(',').append((int) (fs[0] * fs[1] * 100)).append(',').append(zs[0]).append(zs[1]);
        int[][] grid = new int[3][];
        for (int r = 0; r < grid.length; r++) {
            grid[r] = new int[r + 2];
            for (int col = 0; col < grid[r].length; col++) grid[r][col] = r * 10 + col;
        }
        int total = 0;
        for (int[] row : grid)
            for (int v : row) total += v;
        sb.append(',').append(total).append(',').append(grid[2].length);
        int[] copy = a.clone();
        copy[0] = 99;
        System.arraycopy(a, 0, a, 2, 3);
        sb.append(',').append(a[0]).append(a[2]).append(a[4]).append(copy[0]);
        Object[] objects = new String[2];
        try {
            objects[0] = Integer.valueOf(1);
            sb.append(",stored");
        } catch (ArrayStoreException e) {
            sb.append(",refused");
        }
        try {
            sb.append(a[a.length]);
        } catch (ArrayIndexOutOfBoundsException e) {
            sb.append(",outside");
        }
        try {
            sb.append(new int[a[0] - 100].length);
        } catch (NegativeArraySizeException e) {
            sb.append(",negative");
        }
        sb.append(',').append(objects instanceof Object[]).append(objects instanceof String[]).append((Object) a instanceof Object[]);
        return sb.toString();
    }

    public static String strings() {
        String s = "Hello, " + "world";
        StringBuilder sb = new StringBuilder();
        sb.append(s.length()).append(s.charAt(4)).append(s.indexOf('o')).append(s.indexOf("world")).append(s.substring(7)).append(s.substring(1, 3));
        sb.append(s.equals("Hello, world")).append(s == "Hello, world").append(s.compareTo("Hello")).append("abc".compareTo("abd"));
        sb.append(s.hashCode()).append(' ').append("".hashCode()).append(' ').append(s.toUpperCase()).append(s.startsWith("Hell")).append(s.endsWith("x"));
        String t = "";
        for (int i = 0; i < 5; i++) t += i;
        t += 'c';
        t += 1.5;
        t += true;
        t += null;
        t += 10L;
        sb.append(' ').append(t);
        String reversed = "";
        for (int i = s.length() - 1; i >= 0; i--) reversed += s.charAt(i);
        sb.append(' ').append(reversed);
        char[] chars = { 'd', 'e', 'x' };
        String made = new String(chars);
        sb.append(' ').append(made).append(made == "dex").append(made.equals("dex")).append(made.intern() == "dex").append(new String(chars, 1, 2));
        String unicode = "é中😀";
        sb.append(' ').append(unicode.length()).append((int) unicode.charAt(1)).append((int) unicode.charAt(3)).append(unicode.hashCode());
        sb.append(' ').append(Integer.parseInt("-12345") + 1).append(Integer.toHexString(-1)).append(String.valueOf(3.0)).append(CONSTANT);
        return sb.toString();
    }

    public static String objects() {
        StringBuilder sb = new StringBuilder();
        int before = Base.made;
        Shape[] shapes = { new Rect(3, 4), new Square(5), new Shape() {
            public int area() {
                return 42;
            }

            public String name() {
                return "anonymous";
            }
        } };
        for (Shape s : shapes) sb.append(s.describe()).append(' ');
        sb.append(shapes[0]).append(Base.made - before);
        Square a = new Square(2), b = new Square(2), c = new Square(9);
        sb.append(' ').append(a.equals(b)).append(a == b).append(a.equals(c)).append(a.hashCode()).append(a.compareTo(c));
        Comparable<Square> cmp = c;
        sb.append(cmp.compareTo(a));
        Object o = a;
        sb.append(o instanceof Shape).append(o instanceof Rect).append(o instanceof Comparable).append(o instanceof String);
        sb.append(' ').append(o.getClass().getName()).append(' ').append(shapes[2].getClass().getName()).append(' ').append(o.getClass().getSuperclass().getName());
        try {
            sb.append(((Square) (Object) shapes[0]).w);
        } catch (ClassCastException e) {
            sb.append(" not-a-square");
        }
        sb.append(' ').append(new Main().new Inner().peek());
        return sb.toString();
    }

    public static String enums() {
        StringBuilder sb = new StringBuilder();
        for (Color c : Color.values()) sb.append(c).append(c.ordinal()).append(c.bit).append(c.shout()).append(' ');
        sb.append(Color.valueOf("GREEN") == Color.GREEN).append(Color.RED.compareTo(Color.BLUE)).append(Color.BLUE.name());
        try {
            Color.valueOf("PURPLE");
        } catch (IllegalArgumentException e) {
            sb.append(" none");
        }
        sb.append(' ').append(Color.BLUE.getClass() == Color.class).append(Color.RED.getClass() == Color.class);
        return sb.toString();
    }

    static int depth(int n) throws Oops {
        if (n == 0) throw new Oops("bottom", 77);
        try {
            return depth(n - 1) + 1;
        } finally {
            order += 100;
        }
    }

    @SuppressWarnings("finally")
    static int finallyWins() {
        try {
            throw new RuntimeException("lost");
        } finally {
            return 5;
        }
    }

    static String classify(int which) {
        try {
            switch (which) {
            case 0:
                return "" + 1 / (which * 0);
            case 1:
                return ((Object) "text" instanceof String ? (String) null : "").trim();
            case 2:
                return "" + new int[which - 5].length;
            case 3:
                return (String) (Object) Integer.valueOf(3);
            case 4:
                throw new Oops("mine", 4);
            case 5:
                throw new Error("an error");
            default:
                return "nothing";
            }
        } catch (ArithmeticException | NullPointerException e) {
            return "A:" + e.getClass().getName();
        } catch (RuntimeException e) {
            return "R:" + e.getClass().getName();
        } catch (Oops e) {
            return "O:" + e.getMessage() + e.code;
        } catch (Throwable e) {
            return "T:" + e;
        }
    }

    public static String exceptions() {
        StringBuilder sb = new StringBuilder();
        int saved = order;
        try {
            depth(25);
        } catch (Oops e) {
            sb.append(e.getMessage()).append(e.code).append(order - saved).append(e);
        }
        sb.append(' ').append(finallyWins());
        for (int i = 0; i < 7; i++) sb.append(' ').append(classify(i));
        try {
            try {
                throw new IllegalStateException("inner");
            } catch (IllegalStateException e) {
                throw new RuntimeException("outer", e);
            }
        } catch (RuntimeException e) {
            sb.append(' ').append(e.getMessage()).append('<').append(e.getCause().getMessage()).append(e.getCause() instanceof IllegalStateException);
        }
        StringBuilder log = new StringBuilder();
        try (Resource one = new Resource(log, "one", false); Resource two = new Resource(log, "two", true)) {
            log.append("body;");
            throw new Oops("in body", 1);
        } catch (Oops e) {
            log.append("caught ").append(e.getMessage());
        }
        try (Resource three = new Resource(log, "three", true)) {
            log.append(";body3;");
        } catch (IllegalStateException e) {
            log.append("caught ").append(e.getMessage());
        }
        sb.append(' ').append(log);
        return sb.toString();
    }

    static int twice(int a, int b) {
        return 2 * (a + b);
    }

    static int sum(int... values) {
        int total = 0;
        for (int v : values) total += v;
        return total;
    }

    static <T extends Comparable<T>> T largest(T a, T b) {
        return a.compareTo(b) >= 0 ? a : b;
    }

    public static String lambdas() {
        StringBuilder sb = new StringBuilder();
        int captured = 10;
        IntOp plus = IntOp.add();
        IntOp scaled = (a, b) -> (a + b) * captured;
        IntOp ref = Main::twice;
        IntOp anonymous = new IntOp() {
            public int apply(int a, int b) {
                return a * b - captured;
            }
        };
        sb.append(plus.apply(2, 3)).append(',').append(scaled.apply(2, 3)).append(',').append(ref.apply(2, 3)).append(',').append(anonymous.apply(2, 3));
        sb.append(',').append(plus.twice().apply(2, 3)).append(',').append(scaled.twice().twice().apply(1, 1));
        Runnable[] runs = new Runnable[3];
        int[] cell = { 0 };
        for (int i = 0; i < runs.length; i++) {
            int step = i + 1;
            runs[i] = () -> cell[0] += step * step;
        }
        for (Runnable r : runs) r.run();
        sb.append(',').append(cell[0]).append(',').append(sum()).append(sum(1)).append(sum(1, 2, 3));
        sb.append(',').append(largest("pear", "apple")).append(largest(new Square(3), new Square(8)).w);
        Stack<String> stack = new Stack<>();
        for (int i = 0; i < 5; i++) stack.push("s" + i);
        String top = stack.pop();
        sb.append(',').append(top).append(stack.size()).append(stack.pop().length());
        Stack<Integer> numbers = new Stack<>();
        numbers.push(40);
        numbers.push(2);
        int x = numbers.pop() + numbers.pop();
        Object boxed = 3.5;
        Long wide = 1L << 35;
        Character ch = 'q';
        Boolean yes = x == 42;
        sb.append(',').append(x).append(boxed instanceof Double).append(((Number) boxed).intValue()).append(wide).append(ch).append(yes);
        sb.append(boxed.equals(3.5)).append(Integer.valueOf(1000).equals(1000)).append(Integer.valueOf(7).hashCode());
        try {
            numbers.pop();
        } catch (IllegalStateException e) {
            sb.append(',').append(e.getMessage());
        }
        return sb.toString();
    }

    static long gcd(long a, long b) {
        return b == 0 ? a : gcd(b, a % b);
    }

    static int ackermann(int m, int n) {
        return m == 0 ? n + 1 : n == 0 ? ackermann(m - 1, 1) : ackermann(m - 1, ackermann(m, n - 1));
    }

    static synchronized int locked(Object lock, int n) {
        synchronized (lock) {
            synchronized (Main.class) {
                return n + 1;
            }
        }
    }

    public static String numbers() {
        StringBuilder sb = new StringBuilder();
        sb.append(gcd(1071, 462)).append(',').append(gcd(1L << 40, 1L << 23)).append(',').append(ackermann(2, 3)).append(',').append(locked(sb, 41));
        /* a checksum over a table, all in ints that wrap */
        int crc = 0xFFFFFFFF;
        byte[] data = "The quick brown fox".getBytes();
        for (byte value : data) {
            crc ^= value & 0xFF;
            for (int k = 0; k < 8; k++) crc = (crc >>> 1) ^ (0xEDB88320 & -(crc & 1));
        }
        sb.append(',').append(Integer.toHexString(~crc));
        /* a square root by Newton's method, and what doubles do at the edges */
        double x = 2, guess = 1;
        for (int i = 0; i < 30; i++) guess = (guess + x / guess) / 2;
        sb.append(',').append((long) (guess * 1e9)).append(',').append(guess == Math.sqrt(2));
        double zero = 0;
        sb.append(',').append(1 / zero > Double.MAX_VALUE).append(-1 / zero < 0).append(zero / zero != zero / zero).append(0.0 == -0.0);
        sb.append(',').append((float) 16777217).append(',').append((double) (float) 0.1 == 0.1).append(',').append(1e10f).append(',').append(100.0 / 3 > 33.33);
        long h = 1125899906842597L;
        for (int i = 0; i < 1000; i++) h = 31 * h + i * 7919 % 13;
        sb.append(',').append(h).append(',').append(FIRST).append(',').append(BIG);
        return sb.toString();
    }

    public static void main(String[] args) {
        System.out.println("arith=" + arith());
        System.out.println("loops=" + loops());
        System.out.println("switches=" + switches());
        System.out.println("arrays=" + arrays());
        System.out.println("strings=" + strings());
        System.out.println("objects=" + objects());
        System.out.println("enums=" + enums());
        System.out.println("exceptions=" + exceptions());
        System.out.println("lambdas=" + lambdas());
        System.out.println("numbers=" + numbers());
    }
}
