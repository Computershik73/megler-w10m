package d8t;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.text.DecimalFormat;
import java.text.DecimalFormatSymbols;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.BitSet;
import java.util.Collections;
import java.util.Comparator;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.PriorityQueue;
import java.util.Random;
import java.util.TimeZone;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Collectors;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/*
 * The second program of the Dalvik runtime's tests that a real compiler makes (tools/dvm-d8-fixture.sh): the
 * same arrangement as Main.java, but on java.util, java.io and java.text, which means on Android's own
 * libcore. Each test is a static method that takes nothing and returns a string; the JVM's answers are
 * lib-expected.txt.
 *
 * The tests whose names begin with "pure" need nothing but libcore's bytecode and what a runtime does for
 * it. The ones that begin with "lib" need libcore's native libraries as well -- ICU for regular expressions,
 * number and date formats and character sets, the C library for files, zlib, BoringSSL's big numbers -- and
 * are run only where those are loaded.
 *
 * Where Android's libcore does not say what the JVM says, the difference is in lib-android.txt beside the
 * expected answers, with the reason.
 */
public class Lib {
    /* A folder the tests may write in: set by whoever runs them (the JVM's run uses the system's temporary folder). */
    public static String dir = System.getProperty("java.io.tmpdir");

    /* The words of a text, without String.split: that is a regular expression, and those are ICU's. */
    static List<String> words(String text, char separator) {
        List<String> out = new ArrayList<>();
        int from = 0;
        for (int at = text.indexOf(separator); at >= 0; at = text.indexOf(separator, from)) {
            out.add(text.substring(from, at));
            from = at + 1;
        }
        out.add(text.substring(from));
        return out;
    }

    public static String pure_lists() {
        StringBuilder sb = new StringBuilder();
        List<String> list = new ArrayList<>();
        for (String s : words("pear,apple,fig,banana,kiwi,cherry", ',')) list.add(s);
        Collections.sort(list);
        sb.append(list).append(list.indexOf("fig")).append(list.contains("plum"));
        Collections.sort(list, new Comparator<String>() {
            public int compare(String a, String b) {
                return a.length() != b.length() ? a.length() - b.length() : b.compareTo(a);
            }
        });
        sb.append(list);
        list.remove("fig");
        list.remove(0);
        list.add(1, "plum");
        sb.append(list.subList(1, 3)).append(list.size());
        for (Iterator<String> it = list.iterator(); it.hasNext();) if (it.next().length() == 6) it.remove();
        sb.append(list);
        LinkedList<Integer> linked = new LinkedList<>();
        for (int i = 0; i < 6; i++) {
            if ((i & 1) == 0) linked.addFirst(i);
            else linked.addLast(i);
        }
        sb.append(linked).append(linked.pollFirst()).append(linked.peekLast());
        int[] numbers = { 5, -3, 99, 0, 42, 7, -100, 13 };
        Arrays.sort(numbers);
        sb.append(Arrays.toString(numbers)).append(Arrays.binarySearch(numbers, 42));
        Integer[] boxed = { 3, 1, 2 };
        Arrays.sort(boxed, Collections.reverseOrder());
        sb.append(Arrays.asList(boxed));
        List<Integer> many = new ArrayList<>();
        Random random = new Random(42);
        for (int i = 0; i < 1000; i++) many.add(random.nextInt(1000000));
        Collections.sort(many);
        sb.append(many.get(0)).append(',').append(many.get(500)).append(',').append(many.get(999));
        Collections.shuffle(many, new Random(7));
        sb.append(',').append(many.get(0));
        ArrayDeque<String> deque = new ArrayDeque<>();
        deque.push("a");
        deque.addLast("b");
        deque.push("c");
        sb.append(deque).append(deque.pop());
        PriorityQueue<Integer> queue = new PriorityQueue<>(Arrays.asList(5, 1, 4, 2, 3));
        while (!queue.isEmpty()) sb.append(queue.poll());
        try {
            Collections.unmodifiableList(list).add("x");
        } catch (UnsupportedOperationException e) {
            sb.append("fixed");
        }
        try {
            list.get(99);
        } catch (IndexOutOfBoundsException e) {
            sb.append("outside");
        }
        return sb.toString();
    }

    public static String pure_maps() {
        StringBuilder sb = new StringBuilder();
        Map<String, Integer> counts = new HashMap<>();
        String text = "the quick brown fox jumps over the lazy dog and the quick cat";
        for (String word : words(text, ' ')) counts.put(word, counts.containsKey(word) ? counts.get(word) + 1 : 1);
        sb.append(new TreeMap<>(counts)).append(counts.size()).append(counts.get("the")).append(counts.get("none"));
        Map<Integer, String> big = new HashMap<>();
        for (int i = 0; i < 5000; i++) big.put(i * 31, "v" + i);
        int found = 0;
        for (int i = 0; i < 5000; i++) if (("v" + i).equals(big.get(i * 31))) found++;
        for (int i = 0; i < 5000; i += 2) big.remove(i * 31);
        sb.append(found).append(',').append(big.size()).append(big.containsKey(62)).append(big.containsKey(31));
        LinkedHashMap<String, Integer> ordered = new LinkedHashMap<>();
        ordered.put("z", 1);
        ordered.put("a", 2);
        ordered.put("m", 3);
        ordered.put("z", 4);
        sb.append(ordered).append(ordered.keySet()).append(ordered.values());
        TreeMap<String, Integer> tree = new TreeMap<>(ordered);
        sb.append(tree.firstKey()).append(tree.headMap("m")).append(tree.ceilingKey("b")).append(tree.descendingMap());
        HashSet<Integer> set = new HashSet<>(Arrays.asList(3, 1, 4, 1, 5, 9, 2, 6, 5, 3));
        TreeSet<Integer> sorted = new TreeSet<>(set);
        sb.append(set.size()).append(sorted).append(sorted.first()).append(sorted.higher(5));
        for (Map.Entry<String, Integer> e : ordered.entrySet()) e.setValue(e.getValue() * 10);
        sb.append(ordered);
        ConcurrentHashMap<String, Integer> concurrent = new ConcurrentHashMap<>();
        for (int i = 0; i < 100; i++) concurrent.put("k" + (i % 10), i);
        sb.append(concurrent.size()).append(concurrent.get("k7")).append(concurrent.putIfAbsent("k7", 0)).append(concurrent.remove("k3"));
        BitSet bits = new BitSet();
        for (int i = 0; i < 100; i += 7) bits.set(i);
        sb.append(bits.cardinality()).append(bits.nextSetBit(50)).append(bits);
        sb.append(Objects.hash(1, "two", 3.0)).append(Objects.equals(null, null)).append(Optional.ofNullable((String) null).orElse("none"));
        return sb.toString();
    }

