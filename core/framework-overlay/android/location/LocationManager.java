// Overlay of core/framework/gen-src/android/location/LocationManager.java (tools/build-framework.sh).
// It differs in finding the phone: the original is the generated stub, which had no provider, said every one was disabled,
// knew no location and never called a listener -- an app that checks isProviderEnabled("gps") asked the user to switch the
// GPS on (Urent, the scooter rental), and one that asks for updates waited for ever. Here the providers gps, network,
// passive and fused are the phone's location as the shell finds it (Windows.Devices.Geolocation, or a place set in Meglet's
// settings), kept and delivered by meglet/LocationService.java: isProviderEnabled and isLocationEnabled say whether it is on,
// getLastKnownLocation is the newest fix, requestLocationUpdates, requestSingleUpdate and getCurrentLocation in all their
// forms (a listener on a Looper or an Executor, a PendingIntent) are delivered to, removeUpdates stops them. GNSS status
// and NMEA listeners are taken and never called (there are no satellites to report). Every other member of the stub is
// here as it was.
package android.location;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class LocationManager {
    private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
    public static final java.lang.String ACTION_ADAS_GNSS_ENABLED_CHANGED = "android.location.action.ADAS_GNSS_ENABLED_CHANGED";
    public static final java.lang.String ACTION_GNSS_CAPABILITIES_CHANGED = "android.location.action.GNSS_CAPABILITIES_CHANGED";
    public static final long BLOCK_GPS_STATUS_USAGE = 144027538L;
    public static final long BLOCK_IMMUTABLE_PENDING_INTENTS = 171317480L;
    public static final long BLOCK_INCOMPLETE_LOCATIONS = 148964793L;
    public static final long BLOCK_PENDING_INTENT_SYSTEM_API_USAGE = 169887240L;
    public static final long BLOCK_UNTARGETED_PENDING_INTENTS = 148963590L;
    public static final long DELIVER_HISTORICAL_LOCATIONS = 73144566L;
    public static final java.lang.String EXTRA_ADAS_GNSS_ENABLED = "android.location.extra.ADAS_GNSS_ENABLED";
    public static final java.lang.String EXTRA_GNSS_CAPABILITIES = "android.location.extra.GNSS_CAPABILITIES";
    public static final java.lang.String EXTRA_LOCATION_ENABLED = "android.location.extra.LOCATION_ENABLED";
    public static final java.lang.String EXTRA_PROVIDER_ENABLED = "android.location.extra.PROVIDER_ENABLED";
    public static final java.lang.String EXTRA_PROVIDER_NAME = "android.location.extra.PROVIDER_NAME";
    public static final java.lang.String FUSED_PROVIDER = "fused";
    public static final long GET_PROVIDER_SECURITY_EXCEPTIONS = 150935354L;
    public static final java.lang.String GPS_HARDWARE_PROVIDER = "gps_hardware";
    public static final java.lang.String GPS_PROVIDER = "gps";
    public static final java.lang.String HIGH_POWER_REQUEST_CHANGE_ACTION = "android.location.HIGH_POWER_REQUEST_CHANGE";
    public static final java.lang.String KEY_FLUSH_COMPLETE = "flushComplete";
    public static final java.lang.String KEY_LOCATIONS = "locations";
    public static final java.lang.String KEY_LOCATION_CHANGED = "location";
    public static final java.lang.String KEY_PROVIDER_ENABLED = "providerEnabled";
    public static final java.lang.String KEY_PROXIMITY_ENTERING = "entering";
    public static final java.lang.String KEY_STATUS_CHANGED = "status";
    public static final java.lang.String METADATA_SETTINGS_FOOTER_HTML = "com.android.settings.location.FOOTER_HTML";
    public static final java.lang.String METADATA_SETTINGS_FOOTER_STRING = "com.android.settings.location.FOOTER_STRING";
    public static final java.lang.String MODE_CHANGED_ACTION = "android.location.MODE_CHANGED";
    public static final java.lang.String NETWORK_PROVIDER = "network";
    public static final java.lang.String PASSIVE_PROVIDER = "passive";
    public static final java.lang.String PROVIDERS_CHANGED_ACTION = "android.location.PROVIDERS_CHANGED";
    public static final java.lang.String SETTINGS_FOOTER_DISPLAYED_ACTION = "com.android.settings.location.DISPLAYED_FOOTER";
    private android.content.Context mContext;
    public LocationManager(android.content.Context p0, android.location.ILocationManager p1) { mContext = p0; }
    public static void disableLocalLocationEnabledCaches() {}
    public static void invalidateLocalLocationEnabledCaches() {}
    public boolean addGpsMeasurementListener(android.location.GpsMeasurementsEvent.Listener p0) { return false; }
    public boolean addGpsNavigationMessageListener(android.location.GpsNavigationMessageEvent.Listener p0) { return false; }
    public boolean addGpsStatusListener(android.location.GpsStatus.Listener p0) { return true; }
    public boolean addNmeaListener(android.location.GpsStatus.NmeaListener p0) { return false; }
    public boolean addNmeaListener(android.location.OnNmeaMessageListener p0) { return true; }
    public boolean addNmeaListener(android.location.OnNmeaMessageListener p0, android.os.Handler p1) { return true; }
    public boolean addNmeaListener(java.util.concurrent.Executor p0, android.location.OnNmeaMessageListener p1) { return true; }
    public void addProviderRequestChangedListener(java.util.concurrent.Executor p0, android.location.provider.ProviderRequest.ChangedListener p1) {}
    public void addProximityAlert(double p0, double p1, float p2, long p3, android.app.PendingIntent p4) {}
    public void addTestProvider(java.lang.String p0, android.location.provider.ProviderProperties p1) {}
    public void addTestProvider(java.lang.String p0, android.location.provider.ProviderProperties p1, java.util.Set p2) {}
    public void addTestProvider(java.lang.String p0, boolean p1, boolean p2, boolean p3, boolean p4, boolean p5, boolean p6, boolean p7, int p8, int p9) {}
    public void clearTestProviderEnabled(java.lang.String p0) {}
    public void clearTestProviderLocation(java.lang.String p0) {}
    public void clearTestProviderStatus(java.lang.String p0) {}
    public void flushGnssBatch() {}
    public android.os.PackageTagsList getAdasAllowlist() { return (android.os.PackageTagsList) megletProps.get("AdasAllowlist"); }
    public java.util.List getAllProviders() { return providers(false); }
    public java.lang.String[] getBackgroundThrottlingWhitelist() { return (java.lang.String[]) megletProps.get("BackgroundThrottlingWhitelist"); }
    public java.lang.String getBestProvider(android.location.Criteria p0, boolean p1) { return !p1 || meglet.LocationService.enabled() ? provider(p0) : null; }
    public void getCurrentLocation(android.location.LocationRequest p0, android.os.CancellationSignal p1, java.util.concurrent.Executor p2, java.util.function.Consumer p3) { meglet.LocationService.current(providerOf(p0), p1, p2, p3); }
    public void getCurrentLocation(java.lang.String p0, android.location.LocationRequest p1, android.os.CancellationSignal p2, java.util.concurrent.Executor p3, java.util.function.Consumer p4) { check(p0); meglet.LocationService.current(p0, p2, p3, p4); }
    public void getCurrentLocation(java.lang.String p0, android.os.CancellationSignal p1, java.util.concurrent.Executor p2, java.util.function.Consumer p3) { check(p0); meglet.LocationService.current(p0, p1, p2, p3); }
    public java.lang.String getExtraLocationControllerPackage() { return (java.lang.String) megletProps.get("ExtraLocationControllerPackage"); }
    public java.util.List getGnssAntennaInfos() { return (megletProps.get("GnssAntennaInfos") != null ? (java.util.List) megletProps.get("GnssAntennaInfos") : new java.util.ArrayList()); }
    public int getGnssBatchSize() { return (megletProps.get("GnssBatchSize") instanceof Integer ? (Integer) megletProps.get("GnssBatchSize") : 0); }
    public android.location.GnssCapabilities getGnssCapabilities() { return (android.location.GnssCapabilities) megletProps.get("GnssCapabilities"); }
    public java.lang.String getGnssHardwareModelName() { return (java.lang.String) megletProps.get("GnssHardwareModelName"); }
    public int getGnssYearOfHardware() { return (megletProps.get("GnssYearOfHardware") instanceof Integer ? (Integer) megletProps.get("GnssYearOfHardware") : 0); }
    public android.location.GpsStatus getGpsStatus(android.location.GpsStatus p0) { return null; }
    public android.os.PackageTagsList getIgnoreSettingsAllowlist() { return (android.os.PackageTagsList) megletProps.get("IgnoreSettingsAllowlist"); }
    public java.lang.String[] getIgnoreSettingsWhitelist() { return (java.lang.String[]) megletProps.get("IgnoreSettingsWhitelist"); }
    public android.location.Location getLastKnownLocation(java.lang.String p0) { check(p0); return meglet.LocationService.last(p0); }
    public android.location.Location getLastKnownLocation(java.lang.String p0, android.location.LastLocationRequest p1) { return getLastKnownLocation(p0); }
    public android.location.Location getLastLocation() { return meglet.LocationService.last(FUSED_PROVIDER); }
    public android.location.LocationProvider getProvider(java.lang.String p0) { return meglet.LocationService.known(p0) ? new Provider(p0) : null; }
    public java.util.List getProviderPackages(java.lang.String p0) { return new java.util.ArrayList(); }
    public android.location.provider.ProviderProperties getProviderProperties(java.lang.String p0) { return null; }
    public java.util.List getProviders(android.location.Criteria p0, boolean p1) { return providers(p1); }
    public java.util.List getProviders(boolean p0) { return providers(p0); }
    public boolean hasProvider(java.lang.String p0) { return meglet.LocationService.known(p0); }
    public void injectGnssMeasurementCorrections(android.location.GnssMeasurementCorrections p0) {}
    public boolean injectLocation(android.location.Location p0) { return false; }
    public boolean isAdasGnssLocationEnabled() { return (megletProps.get("AdasGnssLocationEnabled") instanceof Boolean ? (Boolean) megletProps.get("AdasGnssLocationEnabled") : false); }
    public boolean isAutomotiveGnssSuspended() { return (megletProps.get("AutomotiveGnssSuspended") instanceof Boolean ? (Boolean) megletProps.get("AutomotiveGnssSuspended") : false); }
    public boolean isExtraLocationControllerPackageEnabled() { return (megletProps.get("ExtraLocationControllerPackageEnabled") instanceof Boolean ? (Boolean) megletProps.get("ExtraLocationControllerPackageEnabled") : false); }
    public boolean isLocationEnabled() { return meglet.LocationService.enabled(); }
    public boolean isLocationEnabledForUser(android.os.UserHandle p0) { return meglet.LocationService.enabled(); }
    public boolean isProviderEnabled(java.lang.String p0) { check(p0); return PASSIVE_PROVIDER.equals(p0) || meglet.LocationService.enabled(); }
    public boolean isProviderEnabledForUser(java.lang.String p0, android.os.UserHandle p1) { return isProviderEnabled(p0); }
    public boolean isProviderPackage(java.lang.String p0) { return false; }
    public boolean isProviderPackage(java.lang.String p0, java.lang.String p1) { return false; }
    public boolean isProviderPackage(java.lang.String p0, java.lang.String p1, java.lang.String p2) { return false; }
    public boolean registerAntennaInfoListener(java.util.concurrent.Executor p0, android.location.GnssAntennaInfo.Listener p1) { return false; }
    public boolean registerGnssBatchedLocationCallback(long p0, boolean p1, android.location.BatchedLocationCallback p2, android.os.Handler p3) { return false; }
    public boolean registerGnssMeasurementsCallback(android.location.GnssMeasurementRequest p0, java.util.concurrent.Executor p1, android.location.GnssMeasurementsEvent.Callback p2) { return false; }
    public boolean registerGnssMeasurementsCallback(android.location.GnssMeasurementsEvent.Callback p0) { return false; }
    public boolean registerGnssMeasurementsCallback(android.location.GnssMeasurementsEvent.Callback p0, android.os.Handler p1) { return false; }
    public boolean registerGnssMeasurementsCallback(android.location.GnssRequest p0, java.util.concurrent.Executor p1, android.location.GnssMeasurementsEvent.Callback p2) { return false; }
    public boolean registerGnssMeasurementsCallback(java.util.concurrent.Executor p0, android.location.GnssMeasurementsEvent.Callback p1) { return false; }
    public boolean registerGnssNavigationMessageCallback(android.location.GnssNavigationMessage.Callback p0) { return false; }
    public boolean registerGnssNavigationMessageCallback(android.location.GnssNavigationMessage.Callback p0, android.os.Handler p1) { return false; }
    public boolean registerGnssNavigationMessageCallback(java.util.concurrent.Executor p0, android.location.GnssNavigationMessage.Callback p1) { return false; }
    public boolean registerGnssStatusCallback(android.location.GnssStatus.Callback p0) { return true; }
    public boolean registerGnssStatusCallback(android.location.GnssStatus.Callback p0, android.os.Handler p1) { return true; }
    public boolean registerGnssStatusCallback(java.util.concurrent.Executor p0, android.location.GnssStatus.Callback p1) { return true; }
    public void removeGpsMeasurementListener(android.location.GpsMeasurementsEvent.Listener p0) {}
    public void removeGpsNavigationMessageListener(android.location.GpsNavigationMessageEvent.Listener p0) {}
    public void removeGpsStatusListener(android.location.GpsStatus.Listener p0) {}
    public void removeNmeaListener(android.location.GpsStatus.NmeaListener p0) {}
    public void removeNmeaListener(android.location.OnNmeaMessageListener p0) {}
    public void removeProviderRequestChangedListener(android.location.provider.ProviderRequest.ChangedListener p0) {}
    public void removeProximityAlert(android.app.PendingIntent p0) {}
    public void removeTestProvider(java.lang.String p0) {}
    public void removeUpdates(android.app.PendingIntent p0) { meglet.LocationService.remove(p0); }
    public void removeUpdates(android.location.LocationListener p0) { meglet.LocationService.remove(p0); }
    public void requestFlush(java.lang.String p0, android.app.PendingIntent p1, int p2) {}
    public void requestFlush(java.lang.String p0, android.location.LocationListener p1, int p2) { meglet.LocationService.flush(p1, p2); }
    public void requestLocationUpdates(long p0, float p1, android.location.Criteria p2, android.app.PendingIntent p3) { meglet.LocationService.request(provider(p2), p0, p1, p3, mContext, 0); }
    public void requestLocationUpdates(long p0, float p1, android.location.Criteria p2, android.location.LocationListener p3, android.os.Looper p4) { meglet.LocationService.request(provider(p2), p0, p1, p3, meglet.LocationService.executorFor(p4), 0); }
    public void requestLocationUpdates(long p0, float p1, android.location.Criteria p2, java.util.concurrent.Executor p3, android.location.LocationListener p4) { meglet.LocationService.request(provider(p2), p0, p1, p4, p3, 0); }
    public void requestLocationUpdates(android.location.LocationRequest p0, android.app.PendingIntent p1) { meglet.LocationService.request(providerOf(p0), intervalOf(p0), p0.getMinUpdateDistanceMeters(), p1, mContext, p0.getMaxUpdates()); }
    public void requestLocationUpdates(android.location.LocationRequest p0, android.location.LocationListener p1, android.os.Looper p2) { meglet.LocationService.request(providerOf(p0), intervalOf(p0), p0.getMinUpdateDistanceMeters(), p1, meglet.LocationService.executorFor(p2), p0.getMaxUpdates()); }
    public void requestLocationUpdates(android.location.LocationRequest p0, java.util.concurrent.Executor p1, android.location.LocationListener p2) { meglet.LocationService.request(providerOf(p0), intervalOf(p0), p0.getMinUpdateDistanceMeters(), p2, p1, p0.getMaxUpdates()); }
    public void requestLocationUpdates(java.lang.String p0, long p1, float p2, android.app.PendingIntent p3) { check(p0); meglet.LocationService.request(p0, p1, p2, p3, mContext, 0); }
    public void requestLocationUpdates(java.lang.String p0, long p1, float p2, android.location.LocationListener p3) { check(p0); meglet.LocationService.request(p0, p1, p2, p3, meglet.LocationService.executorFor(null), 0); }
    public void requestLocationUpdates(java.lang.String p0, long p1, float p2, android.location.LocationListener p3, android.os.Looper p4) { check(p0); meglet.LocationService.request(p0, p1, p2, p3, meglet.LocationService.executorFor(p4), 0); }
    public void requestLocationUpdates(java.lang.String p0, long p1, float p2, java.util.concurrent.Executor p3, android.location.LocationListener p4) { check(p0); meglet.LocationService.request(p0, p1, p2, p4, p3, 0); }
    public void requestLocationUpdates(java.lang.String p0, android.location.LocationRequest p1, android.app.PendingIntent p2) { check(p0); meglet.LocationService.request(p0, intervalOf(p1), p1.getMinUpdateDistanceMeters(), p2, mContext, p1.getMaxUpdates()); }
    public void requestLocationUpdates(java.lang.String p0, android.location.LocationRequest p1, java.util.concurrent.Executor p2, android.location.LocationListener p3) { check(p0); meglet.LocationService.request(p0, intervalOf(p1), p1.getMinUpdateDistanceMeters(), p3, p2, p1.getMaxUpdates()); }
    public void requestSingleUpdate(android.location.Criteria p0, android.app.PendingIntent p1) { meglet.LocationService.request(provider(p0), 0, 0, p1, mContext, 1); }
    public void requestSingleUpdate(android.location.Criteria p0, android.location.LocationListener p1, android.os.Looper p2) { meglet.LocationService.request(provider(p0), 0, 0, p1, meglet.LocationService.executorFor(p2), 1); }
    public void requestSingleUpdate(java.lang.String p0, android.app.PendingIntent p1) { check(p0); meglet.LocationService.request(p0, 0, 0, p1, mContext, 1); }
    public void requestSingleUpdate(java.lang.String p0, android.location.LocationListener p1, android.os.Looper p2) { check(p0); meglet.LocationService.request(p0, 0, 0, p1, meglet.LocationService.executorFor(p2), 1); }
    public boolean sendExtraCommand(java.lang.String p0, java.lang.String p1, android.os.Bundle p2) { return false; }
    public void setAdasGnssLocationEnabled(boolean p0) { megletProps.put("AdasGnssLocationEnabled", Boolean.valueOf(p0)); }
    public void setAutomotiveGnssSuspended(boolean p0) { megletProps.put("AutomotiveGnssSuspended", Boolean.valueOf(p0)); }
    public void setExtraLocationControllerPackage(java.lang.String p0) { megletProps.put("ExtraLocationControllerPackage", p0); }
    public void setExtraLocationControllerPackageEnabled(boolean p0) { megletProps.put("ExtraLocationControllerPackageEnabled", Boolean.valueOf(p0)); }
    public void setLocationControllerExtraPackage(java.lang.String p0) { megletProps.put("LocationControllerExtraPackage", p0); }
    public void setLocationControllerExtraPackageEnabled(boolean p0) { megletProps.put("LocationControllerExtraPackageEnabled", Boolean.valueOf(p0)); }
    public void setLocationEnabledForUser(boolean p0, android.os.UserHandle p1) {}
    public boolean setProviderEnabledForUser(java.lang.String p0, boolean p1, android.os.UserHandle p2) { return false; }
    public void setTestProviderEnabled(java.lang.String p0, boolean p1) {}
    public void setTestProviderLocation(java.lang.String p0, android.location.Location p1) {}
    public void setTestProviderStatus(java.lang.String p0, int p1, android.os.Bundle p2, long p3) {}
    public void unregisterAntennaInfoListener(android.location.GnssAntennaInfo.Listener p0) {}
    public boolean unregisterGnssBatchedLocationCallback(android.location.BatchedLocationCallback p0) { return false; }
    public void unregisterGnssMeasurementsCallback(android.location.GnssMeasurementsEvent.Callback p0) {}
    public void unregisterGnssNavigationMessageCallback(android.location.GnssNavigationMessage.Callback p0) {}
    public void unregisterGnssStatusCallback(android.location.GnssStatus.Callback p0) {}
    LocationManager() { this((android.content.Context) null, (android.location.ILocationManager) null); }

    private static java.util.List providers(boolean enabledOnly) {
        java.util.ArrayList r = new java.util.ArrayList();
        boolean on = meglet.LocationService.enabled();
        for (String p : meglet.LocationService.PROVIDERS)
            if (!enabledOnly || on || PASSIVE_PROVIDER.equals(p)) r.add(p);
        return r;
    }

    private static String provider(android.location.Criteria c) { return GPS_PROVIDER; }

    private static long intervalOf(android.location.LocationRequest r) {
        long ms = r.getIntervalMillis();
        return ms > 0 ? ms : r.getInterval();
    }

    private static String providerOf(android.location.LocationRequest r) {
        String p = r.getProvider();
        return p != null ? p : FUSED_PROVIDER;
    }

    /* One of the four providers, as Android's: a request for any other is refused. */
    private static void check(String provider) {
        if (provider == null) throw new IllegalArgumentException("invalid null provider");
        if (!meglet.LocationService.known(provider)) throw new IllegalArgumentException("provider \"" + provider + "\" does not exist");
    }

    /* A provider the app may be told of. */
    private static final class Provider extends LocationProvider {
        private final String name;
        Provider(String name) { this.name = name; }
        @Override public String getName() { return name; }
        @Override public int getAccuracy() { return NETWORK_PROVIDER.equals(name) ? Criteria.ACCURACY_COARSE : Criteria.ACCURACY_FINE; }
        @Override public int getPowerRequirement() { return GPS_PROVIDER.equals(name) ? Criteria.POWER_HIGH : Criteria.POWER_LOW; }
        @Override public boolean meetsCriteria(Criteria c) { return true; }
        @Override public boolean requiresSatellite() { return GPS_PROVIDER.equals(name); }
        @Override public boolean requiresNetwork() { return NETWORK_PROVIDER.equals(name); }
        @Override public boolean supportsAltitude() { return !NETWORK_PROVIDER.equals(name); }
        @Override public boolean supportsBearing() { return !NETWORK_PROVIDER.equals(name); }
        @Override public boolean supportsSpeed() { return !NETWORK_PROVIDER.equals(name); }
    }
}
