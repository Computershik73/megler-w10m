// Overlay of core/framework/gen-src/android/os/RemoteCallbackList.java (tools/build-framework.sh).
// It differs in being a list. The generated stub registered nothing (register said false), and a broadcast went over no
// callbacks; this is Android's RemoteCallbackList: the callbacks by their binder, each with its cookie, a later registration
// of the same binder taking the earlier one's place, a broadcast over the callbacks as they were when it began
// (beginBroadcast, getBroadcastItem, getBroadcastCookie, finishBroadcast -- and Android's IllegalStateException for a
// broadcast begun inside one, or finished outside one), kill, the counts, and Android 16's Builder with what it was told.
// Nothing dies here: every binder is of this process, so onCallbackDied is never called.
//
// Why. androidx's MediaSessionCompat keeps its controllers' callbacks in one, and tells them of a playback state through
// it (MediaSessionImplApi21.setPlaybackState: beginBroadcast, getBroadcastItem(i).onPlaybackStateChanged, finishBroadcast).
// An app that watches its own session through a MediaControllerCompat -- Deezer's player does -- never heard that anything
// played or stopped. The members are the stub's, all of them.
package android.os;

import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.concurrent.Executor;
import java.util.function.BiConsumer;
import java.util.function.Consumer;

@SuppressWarnings({"unchecked", "rawtypes"})
public class RemoteCallbackList<E extends IInterface> {
    public static final int FROZEN_CALLEE_POLICY_DROP = 3;
    public static final int FROZEN_CALLEE_POLICY_ENQUEUE_ALL = 1;
    public static final int FROZEN_CALLEE_POLICY_ENQUEUE_MOST_RECENT = 2;
    public static final int FROZEN_CALLEE_POLICY_UNSET = 0;
    private static final int DEFAULT_MAX_QUEUE_SIZE = 1000;

    /** One registered callback: the interface, and what was registered with it. */
    private final class Entry implements IBinder.DeathRecipient {
        final E mInterface;
        final Object mCookie;
        Entry(E callbackInterface, Object cookie) { mInterface = callbackInterface; mCookie = cookie; }
        public void binderDied() {
            synchronized (mInterfaces) { mInterfaces.remove(mInterface.asBinder()); }
            onCallbackDied(mInterface, mCookie);
        }
    }

    /** The callbacks by their binder, in the order they were registered; its own lock. */
    private final LinkedHashMap<IBinder, Entry> mInterfaces = new LinkedHashMap<>();
    private Object[] mActiveBroadcast;
    private int mBroadcastCount = -1;
    private boolean mKilled;
    private final int mFrozenCalleePolicy;
    private final int mMaxQueueSize;
    private final Executor mExecutor;

    public RemoteCallbackList() { this(FROZEN_CALLEE_POLICY_UNSET, DEFAULT_MAX_QUEUE_SIZE, null); }
    RemoteCallbackList(int frozenCalleePolicy, int maxQueueSize, Executor executor) {
        mFrozenCalleePolicy = frozenCalleePolicy;
        mMaxQueueSize = maxQueueSize;
        mExecutor = executor;
    }

    public boolean register(E callbackInterface) { return register(callbackInterface, null); }
    /** False once the list is killed. A binder registered before is registered anew, with this cookie. */
    public boolean register(E callbackInterface, Object cookie) {
        synchronized (mInterfaces) {
            if (mKilled) return false;
            IBinder binder = callbackInterface.asBinder();
            Entry entry = new Entry(callbackInterface, cookie);
            unregister(callbackInterface);
            try {
                binder.linkToDeath(entry, 0);
            } catch (RemoteException e) {
                return false;
            }
            mInterfaces.put(binder, entry);
            return true;
        }
    }
    /** True when the callback was registered. A broadcast under way still reaches it. */
    public boolean unregister(E callbackInterface) {
        synchronized (mInterfaces) {
            Entry entry = mInterfaces.remove(callbackInterface.asBinder());
            if (entry == null) return false;
            entry.mInterface.asBinder().unlinkToDeath(entry, 0);
            return true;
        }
    }
    /** Every callback unregistered, and none can be registered after. */
    public void kill() {
        synchronized (mInterfaces) {
            for (Entry entry : mInterfaces.values()) entry.mInterface.asBinder().unlinkToDeath(entry, 0);
            mInterfaces.clear();
            mKilled = true;
        }
    }
    public void onCallbackDied(E callbackInterface) {}
    public void onCallbackDied(E callbackInterface, Object cookie) { onCallbackDied(callbackInterface); }

