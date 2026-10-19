// megler-w10m: a class of this framework's own, new (the original has no camera2). The phone's cameras as android.hardware.camera2
// says them -- their ids and their characteristics -- over megler.CameraNative (core/src/drivers/hk_java_camera.c, the shell's
// megler_camera_host: Windows.Media.Capture on a phone). CameraManager, the overlay's, asks this; MeglerCameraDevice opens them.
package android.hardware.camera2.impl;

import android.graphics.Rect;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraMetadata;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.util.Log;
import android.util.Range;
import android.util.Rational;
import android.util.Size;
import android.util.SizeF;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * A camera's id is its index, as text ("0" the first). Its characteristics are what the camera says of itself: which way it
 * faces and how it is turned (megler.CameraNative.info), and, from opening it once, its sizes, frame rates, focus and torch
 * (caps). They are those of a LEGACY camera, which is what this is to an app: one stream at a time, given to every surface
 * of a request, automatic exposure and white balance, the focus the camera has, no manual control. Each camera is opened
 * once for that, the first time it is asked of, and closed again.
 */
public final class MeglerCameras {
    private MeglerCameras() {}

    static final String TAG = "MeglerCamera2";
    private static final Map<Integer, CameraCharacteristics> sCharacteristics = new HashMap<>();
    private static final Map<Integer, int[]> sCaps = new HashMap<>();

    public static String[] ids() {
        int n = Math.max(0, megler.CameraNative.count());
        String[] ids = new String[n];
        for (int i = 0; i < n; i++) ids[i] = String.valueOf(i);
        return ids;
    }

    /** The index of a camera's id; IllegalArgumentException for an id that is no camera's, as Android says. */
    public static int index(String id) {
        if (id == null) throw new IllegalArgumentException("cameraId was null");
        int i;
        try {
            i = Integer.parseInt(id);
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException("Camera id " + id + " is not a valid camera id");
        }
        if (i < 0 || i >= megler.CameraNative.count()) throw new IllegalArgumentException("Camera id " + id + " is not a valid camera id");
        return i;
    }

    /** { facing (0 back, 1 front), orientation, focus, light }. */
    static int[] info(int index) {
        int[] out = new int[4];
        if (!megler.CameraNative.info(index, out)) out[1] = 90;
        return out;
    }

    /**
     * { focus, macro, torch, n, then width, height, min fps, max fps of each size, the largest first }, from opening the camera
     * once; null when it cannot be opened (the log says why), and asked again next time.
     */
    static synchronized int[] caps(int index) {
        int[] caps = sCaps.get(index);
        if (caps != null) return caps;
        long h = megler.CameraNative.open(index);
        if (h == 0) return null;
        try {
            caps = megler.CameraNative.caps(h);
        } finally {
            megler.CameraNative.close(h);
        }
        if (caps != null && caps.length >= 4 && caps[3] > 0) sCaps.put(index, caps);
        return caps;
    }

    /** A camera's sizes, the largest first; 640 x 480 when it says none. */
    static Size[] sizes(int[] caps) {
        if (caps == null || caps.length < 4 || caps[3] <= 0) return new Size[] { new Size(640, 480) };
        List<Size> sizes = new ArrayList<>();
        for (int i = 0; i < caps[3] && 7 + 4 * i < caps.length; i++) {
            Size s = new Size(caps[4 + 4 * i], caps[5 + 4 * i]);
            if (s.getWidth() > 0 && s.getHeight() > 0 && !sizes.contains(s)) sizes.add(s);
        }
        return sizes.isEmpty() ? new Size[] { new Size(640, 480) } : sizes.toArray(new Size[0]);
    }

    /** The frame rates a camera says it streams at: its sizes' lowest and highest, and the highest alone. */
    static Range<Integer>[] fpsRanges(int[] caps) {
        int min = 30, max = 30;
        if (caps != null && caps.length >= 8 && caps[3] > 0) {
            min = Integer.MAX_VALUE;
            max = 0;
            for (int i = 0; i < caps[3] && 7 + 4 * i < caps.length; i++) {
                min = Math.min(min, caps[6 + 4 * i]);
                max = Math.max(max, caps[7 + 4 * i]);
            }
            if (max <= 0) max = 30;
            if (min <= 0 || min > max) min = max;
        }
        @SuppressWarnings("unchecked")
        Range<Integer>[] ranges = min == max ? new Range[] { Range.create(max, max) } : new Range[] { Range.create(min, max), Range.create(max, max) };
        return ranges;
    }

