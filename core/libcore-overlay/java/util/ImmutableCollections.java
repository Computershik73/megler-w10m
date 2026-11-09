package java.util;

import java.io.InvalidObjectException;
import java.io.ObjectInputStream;
import java.io.ObjectStreamException;
import java.io.Serializable;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.UnaryOperator;

/**
 * The unmodifiable collections of Java 9 and 10's List.of, Set.of, Map.of, Map.ofEntries, Map.entry and copyOf (the libcore
 * overlay's List, Set and Map call these), which Android has from Android 11 (API 30) and Android 9's libcore has not: Amazon
 * Shopping ended at its start on a NoSuchMethodError of Map.of and Set.of, and Shazam on Map.of() and Map.entry (Firebase's).
 * Megler's own, written as the documentation describes them:
 * <ul>
 * <li>no nulls: the factories refuse them with NullPointerException, and so do contains, indexOf, lastIndexOf, containsKey,
 * containsValue and get asked for null, as OpenJDK's do;
 * <li>no duplicate elements of a set made by Set.of, nor keys of a map (IllegalArgumentException); Set.copyOf keeps the first
 * of equal elements;
 * <li>nothing changes: every mutator throws UnsupportedOperationException whatever its arguments, an empty collection's too,
 * and so do the iterators', the views' of a map and its entries';
 * <li>a list is RandomAccess; a set and a map go through their elements in the order they were given;
 * <li>the lists, sets and maps are Serializable, through a proxy that makes them again with the same checks (the entries of
 * Map.entry are not, as documented).
 * </ul>
 * Each holds the array it is made of, which no one else has: a factory is given an array made for the call or a copy.
 */
final class ImmutableCollections {
    private ImmutableCollections() {}

    static UnsupportedOperationException uoe() {
        return new UnsupportedOperationException();
    }

    // ------------------------------------------------------------------------------------------------------------ lists

    static final ImmutableList<Object> EMPTY_LIST = new ImmutableList<>(new Object[0], 0, 0);

    /** A list of `items`, an array no one else has. */
    @SuppressWarnings("unchecked")
    static <E> List<E> list(Object[] items) {
        for (Object o : items) Objects.requireNonNull(o);
        return items.length == 0 ? (List<E>) EMPTY_LIST : new ImmutableList<E>(items, 0, items.length);
    }

    /** A list of an array that may be anyone's, of any element type: copied first (List.of(E...)). */
    static <E> List<E> listCopy(Object[] items) {
        return list(Arrays.copyOf(items, items.length, Object[].class));
    }

    /** List.copyOf: a list of ours as it is, unless it is a part of a longer one. */
    @SuppressWarnings("unchecked")
    static <E> List<E> listOf(Collection<? extends E> c) {
        if (c instanceof ImmutableList && ((ImmutableList<?>) c).isWhole()) return (List<E>) c;
        return listCopy(c.toArray());
    }

    // ------------------------------------------------------------------------------------------------------------- sets

    static final ImmutableSet<Object> EMPTY_SET = new ImmutableSet<>(new Object[0], false, "element");

    /** A set of `items`, an array no one else has; an element given twice is refused. */
    @SuppressWarnings("unchecked")
    static <E> Set<E> set(Object[] items) {
        return items.length == 0 ? (Set<E>) EMPTY_SET : new ImmutableSet<E>(items, false, "element");
    }

    /** A set of an array that may be anyone's, of any element type: copied first (Set.of(E...)). */
    static <E> Set<E> setCopy(Object[] items) {
        return set(Arrays.copyOf(items, items.length, Object[].class));
    }

    /** Set.copyOf: a set of ours as it is; of equal elements the first is kept. */
    @SuppressWarnings("unchecked")
    static <E> Set<E> setOf(Collection<? extends E> c) {
        if (c instanceof ImmutableSet) return (Set<E>) c;
        Object[] items = c.toArray();
        items = Arrays.copyOf(items, items.length, Object[].class);
        return items.length == 0 ? (Set<E>) EMPTY_SET : new ImmutableSet<E>(items, true, "element");
    }

    // ------------------------------------------------------------------------------------------------------------- maps

    static final ImmutableMap<Object, Object> EMPTY_MAP = new ImmutableMap<>(EMPTY_SET, new Object[0]);