    /** The callbacks as they are now, for getBroadcastItem and getBroadcastCookie until finishBroadcast: how many. */
    public int beginBroadcast() {
        synchronized (mInterfaces) {
            if (mBroadcastCount > 0) throw new IllegalStateException("beginBroadcast() called while already in a broadcast");
            int n = mBroadcastCount = mInterfaces.size();
            if (n <= 0) return 0;
            Object[] active = mActiveBroadcast;
            if (active == null || active.length < n) mActiveBroadcast = active = new Object[n];
            int i = 0;
            for (Entry entry : mInterfaces.values()) active[i++] = entry;
            return n;
        }
    }
    public E getBroadcastItem(int index) { return ((Entry) mActiveBroadcast[index]).mInterface; }
    public Object getBroadcastCookie(int index) { return ((Entry) mActiveBroadcast[index]).mCookie; }
    public void finishBroadcast() {
        synchronized (mInterfaces) {
            if (mBroadcastCount < 0) throw new IllegalStateException("finishBroadcast() called outside of a broadcast");
            Object[] active = mActiveBroadcast;
            if (active != null) for (int i = 0; i < mBroadcastCount; i++) active[i] = null;
            mBroadcastCount = -1;
        }
    }
    public void broadcast(Consumer<E> action) {
        int n = beginBroadcast();
        try {
            for (int i = 0; i < n; i++) action.accept(getBroadcastItem(i));
        } finally {
            finishBroadcast();
        }
    }
    public <C> void broadcastForEachCookie(Consumer<C> action) {
        int n = beginBroadcast();
        try {
            for (int i = 0; i < n; i++) action.accept((C) getBroadcastCookie(i));
        } finally {
            finishBroadcast();
        }
    }
    public <C> void broadcast(BiConsumer<E, C> action) {
        int n = beginBroadcast();
        try {
            for (int i = 0; i < n; i++) action.accept(getBroadcastItem(i), (C) getBroadcastCookie(i));
        } finally {
            finishBroadcast();
        }
    }

    public int getRegisteredCallbackCount() {
        synchronized (mInterfaces) { return mKilled ? 0 : mInterfaces.size(); }
    }
    public E getRegisteredCallbackItem(int index) {
        Entry entry = registered(index);
        return entry == null ? null : entry.mInterface;
    }
    public Object getRegisteredCallbackCookie(int index) {
        Entry entry = registered(index);
        return entry == null ? null : entry.mCookie;
    }
    private Entry registered(int index) {
        synchronized (mInterfaces) {
            if (mKilled) return null;
            if (index < 0 || index >= mInterfaces.size()) throw new ArrayIndexOutOfBoundsException(index);
            for (Entry entry : mInterfaces.values()) if (index-- == 0) return entry;
            return null;
        }
    }

    /** What the Builder was told; here no callback's process is ever frozen, and nothing is queued. */
    public int getFrozenCalleePolicy() { return mFrozenCalleePolicy; }
    public int getMaxQueueSize() { return mMaxQueueSize; }
    public Executor getExecutor() { return mExecutor; }

    public void dump(PrintWriter pw, String prefix) {
        synchronized (mInterfaces) {
            pw.print(prefix); pw.print("callbacks: "); pw.println(mInterfaces.size());
            pw.print(prefix); pw.print("killed: "); pw.println(mKilled);
            pw.print(prefix); pw.print("broadcasts count: "); pw.println(mBroadcastCount);
        }
    }

    public static final class Builder<E extends IInterface> {
        private final int mFrozenCalleePolicy;
        private int mMaxQueueSize = DEFAULT_MAX_QUEUE_SIZE;
        private InterfaceDiedCallback mInterfaceDiedCallback;
        private Executor mExecutor;

        public Builder(int frozenCalleePolicy) { mFrozenCalleePolicy = frozenCalleePolicy; }
        public Builder<E> setMaxQueueSize(int maxQueueSize) {
            if (maxQueueSize <= 0) throw new IllegalArgumentException("maxQueueSize must be positive");
            mMaxQueueSize = maxQueueSize;
            return this;
        }
        public Builder<E> setInterfaceDiedCallback(InterfaceDiedCallback<E> callback) {
            mInterfaceDiedCallback = callback;
            return this;
        }
        public Builder<E> setExecutor(Executor executor) {
            mExecutor = executor;
            return this;
        }
        public RemoteCallbackList<E> build() {
            final InterfaceDiedCallback died = mInterfaceDiedCallback;
            if (died == null) return new RemoteCallbackList<E>(mFrozenCalleePolicy, mMaxQueueSize, mExecutor);
            return new RemoteCallbackList<E>(mFrozenCalleePolicy, mMaxQueueSize, mExecutor) {
                @Override public void onCallbackDied(E deadInterface, Object cookie) {
                    died.onInterfaceDied(this, deadInterface, cookie);
                }
            };
        }

        public interface InterfaceDiedCallback<E extends IInterface> {
            void onInterfaceDied(RemoteCallbackList<E> callbackList, E deadInterface, Object cookie);
        }
    }
}
