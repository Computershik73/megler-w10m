// Megler: a class the framework did not have (core/framework-overlay; tools/build-framework.sh adds it). Android's hidden
// DisplayAdjustments, as far as android.content.res.ResourcesImpl's constructor takes one: ResourcesImpl(AssetManager,
// DisplayMetrics, Configuration, DisplayAdjustments) is how Android makes an implementation of resources, and code that makes
// its own by reflection asks for that constructor (ResourcesImpl.java says why the class is here). A display's adjustments are
// a configuration that overrides the display's; nothing here adjusts a display by them.
package android.view;

import android.content.res.Configuration;

/** The adjustments made to a display for an app: a configuration that overrides the display's (Android's hidden class). */
public class DisplayAdjustments {
    public static final DisplayAdjustments DEFAULT_DISPLAY_ADJUSTMENTS = new DisplayAdjustments();
    private final Configuration mConfiguration = new Configuration(Configuration.EMPTY);

    public DisplayAdjustments() {}
    public DisplayAdjustments(Configuration configuration) { if (configuration != null) mConfiguration.setTo(configuration); }
    public DisplayAdjustments(DisplayAdjustments daj) { if (daj != null) mConfiguration.setTo(daj.mConfiguration); }

    public void setConfiguration(Configuration configuration) { mConfiguration.setTo(configuration != null ? configuration : Configuration.EMPTY); }
    public Configuration getConfiguration() { return mConfiguration; }

    @Override public int hashCode() { return mConfiguration.hashCode(); }
    @Override public boolean equals(Object o) { return o instanceof DisplayAdjustments && mConfiguration.equals(((DisplayAdjustments) o).mConfiguration); }
}
