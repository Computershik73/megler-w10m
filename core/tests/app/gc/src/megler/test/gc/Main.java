package megler.test.gc;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.util.Log;

import java.lang.ref.PhantomReference;
import java.lang.ref.Reference;
import java.lang.ref.ReferenceQueue;
import java.lang.ref.SoftReference;
import java.lang.ref.WeakReference;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The collector as an app meets it (core/src/dvm/mg_dvm_gc.c). Each step is a line of the log, "name ok" or "name FAILED" and
 * why. The objects a step drops are made in a method of their own, so that no register of the step's own frame still holds
 * one: the collector reads the interpreter's registers conservatively, and a stale copy would keep an object. For the same
 * reason a step asks for most, not all, of a hundred objects to be gone.
 */
public class Main extends Activity {
    private static final String T = "GcTest";
    private int ok, failed;

    private interface Step {
        boolean run() throws Throwable;
    }

    private String detail;

    private void step(String name, Step s) {
        boolean held;
        detail = null;
        try {
            held = s.run();
        } catch (Throwable t) {
            held = false;
            detail = t.toString();
        }
        if (held) ok++;
        else failed++;
        Log.i(T, name + (held ? " ok" : " FAILED") + (detail == null ? "" : ": " + detail));
    }

    /** Collected, and what is to be finalized finalized: what a test asks twice, to let the reference thread catch up. */
    private static void collect() {
        for (int i = 0; i < 2; i++) {
            System.gc();
            System.runFinalization();
        }
    }

    static class Finalized {
        static final AtomicInteger count = new AtomicInteger();
        final byte[] payload = new byte[64];

        @Override
        protected void finalize() {
            count.incrementAndGet();
        }
    }

    static class Revenant {
        static volatile Revenant back;
        static final AtomicInteger finalized = new AtomicInteger();
        final String name;

        Revenant(String name) {
            this.name = name;
        }

        @Override
        protected void finalize() {
            finalized.incrementAndGet();
            back = this;
        }
    }

    private static ArrayList<WeakReference<Object>> weakOnes(ReferenceQueue<Object> queue) {
        ArrayList<WeakReference<Object>> refs = new ArrayList<>();
        for (int i = 0; i < 100; i++) refs.add(new WeakReference<Object>(new int[16], queue));
        return refs;
    }

    private static ArrayList<PhantomReference<Object>> phantomOnes(ReferenceQueue<Object> queue) {
        ArrayList<PhantomReference<Object>> refs = new ArrayList<>();
        for (int i = 0; i < 100; i++) refs.add(new PhantomReference<Object>(new int[16], queue));
        return refs;
    }

    private static void finalizedOnes() {
        for (int i = 0; i < 100; i++) new Finalized();
    }

    private static void revenant() {
        new Revenant("came back");
    }

    private static int countQueued(ReferenceQueue<Object> queue, long waitMs) throws InterruptedException {
        int n = 0;
        long until = System.currentTimeMillis() + waitMs;
        while (System.currentTimeMillis() < until) {
            Reference<?> r = queue.remove(100);
            if (r != null) n++;
            else if (n >= 100) break;
        }
        return n;
    }

    /** A tree of `depth` levels whose leaves carry their own number: the sum checks that nothing in it was freed or changed. */
    static final class Node {
        Node left, right;
        int value;
    }

    private static Node tree(int depth, int[] next) {
        Node n = new Node();
        if (depth == 0) n.value = next[0]++;
        else {
            n.left = tree(depth - 1, next);
            n.right = tree(depth - 1, next);
        }
        return n;
    }

    private static long sum(Node n) {
        return n == null ? 0 : n.left == null ? n.value : sum(n.left) + sum(n.right);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        new Thread(this::steps, "gc-steps").start();
    }

