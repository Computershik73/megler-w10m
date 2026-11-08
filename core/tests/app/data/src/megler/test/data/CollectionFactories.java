package megler.test.data;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.NotSerializableException;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.io.Serializable;
import java.lang.reflect.InvocationTargetException;
import java.util.AbstractMap;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.ListIterator;
import java.util.Map;
import java.util.RandomAccess;
import java.util.Set;

/**
 * The data app's step collection-factories: Java 9 and 10's List.of, Set.of, Map.of, Map.ofEntries, Map.entry and copyOf,
 * which Android has from Android 11, and Megler's libcore overlay gives Android 9's libcore
 * (core/libcore-overlay/java/util/ImmutableCollections.java). Called by reflection, as an app built for that Android calls
 * them directly: this app is built against Android 10's android.jar, which has none of them. Returns the checks that
 * failed, by name (with what was thrown instead, for a check of an exception), or "" when none did.
 */
final class CollectionFactories {
    private static final Class<? extends Throwable> NPE = NullPointerException.class, UOE = UnsupportedOperationException.class,
                                                    IAE = IllegalArgumentException.class;

    private CollectionFactories() {}

    private interface Throwing {
        void run() throws Throwable;
    }

    /** A static method by reflection; what it throws, thrown as it is. */
    private static Object call(Class<?> c, String name, Class<?>[] types, Object... args) throws Throwable {
        try {
            return c.getMethod(name, types).invoke(null, args);
        } catch (InvocationTargetException e) {
            throw e.getCause();
        }
    }

    /** n parameters of type Object, as List.of's and Map.of's are declared after their erasure. */
    private static Class<?>[] objects(int n) {
        Class<?>[] t = new Class<?>[n];
        Arrays.fill(t, Object.class);
        return t;
    }

    private static void check(StringBuilder bad, String name, boolean ok) {
        if (!ok) bad.append(bad.length() > 0 ? " " : "").append(name);
    }

    private static void refused(StringBuilder bad, String name, Class<? extends Throwable> expected, Throwing r) {
        Throwable got = null;
        try {
            r.run();
        } catch (Throwable t) {
            got = t;
        }
        if (!expected.isInstance(got)) bad.append(bad.length() > 0 ? " " : "").append(name).append('=').append(got == null ? "none" : got.getClass().getSimpleName());
    }

