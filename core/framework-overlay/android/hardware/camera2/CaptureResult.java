// megler-w10m overlay of Megler/src/java-framework/gen-src/android/hardware/camera2/CaptureResult.java, written by
// tools/compat/camera2-overlays.py from that generated stub (run it again rather than editing this file).
// It differs in having keys and values: a picture's result as the camera (camera2/impl/MeglerCameraDevice) fills it, the
// request's values and the picture's own (its timestamp, the state of focus and exposure).
package android.hardware.camera2;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class CaptureResult extends android.hardware.camera2.CameraMetadata {
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
    private android.hardware.camera2.CaptureRequest mMeglerRequest;
    private long mMeglerFrameNumber;
    private int mMeglerSequenceId;
    private java.lang.String mMeglerCameraId;
    /** This layer's: the camera, request, picture and sequence this result is of. */
    public void meglerFrom(java.lang.String cameraId, android.hardware.camera2.CaptureRequest request, long frameNumber, int sequenceId) {
        mMeglerCameraId = cameraId;
        mMeglerRequest = request;
        mMeglerFrameNumber = frameNumber;
        mMeglerSequenceId = sequenceId;
    }
    public static final android.hardware.camera2.CaptureResult.Key BLACK_LEVEL_LOCK = new android.hardware.camera2.CaptureResult.Key("android.black.levelLock", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key COLOR_CORRECTION_ABERRATION_MODE = new android.hardware.camera2.CaptureResult.Key("android.color.correctionAberrationMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key COLOR_CORRECTION_COLOR_TEMPERATURE = new android.hardware.camera2.CaptureResult.Key("android.color.correctionColorTemperature", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key COLOR_CORRECTION_COLOR_TINT = new android.hardware.camera2.CaptureResult.Key("android.color.correctionColorTint", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key COLOR_CORRECTION_GAINS = new android.hardware.camera2.CaptureResult.Key("android.color.correctionGains", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key COLOR_CORRECTION_MODE = new android.hardware.camera2.CaptureResult.Key("android.color.correctionMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key COLOR_CORRECTION_TRANSFORM = new android.hardware.camera2.CaptureResult.Key("android.color.correctionTransform", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AE_ANTIBANDING_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.aeAntibandingMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AE_EXPOSURE_COMPENSATION = new android.hardware.camera2.CaptureResult.Key("android.control.aeExposureCompensation", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AE_LOCK = new android.hardware.camera2.CaptureResult.Key("android.control.aeLock", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AE_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.aeMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AE_PRECAPTURE_TRIGGER = new android.hardware.camera2.CaptureResult.Key("android.control.aePrecaptureTrigger", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AE_PRIORITY_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.aePriorityMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AE_REGIONS = new android.hardware.camera2.CaptureResult.Key("android.control.aeRegions", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AE_STATE = new android.hardware.camera2.CaptureResult.Key("android.control.aeState", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AE_TARGET_FPS_RANGE = new android.hardware.camera2.CaptureResult.Key("android.control.aeTargetFpsRange", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AF_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.afMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AF_REGIONS = new android.hardware.camera2.CaptureResult.Key("android.control.afRegions", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AF_SCENE_CHANGE = new android.hardware.camera2.CaptureResult.Key("android.control.afSceneChange", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AF_STATE = new android.hardware.camera2.CaptureResult.Key("android.control.afState", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AF_TRIGGER = new android.hardware.camera2.CaptureResult.Key("android.control.afTrigger", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AUTOFRAMING = new android.hardware.camera2.CaptureResult.Key("android.control.autoframing", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AUTOFRAMING_STATE = new android.hardware.camera2.CaptureResult.Key("android.control.autoframingState", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AWB_LOCK = new android.hardware.camera2.CaptureResult.Key("android.control.awbLock", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AWB_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.awbMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AWB_REGIONS = new android.hardware.camera2.CaptureResult.Key("android.control.awbRegions", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_AWB_STATE = new android.hardware.camera2.CaptureResult.Key("android.control.awbState", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_CAPTURE_INTENT = new android.hardware.camera2.CaptureResult.Key("android.control.captureIntent", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_EFFECT_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.effectMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_ENABLE_ZSL = new android.hardware.camera2.CaptureResult.Key("android.control.enableZsl", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_EXTENDED_SCENE_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.extendedSceneMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_LOW_LIGHT_BOOST_STATE = new android.hardware.camera2.CaptureResult.Key("android.control.lowLightBoostState", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_POST_RAW_SENSITIVITY_BOOST = new android.hardware.camera2.CaptureResult.Key("android.control.postRawSensitivityBoost", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_SCENE_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.sceneMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_SETTINGS_OVERRIDE = new android.hardware.camera2.CaptureResult.Key("android.control.settingsOverride", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_VIDEO_STABILIZATION_MODE = new android.hardware.camera2.CaptureResult.Key("android.control.videoStabilizationMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_ZOOM_METHOD = new android.hardware.camera2.CaptureResult.Key("android.control.zoomMethod", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key CONTROL_ZOOM_RATIO = new android.hardware.camera2.CaptureResult.Key("android.control.zoomRatio", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key DISTORTION_CORRECTION_MODE = new android.hardware.camera2.CaptureResult.Key("android.distortion.correctionMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key EDGE_MODE = new android.hardware.camera2.CaptureResult.Key("android.edge.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key EXTENSION_CURRENT_TYPE = new android.hardware.camera2.CaptureResult.Key("android.extension.currentType", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key EXTENSION_NIGHT_MODE_INDICATOR = new android.hardware.camera2.CaptureResult.Key("android.extension.nightModeIndicator", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key EXTENSION_STRENGTH = new android.hardware.camera2.CaptureResult.Key("android.extension.strength", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key FLASH_MODE = new android.hardware.camera2.CaptureResult.Key("android.flash.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key FLASH_STATE = new android.hardware.camera2.CaptureResult.Key("android.flash.state", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key FLASH_STRENGTH_LEVEL = new android.hardware.camera2.CaptureResult.Key("android.flash.strengthLevel", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key HOT_PIXEL_MODE = new android.hardware.camera2.CaptureResult.Key("android.hot.pixelMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key JPEG_GPS_COORDINATES = new android.hardware.camera2.CaptureResult.Key("android.jpeg.gpsCoordinates", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key JPEG_GPS_LOCATION = new android.hardware.camera2.CaptureResult.Key("android.jpeg.gpsLocation", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key JPEG_GPS_PROCESSING_METHOD = new android.hardware.camera2.CaptureResult.Key("android.jpeg.gpsProcessingMethod", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key JPEG_GPS_TIMESTAMP = new android.hardware.camera2.CaptureResult.Key("android.jpeg.gpsTimestamp", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key JPEG_ORIENTATION = new android.hardware.camera2.CaptureResult.Key("android.jpeg.orientation", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key JPEG_QUALITY = new android.hardware.camera2.CaptureResult.Key("android.jpeg.quality", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key JPEG_THUMBNAIL_QUALITY = new android.hardware.camera2.CaptureResult.Key("android.jpeg.thumbnailQuality", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key JPEG_THUMBNAIL_SIZE = new android.hardware.camera2.CaptureResult.Key("android.jpeg.thumbnailSize", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LED_TRANSMIT = new android.hardware.camera2.CaptureResult.Key("android.led.transmit", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_APERTURE = new android.hardware.camera2.CaptureResult.Key("android.lens.aperture", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_DISTORTION = new android.hardware.camera2.CaptureResult.Key("android.lens.distortion", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_FILTER_DENSITY = new android.hardware.camera2.CaptureResult.Key("android.lens.filterDensity", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_FOCAL_LENGTH = new android.hardware.camera2.CaptureResult.Key("android.lens.focalLength", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_FOCUS_DISTANCE = new android.hardware.camera2.CaptureResult.Key("android.lens.focusDistance", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_FOCUS_RANGE = new android.hardware.camera2.CaptureResult.Key("android.lens.focusRange", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_INTRINSIC_CALIBRATION = new android.hardware.camera2.CaptureResult.Key("android.lens.intrinsicCalibration", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_OPTICAL_STABILIZATION_MODE = new android.hardware.camera2.CaptureResult.Key("android.lens.opticalStabilizationMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_POSE_ROTATION = new android.hardware.camera2.CaptureResult.Key("android.lens.poseRotation", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_POSE_TRANSLATION = new android.hardware.camera2.CaptureResult.Key("android.lens.poseTranslation", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_RADIAL_DISTORTION = new android.hardware.camera2.CaptureResult.Key("android.lens.radialDistortion", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LENS_STATE = new android.hardware.camera2.CaptureResult.Key("android.lens.state", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_ID = new android.hardware.camera2.CaptureResult.Key("android.logical.multiCameraActivePhysicalId", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key LOGICAL_MULTI_CAMERA_ACTIVE_PHYSICAL_SENSOR_CROP_REGION = new android.hardware.camera2.CaptureResult.Key("android.logical.multiCameraActivePhysicalSensorCropRegion", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key NOISE_REDUCTION_MODE = new android.hardware.camera2.CaptureResult.Key("android.noise.reductionMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key QUIRKS_PARTIAL_RESULT = new android.hardware.camera2.CaptureResult.Key("android.quirks.partialResult", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key REPROCESS_EFFECTIVE_EXPOSURE_FACTOR = new android.hardware.camera2.CaptureResult.Key("android.reprocess.effectiveExposureFactor", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key REQUEST_FRAME_COUNT = new android.hardware.camera2.CaptureResult.Key("android.request.frameCount", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key REQUEST_ID = new android.hardware.camera2.CaptureResult.Key("android.request.id", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key REQUEST_PIPELINE_DEPTH = new android.hardware.camera2.CaptureResult.Key("android.request.pipelineDepth", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SCALER_CROP_REGION = new android.hardware.camera2.CaptureResult.Key("android.scaler.cropRegion", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SCALER_RAW_CROP_REGION = new android.hardware.camera2.CaptureResult.Key("android.scaler.rawCropRegion", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SCALER_ROTATE_AND_CROP = new android.hardware.camera2.CaptureResult.Key("android.scaler.rotateAndCrop", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_DYNAMIC_BLACK_LEVEL = new android.hardware.camera2.CaptureResult.Key("android.sensor.dynamicBlackLevel", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_DYNAMIC_WHITE_LEVEL = new android.hardware.camera2.CaptureResult.Key("android.sensor.dynamicWhiteLevel", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_EXPOSURE_TIME = new android.hardware.camera2.CaptureResult.Key("android.sensor.exposureTime", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_FRAME_DURATION = new android.hardware.camera2.CaptureResult.Key("android.sensor.frameDuration", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_GREEN_SPLIT = new android.hardware.camera2.CaptureResult.Key("android.sensor.greenSplit", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_NEUTRAL_COLOR_POINT = new android.hardware.camera2.CaptureResult.Key("android.sensor.neutralColorPoint", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_NOISE_PROFILE = new android.hardware.camera2.CaptureResult.Key("android.sensor.noiseProfile", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_PIXEL_MODE = new android.hardware.camera2.CaptureResult.Key("android.sensor.pixelMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_RAW_BINNING_FACTOR_USED = new android.hardware.camera2.CaptureResult.Key("android.sensor.rawBinningFactorUsed", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_ROLLING_SHUTTER_SKEW = new android.hardware.camera2.CaptureResult.Key("android.sensor.rollingShutterSkew", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_SENSITIVITY = new android.hardware.camera2.CaptureResult.Key("android.sensor.sensitivity", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_TEST_PATTERN_DATA = new android.hardware.camera2.CaptureResult.Key("android.sensor.testPatternData", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_TEST_PATTERN_MODE = new android.hardware.camera2.CaptureResult.Key("android.sensor.testPatternMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SENSOR_TIMESTAMP = new android.hardware.camera2.CaptureResult.Key("android.sensor.timestamp", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SHADING_MODE = new android.hardware.camera2.CaptureResult.Key("android.shading.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_FACES = new android.hardware.camera2.CaptureResult.Key("android.statistics.faces", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_FACE_DETECT_MODE = new android.hardware.camera2.CaptureResult.Key("android.statistics.faceDetectMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_FACE_IDS = new android.hardware.camera2.CaptureResult.Key("android.statistics.faceIds", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_FACE_LANDMARKS = new android.hardware.camera2.CaptureResult.Key("android.statistics.faceLandmarks", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_FACE_RECTANGLES = new android.hardware.camera2.CaptureResult.Key("android.statistics.faceRectangles", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_FACE_SCORES = new android.hardware.camera2.CaptureResult.Key("android.statistics.faceScores", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_HOT_PIXEL_MAP = new android.hardware.camera2.CaptureResult.Key("android.statistics.hotPixelMap", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_HOT_PIXEL_MAP_MODE = new android.hardware.camera2.CaptureResult.Key("android.statistics.hotPixelMapMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_LENS_INTRINSICS_SAMPLES = new android.hardware.camera2.CaptureResult.Key("android.statistics.lensIntrinsicsSamples", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_LENS_INTRINSIC_SAMPLES = new android.hardware.camera2.CaptureResult.Key("android.statistics.lensIntrinsicSamples", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_LENS_INTRINSIC_TIMESTAMPS = new android.hardware.camera2.CaptureResult.Key("android.statistics.lensIntrinsicTimestamps", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_LENS_SHADING_CORRECTION_MAP = new android.hardware.camera2.CaptureResult.Key("android.statistics.lensShadingCorrectionMap", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_LENS_SHADING_MAP = new android.hardware.camera2.CaptureResult.Key("android.statistics.lensShadingMap", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_LENS_SHADING_MAP_MODE = new android.hardware.camera2.CaptureResult.Key("android.statistics.lensShadingMapMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_OIS_DATA_MODE = new android.hardware.camera2.CaptureResult.Key("android.statistics.oisDataMode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_OIS_SAMPLES = new android.hardware.camera2.CaptureResult.Key("android.statistics.oisSamples", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_OIS_TIMESTAMPS = new android.hardware.camera2.CaptureResult.Key("android.statistics.oisTimestamps", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_OIS_X_SHIFTS = new android.hardware.camera2.CaptureResult.Key("android.statistics.oisXShifts", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_OIS_Y_SHIFTS = new android.hardware.camera2.CaptureResult.Key("android.statistics.oisYShifts", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_PREDICTED_COLOR_GAINS = new android.hardware.camera2.CaptureResult.Key("android.statistics.predictedColorGains", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_PREDICTED_COLOR_TRANSFORM = new android.hardware.camera2.CaptureResult.Key("android.statistics.predictedColorTransform", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key STATISTICS_SCENE_FLICKER = new android.hardware.camera2.CaptureResult.Key("android.statistics.sceneFlicker", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key SYNC_FRAME_NUMBER = new android.hardware.camera2.CaptureResult.Key("android.sync.frameNumber", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key TONEMAP_CURVE = new android.hardware.camera2.CaptureResult.Key("android.tonemap.curve", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key TONEMAP_CURVE_BLUE = new android.hardware.camera2.CaptureResult.Key("android.tonemap.curveBlue", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key TONEMAP_CURVE_GREEN = new android.hardware.camera2.CaptureResult.Key("android.tonemap.curveGreen", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key TONEMAP_CURVE_RED = new android.hardware.camera2.CaptureResult.Key("android.tonemap.curveRed", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key TONEMAP_GAMMA = new android.hardware.camera2.CaptureResult.Key("android.tonemap.gamma", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key TONEMAP_MODE = new android.hardware.camera2.CaptureResult.Key("android.tonemap.mode", java.lang.Object.class);
    public static final android.hardware.camera2.CaptureResult.Key TONEMAP_PRESET_CURVE = new android.hardware.camera2.CaptureResult.Key("android.tonemap.presetCurve", java.lang.Object.class);
    public CaptureResult(android.hardware.camera2.impl.CameraMetadataNative p0, int p1) { super(); }
    public CaptureResult(java.lang.String p0, android.hardware.camera2.impl.CameraMetadataNative p1, android.hardware.camera2.CaptureRequest p2, int p3, long p4) { super(); }
    public CaptureResult(java.lang.String p0, android.hardware.camera2.impl.CameraMetadataNative p1, android.hardware.camera2.CaptureRequest p2, android.hardware.camera2.impl.CaptureResultExtras p3) { super(); }
    public void dumpToLog() {}
    public java.lang.Object get(android.hardware.camera2.CaptureResult.Key p0) { return p0 == null ? null : mMeglerValues.get(p0.getName()); }
    public java.lang.String getCameraId() { return mMeglerCameraId; }
    public long getFrameNumber() { return mMeglerFrameNumber; }
    public java.lang.Class getKeyClass() { return Key.class; }
    public java.util.List getKeys() { return java.util.Collections.unmodifiableList(new java.util.ArrayList<>(mMeglerKeys.values())); }
    public android.hardware.camera2.impl.CameraMetadataNative getNativeCopy() { return (android.hardware.camera2.impl.CameraMetadataNative) meglerProps.get("NativeCopy"); }
    protected java.lang.Object getProtected(android.hardware.camera2.CaptureResult.Key p0) { return get(p0); }
    public java.lang.Object getProtected(java.lang.Object p0) { return p0 instanceof Key ? get((Key) p0) : null; }
    public android.hardware.camera2.CaptureRequest getRequest() { return mMeglerRequest; }
    public int getSequenceId() { return mMeglerSequenceId; }
    CaptureResult() { this((android.hardware.camera2.impl.CameraMetadataNative) null, (int) 0); }
    public static final class Builder {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public Builder() {}
        public Builder(android.hardware.camera2.CaptureResult p0) {}
        public android.hardware.camera2.CaptureResult build() { android.hardware.camera2.CaptureResult x = new android.hardware.camera2.CaptureResult(); x.meglerProps.putAll(meglerProps); return x; }
        public android.hardware.camera2.CaptureResult.Builder set(android.hardware.camera2.CaptureResult.Key p0, java.lang.Object p1) { return this; }
    }
    /** A key: its name is what it is (two keys of one name are the same key); its type is said for the reader. */
    public static final class Key<T> {
        private final java.lang.String mName;
        private final java.lang.Class<T> mType;
        public Key(java.lang.String name, android.hardware.camera2.utils.TypeReference type) { mName = name; mType = (java.lang.Class) java.lang.Object.class; }
        public Key(java.lang.String name, java.lang.Class<T> type) { mName = name; mType = type; }
        public Key(java.lang.String name, java.lang.Class<T> type, long vendorId) { mName = name; mType = type; }
        public Key(java.lang.String name, java.lang.String fallbackName, java.lang.Class<T> type) { mName = name; mType = type; }
        public java.lang.String getName() { return mName; }
        public android.hardware.camera2.impl.CameraMetadataNative.Key getNativeKey() { return null; }
        public long getVendorId() { return 0L; }
        @Override public boolean equals(java.lang.Object o) { return o instanceof Key && ((Key) o).mName.equals(mName); }
        @Override public int hashCode() { return mName.hashCode(); }
        @Override public java.lang.String toString() { return "CaptureResult.Key(" + mName + ")"; }
    }
}