    public static String pure_strings() {
        StringBuilder sb = new StringBuilder("hello world");
        sb.insert(0, ">> ").reverse().setCharAt(0, 'D');
        sb.deleteCharAt(1).append(3.5f).append(1e-5).append(100.0).append(1.0 / 3).append((Object) null).append(new char[] { 'x', 'y' });
        sb.append(' ').append(Integer.toString(255, 16)).append(Integer.toBinaryString(10)).append(Long.toHexString(-1L)).append(Integer.toOctalString(8));
        sb.append(' ').append(Integer.parseInt("-7f", 16)).append(Long.parseLong("9223372036854775807")).append(Double.parseDouble("3.25e2")).append(Float.parseFloat("0.1"));
        sb.append(' ').append(Integer.valueOf(127) == Integer.valueOf(127)).append(Integer.MAX_VALUE + 1).append(Long.MIN_VALUE).append(Double.MAX_VALUE).append(Float.MIN_VALUE);
        sb.append(' ').append(Double.toString(0.1 + 0.2)).append(Double.valueOf(1e21)).append((float) 1e10).append(Double.NaN).append(-0.0).append(1.0f / 0);
        sb.append(' ').append(Integer.compare(3, 5)).append(Long.signum(-9L)).append(Integer.bitCount(255)).append(Integer.reverse(1)).append(Long.numberOfTrailingZeros(64));
        sb.append(' ').append(Character.toUpperCase('q')).append(Character.getNumericValue('7')).append((int) Character.forDigit(11, 16));
        sb.append(' ').append("  padded ".trim()).append("Mississippi".replace("ss", "SS")).append("abc".contains("bc"));
        sb.append(' ').append(String.join("-", "a", "b", "c")).append("x".compareToIgnoreCase("X")).append("hello".lastIndexOf('l')).append("Hello".equalsIgnoreCase("hELLO"));
        sb.append(' ').append("%d%%".length()).append(String.valueOf(new char[] { 'o', 'k' })).append("tEsT".toLowerCase()).append("straße".length());
        return sb.toString();
    }

    static class Point implements Comparable<Point> {
        final int x, y;

        Point(int x, int y) {
            this.x = x;
            this.y = y;
        }

        @Override
        public boolean equals(Object o) {
            return o instanceof Point && ((Point) o).x == x && ((Point) o).y == y;
        }

        @Override
        public int hashCode() {
            return Objects.hash(x, y);
        }

        @Override
        public int compareTo(Point o) {
            return x != o.x ? Integer.compare(x, o.x) : Integer.compare(y, o.y);
        }

        @Override
        public String toString() {
            return "(" + x + "," + y + ")";
        }
    }

    public static String pure_streams() {
        StringBuilder sb = new StringBuilder();
        List<Integer> numbers = Arrays.asList(5, 3, 8, 1, 9, 2, 7);
        sb.append(numbers.stream().filter(n -> n % 2 == 1).map(n -> n * n).reduce(0, Integer::sum));
        sb.append(numbers.stream().sorted().map(String::valueOf).collect(Collectors.joining(",", "[", "]")));
        sb.append(numbers.stream().mapToInt(Integer::intValue).max().getAsInt()).append(numbers.stream().anyMatch(n -> n > 8));
        List<Point> points = new ArrayList<>();
        for (int i = 0; i < 6; i++) points.add(new Point(i % 3, 5 - i));
        Collections.sort(points);
        sb.append(points).append(new HashSet<>(points).contains(new Point(2, 0))).append(points.stream().map(p -> p.x).distinct().count());
        Map<Integer, List<Point>> byX = points.stream().collect(Collectors.groupingBy(p -> p.x, TreeMap::new, Collectors.toList()));
        sb.append(byX);
        numbers.forEach(n -> sb.append(n));
        Map<String, Integer> map = new TreeMap<>();
        map.put("a", 1);
        map.merge("a", 5, Integer::sum);
        map.computeIfAbsent("b", k -> 7);
        map.forEach((k, v) -> sb.append(k).append(v));
        sb.append(Optional.of("x").map(s -> s + "y").get());
        return sb.toString();
    }

    /** What pure_reflect looks at from outside: fields, constructors and methods of several kinds. */
    public static class Target implements Comparable<Target> {
        public int a = 3;
        private String s = "x";
        public static long count;

        public Target() {
            count++;
        }

        public Target(int a, String s) {
            this.a = a;
            this.s = s;
            count++;
        }

        public int twice(int v) {
            return 2 * v;
        }