    /** A map of keys[i] to values[i], two arrays of one length that no one else has; a key given twice is refused. */
    @SuppressWarnings("unchecked")
    static <K, V> Map<K, V> map(Object[] keys, Object[] values) {
        for (int i = 0; i < keys.length; i++) {
            Objects.requireNonNull(keys[i]);
            Objects.requireNonNull(values[i]);
        }
        if (keys.length == 0) return (Map<K, V>) EMPTY_MAP;
        return new ImmutableMap<K, V>(new ImmutableSet<K>(keys, false, "key"), values);
    }

    /** Map.ofEntries: each entry's key and value as they are now. */
    static <K, V> Map<K, V> mapOfEntries(Map.Entry<? extends K, ? extends V>[] entries) {
        Object[] keys = new Object[entries.length], values = new Object[entries.length];
        for (int i = 0; i < entries.length; i++) {
            Map.Entry<? extends K, ? extends V> e = Objects.requireNonNull(entries[i]);
            keys[i] = e.getKey();
            values[i] = e.getValue();
        }
        return map(keys, values);
    }

    /**
     * Map.copyOf: a map of ours as it is. Another's entries are read as its iterator gives them (a concurrent map may change
     * meanwhile, and some maps' iterators give one entry object again and again); keys that are equal (an IdentityHashMap's,
     * a TreeMap's of a comparator of its own) are refused as Map.ofEntries refuses them.
     */
    @SuppressWarnings("unchecked")
    static <K, V> Map<K, V> mapOf(Map<? extends K, ? extends V> map) {
        if (map instanceof ImmutableMap) return (Map<K, V>) map;
        int n = 0, size = Math.min(map.size(), 1 << 20);
        Object[] keys = new Object[size], values = new Object[size];
        for (Map.Entry<? extends K, ? extends V> e : map.entrySet()) {
            if (n == keys.length) {
                keys = Arrays.copyOf(keys, Math.max(8, n * 2));
                values = Arrays.copyOf(values, keys.length);
            }
            keys[n] = e.getKey();
            values[n++] = e.getValue();
        }
        if (n != keys.length) {
            keys = Arrays.copyOf(keys, n);
            values = Arrays.copyOf(values, n);
        }
        return map(keys, values);
    }

    // ---------------------------------------------------------------------------------------------------------- helpers

    /** Spreads a hash's high bits into the low ones, which pick the slot. */
    static int spread(int h) {
        return h ^ (h >>> 16);
    }

    /** An open-addressed table for n elements, at most half full: there is always a free slot to end a search. */
    static int[] newTable(int n) {
        if (n > (1 << 29)) throw new OutOfMemoryError("an unmodifiable set or map of " + n + " elements");
        int cap = 1;
        while (cap < 2 * n) cap <<= 1;
        return new int[cap];
    }

    /** What Collection.toArray(T[]) gives of a[from, to). */
    @SuppressWarnings("unchecked")
    static <T> T[] toArray(Object[] a, int from, int to, T[] into) {
        int n = to - from;
        if (into.length < n) return (T[]) Arrays.copyOfRange(a, from, to, into.getClass());
        System.arraycopy(a, from, into, 0, n);
        if (into.length > n) into[n] = null;
        return into;
    }

    /** Set's equals, for the sets here: the same size, and every element of the other one here. */
    static boolean setEquals(Set<?> set, Object o) {
        if (o == set) return true;
        if (!(o instanceof Set)) return false;
        Collection<?> c = (Collection<?>) o;
        if (c.size() != set.size()) return false;
        try {
            return set.containsAll(c);
        } catch (ClassCastException | NullPointerException e) {
            return false;                  // (null is in the other set, which ours never holds)
        }
    }

    /** What every collection here does: none of Collection's mutators, with any arguments. */
    abstract static class Base<E> extends AbstractCollection<E> {
        @Override public final boolean add(E e) { throw uoe(); }
        @Override public final boolean addAll(Collection<? extends E> c) { throw uoe(); }
        @Override public final void clear() { throw uoe(); }
        @Override public final boolean remove(Object o) { throw uoe(); }
        @Override public final boolean removeAll(Collection<?> c) { throw uoe(); }
        @Override public final boolean removeIf(Predicate<? super E> filter) { throw uoe(); }
        @Override public final boolean retainAll(Collection<?> c) { throw uoe(); }
    }