    /** Whether the camera focuses (from its caps once opened, else from what it said before). */
    static boolean focuses(int[] info, int[] caps) { return caps != null && caps.length > 0 ? caps[0] != 0 : info[2] != 0; }
    static boolean hasTorch(int[] info, int[] caps) { return caps != null && caps.length > 2 ? caps[2] != 0 : info[3] > 0; }

    public static synchronized CameraCharacteristics characteristics(int index) {
        CameraCharacteristics c = sCharacteristics.get(index);
        if (c != null) return c;
        int[] info = info(index), caps = caps(index);
        Size[] sizes = sizes(caps);
        Size largest = sizes[0];
        for (Size s : sizes) if ((long) s.getWidth() * s.getHeight() > (long) largest.getWidth() * largest.getHeight()) largest = s;
        Range<Integer>[] fps = fpsRanges(caps);
        boolean front = info[0] == 1, focus = focuses(info, caps), torch = hasTorch(info, caps);
        c = new CameraCharacteristics((CameraMetadataNative) null);
        c.meglerSet(CameraCharacteristics.LENS_FACING, front ? CameraMetadata.LENS_FACING_FRONT : CameraMetadata.LENS_FACING_BACK);
        c.meglerSet(CameraCharacteristics.SENSOR_ORIENTATION, ((info[1] % 360) + 360) % 360);
        c.meglerSet(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL, CameraMetadata.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY);
        c.meglerSet(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES, new int[] { CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE });
        c.meglerSet(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP, StreamConfigurationMap.meglerOf(sizes, fps[fps.length - 1].getUpper()));
        c.meglerSet(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, fps);
        c.meglerSet(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES, focus
                ? new int[] { CameraMetadata.CONTROL_AF_MODE_OFF, CameraMetadata.CONTROL_AF_MODE_AUTO, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO,
                              CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE }
                : new int[] { CameraMetadata.CONTROL_AF_MODE_OFF });
        c.meglerSet(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES, torch
                ? new int[] { CameraMetadata.CONTROL_AE_MODE_ON, CameraMetadata.CONTROL_AE_MODE_ON_AUTO_FLASH, CameraMetadata.CONTROL_AE_MODE_ON_ALWAYS_FLASH }
                : new int[] { CameraMetadata.CONTROL_AE_MODE_ON });
        c.meglerSet(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES, new int[] { CameraMetadata.CONTROL_AWB_MODE_AUTO });
        c.meglerSet(CameraCharacteristics.CONTROL_AVAILABLE_MODES, new int[] { CameraMetadata.CONTROL_MODE_AUTO });
        c.meglerSet(CameraCharacteristics.CONTROL_AVAILABLE_SCENE_MODES, new int[] { CameraMetadata.CONTROL_SCENE_MODE_DISABLED });
        c.meglerSet(CameraCharacteristics.CONTROL_AVAILABLE_EFFECTS, new int[] { CameraMetadata.CONTROL_EFFECT_MODE_OFF });
        c.meglerSet(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, new int[] { CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF });
        c.meglerSet(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE, Range.create(0, 0));
        c.meglerSet(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP, new Rational(1, 2));
        c.meglerSet(CameraCharacteristics.CONTROL_AE_LOCK_AVAILABLE, Boolean.FALSE);
        c.meglerSet(CameraCharacteristics.CONTROL_AWB_LOCK_AVAILABLE, Boolean.FALSE);
        c.meglerSet(CameraCharacteristics.CONTROL_MAX_REGIONS_AE, 0);
        c.meglerSet(CameraCharacteristics.CONTROL_MAX_REGIONS_AF, 0);
        c.meglerSet(CameraCharacteristics.CONTROL_MAX_REGIONS_AWB, 0);
        c.meglerSet(CameraCharacteristics.FLASH_INFO_AVAILABLE, torch);
        c.meglerSet(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE, focus ? 10.0f : 0.0f);
        c.meglerSet(CameraCharacteristics.LENS_INFO_HYPERFOCAL_DISTANCE, 0.0f);
        c.meglerSet(CameraCharacteristics.LENS_INFO_FOCUS_DISTANCE_CALIBRATION, CameraMetadata.LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED);
        c.meglerSet(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS, new float[] { 4.0f });
        c.meglerSet(CameraCharacteristics.LENS_INFO_AVAILABLE_APERTURES, new float[] { 2.0f });
        c.meglerSet(CameraCharacteristics.LENS_INFO_AVAILABLE_FILTER_DENSITIES, new float[] { 0.0f });
        c.meglerSet(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION, new int[] { CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_OFF });
        c.meglerSet(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE, new Rect(0, 0, largest.getWidth(), largest.getHeight()));
        c.meglerSet(CameraCharacteristics.SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE, new Rect(0, 0, largest.getWidth(), largest.getHeight()));
        c.meglerSet(CameraCharacteristics.SENSOR_INFO_PIXEL_ARRAY_SIZE, new Size(largest.getWidth(), largest.getHeight()));
        c.meglerSet(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE, new SizeF(4.8f, 3.6f));
        c.meglerSet(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE, CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN);
        c.meglerSet(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, 1.0f);
        c.meglerSet(CameraCharacteristics.SCALER_CROPPING_TYPE, CameraMetadata.SCALER_CROPPING_TYPE_CENTER_ONLY);
        c.meglerSet(CameraCharacteristics.REQUEST_PARTIAL_RESULT_COUNT, 1);
        c.meglerSet(CameraCharacteristics.REQUEST_PIPELINE_MAX_DEPTH, (byte) 3);
        c.meglerSet(CameraCharacteristics.REQUEST_MAX_NUM_OUTPUT_STREAMS, new int[] { 0, 3, 1 });
        c.meglerSet(CameraCharacteristics.REQUEST_MAX_NUM_INPUT_STREAMS, 0);
        c.meglerSet(CameraCharacteristics.SYNC_MAX_LATENCY, CameraMetadata.SYNC_MAX_LATENCY_UNKNOWN);
        c.meglerSet(CameraCharacteristics.JPEG_AVAILABLE_THUMBNAIL_SIZES, new Size[] { new Size(0, 0) });
        c.meglerSet(CameraCharacteristics.EDGE_AVAILABLE_EDGE_MODES, new int[] { CameraMetadata.EDGE_MODE_OFF });
        c.meglerSet(CameraCharacteristics.NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES, new int[] { CameraMetadata.NOISE_REDUCTION_MODE_OFF });
        c.meglerSet(CameraCharacteristics.COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES, new int[] { CameraMetadata.COLOR_CORRECTION_ABERRATION_MODE_OFF });
        c.meglerSet(CameraCharacteristics.STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES, new int[] { CameraMetadata.STATISTICS_FACE_DETECT_MODE_OFF });
        c.meglerSet(CameraCharacteristics.STATISTICS_INFO_MAX_FACE_COUNT, 0);
        c.meglerSet(CameraCharacteristics.HOT_PIXEL_AVAILABLE_HOT_PIXEL_MODES, new int[] { CameraMetadata.HOT_PIXEL_MODE_OFF });
        c.meglerSet(CameraCharacteristics.TONEMAP_AVAILABLE_TONE_MAP_MODES, new int[] { CameraMetadata.TONEMAP_MODE_FAST });
        c.meglerSet(CameraCharacteristics.DISTORTION_CORRECTION_AVAILABLE_MODES, new int[] { CameraMetadata.DISTORTION_CORRECTION_MODE_OFF });
        c.meglerSetAvailableKeys(
                Arrays.asList(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_TRIGGER, CaptureRequest.CONTROL_AE_MODE,
                              CaptureRequest.CONTROL_AE_PRECAPTURE_TRIGGER, CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, CaptureRequest.CONTROL_AWB_MODE,
                              CaptureRequest.CONTROL_CAPTURE_INTENT, CaptureRequest.FLASH_MODE, CaptureRequest.SCALER_CROP_REGION, CaptureRequest.JPEG_QUALITY,
                              CaptureRequest.JPEG_ORIENTATION),
                Arrays.asList(CaptureResult.CONTROL_MODE, CaptureResult.CONTROL_AF_MODE, CaptureResult.CONTROL_AF_STATE, CaptureResult.CONTROL_AE_MODE,
                              CaptureResult.CONTROL_AE_STATE, CaptureResult.CONTROL_AWB_MODE, CaptureResult.CONTROL_AWB_STATE, CaptureResult.FLASH_MODE,
                              CaptureResult.FLASH_STATE, CaptureResult.LENS_STATE, CaptureResult.SENSOR_TIMESTAMP, CaptureResult.SCALER_CROP_REGION));
        Log.i(TAG, "camera " + index + ": " + (front ? "front" : "back") + ", turned " + info[1] + ", " + sizes.length + " sizes, the largest "
                   + largest + ", " + fps[0] + " fps" + (focus ? ", focuses" : ", fixed focus") + (torch ? ", a torch" : "")
                   + (caps == null ? " (it could not be opened: these are guesses)" : ""));
        if (caps != null) sCharacteristics.put(index, c);
        return c;
    }
}
