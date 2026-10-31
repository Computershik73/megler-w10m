// Megler: the values the framework's filled-in setters keep for their getters (a class's meglerFill), in a map made at the
// first set. Each object of such a class made its map with it: every Message, MotionEvent, InputEvent and Parcel, a few
// percent of what Yandex Maps made while its map was moved, for setters few of them ever call (package 537).
package megler;

import java.util.HashMap;

public final class Fill {
    private Fill() {}

    /** What was set under `key`, or null when nothing was (or nothing at all, `m` null). */
    public static Object get(HashMap<String, Object> m, String key) { return m == null ? null : m.get(key); }

    /** `m` with `key` set to `v`: a new map when `m` is null. */
    public static HashMap<String, Object> put(HashMap<String, Object> m, String key, Object v) {
        if (m == null) m = new HashMap<>();
        m.put(key, v);
        return m;
    }
}
