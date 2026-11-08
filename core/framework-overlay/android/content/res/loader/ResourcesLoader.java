// Overlay of core/framework/gen-src/android/content/res/loader/ResourcesLoader.java (tools/build-framework.sh).
// It differs in keeping its providers: the generated stub's addProvider kept nothing. They are kept in the order added (a
// provider once), and the Resources a loader is added to (Resources.addLoaders) are told when they change, as Android's
// UpdateCallbacks are, so that what those Resources had cached of the old ones is let go. Resources looks in its loaders'
// providers before its own resources, the last added first (android/content/res/Resources.java, "resources loaders";
// ResourcesProvider.java says why: Google Play services' module resources).
package android.content.res.loader;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class ResourcesLoader {
    private final java.util.ArrayList<ResourcesProvider> mProviders = new java.util.ArrayList<>();
    private volatile ResourcesProvider[] mSnapshot = new ResourcesProvider[0];
    private final java.util.IdentityHashMap<Object, UpdateCallbacks> mCallbacks = new java.util.IdentityHashMap<>();

    public ResourcesLoader() {}

    public java.util.List getProviders() {
        synchronized (this) { return java.util.Collections.unmodifiableList(new java.util.ArrayList<>(mProviders)); }
    }
    /** Megler: the providers in the order added, read without a lock (Resources looks in them at every lookup). */
    public ResourcesProvider[] meglerProviders() { return mSnapshot; }

    public void addProvider(ResourcesProvider p0) {
        if (p0 == null) throw new NullPointerException("resourcesProvider");
        synchronized (this) {
            if (mProviders.contains(p0)) return;
            mProviders.add(p0);
            mSnapshot = mProviders.toArray(new ResourcesProvider[0]);
        }
        changed();
    }
    public void removeProvider(ResourcesProvider p0) {
        synchronized (this) {
            if (!mProviders.remove(p0)) return;
            mSnapshot = mProviders.toArray(new ResourcesProvider[0]);
        }
        changed();
    }
    public void setProviders(java.util.List p0) {
        synchronized (this) {
            mProviders.clear();
            for (Object o : p0) if (o instanceof ResourcesProvider && !mProviders.contains(o)) mProviders.add((ResourcesProvider) o);
            mSnapshot = mProviders.toArray(new ResourcesProvider[0]);
        }
        changed();
    }
    public void clearProviders() {
        synchronized (this) {
            if (mProviders.isEmpty()) return;
            mProviders.clear();
            mSnapshot = new ResourcesProvider[0];
        }
        changed();
    }
    public java.util.List getApkAssets() { return new java.util.ArrayList(); }

    public void registerOnProvidersChangedCallback(java.lang.Object p0, UpdateCallbacks p1) {
        synchronized (mCallbacks) { mCallbacks.put(p0, p1); }
    }
    public void unregisterOnProvidersChangedCallback(java.lang.Object p0) {
        synchronized (mCallbacks) { mCallbacks.remove(p0); }
    }
    private void changed() {
        java.util.ArrayList<UpdateCallbacks> tell;
        synchronized (mCallbacks) { tell = new java.util.ArrayList<>(mCallbacks.values()); }
        for (UpdateCallbacks c : tell) c.onLoaderUpdated(this);
    }

    public interface UpdateCallbacks {
        void onLoaderUpdated(ResourcesLoader p0);
    }
}