    /** Through a[from, to), either way; changes nothing. */
    static final class Itr<E> implements ListIterator<E> {
        private final Object[] a;
        private final int from, to;
        private int at;

        Itr(Object[] a, int from, int to, int at) {
            this.a = a;
            this.from = from;
            this.to = to;
            this.at = at;
        }

        @Override public boolean hasNext() { return at < to; }
        @Override public boolean hasPrevious() { return at > from; }
        @Override public int nextIndex() { return at - from; }
        @Override public int previousIndex() { return at - from - 1; }

        @SuppressWarnings("unchecked")
        @Override
        public E next() {
            if (at >= to) throw new NoSuchElementException();
            return (E) a[at++];
        }

        @SuppressWarnings("unchecked")
        @Override
        public E previous() {
            if (at <= from) throw new NoSuchElementException();
            return (E) a[--at];
        }

        @Override public void remove() { throw uoe(); }
        @Override public void set(E e) { throw uoe(); }
        @Override public void add(E e) { throw uoe(); }
    }

    // --------------------------------------------------------------------------------------------------- the list class

    /** elements[from, to): a whole list, or a part of one (subList), which shares its array. */
    static final class ImmutableList<E> extends Base<E> implements List<E>, RandomAccess, Serializable {
        private static final long serialVersionUID = 1L;
        private final Object[] elements;
        private final int from, to;

        ImmutableList(Object[] elements, int from, int to) {
            this.elements = elements;
            this.from = from;
            this.to = to;
        }

        boolean isWhole() {
            return from == 0 && to == elements.length;
        }

        @Override public int size() { return to - from; }
        @Override public boolean isEmpty() { return to == from; }

        @SuppressWarnings("unchecked")
        @Override
        public E get(int index) {
            if (index < 0 || index >= to - from) throw new IndexOutOfBoundsException("Index: " + index + ", Size: " + (to - from));
            return (E) elements[from + index];
        }

        @Override
        public int indexOf(Object o) {
            Objects.requireNonNull(o);
            for (int i = from; i < to; i++) if (o.equals(elements[i])) return i - from;
            return -1;
        }

        @Override
        public int lastIndexOf(Object o) {
            Objects.requireNonNull(o);
            for (int i = to - 1; i >= from; i--) if (o.equals(elements[i])) return i - from;
            return -1;
        }

        @Override public boolean contains(Object o) { return indexOf(o) >= 0; }
        @Override public Iterator<E> iterator() { return new Itr<>(elements, from, to, from); }
        @Override public ListIterator<E> listIterator() { return new Itr<>(elements, from, to, from); }

        @Override
        public ListIterator<E> listIterator(int index) {
            if (index < 0 || index > to - from) throw new IndexOutOfBoundsException("Index: " + index + ", Size: " + (to - from));
            return new Itr<>(elements, from, to, from + index);
        }

        @Override
        public List<E> subList(int fromIndex, int toIndex) {
            if (fromIndex < 0) throw new IndexOutOfBoundsException("fromIndex = " + fromIndex);
            if (toIndex > to - from) throw new IndexOutOfBoundsException("toIndex = " + toIndex);
            if (fromIndex > toIndex) throw new IllegalArgumentException("fromIndex(" + fromIndex + ") > toIndex(" + toIndex + ")");
            return new ImmutableList<>(elements, from + fromIndex, from + toIndex);
        }

        @Override public Object[] toArray() { return Arrays.copyOfRange(elements, from, to, Object[].class); }
        @Override public <T> T[] toArray(T[] a) { return ImmutableCollections.toArray(elements, from, to, a); }

        @Override
        public Spliterator<E> spliterator() {
            return Spliterators.spliterator(elements, from, to, Spliterator.ORDERED | Spliterator.IMMUTABLE | Spliterator.NONNULL);
        }

        @SuppressWarnings("unchecked")
        @Override
        public void forEach(Consumer<? super E> action) {
            Objects.requireNonNull(action);
            for (int i = from; i < to; i++) action.accept((E) elements[i]);
        }

