package android.database;
/*
 * As Android's: a change is dispatched as a collection of URIs, with the flags it was told with, and comes to the app's
 * onChange through the chain of overloads Android's does, the most particular first (the user's, the collection's, one URI
 * with its flags, one URI, none), so that an app that overrides any of them hears it. dispatchChange with a URI and flags
 * and with a collection were generated stubs that did nothing, and the onChange of a collection with a user was never called.
 */
public abstract class ContentObserver {
    private final android.os.Handler mHandler;
    public ContentObserver(android.os.Handler h) { mHandler = h; }
    public boolean deliverSelfNotifications() { return false; }
    public void onChange(boolean self) {}
    public void onChange(boolean self, android.net.Uri u) { onChange(self); }
    public void onChange(boolean self, android.net.Uri u, int flags) { onChange(self, u); }
    public void onChange(boolean self, java.util.Collection<android.net.Uri> uris, int flags) { for (android.net.Uri u : uris) onChange(self, u, flags); }
    public void onChange(boolean self, java.util.Collection<android.net.Uri> uris, int flags, android.os.UserHandle user) { onChange(self, uris, flags); }
    public void onChange(boolean self, java.util.Collection<android.net.Uri> uris, int flags, int userId) { onChange(self, uris, flags, android.os.UserHandle.of(userId)); }
    public final void dispatchChange(boolean self) { dispatchChange(self, null); }
    public final void dispatchChange(boolean self, android.net.Uri u) { dispatchChange(self, u, 0); }
    public final void dispatchChange(boolean self, android.net.Uri u, int flags) { dispatchChange(self, java.util.Collections.singletonList(u), flags); }
    public final void dispatchChange(boolean self, java.util.Collection<android.net.Uri> uris, int flags) { dispatchChange(self, uris, flags, 0); }
    public final void dispatchChange(boolean self, java.util.Collection<android.net.Uri> uris, int flags, int userId) {
        if (mHandler == null) onChange(self, uris, flags, userId);
        else mHandler.post(() -> onChange(self, uris, flags, userId));
    }
}