    private void steps() {
        final ReferenceQueue<Object> weakQueue = new ReferenceQueue<>();
        step("weak", () -> {
            ArrayList<WeakReference<Object>> refs = weakOnes(weakQueue);
            collect();
            int cleared = 0;
            for (WeakReference<Object> r : refs) if (r.get() == null) cleared++;
            int queued = countQueued(weakQueue, 5000);
            detail = "cleared " + cleared + " enqueued " + queued;
            return cleared >= 90 && queued >= 90;
        });
        step("soft", () -> {
            SoftReference<Object> soft = new SoftReference<Object>(new int[16]);
            collect();
            detail = soft.get() == null ? "cleared" : "kept";
            /* Kept while there is room: soft references are cleared only before an OutOfMemoryError. */
            return soft.get() != null;
        });
        step("phantom", () -> {
            ReferenceQueue<Object> queue = new ReferenceQueue<>();
            ArrayList<PhantomReference<Object>> refs = phantomOnes(queue);
            collect();
            int queued = countQueued(queue, 5000);
            detail = "enqueued " + queued + " of " + refs.size();
            return queued >= 90;
        });
        step("finalize", () -> {
            Finalized.count.set(0);
            finalizedOnes();
            collect();
            detail = "finalized " + Finalized.count.get();
            return Finalized.count.get() >= 90;
        });
        step("resurrect", () -> {
            Revenant.back = null;
            revenant();
            collect();
            Revenant r = Revenant.back;
            detail = "finalized " + Revenant.finalized.get() + " back " + (r == null ? "null" : r.name);
            boolean back = r != null && "came back".equals(r.name);
            /* Gone again: finalize() is not called a second time. */
            Revenant.back = null;
            r = null;
            collect();
            return back && Revenant.finalized.get() == 1;
        });
        step("churn", () -> {
            /* Far more made than the heap holds: a window of a thousand arrays kept, each checked when it leaves. */
            byte[][] window = new byte[1000][];
            long made = 0;
            for (int i = 0; i < 60000; i++) {
                int at = i % window.length;
                if (window[at] != null && (window[at][0] != (byte) (i - window.length) || window[at][window[at].length - 1] != (byte) (i - window.length))) {
                    detail = "array " + (i - window.length) + " changed";
                    return false;
                }
                byte[] b = new byte[200 + (i % 7) * 300];
                b[0] = (byte) i;
                b[b.length - 1] = (byte) i;
                window[at] = b;
                made += b.length;
            }
            detail = "made " + (made >> 20) + " MB";
            return true;
        });
        step("threads", () -> {
            /* Four threads build and check trees at once while each other's allocations bring collections. */
            final AtomicInteger good = new AtomicInteger(), bad = new AtomicInteger();
            Thread[] threads = new Thread[4];
            for (int t = 0; t < threads.length; t++) {
                threads[t] = new Thread(() -> {
                    for (int round = 0; round < 12; round++) {
                        int[] next = { 0 };
                        Node root = tree(9, next);
                        ArrayList<String> words = new ArrayList<>();
                        for (int k = 0; k < 300; k++) words.add("word " + k + " of round " + round);
                        long want = (long) next[0] * (next[0] - 1) / 2;
                        boolean same = sum(root) == want;
                        for (int k = 0; k < words.size() && same; k++) same = words.get(k).equals("word " + k + " of round " + round);
                        if (same) good.incrementAndGet();
                        else bad.incrementAndGet();
                    }
                }, "gc-thread-" + t);
                threads[t].start();
            }
            for (Thread t : threads) t.join();
            detail = good.get() + " rounds right, " + bad.get() + " wrong";
            return bad.get() == 0 && good.get() == 48;
        });
        step("bitmaps", () -> {
            /* Bitmaps nobody keeps: their pixels go when they are finalized (Bitmap.finalize recycles). */
            for (int i = 0; i < 40; i++) Bitmap.createBitmap(256, 256, Bitmap.Config.ARGB_8888).eraseColor(0xFF00FF00);
            collect();
            return true;
        });
        Log.i(T, "done ok=" + ok + " failed=" + failed);
        runOnUiThread(this::finish);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "onDestroy");
    }
}
