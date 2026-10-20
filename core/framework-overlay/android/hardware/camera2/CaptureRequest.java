// Overlay of core/framework/gen-src/android/hardware/camera2/CaptureRequest.java, written by
// tools/compat/camera2-overlays.py from that generated stub (run it again rather than editing this file).
// It differs in having keys, values and targets: a request built by its Builder keeps what was set and the surfaces its
// pictures go to, which the camera (camera2/impl/MeglerCameraDevice) reads.
package android.hardware.camera2;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class CaptureRequest extends android.hardware.camera2.CameraMetadata implements android.os.Parcelable {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    // ---- megler-w10m: the values, by their keys' names, and the keys themselves in the order they were set.
    private final java.util.LinkedHashMap<java.lang.String, java.lang.Object> mMeglerValues = new java.util.LinkedHashMap<>();
    private final java.util.LinkedHashMap<java.lang.String, Key> mMeglerKeys = new java.util.LinkedHashMap<>();
    /** This layer's: a key's value set (null takes it away). */
    public void meglerSet(Key key, java.lang.Object value) {
        if (key == null) return;
        if (value == null) {
            mMeglerValues.remove(key.getName());
            mMeglerKeys.remove(key.getName());
        } else {
            mMeglerValues.put(key.getName(), value);
            mMeglerKeys.put(key.getName(), key);
        }
    }
    /** This layer's: the value of the key of this name, or null. */
    public java.lang.Object meglerGet(java.lang.String name) { return mMeglerValues.get(name); }
    final java.util.LinkedHashSet<android.view.Surface> mMeglerTargets = new java.util.LinkedHashSet<>();
    java.lang.Object mMeglerTag;
    private CaptureRequest(CaptureRequest from) {
        super();
        mMeglerValues.putAll(from.mMeglerValues);
        mMeglerKeys.putAll(from.mMeglerKeys);
        mMeglerTargets.addAll(from.mMeglerTargets);
        mMeglerTag = from.mMeglerTag;
    }
    /** This layer's: the surfaces this request's pictures go to, in the order they were added. */
    public java.util.List<android.view.Surface> meglerTargets() { return new java.util.ArrayList<>(mMeglerTargets); }
    public static final android.hardware.camera2.CaptureRequest.Key BLACK_LEVEL_LOCK = new android.hardware.camera2.CaptureRequest.Key("android.black.levelLock", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key COLOR_CORRECTION_ABERRATION_MODE = new android.hardware.camera2.CaptureRequest.Key("android.color.correctionAberrationMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key COLOR_CORRECTION_COLOR_TEMPERATURE = new android.hardware.camera2.CaptureRequest.Key("android.color.correctionColorTemperature", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key COLOR_CORRECTION_COLOR_TINT = new android.hardware.camera2.CaptureRequest.Key("android.color.correctionColorTint", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key COLOR_CORRECTION_GAINS = new android.hardware.camera2.CaptureRequest.Key("android.color.correctionGains", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key COLOR_CORRECTION_MODE = new android.hardware.camera2.CaptureRequest.Key("android.color.correctionMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key COLOR_CORRECTION_TRANSFORM = new android.hardware.camera2.CaptureRequest.Key("android.color.correctionTransform", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AE_ANTIBANDING_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.aeAntibandingMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AE_EXPOSURE_COMPENSATION = new android.hardware.camera2.CaptureRequest.Key("android.control.aeExposureCompensation", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AE_LOCK = new android.hardware.camera2.CaptureRequest.Key("android.control.aeLock", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AE_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.aeMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AE_PRECAPTURE_TRIGGER = new android.hardware.camera2.CaptureRequest.Key("android.control.aePrecaptureTrigger", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AE_PRIORITY_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.aePriorityMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AE_REGIONS = new android.hardware.camera2.CaptureRequest.Key("android.control.aeRegions", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AE_REGIONS_SET = new android.hardware.camera2.CaptureRequest.Key("android.control.aeRegionsSet", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AE_TARGET_FPS_RANGE = new android.hardware.camera2.CaptureRequest.Key("android.control.aeTargetFpsRange", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AF_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.afMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AF_REGIONS = new android.hardware.camera2.CaptureRequest.Key("android.control.afRegions", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AF_REGIONS_SET = new android.hardware.camera2.CaptureRequest.Key("android.control.afRegionsSet", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AF_TRIGGER = new android.hardware.camera2.CaptureRequest.Key("android.control.afTrigger", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AUTOFRAMING = new android.hardware.camera2.CaptureRequest.Key("android.control.autoframing", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AWB_LOCK = new android.hardware.camera2.CaptureRequest.Key("android.control.awbLock", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AWB_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.awbMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AWB_REGIONS = new android.hardware.camera2.CaptureRequest.Key("android.control.awbRegions", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_AWB_REGIONS_SET = new android.hardware.camera2.CaptureRequest.Key("android.control.awbRegionsSet", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_CAPTURE_INTENT = new android.hardware.camera2.CaptureRequest.Key("android.control.captureIntent", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_EFFECT_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.effectMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_ENABLE_ZSL = new android.hardware.camera2.CaptureRequest.Key("android.control.enableZsl", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_EXTENDED_SCENE_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.extendedSceneMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_POST_RAW_SENSITIVITY_BOOST = new android.hardware.camera2.CaptureRequest.Key("android.control.postRawSensitivityBoost", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_SCENE_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.sceneMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_SETTINGS_OVERRIDE = new android.hardware.camera2.CaptureRequest.Key("android.control.settingsOverride", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_VIDEO_STABILIZATION_MODE = new android.hardware.camera2.CaptureRequest.Key("android.control.videoStabilizationMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_ZOOM_METHOD = new android.hardware.camera2.CaptureRequest.Key("android.control.zoomMethod", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key CONTROL_ZOOM_RATIO = new android.hardware.camera2.CaptureRequest.Key("android.control.zoomRatio", java.lang.Object.class);
    public static android.os.Parcelable.Creator CREATOR;
    public static final android.hardware.camera2.CaptureRequest.Key DISTORTION_CORRECTION_MODE = new android.hardware.camera2.CaptureRequest.Key("android.distortion.correctionMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key EDGE_MODE = new android.hardware.camera2.CaptureRequest.Key("android.edge.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key EXTENSION_STRENGTH = new android.hardware.camera2.CaptureRequest.Key("android.extension.strength", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key FLASH_MODE = new android.hardware.camera2.CaptureRequest.Key("android.flash.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key FLASH_STRENGTH_LEVEL = new android.hardware.camera2.CaptureRequest.Key("android.flash.strengthLevel", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key HOT_PIXEL_MODE = new android.hardware.camera2.CaptureRequest.Key("android.hot.pixelMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key JPEG_GPS_COORDINATES = new android.hardware.camera2.CaptureRequest.Key("android.jpeg.gpsCoordinates", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key JPEG_GPS_LOCATION = new android.hardware.camera2.CaptureRequest.Key("android.jpeg.gpsLocation", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key JPEG_GPS_PROCESSING_METHOD = new android.hardware.camera2.CaptureRequest.Key("android.jpeg.gpsProcessingMethod", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key JPEG_GPS_TIMESTAMP = new android.hardware.camera2.CaptureRequest.Key("android.jpeg.gpsTimestamp", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key JPEG_ORIENTATION = new android.hardware.camera2.CaptureRequest.Key("android.jpeg.orientation", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key JPEG_QUALITY = new android.hardware.camera2.CaptureRequest.Key("android.jpeg.quality", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key JPEG_THUMBNAIL_QUALITY = new android.hardware.camera2.CaptureRequest.Key("android.jpeg.thumbnailQuality", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key JPEG_THUMBNAIL_SIZE = new android.hardware.camera2.CaptureRequest.Key("android.jpeg.thumbnailSize", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key LED_TRANSMIT = new android.hardware.camera2.CaptureRequest.Key("android.led.transmit", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key LENS_APERTURE = new android.hardware.camera2.CaptureRequest.Key("android.lens.aperture", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key LENS_FILTER_DENSITY = new android.hardware.camera2.CaptureRequest.Key("android.lens.filterDensity", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key LENS_FOCAL_LENGTH = new android.hardware.camera2.CaptureRequest.Key("android.lens.focalLength", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key LENS_FOCUS_DISTANCE = new android.hardware.camera2.CaptureRequest.Key("android.lens.focusDistance", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key LENS_OPTICAL_STABILIZATION_MODE = new android.hardware.camera2.CaptureRequest.Key("android.lens.opticalStabilizationMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key NOISE_REDUCTION_MODE = new android.hardware.camera2.CaptureRequest.Key("android.noise.reductionMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key REPROCESS_EFFECTIVE_EXPOSURE_FACTOR = new android.hardware.camera2.CaptureRequest.Key("android.reprocess.effectiveExposureFactor", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key REQUEST_ID = new android.hardware.camera2.CaptureRequest.Key("android.request.id", java.lang.Object.class);
    public static final int REQUEST_TYPE_COUNT = 3;
    public static final int REQUEST_TYPE_REGULAR = 0;
    public static final int REQUEST_TYPE_REPROCESS = 1;
    public static final int REQUEST_TYPE_ZSL_STILL = 2;
    public static final android.hardware.camera2.CaptureRequest.Key SCALER_CROP_REGION = new android.hardware.camera2.CaptureRequest.Key("android.scaler.cropRegion", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key SCALER_CROP_REGION_SET = new android.hardware.camera2.CaptureRequest.Key("android.scaler.cropRegionSet", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key SCALER_ROTATE_AND_CROP = new android.hardware.camera2.CaptureRequest.Key("android.scaler.rotateAndCrop", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key SENSOR_EXPOSURE_TIME = new android.hardware.camera2.CaptureRequest.Key("android.sensor.exposureTime", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key SENSOR_FRAME_DURATION = new android.hardware.camera2.CaptureRequest.Key("android.sensor.frameDuration", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key SENSOR_PIXEL_MODE = new android.hardware.camera2.CaptureRequest.Key("android.sensor.pixelMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key SENSOR_SENSITIVITY = new android.hardware.camera2.CaptureRequest.Key("android.sensor.sensitivity", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key SENSOR_TEST_PATTERN_DATA = new android.hardware.camera2.CaptureRequest.Key("android.sensor.testPatternData", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key SENSOR_TEST_PATTERN_MODE = new android.hardware.camera2.CaptureRequest.Key("android.sensor.testPatternMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key SHADING_MODE = new android.hardware.camera2.CaptureRequest.Key("android.shading.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key STATISTICS_FACE_DETECT_MODE = new android.hardware.camera2.CaptureRequest.Key("android.statistics.faceDetectMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key STATISTICS_HOT_PIXEL_MAP_MODE = new android.hardware.camera2.CaptureRequest.Key("android.statistics.hotPixelMapMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key STATISTICS_LENS_SHADING_MAP_MODE = new android.hardware.camera2.CaptureRequest.Key("android.statistics.lensShadingMapMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key STATISTICS_OIS_DATA_MODE = new android.hardware.camera2.CaptureRequest.Key("android.statistics.oisDataMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key TONEMAP_CURVE = new android.hardware.camera2.CaptureRequest.Key("android.tonemap.curve", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key TONEMAP_CURVE_BLUE = new android.hardware.camera2.CaptureRequest.Key("android.tonemap.curveBlue", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key TONEMAP_CURVE_GREEN = new android.hardware.camera2.CaptureRequest.Key("android.tonemap.curveGreen", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key TONEMAP_CURVE_RED = new android.hardware.camera2.CaptureRequest.Key("android.tonemap.curveRed", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key TONEMAP_GAMMA = new android.hardware.camera2.CaptureRequest.Key("android.tonemap.gamma", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key TONEMAP_MODE = new android.hardware.camera2.CaptureRequest.Key("android.tonemap.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureRequest.Key TONEMAP_PRESET_CURVE = new android.hardware.camera2.CaptureRequest.Key("android.tonemap.presetCurve", java.lang.Object.class);
    public boolean containsTarget(android.view.Surface p0) { return mMeglerTargets.contains(p0); }
    public void convertSurfaceToStreamId(android.util.SparseArray p0) {}
    public int describeContents() { return 0; }
    protected void finalize() {}
    public java.lang.Object get(android.hardware.camera2.CaptureRequest.Key p0) { return p0 == null ? null : mMeglerValues.get(p0.getName()); }
    public java.lang.Class getKeyClass() { return Key.class; }
    public java.util.List getKeys() { return java.util.Collections.unmodifiableList(new java.util.ArrayList<>(mMeglerKeys.values())); }
    public java.lang.String getLogicalCameraId() { return (java.lang.String) meglerProps.get("LogicalCameraId"); }
    public android.hardware.camera2.impl.CameraMetadataNative getNativeCopy() { return (android.hardware.camera2.impl.CameraMetadataNative) meglerProps.get("NativeCopy"); }
    protected java.lang.Object getProtected(android.hardware.camera2.CaptureRequest.Key p0) { return get(p0); }
    public java.lang.Object getProtected(java.lang.Object p0) { return p0 instanceof Key ? get((Key) p0) : null; }
    public int getReprocessableSessionId() { return (meglerProps.get("ReprocessableSessionId") instanceof Integer ? (Integer) meglerProps.get("ReprocessableSessionId") : 0); }
    public int getRequestType() { return (meglerProps.get("RequestType") instanceof Integer ? (Integer) meglerProps.get("RequestType") : 0); }
    public int[] getStreamIds() { return (int[]) meglerProps.get("StreamIds"); }
    public int[] getSurfaceIds() { return (int[]) meglerProps.get("SurfaceIds"); }
    public java.lang.Object getTag() { return mMeglerTag; }
    public java.util.Collection getTargets() { return java.util.Collections.unmodifiableCollection(mMeglerTargets); }
    public boolean isPartOfCRequestList() { return (meglerProps.get("PartOfCRequestList") instanceof Boolean ? (Boolean) meglerProps.get("PartOfCRequestList") : false); }
    public boolean isReprocess() { return false; }
    public void recoverStreamIdToSurface() {}
    public void writeToParcel(android.os.Parcel p0, int p1) {}
    protected CaptureRequest() { super(); }
    /** A request being made: its values and its target surfaces, kept until build(). */
    public static final class Builder {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        private final CaptureRequest mRequest = new CaptureRequest();
        public Builder(android.hardware.camera2.impl.CameraMetadataNative p0, boolean p1, int p2, java.lang.String p3, java.util.Set p4) {}
        public void addTarget(android.view.Surface p0) {
            if (p0 == null) throw new IllegalArgumentException("Null target surface");
            mRequest.mMeglerTargets.add(p0);
        }
        public android.hardware.camera2.CaptureRequest build() { return new CaptureRequest(mRequest); }
        public java.lang.Object get(android.hardware.camera2.CaptureRequest.Key p0) { return mRequest.get(p0); }
        public java.lang.Object getPhysicalCameraKey(android.hardware.camera2.CaptureRequest.Key p0, java.lang.String p1) { return mRequest.get(p0); }
        public boolean isEmpty() { return mRequest.mMeglerValues.isEmpty() && mRequest.mMeglerTargets.isEmpty(); }
        public void removeTarget(android.view.Surface p0) { mRequest.mMeglerTargets.remove(p0); }
        public void set(android.hardware.camera2.CaptureRequest.Key p0, java.lang.Object p1) {
            if (p0 == null) throw new IllegalArgumentException("key must not be null");
            mRequest.meglerSet(p0, p1);
        }
        public void setPartOfCHSRequestList(boolean p0) { meglerProps.put("PartOfCHSRequestList", Boolean.valueOf(p0)); }
        public android.hardware.camera2.CaptureRequest.Builder setPhysicalCameraKey(android.hardware.camera2.CaptureRequest.Key p0, java.lang.Object p1, java.lang.String p2) { set(p0, p1); return this; }
        public void setTag(java.lang.Object p0) { mRequest.mMeglerTag = p0; }
        Builder() { this((android.hardware.camera2.impl.CameraMetadataNative) null, (boolean) false, (int) 0, (java.lang.String) null, (java.util.Set) null); }
    }
    /** A key: its name is what it is (two keys of one name are the same key); its type is said for the reader. */
    public static final class Key<T> {
        private final java.lang.String mName;
        private final java.lang.Class<T> mType;
        public Key(java.lang.String name, android.hardware.camera2.utils.TypeReference type) { mName = name; mType = (java.lang.Class) java.lang.Object.class; }
        public Key(java.lang.String name, java.lang.Class<T> type) { mName = name; mType = type; }
        public Key(java.lang.String name, java.lang.Class<T> type, long vendorId) { mName = name; mType = type; }
        public java.lang.String getName() { return mName; }
        public android.hardware.camera2.impl.CameraMetadataNative.Key getNativeKey() { return null; }
        public long getVendorId() { return 0L; }
        @Override public boolean equals(java.lang.Object o) { return o instanceof Key && ((Key) o).mName.equals(mName); }
        @Override public int hashCode() { return mName.hashCode(); }
        @Override public java.lang.String toString() { return "CaptureRequest.Key(" + mName + ")"; }
    }
}
