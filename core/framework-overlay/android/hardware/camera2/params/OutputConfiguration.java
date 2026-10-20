// Overlay of core/framework/gen-src/android/hardware/camera2/params/OutputConfiguration.java, written by
// tools/compat/camera2-overlays.py from that generated stub (run it again rather than editing this file).
// It differs in keeping its surfaces (or a deferred one's size) and its group, which the stub's constructors
// dropped: a session's outputs are read from them.
package android.hardware.camera2.params;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class OutputConfiguration implements android.os.Parcelable {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static android.os.Parcelable.Creator CREATOR;
    public static final int MIRROR_MODE_AUTO = 0;
    public static final int MIRROR_MODE_H = 2;
    public static final int MIRROR_MODE_NONE = 1;
    public static final int MIRROR_MODE_V = 3;
    public static final int ROTATION_0 = 0;
    public static final int ROTATION_180 = 2;
    public static final int ROTATION_270 = 3;
    public static final int ROTATION_90 = 1;
    public static final int SURFACE_GROUP_ID_NONE = -1;
    public static final int TIMESTAMP_BASE_CHOREOGRAPHER_SYNCED = 4;
    public static final int TIMESTAMP_BASE_DEFAULT = 0;
    public static final int TIMESTAMP_BASE_MONOTONIC = 2;
    public static final int TIMESTAMP_BASE_READOUT_SENSOR = 5;
    public static final int TIMESTAMP_BASE_REALTIME = 3;
    public static final int TIMESTAMP_BASE_SENSOR = 1;
    public OutputConfiguration(int p0, int p1, android.util.Size p2) {}
    public OutputConfiguration(int p0, int p1, android.util.Size p2, long p3) {}
    public OutputConfiguration(int p0, android.util.Size p1) {}
    public OutputConfiguration(int p0, android.util.Size p1, long p2) {}
    public OutputConfiguration(int p0, android.view.Surface p1) { java.util.ArrayList<android.view.Surface> l = new java.util.ArrayList<>(); if (p1 != null) l.add(p1); meglerProps.put("Surfaces", l); meglerProps.put("Surface", p1); meglerProps.put("SurfaceGroupId", Integer.valueOf(p0)); }
    public OutputConfiguration(int p0, android.view.Surface p1, int p2) { java.util.ArrayList<android.view.Surface> l = new java.util.ArrayList<>(); if (p1 != null) l.add(p1); meglerProps.put("Surfaces", l); meglerProps.put("Surface", p1); meglerProps.put("SurfaceGroupId", Integer.valueOf(p0)); meglerProps.put("Rotation", Integer.valueOf(p2)); }
    public OutputConfiguration(android.hardware.camera2.params.OutputConfiguration p0) { if (p0 != null) { meglerProps.putAll(p0.meglerProps); meglerProps.put("Surfaces", new java.util.ArrayList<>(p0.getSurfaces())); } else { meglerProps.put("Surfaces", new java.util.ArrayList<android.view.Surface>()); } }
    public OutputConfiguration(android.util.Size p0, java.lang.Class p1) { meglerProps.put("Surfaces", new java.util.ArrayList<android.view.Surface>()); meglerProps.put("ConfiguredSize", p0); meglerProps.put("DeferredConfiguration", Boolean.TRUE); meglerProps.put("SurfaceGroupId", Integer.valueOf(SURFACE_GROUP_ID_NONE)); }
    public OutputConfiguration(android.view.Surface p0) { java.util.ArrayList<android.view.Surface> l = new java.util.ArrayList<>(); if (p0 != null) l.add(p0); meglerProps.put("Surfaces", l); meglerProps.put("Surface", p0); meglerProps.put("SurfaceGroupId", Integer.valueOf(SURFACE_GROUP_ID_NONE)); }
    public OutputConfiguration(android.view.Surface p0, int p1) { java.util.ArrayList<android.view.Surface> l = new java.util.ArrayList<>(); if (p0 != null) l.add(p0); meglerProps.put("Surfaces", l); meglerProps.put("Surface", p0); meglerProps.put("SurfaceGroupId", Integer.valueOf(SURFACE_GROUP_ID_NONE)); meglerProps.put("Rotation", Integer.valueOf(p1)); }
    public static java.util.Collection createInstancesForMultiResolutionOutput(android.hardware.camera2.MultiResolutionImageReader p0) { return new java.util.ArrayList(); }
    public static java.util.List createInstancesForMultiResolutionOutput(java.util.Collection p0, int p1) { return new java.util.ArrayList(); }
    public static void setSurfacesForMultiResolutionOutput(java.util.Collection p0, android.hardware.camera2.MultiResolutionImageReader p1) {}
    public void addSensorPixelModeUsed(int p0) {}
    public void addSurface(android.view.Surface p0) { if (p0 == null) throw new IllegalArgumentException("Surface must not be null"); java.util.List l = getSurfaces(); if (!l.contains(p0)) l.add(p0); meglerProps.put("Surfaces", l); if (meglerProps.get("Surface") == null) meglerProps.put("Surface", p0); }
    public void clearColorSpace() {}
    public int describeContents() { return 0; }
    public void enableSurfaceSharing() {}
    public android.graphics.ColorSpace getColorSpace() { return (android.graphics.ColorSpace) meglerProps.get("ColorSpace"); }
    public int getConfiguredFormat() { return (meglerProps.get("ConfiguredFormat") instanceof Integer ? (Integer) meglerProps.get("ConfiguredFormat") : 0); }
    public android.util.Size getConfiguredSize() { return (android.util.Size) meglerProps.get("ConfiguredSize"); }
    public long getDynamicRangeProfile() { return (meglerProps.get("DynamicRangeProfile") instanceof Long ? (Long) meglerProps.get("DynamicRangeProfile") : 0L); }
    public int getMaxSharedSurfaceCount() { return 1; }
    public int getMirrorMode() { return (meglerProps.get("MirrorMode") instanceof Integer ? (Integer) meglerProps.get("MirrorMode") : 0); }
    public int getMirrorMode(android.view.Surface p0) { return 0; }
    public java.lang.String getPhysicalCameraId() { return (java.lang.String) meglerProps.get("PhysicalCameraId"); }
    public int getRotation() { return (meglerProps.get("Rotation") instanceof Integer ? (Integer) meglerProps.get("Rotation") : 0); }
    public java.util.List getSensorPixelModes() { return (meglerProps.get("SensorPixelModes") != null ? (java.util.List) meglerProps.get("SensorPixelModes") : new java.util.ArrayList()); }
    public long getStreamUseCase() { return (meglerProps.get("StreamUseCase") instanceof Long ? (Long) meglerProps.get("StreamUseCase") : 0L); }
    public android.view.Surface getSurface() { return (android.view.Surface) meglerProps.get("Surface"); }
    public int getSurfaceGroupId() { return (meglerProps.get("SurfaceGroupId") instanceof Integer ? (Integer) meglerProps.get("SurfaceGroupId") : 0); }
    public java.util.List getSurfaces() { return (meglerProps.get("Surfaces") != null ? (java.util.List) meglerProps.get("Surfaces") : new java.util.ArrayList()); }
    public int getTimestampBase() { return (meglerProps.get("TimestampBase") instanceof Integer ? (Integer) meglerProps.get("TimestampBase") : 0); }
    public long getUsage() { return (meglerProps.get("Usage") instanceof Long ? (Long) meglerProps.get("Usage") : 0L); }
    public boolean isDeferredConfiguration() { return (meglerProps.get("DeferredConfiguration") instanceof Boolean ? (Boolean) meglerProps.get("DeferredConfiguration") : false); }
    public boolean isForPhysicalCamera() { return (meglerProps.get("ForPhysicalCamera") instanceof Boolean ? (Boolean) meglerProps.get("ForPhysicalCamera") : false); }
    public boolean isMultiResolution() { return (meglerProps.get("MultiResolution") instanceof Boolean ? (Boolean) meglerProps.get("MultiResolution") : false); }
    public boolean isReadoutTimestampEnabled() { return (meglerProps.get("ReadoutTimestampEnabled") instanceof Boolean ? (Boolean) meglerProps.get("ReadoutTimestampEnabled") : false); }
    public boolean isShared() { return (meglerProps.get("Shared") instanceof Boolean ? (Boolean) meglerProps.get("Shared") : false); }
    public void removeSensorPixelModeUsed(int p0) {}
    public void removeSurface(android.view.Surface p0) { java.util.List l = getSurfaces(); l.remove(p0); meglerProps.put("Surfaces", l); if (p0 == meglerProps.get("Surface")) meglerProps.put("Surface", l.isEmpty() ? null : l.get(0)); }
    public void setColorSpace(android.graphics.ColorSpace.Named p0) { meglerProps.put("ColorSpace", p0); }
    public void setDynamicRangeProfile(long p0) { meglerProps.put("DynamicRangeProfile", Long.valueOf(p0)); }
    public void setMirrorMode(int p0) { meglerProps.put("MirrorMode", Integer.valueOf(p0)); }
    public void setMirrorMode(android.view.Surface p0, int p1) {}
    public void setMultiResolutionOutput() {}
    public void setPhysicalCameraId(java.lang.String p0) { meglerProps.put("PhysicalCameraId", p0); }
    public void setReadoutTimestampEnabled(boolean p0) { meglerProps.put("ReadoutTimestampEnabled", Boolean.valueOf(p0)); }
    public void setStreamUseCase(long p0) { meglerProps.put("StreamUseCase", Long.valueOf(p0)); }
    public void setTimestampBase(int p0) { meglerProps.put("TimestampBase", Integer.valueOf(p0)); }
    public void updateCachedSurfaceSize() {}
    public void writeToParcel(android.os.Parcel p0, int p1) {}
    OutputConfiguration() { this((android.hardware.camera2.params.OutputConfiguration) null); }
}