        @Override public void add(int index, E element) { throw uoe(); }
        @Override public boolean addAll(int index, Collection<? extends E> c) { throw uoe(); }
        @Override public E remove(int index) { throw uoe(); }
        @Override public E set(int index, E element) { throw uoe(); }
        @Override public void replaceAll(UnaryOperator<E> operator) { throw uoe(); }
        @Override public void sort(Comparator<? super E> c) { throw uoe(); }

        @Override
        public boolean equals(Object o) {
            if (o == this) return true;
            if (!(o instanceof List)) return false;
            Iterator<?> other = ((List<?>) o).iterator();
            for (int i = from; i < to; i++)
                if (!other.hasNext() || !elements[i].equals(other.next())) return false;
            return !other.hasNext();
        }

        @Override
        public int hashCode() {
            int h = 1;
            for (int i = from; i < to; i++) h = 31 * h + elements[i].hashCode();
            return h;
        }

        private Object writeReplace() {
            return new Ser(Ser.LIST, toArray());
        }

        private void readObject(ObjectInputStream in) throws InvalidObjectException {
            throw new InvalidObjectException("an unmodifiable list is read through its proxy");
        }
    }

    // ---------------------------------------------------------------------------------------------------- the set class

    /** elements, in the order given, found by their hashes through table (each slot the index of an element + 1, or 0). */
    static final class ImmutableSet<E> extends Base<E> implements Set<E>, Serializable {
        private static final long serialVersionUID = 1L;
        final Object[] elements;
        private final int[] table;

        /**
         * A set of `items`, an array no one else has, each element checked: null refused, and an element equal to one before
         * it refused (IllegalArgumentException, "duplicate " + what) or, with dropDuplicates, left out.
         */
        ImmutableSet(Object[] items, boolean dropDuplicates, String what) {
            int n = items.length, kept = 0;
            int[] t = newTable(n);
            int mask = t.length - 1;
            next:
            for (int i = 0; i < n; i++) {
                Object e = Objects.requireNonNull(items[i]);
                int slot = spread(e.hashCode()) & mask;
                for (int at; (at = t[slot]) != 0; slot = (slot + 1) & mask) {
                    if (!e.equals(items[at - 1])) continue;
                    if (!dropDuplicates) throw new IllegalArgumentException("duplicate " + what + ": " + e);
                    continue next;
                }
                items[kept] = e;                // (kept <= i: the elements kept so far stay at the front)
                t[slot] = ++kept;
            }
            elements = kept == n ? items : Arrays.copyOf(items, kept);
            table = t;
        }

        /** Where o is in elements, or -1; null refused. */
        int find(Object o) {
            int[] t = table;
            int mask = t.length - 1;
            for (int slot = spread(o.hashCode()) & mask, at; (at = t[slot]) != 0; slot = (slot + 1) & mask)
                if (o.equals(elements[at - 1])) return at - 1;
            return -1;
        }

        @Override public int size() { return elements.length; }
        @Override public boolean isEmpty() { return elements.length == 0; }
        @Override public boolean contains(Object o) { return find(o) >= 0; }
        @Override public Iterator<E> iterator() { return new Itr<>(elements, 0, elements.length, 0); }
        @Override public Object[] toArray() { return Arrays.copyOf(elements, elements.length, Object[].class); }
        @Override public <T> T[] toArray(T[] a) { return ImmutableCollections.toArray(elements, 0, elements.length, a); }

        @Override
        public Spliterator<E> spliterator() {
            return Spliterators.spliterator(elements, Spliterator.DISTINCT | Spliterator.IMMUTABLE | Spliterator.NONNULL);
        }

        @SuppressWarnings("unchecked")
        @Override
        public void forEach(Consumer<? super E> action) {
            Objects.requireNonNull(action);
            for (Object e : elements) action.accept((E) e);
        }

        @Override public boolean equals(Object o) { return setEquals(this, o); }

        @Override
        public int hashCode() {
            int h = 0;
            for (Object e : elements) h += e.hashCode();
            return h;
        }

        private Object writeReplace() {
            return new Ser(Ser.SET, toArray());
        }

        private void readObject(ObjectInputStream in) throws InvalidObjectException {
            throw new InvalidObjectException("an unmodifiable set is read through its proxy");
        }
    }

