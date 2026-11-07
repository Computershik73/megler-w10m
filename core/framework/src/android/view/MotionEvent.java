package android.view;

/** A touch: the pointers down now, the action and which pointer it is about. Made by Megler from the screen's touches. */
public final class MotionEvent extends InputEvent {
    public static final int ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3, ACTION_OUTSIDE = 4, ACTION_POINTER_DOWN = 5,
        ACTION_POINTER_UP = 6, ACTION_HOVER_MOVE = 7, ACTION_SCROLL = 8, ACTION_HOVER_ENTER = 9, ACTION_HOVER_EXIT = 10, ACTION_BUTTON_PRESS = 11,
        ACTION_BUTTON_RELEASE = 12, ACTION_MASK = 0xff, ACTION_POINTER_INDEX_MASK = 0xff00, ACTION_POINTER_INDEX_SHIFT = 8, ACTION_POINTER_ID_MASK = 0xff00,
        ACTION_POINTER_ID_SHIFT = 8, ACTION_POINTER_1_DOWN = 5, ACTION_POINTER_2_DOWN = 0x105, ACTION_POINTER_1_UP = 6, ACTION_POINTER_2_UP = 0x106;
    public static final int AXIS_X = 0, AXIS_Y = 1, AXIS_PRESSURE = 2, AXIS_SIZE = 3, AXIS_VSCROLL = 9, AXIS_HSCROLL = 10, AXIS_Z = 11, AXIS_RX = 12,
        AXIS_RY = 13, AXIS_RZ = 14, AXIS_HAT_X = 15, AXIS_HAT_Y = 16, AXIS_LTRIGGER = 17, AXIS_RTRIGGER = 18, AXIS_GAS = 22, AXIS_BRAKE = 23;
    public static final int BUTTON_PRIMARY = 1, BUTTON_SECONDARY = 2, BUTTON_TERTIARY = 4, BUTTON_BACK = 8, BUTTON_FORWARD = 16;
    public static final int TOOL_TYPE_UNKNOWN = 0, TOOL_TYPE_FINGER = 1, TOOL_TYPE_STYLUS = 2, TOOL_TYPE_MOUSE = 3;
    public static final int EDGE_TOP = 1, EDGE_BOTTOM = 2, EDGE_LEFT = 4, EDGE_RIGHT = 8, FLAG_WINDOW_IS_OBSCURED = 1;

    private int action, count, source = InputDevice.SOURCE_TOUCHSCREEN, buttonState, flags, edgeFlags, metaState;
    private int[] ids;
    private float[] xs, ys;             /* in the receiving view's coordinates */
    private float[] rxs, rys;           /* on the screen */
    private long downTime, eventTime;
    private float[] axisScroll;
    /*
     * The samples a batched move gathered before its current one, oldest first (getHistorySize, getHistorical*): hcount of
     * them, each `count` pointers wide (hxs[sample * count + pointer]), in the receiving view's coordinates and on the
     * screen, and their times. Megler's driver batches the moves that come between two frames into one event, as Android
     * does (megler.AppRunner.touch), and VelocityTracker reads every sample. At most MAX_HISTORY, the oldest dropped.
     */
    private static final int MAX_HISTORY = 256;
    private int hcount;
    private long[] htimes;
    private float[] hxs, hys, hrxs, hrys;

