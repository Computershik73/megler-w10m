// Added by the overlay (tools/build-framework.sh): android.app.StatsManager, a system API of the platform's framework that
// Megler's had not. Google Play services (26.41) hands statsd its anomaly configurations through it
// (com.google.android.gms.common.subscriber.AnomalyConfigIntentOperation), and with no class its threads ended with
// NoClassDefFoundError: android/app/StatsManager in every app running Google's own services -- a user's VTB, Ozon and
// Discord (package 669). There is no statsd here: each call says so the way Android does while statsd is not running,
// with StatsUnavailableException, which its callers must catch.
package android.app;

import android.content.Context;
import android.util.AndroidException;

public final class StatsManager {
    public static final String ACTION_STATSD_STARTED = "android.app.action.STATSD_STARTED";
    public static final String EXTRA_STATS_ACTIVE_CONFIG_KEYS = "android.app.extra.STATS_ACTIVE_CONFIG_KEYS";
    public static final String EXTRA_STATS_BROADCAST_SUBSCRIBER_COOKIES = "android.app.extra.STATS_BROADCAST_SUBSCRIBER_COOKIES";
    public static final String EXTRA_STATS_CONFIG_KEY = "android.app.extra.STATS_CONFIG_KEY";
    public static final String EXTRA_STATS_CONFIG_UID = "android.app.extra.STATS_CONFIG_UID";
    public static final String EXTRA_STATS_DIMENSIONS_VALUE = "android.app.extra.STATS_DIMENSIONS_VALUE";
    public static final String EXTRA_STATS_SUBSCRIPTION_ID = "android.app.extra.STATS_SUBSCRIPTION_ID";
    public static final String EXTRA_STATS_SUBSCRIPTION_RULE_ID = "android.app.extra.STATS_SUBSCRIPTION_RULE_ID";
    public static final int PULL_SUCCESS = 0, PULL_SKIP = 1;

    public StatsManager(Context context) {}

    public void addConfig(long configKey, byte[] config) throws StatsUnavailableException { throw unavailable(); }
    public void removeConfig(long configKey) throws StatsUnavailableException { throw unavailable(); }
    public void setBroadcastSubscriber(PendingIntent pendingIntent, long configKey, long subscriberId) throws StatsUnavailableException { throw unavailable(); }
    public void setFetchReportsOperation(PendingIntent pendingIntent, long configKey) throws StatsUnavailableException { throw unavailable(); }
    public long[] setActiveConfigsChangedOperation(PendingIntent pendingIntent) throws StatsUnavailableException { throw unavailable(); }
    public byte[] getReports(long configKey) throws StatsUnavailableException { throw unavailable(); }
    public byte[] getStatsMetadata() throws StatsUnavailableException { throw unavailable(); }
    public long[] getRegisteredExperimentIds() throws StatsUnavailableException { throw unavailable(); }

    private static StatsUnavailableException unavailable() { return new StatsUnavailableException("there is no statsd on this system"); }

    public static final class StatsUnavailableException extends AndroidException {
        public StatsUnavailableException(String reason) { super("Failed to connect to statsd: " + reason); }
        public StatsUnavailableException(String reason, Throwable e) { super("Failed to connect to statsd: " + reason, e); }
    }
}