    // ---------------------------------------------------------------------------------------------------- the map class

    /** keys (a set of ours, in the order given) and values[i], the value of keys.elements[i]. */
    static final class ImmutableMap<K, V> implements Map<K, V>, Serializable {
        private static final long serialVersionUID = 1L;
        private final ImmutableSet<K> keys;
        private final Object[] values;

        ImmutableMap(ImmutableSet<K> keys, Object[] values) {
            this.keys = keys;
            this.values = values;
        }

        @Override public int size() { return values.length; }
        @Override public boolean isEmpty() { return values.length == 0; }
        @Override public boolean containsKey(Object key) { return keys.find(key) >= 0; }

        @Override
        public boolean containsValue(Object value) {
            Objects.requireNonNull(value);
            for (Object v : values) if (value.equals(v)) return true;
            return false;
        }

        @SuppressWarnings("unchecked")
        @Override
        public V get(Object key) {
            int i = keys.find(key);
            return i < 0 ? null : (V) values[i];
        }

        @SuppressWarnings("unchecked")
        @Override
        public V getOrDefault(Object key, V defaultValue) {
            int i = keys.find(key);
            return i < 0 ? defaultValue : (V) values[i];
        }

        @Override public Set<K> keySet() { return keys; }
        @Override public Collection<V> values() { return new Values(); }
        @Override public Set<Map.Entry<K, V>> entrySet() { return new Entries(); }

        @SuppressWarnings("unchecked")
        @Override
        public void forEach(BiConsumer<? super K, ? super V> action) {
            Objects.requireNonNull(action);
            for (int i = 0; i < values.length; i++) action.accept((K) keys.elements[i], (V) values[i]);
        }

        @Override public V put(K key, V value) { throw uoe(); }
        @Override public V remove(Object key) { throw uoe(); }
        @Override public void putAll(Map<? extends K, ? extends V> m) { throw uoe(); }
        @Override public void clear() { throw uoe(); }
        @Override public void replaceAll(BiFunction<? super K, ? super V, ? extends V> function) { throw uoe(); }
        @Override public V putIfAbsent(K key, V value) { throw uoe(); }
        @Override public boolean remove(Object key, Object value) { throw uoe(); }
        @Override public boolean replace(K key, V oldValue, V newValue) { throw uoe(); }
        @Override public V replace(K key, V value) { throw uoe(); }
        @Override public V computeIfAbsent(K key, Function<? super K, ? extends V> mappingFunction) { throw uoe(); }
        @Override public V computeIfPresent(K key, BiFunction<? super K, ? super V, ? extends V> remappingFunction) { throw uoe(); }
        @Override public V compute(K key, BiFunction<? super K, ? super V, ? extends V> remappingFunction) { throw uoe(); }
        @Override public V merge(K key, V value, BiFunction<? super V, ? super V, ? extends V> remappingFunction) { throw uoe(); }

        @Override
        public boolean equals(Object o) {
            if (o == this) return true;
            if (!(o instanceof Map)) return false;
            Map<?, ?> m = (Map<?, ?>) o;
            if (m.size() != values.length) return false;
            try {
                for (int i = 0; i < values.length; i++)
                    if (!values[i].equals(m.get(keys.elements[i]))) return false;
            } catch (ClassCastException | NullPointerException e) {
                return false;
            }
            return true;
        }

        @Override
        public int hashCode() {
            int h = 0;
            for (int i = 0; i < values.length; i++) h += keys.elements[i].hashCode() ^ values[i].hashCode();
            return h;
        }

        @Override
        public String toString() {
            StringBuilder s = new StringBuilder("{");
            for (int i = 0; i < values.length; i++) {
                if (i > 0) s.append(", ");
                s.append(keys.elements[i]).append('=').append(values[i]);
            }
            return s.append('}').toString();
        }

        private Object writeReplace() {
            Object[] pairs = new Object[2 * values.length];
            for (int i = 0; i < values.length; i++) {
                pairs[2 * i] = keys.elements[i];
                pairs[2 * i + 1] = values[i];
            }
            return new Ser(Ser.MAP, pairs);
        }

