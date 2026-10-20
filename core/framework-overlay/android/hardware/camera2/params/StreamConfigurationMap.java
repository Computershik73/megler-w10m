// Overlay of core/framework/gen-src/android/hardware/camera2/params/StreamConfigurationMap.java, written by
// tools/compat/camera2-overlays.py from that generated stub (run it again rather than editing this file).
// It differs in saying the camera's sizes (meglerOf), for YUV_420_888, PRIVATE, JPEG and NV21 alike, where the
// stub said null, which CameraX takes for a camera it cannot use.
package android.hardware.camera2.params;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class StreamConfigurationMap {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    // ---- megler-w10m: the camera's sizes, the largest first, the same for every format it gives; and its fastest picture.
    /** YUV_420_888, PRIVATE (a SurfaceTexture's or a SurfaceView's), JPEG (said, so that a size is there to choose; not taken) and NV21. */
    private static final int[] MEGLER_FORMATS = { 35, 34, 256, 17 };
    private android.util.Size[] mMeglerSizes = new android.util.Size[0];
    private long mMeglerMinFrameNs;
    /** This layer's: a map of these sizes in every format it gives, each at most 1e9 / `maxFps` ns a picture. */
    public static StreamConfigurationMap meglerOf(android.util.Size[] sizes, int maxFps) {
        StreamConfigurationMap m = new StreamConfigurationMap();
        m.mMeglerSizes = sizes.clone();
        m.mMeglerMinFrameNs = 1000000000L / (maxFps > 0 ? maxFps : 30);
        return m;
    }
    private boolean meglerHas(android.util.Size s) {
        for (android.util.Size x : mMeglerSizes) if (x.equals(s)) return true;
        return false;
    }
    public static final int HAL_DATASPACE_ARBITRARY = 1;
    public static final int HAL_DATASPACE_DEPTH = 4096;
    public static final int HAL_DATASPACE_DYNAMIC_DEPTH = 4098;
    public static final int HAL_DATASPACE_HEIF = 4100;
    public static final int HAL_DATASPACE_JFIF = 146931712;
    public static final int HAL_DATASPACE_JPEG_R = 4101;
    public static final int HAL_DATASPACE_V0_JFIF = 146931712;
    public static final int HAL_PIXEL_FORMAT_BLOB = 33;
    public StreamConfigurationMap(android.hardware.camera2.params.StreamConfiguration[] p0, android.hardware.camera2.params.StreamConfigurationDuration[] p1, android.hardware.camera2.params.StreamConfigurationDuration[] p2, android.hardware.camera2.params.StreamConfiguration[] p3, android.hardware.camera2.params.StreamConfigurationDuration[] p4, android.hardware.camera2.params.StreamConfigurationDuration[] p5, android.hardware.camera2.params.StreamConfiguration[] p6, android.hardware.camera2.params.StreamConfigurationDuration[] p7, android.hardware.camera2.params.StreamConfigurationDuration[] p8, android.hardware.camera2.params.StreamConfiguration[] p9, android.hardware.camera2.params.StreamConfigurationDuration[] p10, android.hardware.camera2.params.StreamConfigurationDuration[] p11, android.hardware.camera2.params.StreamConfiguration[] p12, android.hardware.camera2.params.StreamConfigurationDuration[] p13, android.hardware.camera2.params.StreamConfigurationDuration[] p14, android.hardware.camera2.params.StreamConfiguration[] p15, android.hardware.camera2.params.StreamConfigurationDuration[] p16, android.hardware.camera2.params.StreamConfigurationDuration[] p17, android.hardware.camera2.params.HighSpeedVideoConfiguration[] p18, android.hardware.camera2.params.ReprocessFormatsMap p19, boolean p20) {}
    public StreamConfigurationMap(android.hardware.camera2.params.StreamConfiguration[] p0, android.hardware.camera2.params.StreamConfigurationDuration[] p1, android.hardware.camera2.params.StreamConfigurationDuration[] p2, android.hardware.camera2.params.StreamConfiguration[] p3, android.hardware.camera2.params.StreamConfigurationDuration[] p4, android.hardware.camera2.params.StreamConfigurationDuration[] p5, android.hardware.camera2.params.StreamConfiguration[] p6, android.hardware.camera2.params.StreamConfigurationDuration[] p7, android.hardware.camera2.params.StreamConfigurationDuration[] p8, android.hardware.camera2.params.StreamConfiguration[] p9, android.hardware.camera2.params.StreamConfigurationDuration[] p10, android.hardware.camera2.params.StreamConfigurationDuration[] p11, android.hardware.camera2.params.StreamConfiguration[] p12, android.hardware.camera2.params.StreamConfigurationDuration[] p13, android.hardware.camera2.params.StreamConfigurationDuration[] p14, android.hardware.camera2.params.StreamConfiguration[] p15, android.hardware.camera2.params.StreamConfigurationDuration[] p16, android.hardware.camera2.params.StreamConfigurationDuration[] p17, android.hardware.camera2.params.HighSpeedVideoConfiguration[] p18, android.hardware.camera2.params.ReprocessFormatsMap p19, boolean p20, boolean p21) {}
    public static int compareSizes(int p0, int p1, int p2, int p3) { return 0; }
    public static int depthFormatToPublic(int p0) { return 0; }
    public static java.lang.String formatToString(int p0) { return null; }
    public static int[] imageFormatToInternal(int[] p0) { return null; }
    public static int imageFormatToPublic(int p0) { return 0; }
    public static boolean isOutputSupportedFor(java.lang.Class p0) { return p0 == android.graphics.SurfaceTexture.class || p0 == android.view.SurfaceHolder.class || p0 == android.media.ImageReader.class; }
    public android.util.Size[] getHighResolutionOutputSizes(int p0) { return null; }
    public android.util.Range[] getHighSpeedVideoFpsRanges() { return new android.util.Range[0]; }
    public android.util.Range[] getHighSpeedVideoFpsRangesFor(android.util.Size p0) { return null; }
    public android.util.Size[] getHighSpeedVideoSizes() { return new android.util.Size[0]; }
    public android.util.Size[] getHighSpeedVideoSizesFor(android.util.Range p0) { return null; }
    public int[] getInputFormats() { return new int[0]; }
    public android.util.Size[] getInputSizes(int p0) { return null; }
    public int[] getOutputFormats() { return MEGLER_FORMATS.clone(); }
    public long getOutputMinFrameDuration(int p0, android.util.Size p1) { return meglerHas(p1) ? mMeglerMinFrameNs : 0L; }
    public long getOutputMinFrameDuration(java.lang.Class p0, android.util.Size p1) { return meglerHas(p1) ? mMeglerMinFrameNs : 0L; }
    public android.util.Size[] getOutputSizes(int p0) { return isOutputSupportedFor(p0) ? mMeglerSizes.clone() : null; }
    public android.util.Size[] getOutputSizes(java.lang.Class p0) { return isOutputSupportedFor(p0) ? mMeglerSizes.clone() : null; }
    public long getOutputStallDuration(int p0, android.util.Size p1) { return 0L; }
    public long getOutputStallDuration(java.lang.Class p0, android.util.Size p1) { return 0L; }
    public int[] getValidOutputFormatsForInput(int p0) { return new int[0]; }
    public boolean isOutputSupportedFor(int p0) { for (int f : MEGLER_FORMATS) if (f == p0) return true; return false; }
    public boolean isOutputSupportedFor(android.util.Size p0, int p1) { return isOutputSupportedFor(p1) && meglerHas(p0); }
    public boolean isOutputSupportedFor(android.view.Surface p0) { return p0 != null; }
    StreamConfigurationMap() { this((android.hardware.camera2.params.StreamConfiguration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfiguration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfiguration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfiguration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfiguration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfiguration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.StreamConfigurationDuration[]) null, (android.hardware.camera2.params.HighSpeedVideoConfiguration[]) null, (android.hardware.camera2.params.ReprocessFormatsMap) null, (boolean) false); }
}