    private static Object[] roundTrip(Object... objects) throws Exception {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ObjectOutputStream out = new ObjectOutputStream(bytes)) {
            for (Object o : objects) out.writeObject(o);
        }
        Object[] read = new Object[objects.length];
        try (ObjectInputStream in = new ObjectInputStream(new ByteArrayInputStream(bytes.toByteArray()))) {
            for (int i = 0; i < read.length; i++) read[i] = in.readObject();
        }
        return read;
    }

    @SuppressWarnings("unchecked")
    static String factories() throws Throwable {
        StringBuilder bad = new StringBuilder();
        Class<?>[] array = { Object[].class }, collection = { Collection.class }, mapOnly = { Map.class }, entryArray = { Map.Entry[].class };
        // Every arity of List.of, Set.of and Map.of, 0 to 10, and the array forms of List.of and Set.of to 12: what each was
        // made of, as equals and hashCode of List, Set and Map have it.
        for (int n = 0; n <= 12; n++) {
            Object[] items = new Object[n], pairs = new Object[2 * n];
            Map<Object, Object> expected = new HashMap<>();
            for (int i = 0; i < n; i++) {
                items[i] = "e" + i;
                pairs[2 * i] = "k" + i;
                pairs[2 * i + 1] = i;
                expected.put("k" + i, i);
            }
            List<Object> asList = Arrays.asList(items);
            Set<Object> asSet = new HashSet<>(asList);
            List<?> fromArray = (List<?>) call(List.class, "of", array, (Object) items.clone());
            Set<?> setFromArray = (Set<?>) call(Set.class, "of", array, (Object) items.clone());
            check(bad, "list-array-" + n, fromArray.equals(asList) && fromArray.hashCode() == asList.hashCode() && fromArray.size() == n);
            check(bad, "set-array-" + n, setFromArray.equals(asSet) && setFromArray.hashCode() == asSet.hashCode() && setFromArray.size() == n);
            if (n > 10) continue;
            List<?> list = (List<?>) call(List.class, "of", objects(n), items);
            Set<?> set = (Set<?>) call(Set.class, "of", objects(n), items);
            Map<?, ?> map = (Map<?, ?>) call(Map.class, "of", objects(2 * n), pairs);
            check(bad, "list-" + n, list.equals(asList) && asList.equals(list) && list.hashCode() == asList.hashCode() && list.size() == n);
            check(bad, "set-" + n, set.equals(asSet) && asSet.equals(set) && set.hashCode() == asSet.hashCode() && set.size() == n);
            check(bad, "map-" + n, map.equals(expected) && expected.equals(map) && map.hashCode() == expected.hashCode() && map.size() == n
                                   && map.entrySet().equals(expected.entrySet()) && map.keySet().equals(expected.keySet()));
        }
        // The array form copies the array: changing it afterwards changes nothing, and a String[] gives an Object[] back.
        Object[] given = { "a", "b", "c" };
        List<?> copied = (List<?>) call(List.class, "of", array, (Object) given);
        given[0] = "changed";
        List<?> strings = (List<?>) call(List.class, "of", array, (Object) new String[] { "x", "y" });
        check(bad, "array-copied", "a".equals(copied.get(0)) && strings.toArray().getClass() == Object[].class);

        // Nulls refused, by every factory; an element or a key given twice refused.
        refused(bad, "list-null", NPE, () -> call(List.class, "of", objects(1), (Object) null));
        refused(bad, "list-null-2", NPE, () -> call(List.class, "of", objects(2), "a", null));
        refused(bad, "list-array-null", NPE, () -> call(List.class, "of", array, (Object) new Object[] { "a", null }));
        refused(bad, "list-no-array", NPE, () -> call(List.class, "of", array, (Object) null));
        refused(bad, "set-null", NPE, () -> call(Set.class, "of", objects(3), "a", "b", null));
        refused(bad, "map-null-key", NPE, () -> call(Map.class, "of", objects(2), null, 1));
        refused(bad, "map-null-value", NPE, () -> call(Map.class, "of", objects(4), "a", 1, "b", null));
        refused(bad, "entry-null", NPE, () -> call(Map.class, "entry", objects(2), "k", null));
        refused(bad, "entries-null", NPE, () -> call(Map.class, "ofEntries", entryArray, (Object) new Map.Entry<?, ?>[] { new AbstractMap.SimpleEntry<>("k", 1), null }));
        refused(bad, "set-twice", IAE, () -> call(Set.class, "of", objects(3), "a", "b", "a"));
        refused(bad, "set-array-twice", IAE, () -> call(Set.class, "of", array, (Object) new Object[] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 1 }));
        refused(bad, "map-twice", IAE, () -> call(Map.class, "of", objects(4), "k", 1, "k", 2));
        refused(bad, "entries-twice", IAE, () -> call(Map.class, "ofEntries", entryArray,
                                                      (Object) new Map.Entry<?, ?>[] { new AbstractMap.SimpleEntry<>("k", 1), new AbstractMap.SimpleEntry<>("k", 2) }));

        // A list: RandomAccess and Serializable, its queries, and every mutator refused, an empty list's and its iterators' too.
        List<Object> list = (List<Object>) call(List.class, "of", objects(3), "a", "b", "c");
        List<Object> empty = (List<Object>) call(List.class, "of", objects(0));
        check(bad, "list-kinds", list instanceof RandomAccess && list instanceof Serializable && empty instanceof RandomAccess);
        check(bad, "list-queries", list.indexOf("b") == 1 && list.lastIndexOf("c") == 2 && list.indexOf("z") == -1 && list.contains("a") && "c".equals(list.get(2))
                                   && list.subList(1, 3).equals(Arrays.asList("b", "c")) && "[a, b, c]".equals(list.toString()) && list.containsAll(Arrays.asList("c", "a")));
        refused(bad, "list-contains-null", NPE, () -> list.contains(null));
        refused(bad, "list-index-of-null", NPE, () -> list.indexOf(null));
        refused(bad, "empty-contains-null", NPE, () -> empty.contains(null));
        refused(bad, "list-get-out", IndexOutOfBoundsException.class, () -> list.get(3));
        refused(bad, "sublist-out", IndexOutOfBoundsException.class, () -> list.subList(0, 4));
        refused(bad, "list-add", UOE, () -> list.add("d"));
        refused(bad, "list-add-at", UOE, () -> list.add(0, "d"));
        refused(bad, "list-set", UOE, () -> list.set(0, "d"));
        refused(bad, "list-remove-at", UOE, () -> list.remove(0));
        refused(bad, "list-remove-absent", UOE, () -> list.remove("z"));
        refused(bad, "list-add-nothing", UOE, () -> list.addAll(new ArrayList<>()));
        refused(bad, "list-retain", UOE, () -> list.retainAll(Arrays.asList("a", "b", "c")));
        refused(bad, "list-remove-if-none", UOE, () -> list.removeIf(x -> false));
        refused(bad, "list-replace-all", UOE, () -> list.replaceAll(x -> x));
        refused(bad, "list-sort", UOE, () -> list.sort(null));
        refused(bad, "empty-clear", UOE, () -> empty.clear());
        refused(bad, "list-iterator-remove", UOE, () -> {
            Iterator<Object> i = list.iterator();
            i.next();
            i.remove();
        });
        refused(bad, "list-iterator-set", UOE, () -> {
            ListIterator<Object> i = list.listIterator();
            i.next();
            i.set("d");
        });
        refused(bad, "list-iterator-add", UOE, () -> list.listIterator(1).add("d"));
        refused(bad, "sublist-add", UOE, () -> list.subList(0, 2).add("d"));
        refused(bad, "sublist-clear-empty", UOE, () -> list.subList(1, 1).clear());

        // A set: its queries, and its mutators refused.
        Set<Object> set = (Set<Object>) call(Set.class, "of", objects(3), "a", "b", "c");
        Set<Object> emptySet = (Set<Object>) call(Set.class, "of", objects(0));
        check(bad, "set-queries", set.contains("b") && !set.contains("z") && set instanceof Serializable && !emptySet.contains("a")
                                  && set.containsAll(Arrays.asList("c", "a")) && !set.equals(new HashSet<>(Arrays.asList("a", "b", null))));
        refused(bad, "set-contains-null", NPE, () -> set.contains(null));
        refused(bad, "set-add", UOE, () -> set.add("d"));
        refused(bad, "set-remove", UOE, () -> set.remove("a"));
        refused(bad, "empty-set-clear", UOE, () -> emptySet.clear());
        refused(bad, "set-iterator-remove", UOE, () -> {
            Iterator<Object> i = set.iterator();
            i.next();
            i.remove();
        });

        // A map: its queries, and its mutators refused, those of its views and entries too.
        Map<Object, Object> map = (Map<Object, Object>) call(Map.class, "of", objects(4), "a", 1, "b", 2);
        Map<Object, Object> emptyMap = (Map<Object, Object>) call(Map.class, "of", objects(0));
        check(bad, "map-queries", Integer.valueOf(2).equals(map.get("b")) && map.get("z") == null && map.containsKey("a") && map.containsValue(1)
                                  && !map.containsValue(5) && Integer.valueOf(7).equals(map.getOrDefault("z", 7)) && map instanceof Serializable
                                  && map.values().containsAll(Arrays.asList(1, 2)) && map.entrySet().contains(new AbstractMap.SimpleEntry<>("a", 1)));
        refused(bad, "map-get-null", NPE, () -> map.get(null));
        refused(bad, "map-contains-key-null", NPE, () -> map.containsKey(null));
        refused(bad, "map-contains-value-null", NPE, () -> map.containsValue(null));
        refused(bad, "empty-map-get-null", NPE, () -> emptyMap.get(null));
        refused(bad, "map-put", UOE, () -> map.put("c", 3));
        refused(bad, "map-remove", UOE, () -> map.remove("a"));
        refused(bad, "map-put-nothing", UOE, () -> map.putAll(new HashMap<>()));
        refused(bad, "empty-map-clear", UOE, () -> emptyMap.clear());
        refused(bad, "map-put-if-absent", UOE, () -> map.putIfAbsent("a", 5));
        refused(bad, "map-compute-if-present", UOE, () -> map.computeIfPresent("z", (k, v) -> v));
        refused(bad, "map-merge", UOE, () -> map.merge("a", 1, (x, y) -> x));
        refused(bad, "map-replace-all", UOE, () -> map.replaceAll((k, v) -> v));
        refused(bad, "map-set-value", UOE, () -> map.entrySet().iterator().next().setValue(9));
        refused(bad, "map-entries-iterator-remove", UOE, () -> {
            Iterator<?> i = map.entrySet().iterator();
            i.next();
            i.remove();
        });
        refused(bad, "map-key-remove", UOE, () -> map.keySet().remove("a"));
        refused(bad, "map-values-clear", UOE, () -> map.values().clear());

        // Map.entry: unmodifiable, equal to an entry of the same pair (and its hash), not Serializable; Map.ofEntries.
        Map.Entry<Object, Object> entry = (Map.Entry<Object, Object>) call(Map.class, "entry", objects(2), "k", "v");
        Map.Entry<Object, Object> simple = new AbstractMap.SimpleEntry<>("k", "v");
        check(bad, "entry", "k".equals(entry.getKey()) && "v".equals(entry.getValue()) && entry.equals(simple) && simple.equals(entry)
                            && entry.hashCode() == simple.hashCode() && "k=v".equals(entry.toString()) && !(entry instanceof Serializable));
        refused(bad, "entry-set-value", UOE, () -> entry.setValue("w"));
        Map<?, ?> entries = (Map<?, ?>) call(Map.class, "ofEntries", entryArray, (Object) new Map.Entry<?, ?>[] { entry, new AbstractMap.SimpleEntry<>("j", "w") });
        check(bad, "of-entries", entries.size() == 2 && "v".equals(entries.get("k")) && "w".equals(entries.get("j")));

        // copyOf: a copy of another's collection (not changed by it afterwards), ours as it is; a set's copy keeps one of
        // equal elements; nulls refused.
        ArrayList<Object> source = new ArrayList<>(Arrays.asList("x", "y", "x"));
        List<?> listCopy = (List<?>) call(List.class, "copyOf", collection, source);
        source.set(0, "changed");
        check(bad, "list-copy", listCopy.equals(Arrays.asList("x", "y", "x")) && call(List.class, "copyOf", collection, list) == list);
        Set<?> setCopy = (Set<?>) call(Set.class, "copyOf", collection, Arrays.asList("x", "y", "x"));
        check(bad, "set-copy", setCopy.size() == 2 && setCopy.contains("x") && setCopy.contains("y") && call(Set.class, "copyOf", collection, set) == set);
        HashMap<Object, Object> hashMap = new HashMap<>();
        hashMap.put("p", 1);
        hashMap.put("q", 2);
        Map<?, ?> mapCopy = (Map<?, ?>) call(Map.class, "copyOf", mapOnly, hashMap);
        Map<Object, Object> before = new HashMap<>(hashMap);
        hashMap.put("r", 3);
        check(bad, "map-copy", mapCopy.equals(before) && call(Map.class, "copyOf", mapOnly, map) == map);
        HashMap<Object, Object> nullValue = new HashMap<>();
        nullValue.put("a", null);
        refused(bad, "list-copy-null", NPE, () -> call(List.class, "copyOf", collection, Arrays.asList("a", null)));
        refused(bad, "set-copy-null", NPE, () -> call(Set.class, "copyOf", collection, Arrays.asList("a", null)));
        refused(bad, "map-copy-null", NPE, () -> call(Map.class, "copyOf", mapOnly, nullValue));
        refused(bad, "copy-of-nothing", NPE, () -> call(List.class, "copyOf", collection, (Object) null));

        // Serialized and read back: equal, of the same class, and unmodifiable still; Map.entry's entry refused.
        Object[] read = roundTrip(list, set, map, empty);
        check(bad, "serialized", list.equals(read[0]) && set.equals(read[1]) && map.equals(read[2]) && empty.equals(read[3]) && read[0].getClass() == list.getClass()
                                 && read[1].getClass() == set.getClass() && read[2].getClass() == map.getClass());
        refused(bad, "serialized-add", UOE, () -> ((List<Object>) read[0]).add("d"));
        refused(bad, "serialized-put", UOE, () -> ((Map<Object, Object>) read[2]).put("c", 3));
        refused(bad, "entry-serialized", NotSerializableException.class, () -> new ObjectOutputStream(new ByteArrayOutputStream()).writeObject(entry));
        return bad.toString();
    }
}