        private void readObject(ObjectInputStream in) throws InvalidObjectException {
            throw new InvalidObjectException("an unmodifiable map is read through its proxy");
        }

        /** values(): a collection, not a list (its equals is Object's, as a map's values' are). */
        final class Values extends Base<V> {
            @Override public int size() { return values.length; }
            @Override public boolean contains(Object o) { return containsValue(o); }
            @Override public Iterator<V> iterator() { return new Itr<>(values, 0, values.length, 0); }
        }

        /** entrySet(): an entry for each key, made as it is asked for. */
        final class Entries extends Base<Map.Entry<K, V>> implements Set<Map.Entry<K, V>> {
            @Override public int size() { return values.length; }
            @Override public Iterator<Map.Entry<K, V>> iterator() { return new EntryItr(); }

            @Override
            public boolean contains(Object o) {
                if (!(o instanceof Map.Entry)) return false;
                Map.Entry<?, ?> e = (Map.Entry<?, ?>) o;
                Object key = e.getKey();
                int i = key == null ? -1 : keys.find(key);
                return i >= 0 && values[i].equals(e.getValue());
            }

            @Override public boolean equals(Object o) { return setEquals(this, o); }
            @Override public int hashCode() { return ImmutableMap.this.hashCode(); }
        }

        final class EntryItr implements Iterator<Map.Entry<K, V>> {
            private int at;

            @Override public boolean hasNext() { return at < values.length; }

            @SuppressWarnings("unchecked")
            @Override
            public Map.Entry<K, V> next() {
                if (at >= values.length) throw new NoSuchElementException();
                Map.Entry<K, V> e = new ImmutableEntry<>((K) keys.elements[at], (V) values[at]);
                at++;
                return e;
            }

            @Override public void remove() { throw uoe(); }
        }
    }

    // ------------------------------------------------------------------------------------------------------ Map.entry

    /** Map.entry's, and a map's entries: neither key nor value null, setValue refused, equal to any entry of the same pair. */
    static final class ImmutableEntry<K, V> implements Map.Entry<K, V> {
        private final K key;
        private final V value;

        ImmutableEntry(K key, V value) {
            this.key = Objects.requireNonNull(key);
            this.value = Objects.requireNonNull(value);
        }

        @Override public K getKey() { return key; }
        @Override public V getValue() { return value; }
        @Override public V setValue(V value) { throw uoe(); }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof Map.Entry)) return false;
            Map.Entry<?, ?> e = (Map.Entry<?, ?>) o;
            return key.equals(e.getKey()) && value.equals(e.getValue());
        }

        @Override public int hashCode() { return key.hashCode() ^ value.hashCode(); }
        @Override public String toString() { return key + "=" + value; }
    }

    // -------------------------------------------------------------------------------------------------- serialization

    /**
     * What a list, a set or a map of these is written as: its kind and its elements (a map's keys and values in turns). Read,
     * it is made again by the same factories, with their checks: a stream that says otherwise (a null, an element twice)
     * is refused with InvalidObjectException, and the collection read has an array of its own.
     */
    private static final class Ser implements Serializable {
        private static final long serialVersionUID = 1L;
        static final int LIST = 1, SET = 2, MAP = 3;
        private final int kind;
        private final Object[] items;

        Ser(int kind, Object[] items) {
            this.kind = kind;
            this.items = items;
        }

        private Object readResolve() throws ObjectStreamException {
            try {
                Object[] own = Arrays.copyOf(Objects.requireNonNull(items), items.length, Object[].class);
                switch (kind) {
                case LIST:
                    return list(own);
                case SET:
                    return set(own);
                case MAP:
                    if ((own.length & 1) != 0) throw new IllegalArgumentException("a key without its value");
                    Object[] keys = new Object[own.length / 2], values = new Object[own.length / 2];
                    for (int i = 0; i < keys.length; i++) {
                        keys[i] = own[2 * i];
                        values[i] = own[2 * i + 1];
                    }
                    return map(keys, values);
                default:
                    throw new IllegalArgumentException("no collection of kind " + kind);
                }
            } catch (NullPointerException | IllegalArgumentException e) {
                InvalidObjectException refused = new InvalidObjectException("not an unmodifiable collection: " + e);
                refused.initCause(e);
                throw refused;
            }
        }
    }
}