        public double mix(byte b, char c, long l, float f, double d, boolean z, short h) {
            return b + c + l + f + d + (z ? 1 : 0) + h;
        }

        private static String join(String x, Object y) {
            return x + y;
        }

        public void fail() {
            throw new IllegalStateException("on purpose");
        }

        @Override
        public int compareTo(Target other) {
            return Integer.compare(a, other.a);
        }
    }

    /** An annotation reflection can see, with every kind of element and a default for most. */
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
    public @interface Mark {
        String value();

        int number() default 7;

        long big() default 1L << 40;

        double ratio() default 0.5;

        boolean flag() default true;

        char letter() default 'q';

        byte small() default -3;

        Class<?> type() default Object.class;

        Thread.State state() default Thread.State.NEW;

        String[] names() default { "a", "b" };

        int[] numbers() default { 1, 2, 3 };

        Tag tag() default @Tag(id = 5);
    }

    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.RUNTIME)
    public @interface Tag {
        int id();
    }

    /** One the compiler keeps and reflection must not see. */
    @java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.CLASS)
    public @interface Hidden {
    }

    @Mark(value = "on the class", number = 11, type = String.class, state = Thread.State.RUNNABLE, names = { "x" }, numbers = {}, tag = @Tag(id = 9), small = 100, letter = 'Z', ratio = -2.25,
            flag = false, big = -5)
    @Hidden
    public static class Marked extends ArrayList<String> implements Comparable<Marked> {
        @Mark("on the field")
        public Map<String, List<Integer>> field;

        @Mark("on the method")
        @Deprecated
        public <T extends Number> List<T> method(@Tag(id = 1) T first, int second, @Tag(id = 3) @Mark("on a parameter") List<? super T> third) throws IOException, IllegalStateException {
            return null;
        }

        public Marked() {
        }

        @Override
        public int compareTo(Marked o) {
            return 0;
        }
    }

    private static String mark(Mark m) {
        if (m == null) return "none";
        return m.value() + "/" + m.number() + "/" + m.big() + "/" + m.ratio() + "/" + m.flag() + "/" + m.letter() + "/" + m.small() + "/" + m.type().getName() + "/" + m.state() + "/"
                + Arrays.toString(m.names()) + "/" + Arrays.toString(m.numbers()) + "/" + m.tag().id();
    }

    /** Annotations as reflection reads them, and what a DEX file keeps as annotations of its own: nesting, generic signatures, declared exceptions. */
    public static String pure_annotations() throws Exception {
        StringBuilder sb = new StringBuilder();
        Class<?> c = Marked.class;
        sb.append(mark(c.getAnnotation(Mark.class))).append(' ').append(c.isAnnotationPresent(Mark.class)).append(c.isAnnotationPresent(Hidden.class)).append(c.isAnnotationPresent(Tag.class))
                .append(c.getAnnotations().length).append(c.getDeclaredAnnotations().length);
        sb.append(' ').append(mark(c.getField("field").getAnnotation(Mark.class)));
        java.lang.reflect.Method method = c.getMethod("method", Number.class, int.class, List.class);
        sb.append(' ').append(mark(method.getAnnotation(Mark.class))).append(method.isAnnotationPresent(Deprecated.class)).append(method.getDeclaredAnnotations().length);
        java.lang.annotation.Annotation[][] params = method.getParameterAnnotations();
        sb.append(' ').append(params.length).append(params[0].length).append(params[1].length).append(params[2].length).append(((Tag) params[0][0]).id());
        for (java.lang.annotation.Annotation a : params[2]) if (a instanceof Mark) sb.append(((Mark) a).value());
        Mark m = c.getAnnotation(Mark.class);
        sb.append(' ').append(m.annotationType().getSimpleName()).append(m.equals(c.getAnnotation(Mark.class))).append(m.equals(method.getAnnotation(Mark.class)))
                .append(m.hashCode() == c.getAnnotation(Mark.class).hashCode()).append(Mark.class.getMethod("number").getDefaultValue()).append(Mark.class.getMethod("value").getDefaultValue());
        sb.append(' ').append(Arrays.toString(method.getExceptionTypes()).replace("class ", "")).append(c.getConstructor().getExceptionTypes().length);

        sb.append(' ').append(c.getSimpleName()).append(',').append(c.getDeclaringClass().getSimpleName()).append(',').append(c.getEnclosingClass() == Lib.class).append(c.isMemberClass())
                .append(c.isAnonymousClass()).append(java.lang.reflect.Modifier.isStatic(c.getModifiers())).append(Lib.class.getDeclaringClass());
        Object anonymous = new Runnable() {
            @Override
            public void run() {
            }
        };
        Class<?> a = anonymous.getClass();
        sb.append(' ').append(a.isAnonymousClass()).append('[').append(a.getSimpleName()).append(']').append(a.getEnclosingClass() == Lib.class).append(a.getDeclaringClass())
                .append(a.getEnclosingMethod().getName()).append(a.isMemberClass());
        boolean found = false;
        for (Class<?> member : Lib.class.getDeclaredClasses()) found |= member == Marked.class;
        sb.append(found).append(Thread.State.class.getSimpleName()).append(Map.Entry.class.getDeclaringClass().getName());

        sb.append(' ').append(c.getGenericSuperclass()).append('|').append(c.getGenericInterfaces()[0]).append('|').append(c.getField("field").getGenericType()).append('|')
                .append(method.getGenericReturnType()).append('|').append(method.getTypeParameters()[0]).append('|').append(Arrays.toString(method.getGenericParameterTypes()));
        return sb.toString();
    }

    public interface Shape {
        int area(int scale);

        String name();

        long big(long a, double b);

        default String describe() {
            return name() + ":" + area(1);
        }
    }

    /** Reflection on the program's own classes, a proxy, a thread's own variable, references: what a framework does to an app's classes. */
    public static String pure_reflect() throws Exception {
        StringBuilder sb = new StringBuilder();
        Class<?> c = Class.forName("d8t.Lib$Target");
        Object t = c.newInstance();
        Object u = c.getConstructor(int.class, String.class).newInstance(7, "seven");
        sb.append(c.getName()).append(' ').append(c.getField("a").getInt(u)).append(c.getField("a").get(t)).append(Target.count);
        java.lang.reflect.Field f = c.getDeclaredField("s");
        f.setAccessible(true);
        sb.append(f.get(u)).append(f.getType().getName());
        f.set(t, "y");
        sb.append(f.get(t));
        c.getField("a").setInt(t, 40);
        c.getField("count").setLong(null, 100L);
        sb.append(' ').append(((Target) t).a).append(Target.count);
        sb.append(' ').append(c.getMethod("twice", int.class).invoke(u, 21));
        sb.append(c.getMethod("mix", byte.class, char.class, long.class, float.class, double.class, boolean.class, short.class).invoke(u, (byte) 1, 'A', 1000L, 0.5f, 0.25, true, (short) 7));
        java.lang.reflect.Method join = c.getDeclaredMethod("join", String.class, Object.class);
        join.setAccessible(true);
        sb.append(join.invoke(null, "j", 5)).append(java.lang.reflect.Modifier.isStatic(join.getModifiers())).append(join.getReturnType().getSimpleName());
        try {
            c.getMethod("twice", int.class).invoke(u, "no");
            sb.append(" invoked");
        } catch (IllegalArgumentException e) {
            sb.append(" IAE");
        }
        try {
            c.getMethod("nothing");
        } catch (NoSuchMethodException e) {
            sb.append(" NSME");
        }
        try {
            c.getMethod("fail").invoke(t);
        } catch (java.lang.reflect.InvocationTargetException e) {
            sb.append(" ITE:").append(e.getCause().getClass().getSimpleName()).append(':').append(e.getCause().getMessage());
        }
        sb.append(' ').append(c.getConstructors().length).append(c.getSuperclass().getName()).append(c.getInterfaces()[0].getName()).append(Comparable.class.isAssignableFrom(c));
        sb.append(' ').append(Class.forName("java.util.ArrayList").getName()).append(int[].class.getName()).append(c.isInstance(t)).append(String[].class.getComponentType().getName());
        Object array = java.lang.reflect.Array.newInstance(c, 3);
        java.lang.reflect.Array.set(array, 1, t);
        sb.append(' ').append(java.lang.reflect.Array.getLength(array)).append(((Target[]) array)[1] == t).append(array.getClass().getName());

        final List<String> calls = new ArrayList<>();
        Shape shape = (Shape) java.lang.reflect.Proxy.newProxyInstance(Lib.class.getClassLoader(), new Class<?>[] { Shape.class, Runnable.class }, (proxy, method, args) -> {
            calls.add(method.getName() + (args == null ? 0 : args.length));
            switch (method.getName()) {
            case "area": return 6 * (Integer) args[0];
            case "name": return "hex";
            case "big": return (Long) args[0] + Math.round((Double) args[1]);
            case "describe": return "described";
            case "toString": return "a proxy";
            case "hashCode": return 77;
            case "equals": return args[0] == proxy;
            default: return null;
            }
        });
        sb.append(' ').append(shape.area(2)).append(shape.name()).append(shape.big(1L << 40, 2.6)).append(shape.describe()).append(shape).append(shape.hashCode()).append(shape.equals(shape))
                .append(shape.equals("x"));
        ((Runnable) shape).run();
        sb.append(shape instanceof Runnable).append(java.lang.reflect.Proxy.isProxyClass(shape.getClass())).append(java.lang.reflect.Proxy.isProxyClass(c)).append(calls);
        sb.append(shape.getClass().getInterfaces().length).append(shape.getClass().getSuperclass().getName());

        final ThreadLocal<Integer> local = new ThreadLocal<Integer>() {
            @Override
            protected Integer initialValue() {
                return 5;
            }
        };
        final int[] other = new int[2];
        local.set(9);
        Thread thread = new Thread(() -> {
            other[0] = local.get();
            local.set(1);
            other[1] = local.get();
        });
        thread.start();
        thread.join();
        sb.append(' ').append(local.get()).append(other[0]).append(other[1]);
        java.lang.ref.WeakReference<Object> weak = new java.lang.ref.WeakReference<>(t);
        java.lang.ref.SoftReference<Object> soft = new java.lang.ref.SoftReference<>(u);
        sb.append(weak.get() == t).append(soft.get() == u);
        weak.clear();
        sb.append(weak.get() == null);
        java.util.WeakHashMap<Object, String> map = new java.util.WeakHashMap<>();
        map.put(t, "kept");
        sb.append(map.get(t)).append(map.size());
        return sb.toString();
    }

    public static String pure_bytes() throws IOException {
        StringBuilder sb = new StringBuilder();
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        DataOutputStream out = new DataOutputStream(bytes);
        out.writeInt(0x12345678);
        out.writeLong(-2L);
        out.writeDouble(Math.PI);
        out.writeBoolean(true);
        out.writeShort(-3);
        out.flush();
        byte[] data = bytes.toByteArray();
        DataInputStream in = new DataInputStream(new ByteArrayInputStream(data));
        sb.append(data.length).append(Integer.toHexString(in.readInt())).append(in.readLong()).append(in.readDouble()).append(in.readBoolean()).append(in.readShort());
        ByteBuffer buffer = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        buffer.putInt(0x11223344).putLong(0x0102030405060708L).putShort((short) -2);
        buffer.flip();
        sb.append(' ').append(buffer.remaining()).append(buffer.get(0)).append(buffer.order(ByteOrder.BIG_ENDIAN).getInt(0)).append(buffer.getLong(4) == 0x0807060504030201L);
        long[] longs = new long[4];
        Arrays.fill(longs, 1L << 40);
        longs[2]++;
        sb.append(' ').append(Arrays.stream(longs).sum()).append(Arrays.equals(longs, longs.clone())).append(Arrays.hashCode(new int[] { 1, 2, 3 }));
        AtomicInteger counter = new AtomicInteger(5);
        AtomicLong total = new AtomicLong(1L << 40);
        sb.append(' ').append(counter.incrementAndGet()).append(counter.compareAndSet(6, 10)).append(counter.getAndAdd(5)).append(counter.get()).append(total.addAndGet(1L << 40));
        return sb.toString();
    }

    public static String lib_regex() {
        StringBuilder sb = new StringBuilder();
        Pattern pattern = Pattern.compile("(\\w+)@(\\w+)\\.(com|org)");
        Matcher matcher = pattern.matcher("mail ann@example.com and bob@test.org, not carl@nowhere.net");
        while (matcher.find()) sb.append(matcher.group(1)).append('/').append(matcher.group(2)).append('/').append(matcher.start()).append(' ');
        sb.append("a1b22c333".replaceAll("[0-9]+", "#")).append(' ').append(Arrays.toString("one, two;three  four".split("[,;\\s]+")));
        sb.append(' ').append("a,b,,c,".split(",").length).append("2026-10-13".matches("\\d{4}-\\d{2}-\\d{2}")).append(Pattern.compile("^h.*o$", Pattern.CASE_INSENSITIVE).matcher("HELLO").matches());
        sb.append(' ').append("x=1, y=22".replaceAll("(\\w)=(\\d+)", "$2:$1")).append(Pattern.quote("a.b")).append("a.b".split(Pattern.quote(".")).length);
        return sb.toString();
    }

    public static String lib_format() {
        StringBuilder sb = new StringBuilder();
        sb.append(String.format(Locale.US, "%5d|%-6s|%08.3f|%x|%,d|%c|%b|%10.2e|%s", 42, "ab", Math.E, 255, 1234567, 'z', true, 12345.678, Arrays.asList(1, 2)));
        sb.append(String.format(Locale.US, " %+d %o %X %05d %.0f %10s|%-10s|", 7, 8, 255, -42, 2.5, "right", "left"));
        sb.append(new DecimalFormat("#,##0.00", new DecimalFormatSymbols(Locale.US)).format(1234567.891));
        sb.append(' ').append(new DecimalFormat("0.###E0", new DecimalFormatSymbols(Locale.US)).format(0.000123456));
        SimpleDateFormat date = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS EEE", Locale.US);
        date.setTimeZone(TimeZone.getTimeZone("UTC"));
        sb.append(' ').append(date.format(new Date(1234567890123L)));
        date.setTimeZone(TimeZone.getTimeZone("Europe/Moscow"));
        sb.append(' ').append(date.format(new Date(1234567890123L)));
        sb.append(' ').append(new BigInteger("2").pow(100)).append(' ').append(new BigInteger("123456789012345678901234567890").multiply(new BigInteger("987654321")).mod(new BigInteger("1000000007")));
        sb.append(' ').append(new BigDecimal("1.10").add(new BigDecimal("2.205")).setScale(2, BigDecimal.ROUND_HALF_UP));
        sb.append(' ').append("straße ΑΒΓ".toUpperCase(Locale.ROOT)).append("İSTANBUL".toLowerCase(Locale.ROOT).length()).append(Character.isLetter('я')).append(Character.isDigit('٣'));
        return sb.toString();
    }

    public static String lib_charsets() throws IOException {
        StringBuilder sb = new StringBuilder();
        String text = "héllo, 世界 😀";
        byte[] utf8 = text.getBytes(StandardCharsets.UTF_8);
        sb.append(utf8.length).append(new String(utf8, StandardCharsets.UTF_8).equals(text)).append(text.getBytes("UTF-16LE").length).append(text.getBytes(StandardCharsets.ISO_8859_1)[1] & 0xFF);
        sb.append(new String(new byte[] { (byte) 0xD0, (byte) 0x9F, (byte) 0xD1, (byte) 0x80 }, "UTF-8").length()).append(new String(new byte[] { (byte) 0xCF, (byte) 0xF0 }, "windows-1251"));
        sb.append(' ').append("The quick brown fox".getBytes().length).append(new String("abc".getBytes(StandardCharsets.US_ASCII), StandardCharsets.US_ASCII));
        CRC32 crc = new CRC32();
        crc.update("The quick brown fox".getBytes(StandardCharsets.US_ASCII));
        sb.append(' ').append(Long.toHexString(crc.getValue()));
        byte[] input = new byte[2000];
        for (int i = 0; i < input.length; i++) input[i] = (byte) (i % 17);
        Deflater deflater = new Deflater();
        deflater.setInput(input);
        deflater.finish();
        byte[] packed = new byte[2000];
        int packedLength = deflater.deflate(packed);
        Inflater inflater = new Inflater();
        inflater.setInput(packed, 0, packedLength);
        byte[] back = new byte[2000];
        int backLength;
        try {
            backLength = inflater.inflate(back);
        } catch (java.util.zip.DataFormatException e) {
            backLength = -1;
        }
        sb.append(' ').append(packedLength < 200).append(backLength).append(Arrays.equals(input, back));
        return sb.toString();
    }

    public static String lib_files() throws IOException {
        StringBuilder sb = new StringBuilder();
        File folder = new File(dir, "d8t-files");
        File[] old = folder.listFiles();
        if (old != null) for (File f : old) f.delete();
        folder.delete();
        sb.append(folder.exists()).append(folder.mkdirs()).append(folder.isDirectory());
        File file = new File(folder, "lines.txt");
        try (BufferedWriter writer = new BufferedWriter(new FileWriter(file))) {
            for (int i = 1; i <= 100; i++) {
                writer.write("line " + i + " é");
                writer.newLine();
            }
        }
        sb.append(' ').append(file.exists()).append(file.length() >= 900).append(file.getName()).append(file.isFile());
        int lines = 0, total = 0;
        String last = null;
        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            for (String line = reader.readLine(); line != null; line = reader.readLine()) {
                lines++;
                total += line.length();
                last = line;
            }
        }
        sb.append(' ').append(lines).append(',').append(total).append(',').append(last);
        File binary = new File(folder, "data.bin");
        try (FileOutputStream out = new FileOutputStream(binary)) {
            for (int i = 0; i < 300; i++) out.write(i);
        }
        try (FileOutputStream out = new FileOutputStream(binary, true)) {
            out.write(new byte[] { 1, 2, 3 });
        }
        byte[] buffer = new byte[400];
        int read;
        try (FileInputStream in = new FileInputStream(binary)) {
            read = in.read(buffer);
        }
        sb.append(' ').append(read).append(buffer[255] & 0xFF).append(buffer[256]).append(buffer[302]);
        try (RandomAccessFile random = new RandomAccessFile(binary, "rw")) {
            random.seek(100);
            random.writeInt(0xCAFEBABE);
            random.seek(98);
            sb.append(' ').append(random.length()).append(random.read()).append(random.read()).append(Integer.toHexString(random.readInt())).append(random.getFilePointer());
        }
        String[] names = folder.list();
        Arrays.sort(names);
        sb.append(' ').append(Arrays.toString(names));
        File renamed = new File(folder, "renamed.bin");
        sb.append(binary.renameTo(renamed)).append(renamed.exists()).append(binary.exists()).append(renamed.delete()).append(file.delete()).append(folder.delete()).append(folder.exists());
        try {
            new FileInputStream(new File(folder, "nothing")).close();
        } catch (IOException e) {
            sb.append(' ').append(e.getClass().getName());
        }
        return sb.toString();
    }

    static int shared;

    /* java.nio.file: on Android its native half is not registered by the library but found by name (Java_sun_nio_fs_UnixNativeDispatcher_...). */
    public static String lib_nio() throws IOException {
        StringBuilder sb = new StringBuilder();
        Path folder = Paths.get(dir, "d8t-nio");
        if (Files.isDirectory(folder)) {
            try (DirectoryStream<Path> old = Files.newDirectoryStream(folder)) {
                for (Path p : old) Files.delete(p);
            }
            Files.delete(folder);
        }
        sb.append(Files.exists(folder));
        Files.createDirectories(folder);
        sb.append(Files.isDirectory(folder));
        Path one = folder.resolve("one.txt");
        Files.write(one, "first\nsecond é\nthird\n".getBytes(StandardCharsets.UTF_8));
        sb.append(' ').append(Files.size(one)).append(Files.isRegularFile(one)).append(Files.isReadable(one));
        List<String> lines = Files.readAllLines(one, StandardCharsets.UTF_8);
        sb.append(' ').append(lines.size()).append(lines.get(1));
        Path two = folder.resolve("two.bin");
        Files.copy(one, two);
        byte[] copy = Files.readAllBytes(two);
        sb.append(' ').append(copy.length).append((char) copy[0]);
        Path three = folder.resolve("three.bin");
        Files.move(two, three);
        sb.append(' ').append(Files.exists(two)).append(Files.exists(three));
        try (FileChannel channel = FileChannel.open(three, StandardOpenOption.READ, StandardOpenOption.WRITE)) {
            ByteBuffer buffer = ByteBuffer.allocate(6);
            channel.position(6);
            int n = channel.read(buffer);
            sb.append(' ').append(channel.size()).append(',').append(n).append(',').append(new String(buffer.array(), 0, n, StandardCharsets.UTF_8));
            channel.write(ByteBuffer.wrap(new byte[] { 'X', 'Y' }), 0);
            channel.truncate(8);
            sb.append(',').append(channel.size());
        }
        sb.append(' ').append(new String(Files.readAllBytes(three), StandardCharsets.UTF_8).replace('\n', '|'));
        List<String> names = new ArrayList<>();
        try (DirectoryStream<Path> all = Files.newDirectoryStream(folder)) {
            for (Path p : all) names.add(p.getFileName().toString());
        }
        Collections.sort(names);
        sb.append(' ').append(names);
        try {
            Files.delete(two);
            sb.append(" deleted");
        } catch (NoSuchFileException e) {
            sb.append(" NoSuchFileException");
        }
        sb.append(Files.deleteIfExists(three)).append(Files.deleteIfExists(three));
        Files.delete(one);
        Files.delete(folder);
        sb.append(Files.exists(folder));
        return sb.toString();
    }

    private static String hex(byte[] bytes) {
        StringBuilder sb = new StringBuilder();
        for (byte b : bytes) sb.append(Character.forDigit((b >> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        return sb.toString();
    }

    /** What apps use of java.security most: digests, random numbers, UUIDs; and the checksums of java.util.zip beside them. */
    public static String lib_security() throws Exception {
        StringBuilder sb = new StringBuilder();
        byte[] abc = "abc".getBytes(StandardCharsets.US_ASCII);
        sb.append(hex(java.security.MessageDigest.getInstance("MD5").digest(abc))).append(' ').append(hex(java.security.MessageDigest.getInstance("SHA-1").digest(abc))).append(' ')
                .append(hex(java.security.MessageDigest.getInstance("SHA-256").digest(abc)));
        java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-512");
        md.update(abc, 0, 1);
        md.update(abc, 1, 2);
        byte[] long1 = md.digest();
        md.update(new byte[1000]);
        sb.append(' ').append(hex(long1).substring(0, 16)).append(md.getDigestLength()).append(hex(md.digest()).substring(0, 8));
        try {
            java.security.MessageDigest.getInstance("NoSuchDigest");
            sb.append(" found");
        } catch (java.security.NoSuchAlgorithmException e) {
            sb.append(" NSAE");
        }
        java.security.SecureRandom random = new java.security.SecureRandom();
        byte[] a = new byte[16], b = new byte[16];
        random.nextBytes(a);
        random.nextBytes(b);
        int small = random.nextInt(10);
        sb.append(' ').append(!Arrays.equals(a, b)).append(!Arrays.equals(a, new byte[16])).append(small >= 0 && small < 10).append(random.generateSeed(8).length);
        java.util.UUID u = java.util.UUID.randomUUID();
        sb.append(' ').append(u.version()).append(u.variant()).append(u.toString().length()).append(!u.equals(java.util.UUID.randomUUID()));
        sb.append(' ').append(java.util.UUID.nameUUIDFromBytes(abc)).append(' ').append(java.util.UUID.fromString("00112233-4455-6677-8899-aabbccddeeff").getMostSignificantBits());
        CRC32 crc = new CRC32();
        crc.update(abc);
        java.util.zip.Adler32 adler = new java.util.zip.Adler32();
        adler.update(abc);
        sb.append(' ').append(Long.toHexString(crc.getValue())).append(' ').append(Long.toHexString(adler.getValue()));
        return sb.toString();
    }

    /** One step further: a MAC, a cipher, a key derived from a password, Base64. On Android these are BouncyCastle's or conscrypt's. */
    public static String lib_crypto() throws Exception {
        StringBuilder sb = new StringBuilder();
        byte[] key = "0123456789abcdef".getBytes(StandardCharsets.US_ASCII), text = "attack at dawn, or a little later".getBytes(StandardCharsets.US_ASCII);
        javax.crypto.Mac mac = javax.crypto.Mac.getInstance("HmacSHA256");
        mac.init(new javax.crypto.spec.SecretKeySpec(key, "HmacSHA256"));
        sb.append(hex(mac.doFinal(text)).substring(0, 24));
        javax.crypto.Cipher aes = javax.crypto.Cipher.getInstance("AES/CBC/PKCS5Padding");
        aes.init(javax.crypto.Cipher.ENCRYPT_MODE, new javax.crypto.spec.SecretKeySpec(key, "AES"), new javax.crypto.spec.IvParameterSpec(new byte[16]));
        byte[] sealed = aes.doFinal(text);
        aes.init(javax.crypto.Cipher.DECRYPT_MODE, new javax.crypto.spec.SecretKeySpec(key, "AES"), new javax.crypto.spec.IvParameterSpec(new byte[16]));
        sb.append(' ').append(sealed.length).append(hex(sealed).substring(0, 16)).append(new String(aes.doFinal(sealed), StandardCharsets.US_ASCII).equals("attack at dawn, or a little later"));
        javax.crypto.SecretKeyFactory factory = javax.crypto.SecretKeyFactory.getInstance("PBKDF2WithHmacSHA1");
        byte[] derived = factory.generateSecret(new javax.crypto.spec.PBEKeySpec("password".toCharArray(), key, 100, 128)).getEncoded();
        sb.append(' ').append(hex(derived));
        sb.append(' ').append(java.util.Base64.getEncoder().encodeToString(text).substring(0, 16)).append(new String(java.util.Base64.getDecoder().decode("aGVsbG8="), StandardCharsets.US_ASCII));
        return sb.toString();
    }

    public static String lib_threads() throws InterruptedException {
        StringBuilder sb = new StringBuilder();
        final int[] result = new int[1];
        final String[] name = new String[1];
        Thread worker = new Thread(new Runnable() {
            public void run() {
                int sum = 0;
                for (int i = 1; i <= 1000; i++) sum += i;
                result[0] = sum;
                name[0] = Thread.currentThread().getName();
            }
        }, "worker");
        sb.append(worker.isAlive());
        worker.start();
        worker.join();
        sb.append(worker.isAlive()).append(result[0]).append(name[0]).append(Thread.currentThread() != worker);
        final Object lock = new Object();
        final AtomicInteger atomic = new AtomicInteger();
        final CountDownLatch done = new CountDownLatch(4);
        shared = 0;
        Thread[] threads = new Thread[4];
        for (int t = 0; t < threads.length; t++) {
            threads[t] = new Thread(() -> {
                for (int i = 0; i < 5000; i++) {
                    synchronized (lock) {
                        shared++;
                    }
                    atomic.incrementAndGet();
                }
                done.countDown();
            });
            threads[t].start();
        }
        done.await();
        for (Thread t : threads) t.join();
        sb.append(' ').append(shared).append(',').append(atomic.get());
        final StringBuilder order = new StringBuilder();
        final boolean[] ready = new boolean[1];
        Thread waiter = new Thread(() -> {
            synchronized (lock) {
                while (!ready[0]) {
                    try {
                        lock.wait();
                    } catch (InterruptedException e) {
                        return;
                    }
                }
                order.append("woken");
            }
        });
        waiter.start();
        Thread.sleep(20);
        synchronized (lock) {
            order.append("notify,");
            ready[0] = true;
            lock.notifyAll();
        }
        waiter.join();
        sb.append(' ').append(order);
        final Throwable[] caught = new Throwable[1];
        Thread failing = new Thread(() -> {
            throw new IllegalStateException("in a thread");
        });
        failing.setUncaughtExceptionHandler((t, e) -> caught[0] = e);
        failing.start();
        failing.join();
        sb.append(' ').append(caught[0].getMessage());
        ThreadLocal<Integer> local = new ThreadLocal<>();
        local.set(5);
        final Integer[] seen = new Integer[1];
        Thread other = new Thread(() -> seen[0] = local.get());
        other.start();
        other.join();
        sb.append(' ').append(local.get()).append(seen[0]);
        return sb.toString();
    }

    static int zero() {
        return 0;
    }

    public static String lib_exceptions() {
        StringBuilder sb = new StringBuilder();
        try {
            sb.append(1 / zero());
        } catch (ArithmeticException e) {
            sb.append(e.getClass().getName()).append(':').append(e.getMessage());
        }
        try {
            sb.append(((String) (Object) (zero() == 0 ? null : "")).length());
        } catch (NullPointerException e) {
            sb.append(' ').append(e.getClass().getName());
        }
        try {
            sb.append(new int[3][zero() + 5][0].length + new int[2][3 - 1][zero() - 1].length);
        } catch (NegativeArraySizeException e) {
            sb.append(' ').append(e.getClass().getName()).append(':').append(e.getMessage());
        }
        try {
            int[] five = new int[5];
            sb.append(five[zero() + 5]);
        } catch (ArrayIndexOutOfBoundsException e) {
            sb.append(' ').append(e.getClass().getName()).append(':').append(e.getMessage());
        }
        try {
            Object o = "text";
            sb.append((Integer) o);
        } catch (ClassCastException e) {
            sb.append(' ').append(e.getClass().getName());
        }
        try {
            sb.append("abc".substring(2, 1));
        } catch (StringIndexOutOfBoundsException e) {
            sb.append(' ').append(e.getClass().getName());
        }
        try {
            sb.append(Integer.parseInt("12x"));
        } catch (NumberFormatException e) {
            sb.append(' ').append(e.getMessage());
        }
        try {
            throw new IOException("made", new RuntimeException("cause"));
        } catch (IOException e) {
            StackTraceElement top = e.getStackTrace()[0];
            sb.append(' ').append(e).append('|').append(e.getCause().getMessage()).append('|').append(top.getClassName()).append('.').append(top.getMethodName());
        }
        try {
            Class.forName("no.such.Class");
        } catch (ClassNotFoundException e) {
            sb.append(' ').append(e.getMessage());
        }
        try {
            sb.append(Class.forName("java.util.ArrayList").getSimpleName()).append(Class.forName("[I").getName()).append(ArrayList.class.getSuperclass().getName());
            sb.append(Lib.class.getDeclaredMethod("zero").invoke(null)).append(Point.class.getDeclaredConstructor(int.class, int.class).newInstance(3, 4));
            sb.append(Point.class.getDeclaredField("x").getInt(new Point(8, 9))).append(List.class.isAssignableFrom(ArrayList.class)).append(Runnable.class.isInterface());
        } catch (Exception e) {
            sb.append(' ').append(e);
        }
        return sb.toString();
    }

    public static String lib_system() {
        StringBuilder sb = new StringBuilder();
        long now = System.currentTimeMillis(), nano = System.nanoTime();
        sb.append(now > 1500000000000L).append(now < 4000000000000L).append(System.nanoTime() >= nano);
        sb.append(' ').append(System.getProperty("line.separator").length() > 0).append(System.getProperty("java.io.tmpdir") != null);
        sb.append(' ').append(System.getProperty("no.such.property", "default")).append(System.identityHashCode(null)).append(Runtime.getRuntime().availableProcessors() > 0);
        int[] a = { 1, 2, 3, 4, 5 };
        System.arraycopy(a, 0, a, 1, 4);
        sb.append(' ').append(Arrays.toString(a));
        System.out.println("printed by d8t.Lib through System.out");
        System.err.println("printed by d8t.Lib through System.err");
        System.out.printf(Locale.US, "and formatted: %d items at %.1f%%%n", 3, 99.5);
        return sb.toString();
    }

    public static void main(String[] args) throws Exception {
        System.out.println("pure_lists=" + pure_lists());
        System.out.println("pure_maps=" + pure_maps());
        System.out.println("pure_strings=" + pure_strings());
        System.out.println("pure_streams=" + pure_streams());
        System.out.println("pure_bytes=" + pure_bytes());
        System.out.println("pure_reflect=" + pure_reflect());
        System.out.println("pure_annotations=" + pure_annotations());
        System.out.println("lib_regex=" + lib_regex());
        System.out.println("lib_format=" + lib_format());
        System.out.println("lib_charsets=" + lib_charsets());
        System.out.println("lib_files=" + lib_files());
        System.out.println("lib_nio=" + lib_nio());
        System.out.println("lib_security=" + lib_security());
        System.out.println("lib_crypto=" + lib_crypto());
        System.out.println("lib_threads=" + lib_threads());
        System.out.println("lib_exceptions=" + lib_exceptions());
        System.out.println("lib_system=" + lib_system());
    }
}