    private MotionEvent() {}
    /** Megler's driver: a whole event at once, in screen coordinates. */
    public static MotionEvent meglerObtain(long downTime, long eventTime, int action, int count, int[] ids, float[] xs, float[] ys) {
        MotionEvent e = new MotionEvent();
        e.downTime = downTime; e.eventTime = eventTime; e.action = action; e.count = count; e.ids = ids; e.xs = xs; e.ys = ys;
        e.rxs = xs.clone(); e.rys = ys.clone();
        return e;
    }
    public static MotionEvent obtain(long downTime, long eventTime, int action, float x, float y, int metaState) {
        MotionEvent e = meglerObtain(downTime, eventTime, action, 1, new int[] { 0 }, new float[] { x }, new float[] { y });
        e.metaState = metaState;
        return e;
    }
    public static MotionEvent obtain(long downTime, long eventTime, int action, float x, float y, float pressure, float size, int metaState, float xPrecision, float yPrecision, int deviceId, int edgeFlags) {
        return obtain(downTime, eventTime, action, x, y, metaState);
    }
    public static MotionEvent obtain(long downTime, long eventTime, int action, int pointerCount, PointerProperties[] props, PointerCoords[] coords, int metaState, int buttonState, float xPrecision, float yPrecision, int deviceId, int edgeFlags, int source, int flags) {
        int[] ids = new int[pointerCount]; float[] x = new float[pointerCount], y = new float[pointerCount];
        for (int i = 0; i < pointerCount; i++) { ids[i] = props[i].id; x[i] = coords[i].x; y[i] = coords[i].y; }
        MotionEvent e = meglerObtain(downTime, eventTime, action, pointerCount, ids, x, y);
        e.source = source; e.buttonState = buttonState; e.metaState = metaState; e.flags = flags;
        return e;
    }
    public static MotionEvent obtain(MotionEvent o) {
        MotionEvent e = obtainNoHistory(o);
        if (o.hcount > 0) {
            int n = o.hcount * o.count;
            e.hcount = o.hcount;
            e.htimes = java.util.Arrays.copyOf(o.htimes, o.hcount);
            e.hxs = java.util.Arrays.copyOf(o.hxs, n); e.hys = java.util.Arrays.copyOf(o.hys, n);
            e.hrxs = java.util.Arrays.copyOf(o.hrxs, n); e.hrys = java.util.Arrays.copyOf(o.hrys, n);
        }
        return e;
    }
    public static MotionEvent obtainNoHistory(MotionEvent o) {
        MotionEvent e = new MotionEvent();
        e.downTime = o.downTime; e.eventTime = o.eventTime; e.action = o.action; e.count = o.count; e.ids = o.ids.clone(); e.xs = o.xs.clone(); e.ys = o.ys.clone();
        e.rxs = o.rxs.clone(); e.rys = o.rys.clone(); e.source = o.source; e.buttonState = o.buttonState; e.flags = o.flags; e.metaState = o.metaState;
        return e;
    }
    public final MotionEvent copy() { return obtain(this); }
    public final void recycle() {}
    public final int getAction() { return action; }
    public final int getActionMasked() { return action & ACTION_MASK; }
    public final int getActionIndex() { return (action & ACTION_POINTER_INDEX_MASK) >> ACTION_POINTER_INDEX_SHIFT; }
    public final void setAction(int a) { action = a; }
    public final int getPointerCount() { return count; }
    public final int getPointerId(int i) { return ids[i]; }
    public int getPointerIdBits() {
        int bits = 0;
        for (int i = 0; i < count; i++) bits |= 1 << ids[i];
        return bits;
    }
    public final int findPointerIndex(int id) { for (int i = 0; i < count; i++) if (ids[i] == id) return i; return -1; }
    public final float getX() { return xs[0]; }
    public final float getY() { return ys[0]; }
    public final float getX(int i) { return xs[i]; }
    public final float getY(int i) { return ys[i]; }
    public final float getRawX() { return rxs[0]; }
    public final float getRawY() { return rys[0]; }
    public float getRawX(int i) { return rxs[i]; }
    public float getRawY(int i) { return rys[i]; }
    public final float getPressure() { return getActionMasked() == ACTION_UP ? 0f : 1f; }
    public final float getPressure(int i) { return getPressure(); }
    public final float getSize() { return 0.1f; }
    public final float getSize(int i) { return 0.1f; }
    public final float getTouchMajor() { return 10f; }
    public final float getTouchMajor(int i) { return 10f; }
    public final float getTouchMinor() { return 10f; }
    public final float getTouchMinor(int i) { return 10f; }
    public final float getToolMajor() { return 10f; }
    public final float getToolMinor() { return 10f; }
    public final float getOrientation() { return 0f; }
    public final float getOrientation(int i) { return 0f; }
    public final float getXPrecision() { return 1f; }
    public final float getYPrecision() { return 1f; }
    public final float getAxisValue(int axis) { return getAxisValue(axis, 0); }
    public final float getAxisValue(int axis, int i) {
        if (axis == AXIS_X) return getX(i);
        if (axis == AXIS_Y) return getY(i);
        if (axis == AXIS_PRESSURE) return getPressure();
        if (axisScroll != null && (axis == AXIS_VSCROLL || axis == AXIS_HSCROLL)) return axisScroll[axis == AXIS_VSCROLL ? 0 : 1];
        return 0f;
    }
    public final void getPointerCoords(int i, PointerCoords out) { out.x = xs[i]; out.y = ys[i]; out.pressure = getPressure(); out.size = 0.1f; }
    public final void getPointerProperties(int i, PointerProperties out) { out.id = ids[i]; out.toolType = TOOL_TYPE_FINGER; }
    public final int getToolType(int i) { return TOOL_TYPE_FINGER; }
    public final int getButtonState() { return buttonState; }
    public final void setButtonState(int b) { buttonState = b; }
    public final int getActionButton() { return 0; }
    public final int getHistorySize() { return hcount; }
    public final float getHistoricalX(int h) { return getHistoricalX(0, h); }
    public final float getHistoricalY(int h) { return getHistoricalY(0, h); }
    /* (A sample out of range is the current one, as these were before there was a history: Android's throws.) */
    public final float getHistoricalX(int p, int h) { return h >= 0 && h < hcount ? hxs[h * count + p] : getX(p); }
    public final float getHistoricalY(int p, int h) { return h >= 0 && h < hcount ? hys[h * count + p] : getY(p); }
    public final float getHistoricalAxisValue(int axis, int p, int h) {
        if (axis == AXIS_X) return getHistoricalX(p, h);
        if (axis == AXIS_Y) return getHistoricalY(p, h);
        if (axis == AXIS_PRESSURE) return 1f;
        return getAxisValue(axis, p);
    }
    public final float getHistoricalAxisValue(int axis, int h) { return getHistoricalAxisValue(axis, 0, h); }
    public final float getHistoricalPressure(int p, int h) { return 1f; }
    public final float getHistoricalPressure(int h) { return 1f; }
    public final long getHistoricalEventTime(int h) { return h >= 0 && h < hcount ? htimes[h] : eventTime; }
    public final long getHistoricalEventTimeNanos(int h) { return getHistoricalEventTime(h) * 1000000L; }
    public final void getHistoricalPointerCoords(int p, int h, PointerCoords out) {
        getPointerCoords(p, out);
        out.x = getHistoricalX(p, h); out.y = getHistoricalY(p, h); out.pressure = 1f;
    }
    /* Megler: a sample's place on the screen, for VelocityTracker, which goes by the screen. */
    final float meglerHistoricalRawX(int p, int h) { return h >= 0 && h < hcount ? hrxs[h * count + p] : getRawX(p); }
    final float meglerHistoricalRawY(int p, int h) { return h >= 0 && h < hcount ? hrys[h * count + p] : getRawY(p); }

    /** Android's: the current sample into the history, and pointer 0 at (x, y) of this event's coordinates the current. */
    public final void addBatch(long eventTime, float x, float y, float pressure, float size, int metaState) {
        float[] nx = xs.clone(), ny = ys.clone();
        nx[0] = x; ny[0] = y;
        addSample(eventTime, nx, ny, false);
        this.metaState = metaState;
    }
    /** Android's: the current sample into the history, and every pointer where `coords` says the current. */
    public final void addBatch(long eventTime, PointerCoords[] coords, int metaState) {
        float[] nx = xs.clone(), ny = ys.clone();
        for (int i = 0; i < count && i < coords.length; i++) { nx[i] = coords[i].x; ny[i] = coords[i].y; }
        addSample(eventTime, nx, ny, false);
        this.metaState = metaState;
    }
    /** Android's: another move of the same pointers, its samples after this one's; false (nothing added) for anything else. */
    public final boolean addBatch(MotionEvent e) {
        if (e == null || getActionMasked() != ACTION_MOVE || e.getActionMasked() != ACTION_MOVE || e.count != count || e.source != source) return false;
        for (int i = 0; i < count; i++) if (e.ids[i] != ids[i]) return false;
        for (int h = 0; h < e.hcount; h++) {
            float[] nx = new float[count], ny = new float[count];
            System.arraycopy(e.hrxs, h * count, nx, 0, count); System.arraycopy(e.hrys, h * count, ny, 0, count);
            addSample(e.htimes[h], nx, ny, true);
        }
        addSample(e.eventTime, e.rxs.clone(), e.rys.clone(), true);
        return true;
    }
    /** Megler's driver: one more sample of a batched move, its pointers -- the same ones, in the same order -- at (sx, sy) on the screen. */
    public final void meglerAddSample(long eventTime, float[] sx, float[] sy) { addSample(eventTime, sx, sy, true); }
    /** Megler's driver: whether this event's pointers are the first n of `pointerIds`, in that order. */
    public final boolean meglerSamePointers(int[] pointerIds, int n) {
        if (n != count) return false;
        for (int i = 0; i < n; i++) if (ids[i] != pointerIds[i]) return false;
        return true;
    }
    /*
     * The current sample into the history and (nx, ny) the current one, at `t`: in this event's coordinates, or on the
     * screen (`screen`), the other of the two kept as far from them as the current sample's are.
     */
    private void addSample(long t, float[] nx, float[] ny, boolean screen) {
        int n = count;
        if (hcount == MAX_HISTORY) {
            System.arraycopy(htimes, 1, htimes, 0, hcount - 1);
            System.arraycopy(hxs, n, hxs, 0, (hcount - 1) * n); System.arraycopy(hys, n, hys, 0, (hcount - 1) * n);
            System.arraycopy(hrxs, n, hrxs, 0, (hcount - 1) * n); System.arraycopy(hrys, n, hrys, 0, (hcount - 1) * n);
            hcount--;
        }
        if (htimes == null || hcount == htimes.length) {
            int cap = htimes == null ? 4 : Math.min(MAX_HISTORY, htimes.length * 2);
            htimes = htimes == null ? new long[cap] : java.util.Arrays.copyOf(htimes, cap);
            hxs = hxs == null ? new float[cap * n] : java.util.Arrays.copyOf(hxs, cap * n);
            hys = hys == null ? new float[cap * n] : java.util.Arrays.copyOf(hys, cap * n);
            hrxs = hrxs == null ? new float[cap * n] : java.util.Arrays.copyOf(hrxs, cap * n);
            hrys = hrys == null ? new float[cap * n] : java.util.Arrays.copyOf(hrys, cap * n);
        }
        int at = hcount * n;
        htimes[hcount] = eventTime;
        System.arraycopy(xs, 0, hxs, at, n); System.arraycopy(ys, 0, hys, at, n);
        System.arraycopy(rxs, 0, hrxs, at, n); System.arraycopy(rys, 0, hrys, at, n);
        hcount++;
        for (int i = 0; i < n; i++) {
            float dx = rxs[i] - xs[i], dy = rys[i] - ys[i];
            if (screen) { rxs[i] = nx[i]; rys[i] = ny[i]; xs[i] = nx[i] - dx; ys[i] = ny[i] - dy; }
            else { xs[i] = nx[i]; ys[i] = ny[i]; rxs[i] = nx[i] + dx; rys[i] = ny[i] + dy; }
        }
        eventTime = t;
    }
    public final long getDownTime() { return downTime; }
    public final void setDownTime(long t) { downTime = t; }
    public final long getEventTime() { return eventTime; }
    public final long getEventTimeNanos() { return eventTime * 1000000L; }
    public final int getSource() { return source; }
    public final void setSource(int s) { source = s; }
    public final boolean isFromSource(int s) { return (source & s) == s; }
    public final int getMetaState() { return metaState; }
    public final int getFlags() { return flags; }
    public final int getEdgeFlags() { return edgeFlags; }
    public final void setEdgeFlags(int f) { edgeFlags = f; }
    public final int getDeviceId() { return 1; }
    public final InputDevice getDevice() { return InputDevice.getDevice(1); }
    public final int getDisplayId() { return 0; }
    public final void offsetLocation(float dx, float dy) {
        if (dx == 0 && dy == 0) return;
        for (int i = 0; i < count; i++) { xs[i] += dx; ys[i] += dy; }
        for (int i = 0, n = hcount * count; i < n; i++) { hxs[i] += dx; hys[i] += dy; }
    }
    public final void setLocation(float x, float y) { offsetLocation(x - xs[0], y - ys[0]); }
    public final void transform(android.graphics.Matrix m) {
        float[] p = new float[2];
        for (int i = 0; i < count; i++) { p[0] = xs[i]; p[1] = ys[i]; m.mapPoints(p); xs[i] = p[0]; ys[i] = p[1]; }
        for (int i = 0, n = hcount * count; i < n; i++) { p[0] = hxs[i]; p[1] = hys[i]; m.mapPoints(p); hxs[i] = p[0]; hys[i] = p[1]; }
    }
    public final void applyTransform(android.graphics.Matrix m) { transform(m); }
    /** Megler: a mouse wheel or trackpad scroll. */
    public static MotionEvent meglerScroll(float x, float y, float v, float h) {
        MotionEvent e = obtain(android.os.SystemClock.uptimeMillis(), android.os.SystemClock.uptimeMillis(), ACTION_SCROLL, x, y, 0);
        e.axisScroll = new float[] { v, h }; e.source = InputDevice.SOURCE_MOUSE;
        return e;
    }
    /** A copy holding only the pointers in idBits, its action recast for them (ViewGroup's split touches). */
    public final MotionEvent split(int idBits) {
        int n = 0;
        for (int i = 0; i < count; i++) if ((idBits & (1 << ids[i])) != 0) n++;
        MotionEvent e = obtainNoHistory(this);
        e.count = n; e.ids = new int[n]; e.xs = new float[n]; e.ys = new float[n]; e.rxs = new float[n]; e.rys = new float[n];
        int k = 0, newIndex = -1, am = getActionMasked(), ai = getActionIndex();
        for (int i = 0; i < count; i++) if ((idBits & (1 << ids[i])) != 0) {
            if (i == ai) newIndex = k;
            e.ids[k] = ids[i]; e.xs[k] = xs[i]; e.ys[k] = ys[i]; e.rxs[k] = rxs[i]; e.rys[k] = rys[i]; k++;
        }
        if (hcount > 0 && n > 0) {
            e.hcount = hcount;
            e.htimes = java.util.Arrays.copyOf(htimes, hcount);
            e.hxs = new float[hcount * n]; e.hys = new float[hcount * n]; e.hrxs = new float[hcount * n]; e.hrys = new float[hcount * n];
            for (int h = 0; h < hcount; h++) {
                int j = 0;
                for (int i = 0; i < count; i++) if ((idBits & (1 << ids[i])) != 0) {
                    e.hxs[h * n + j] = hxs[h * count + i]; e.hys[h * n + j] = hys[h * count + i];
                    e.hrxs[h * n + j] = hrxs[h * count + i]; e.hrys[h * n + j] = hrys[h * count + i]; j++;
                }
            }
        }
        if (am == ACTION_POINTER_DOWN || am == ACTION_POINTER_UP) {
            if (newIndex < 0) e.action = ACTION_MOVE;
            else if (n == 1) e.action = am == ACTION_POINTER_DOWN ? ACTION_DOWN : ACTION_UP;
            else e.action = am | (newIndex << ACTION_POINTER_INDEX_SHIFT);
        }
        return e;
    }
    public final boolean isButtonPressed(int b) { return (buttonState & b) != 0; }
    public static String actionToString(int a) {
        switch (a & ACTION_MASK) { case ACTION_DOWN: return "ACTION_DOWN"; case ACTION_UP: return "ACTION_UP"; case ACTION_MOVE: return "ACTION_MOVE"; case ACTION_CANCEL: return "ACTION_CANCEL";
        case ACTION_POINTER_DOWN: return "ACTION_POINTER_DOWN(" + ((a & ACTION_POINTER_INDEX_MASK) >> 8) + ")"; case ACTION_POINTER_UP: return "ACTION_POINTER_UP(" + ((a & ACTION_POINTER_INDEX_MASK) >> 8) + ")"; }
        return Integer.toString(a);
    }
    @Override public String toString() { return "MotionEvent { action=" + actionToString(action) + ", x=" + xs[0] + ", y=" + ys[0] + ", pointers=" + count + " }"; }
    public static final class PointerCoords {
        public float x, y, pressure, size, touchMajor, touchMinor, toolMajor, toolMinor, orientation;
        public PointerCoords() {}
        public PointerCoords(PointerCoords o) { copyFrom(o); }
        public void copyFrom(PointerCoords o) { x = o.x; y = o.y; pressure = o.pressure; size = o.size; }
        public void clear() { x = y = pressure = size = 0; }
        public float getAxisValue(int a) { return a == AXIS_X ? x : a == AXIS_Y ? y : a == AXIS_PRESSURE ? pressure : 0; }
        public void setAxisValue(int a, float v) { if (a == AXIS_X) x = v; else if (a == AXIS_Y) y = v; else if (a == AXIS_PRESSURE) pressure = v; }
        // ---- generated by tools/compat/fillmembers.py (PointerCoords): the platform's members this class does not write (signatures only)
        public boolean isResampled;
        public float relativeX;
        public float relativeY;
        public static android.view.MotionEvent.PointerCoords[] createArray(int p0) { return null; }
        public boolean isResampled() { return false; }
        // ---- end of generated members (PointerCoords)
    }
    public static final class PointerProperties {
        public int id, toolType;
        public PointerProperties() {}
        public PointerProperties(PointerProperties o) { copyFrom(o); }
        public void copyFrom(PointerProperties o) { id = o.id; toolType = o.toolType; }
        public void clear() { id = -1; toolType = 0; }
        // ---- generated by tools/compat/fillmembers.py (PointerProperties): the platform's members this class does not write (signatures only)
        public static android.view.MotionEvent.PointerProperties[] createArray(int p0) { return null; }
        // ---- end of generated members (PointerProperties)
    }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    private java.util.HashMap<String, Object> meglerFill; /* made at the first set (megler.Fill) */
    public static final int ACTION_POINTER_3_DOWN = 517;
    public static final int ACTION_POINTER_3_UP = 518;
    public static final int AXIS_DISTANCE = 24;
    public static final int AXIS_GENERIC_1 = 32;
    public static final int AXIS_GENERIC_10 = 41;
    public static final int AXIS_GENERIC_11 = 42;
    public static final int AXIS_GENERIC_12 = 43;
    public static final int AXIS_GENERIC_13 = 44;
    public static final int AXIS_GENERIC_14 = 45;
    public static final int AXIS_GENERIC_15 = 46;
    public static final int AXIS_GENERIC_16 = 47;
    public static final int AXIS_GENERIC_2 = 33;
    public static final int AXIS_GENERIC_3 = 34;
    public static final int AXIS_GENERIC_4 = 35;
    public static final int AXIS_GENERIC_5 = 36;
    public static final int AXIS_GENERIC_6 = 37;
    public static final int AXIS_GENERIC_7 = 38;
    public static final int AXIS_GENERIC_8 = 39;
    public static final int AXIS_GENERIC_9 = 40;
    public static final int AXIS_GESTURE_PINCH_SCALE_FACTOR = 52;
    public static final int AXIS_GESTURE_SCROLL_X_DISTANCE = 50;
    public static final int AXIS_GESTURE_SCROLL_Y_DISTANCE = 51;
    public static final int AXIS_GESTURE_SWIPE_FINGER_COUNT = 53;
    public static final int AXIS_GESTURE_X_OFFSET = 48;
    public static final int AXIS_GESTURE_Y_OFFSET = 49;
    public static final int AXIS_ORIENTATION = 8;
    public static final int AXIS_RELATIVE_X = 27;
    public static final int AXIS_RELATIVE_Y = 28;
    public static final int AXIS_RUDDER = 20;
    public static final int AXIS_SCROLL = 26;
    public static final int AXIS_THROTTLE = 19;
    public static final int AXIS_TILT = 25;
    public static final int AXIS_TOOL_MAJOR = 6;
    public static final int AXIS_TOOL_MINOR = 7;
    public static final int AXIS_TOUCH_MAJOR = 4;
    public static final int AXIS_TOUCH_MINOR = 5;
    public static final int AXIS_WHEEL = 21;
    public static final int BUTTON_STYLUS_PRIMARY = 32;
    public static final int BUTTON_STYLUS_SECONDARY = 64;
    public static final int CLASSIFICATION_AMBIGUOUS_GESTURE = 1;
    public static final int CLASSIFICATION_DEEP_PRESS = 2;
    public static final int CLASSIFICATION_MULTI_FINGER_SWIPE = 4;
    public static final int CLASSIFICATION_NONE = 0;
    public static final int CLASSIFICATION_PINCH = 5;
    public static final int CLASSIFICATION_TWO_FINGER_SWIPE = 3;
    public static android.os.Parcelable.Creator CREATOR;
    public static final int FLAG_CANCELED = 32;
    public static final int FLAG_HOVER_EXIT_PENDING = 4;
    public static final int FLAG_INJECTED_FROM_ACCESSIBILITY_TOOL = 4096;
    public static final int FLAG_IS_ACCESSIBILITY_EVENT = 2048;
    public static final int FLAG_IS_GENERATED_GESTURE = 8;
    public static final int FLAG_NO_FOCUS_CHANGE = 64;
    public static final int FLAG_SUPPORTS_DIRECTIONAL_ORIENTATION = 256;
    public static final int FLAG_SUPPORTS_ORIENTATION = 128;
    public static final int FLAG_TAINTED = -2147483648;
    public static final int FLAG_TARGET_ACCESSIBILITY_FOCUS = 1073741824;
    public static final int FLAG_WINDOW_IS_PARTIALLY_OBSCURED = 2;
    public static final int INVALID_POINTER_ID = -1;
    public static final int TOOL_TYPE_ERASER = 4;
    public static final int TOOL_TYPE_PALM = 5;
    private static final String[] AXIS_NAMES = { "AXIS_X", "AXIS_Y", "AXIS_PRESSURE", "AXIS_SIZE", "AXIS_TOUCH_MAJOR", "AXIS_TOUCH_MINOR", "AXIS_TOOL_MAJOR",
            "AXIS_TOOL_MINOR", "AXIS_ORIENTATION", "AXIS_VSCROLL", "AXIS_HSCROLL", "AXIS_Z", "AXIS_RX", "AXIS_RY", "AXIS_RZ", "AXIS_HAT_X", "AXIS_HAT_Y",
            "AXIS_LTRIGGER", "AXIS_RTRIGGER", "AXIS_THROTTLE", "AXIS_RUDDER", "AXIS_WHEEL", "AXIS_GAS", "AXIS_BRAKE", "AXIS_DISTANCE", "AXIS_TILT",
            "AXIS_SCROLL", "AXIS_RELATIVE_X", "AXIS_RELATIVE_Y" };
    public static int axisFromString(String s) {
        if (s == null) return -1;
        if (s.startsWith("AXIS_GENERIC_")) { try { int n = Integer.parseInt(s.substring(13)); return n >= 1 && n <= 16 ? 31 + n : -1; } catch (NumberFormatException e) { return -1; } }
        for (int i = 0; i < AXIS_NAMES.length; i++) if (AXIS_NAMES[i].equals(s)) return i;
        try { return Integer.parseInt(s); } catch (NumberFormatException e) { return -1; }
    }
    public static java.lang.String axisToString(int p0) { return null; }
    public static java.lang.String buttonStateToString(int p0) { return null; }
    public static java.lang.String classificationToString(int p0) { return null; }
    public static android.view.MotionEvent createFromParcelBody(android.os.Parcel p0) { return null; }
    public static android.graphics.Matrix createRotateMatrix(int p0, int p1, int p2) { return null; }
    public static android.view.MotionEvent obtain(long p0, long p1, int p2, float p3, float p4, float p5, float p6, int p7, float p8, float p9, int p10, int p11, int p12, int p13) { return null; }
    public static android.view.MotionEvent obtain(long p0, long p1, int p2, int p3, float p4, float p5, float p6, float p7, int p8, float p9, float p10, int p11, int p12) { return null; }
    public static android.view.MotionEvent obtain(long p0, long p1, int p2, int p3, int[] p4, android.view.MotionEvent.PointerCoords[] p5, int p6, float p7, float p8, int p9, int p10, int p11, int p12) { return null; }
    public static android.view.MotionEvent obtain(long p0, long p1, int p2, int p3, android.view.MotionEvent.PointerProperties[] p4, android.view.MotionEvent.PointerCoords[] p5, int p6, int p7, float p8, float p9, int p10, int p11, int p12, int p13, int p14) { return null; }
    public static android.view.MotionEvent obtain(long p0, long p1, int p2, int p3, android.view.MotionEvent.PointerProperties[] p4, android.view.MotionEvent.PointerCoords[] p5, int p6, int p7, float p8, float p9, int p10, int p11, int p12, int p13, int p14, int p15) { return null; }
    public static java.lang.String toolTypeToString(int p0) { return null; }
    public void cancel() {}
    public android.view.MotionEvent clampNoHistory(float p0, float p1, float p2, float p3) { return this; }
    public int getClassification() { return 0; }
    public float getHistoricalOrientation(int p0) { return 0f; }
    public float getHistoricalOrientation(int p0, int p1) { return 0f; }
    public float getHistoricalSize(int p0) { return 0f; }
    public float getHistoricalSize(int p0, int p1) { return 0f; }
    public float getHistoricalToolMajor(int p0) { return 0f; }
    public float getHistoricalToolMajor(int p0, int p1) { return 0f; }
    public float getHistoricalToolMinor(int p0) { return 0f; }
    public float getHistoricalToolMinor(int p0, int p1) { return 0f; }
    public float getHistoricalTouchMajor(int p0) { return 0f; }
    public float getHistoricalTouchMajor(int p0, int p1) { return 0f; }
    public float getHistoricalTouchMinor(int p0) { return 0f; }
    public float getHistoricalTouchMinor(int p0, int p1) { return 0f; }
    public int getId() { return 0; }
    public int getSurfaceRotation() { return 0; }
    public float getToolMajor(int p0) { return 0f; }
    public float getToolMinor(int p0) { return 0f; }
    public float getXCursorPosition() { return 0f; }
    public float getXDispatchLocation(int p0) { return 0f; }
    public float getYCursorPosition() { return 0f; }
    public float getYDispatchLocation(int p0) { return 0f; }
    public boolean isHoverEvent() { return false; }
    public boolean isHoverExitPending() { return (megler.Fill.get(meglerFill, "HoverExitPending") instanceof Boolean ? (Boolean) megler.Fill.get(meglerFill, "HoverExitPending") : false); }
    public boolean isInjectedFromAccessibilityService() { return false; }
    public boolean isInjectedFromAccessibilityTool() { return false; }
    public boolean isStylusPointer() { return false; }
    public boolean isSynthesizedTouchpadGesture() { return false; }
    public boolean isTainted() { return (megler.Fill.get(meglerFill, "Tainted") instanceof Boolean ? (Boolean) megler.Fill.get(meglerFill, "Tainted") : false); }
    public boolean isTargetAccessibilityFocus() { return (megler.Fill.get(meglerFill, "TargetAccessibilityFocus") instanceof Boolean ? (Boolean) megler.Fill.get(meglerFill, "TargetAccessibilityFocus") : false); }
    public boolean isTouchEvent() { return false; }
    public boolean isWithinBoundsNoHistory(float p0, float p1, float p2, float p3) { return false; }
    public void scale(float p0) {}
    public void setActionButton(int p0) {}
    public void setDisplayId(int p0) {}
    public void setHoverExitPending(boolean p0) { meglerFill = megler.Fill.put(meglerFill, "HoverExitPending", Boolean.valueOf(p0)); }
    public void setTainted(boolean p0) { meglerFill = megler.Fill.put(meglerFill, "Tainted", Boolean.valueOf(p0)); }
    public void setTargetAccessibilityFocus(boolean p0) { meglerFill = megler.Fill.put(meglerFill, "TargetAccessibilityFocus", Boolean.valueOf(p0)); }
    public void writeToParcel(android.os.Parcel p0, int p1) {}
    // ---- end of generated members
}
