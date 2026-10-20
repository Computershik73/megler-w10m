// Overlay of core/framework/gen-src/android/hardware/camera2/CameraManager.java (tools/build-framework.sh).
// It differs in giving the phone's cameras: their ids, their characteristics and opening them (camera2/impl/MeglerCameras and
// MeglerCameraDevice, over megler.CameraNative), where the generated stub answered null. A camera's id is its index ("0").
//
// Why. CameraX, which most apps that scan a code use (urent's QR scanner), knows only camera2: with no cameras here it said
// "No available camera can be found" and showed black. (The list was empty, not null, before camera2 was built: Android's
// System WebView asks how many cameras there are as soon as a page enumerates its media devices, and a null ended the app
// in Chromium's native code.) An app that asks the old android.hardware.Camera still has it, the overlay's.
//
// The availability callbacks hear every camera available once, at once; torch callbacks and setTorchMode are not done.
// Everything else is the original's, unchanged.
package android.hardware.camera2;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class CameraManager {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static final java.lang.String LANDSCAPE_TO_PORTRAIT_PROP = "camera.enable_landscape_to_portrait";
    public static final long OVERRIDE_CAMERA_LANDSCAPE_TO_PORTRAIT = 250678880L;
    public CameraManager(android.content.Context p0) {}
    public static android.content.res.CameraCompatibilityInfo getRotationOverride(android.content.Context p0) { return null; }
    public static android.content.res.CameraCompatibilityInfo getRotationOverride(android.content.Context p0, android.content.pm.PackageManager p1, java.lang.String p2) { return null; }
    public static boolean isHiddenPhysicalCamera(java.lang.String p0) { return false; }
    public static boolean physicalCallbacksAreEnabledForUnavailableCamera() { return false; }
    public static java.lang.String reportExtensionSessionStats(android.hardware.CameraExtensionSessionStats p0) { return null; }
    public android.hardware.camera2.CameraCharacteristics getCameraCharacteristics(java.lang.String p0) {
        return android.hardware.camera2.impl.MeglerCameras.characteristics(android.hardware.camera2.impl.MeglerCameras.index(p0));
    }
    public android.hardware.camera2.CameraCharacteristics getCameraCharacteristics(java.lang.String p0, boolean p1) { return getCameraCharacteristics(p0); }
    public android.hardware.camera2.CameraDevice.CameraDeviceSetup getCameraDeviceSetup(java.lang.String p0) { return null; }
    public android.hardware.camera2.CameraExtensionCharacteristics getCameraExtensionCharacteristics(java.lang.String p0) { return null; }
    public java.lang.String[] getCameraIdList() { return android.hardware.camera2.impl.MeglerCameras.ids(); }
    public java.lang.String[] getCameraIdListNoLazy() { return android.hardware.camera2.impl.MeglerCameras.ids(); }
    public android.hardware.ICameraService getCameraService() { return (android.hardware.ICameraService) meglerProps.get("CameraService"); }
    public android.content.AttributionSourceState getClientAttribution() { return (android.content.AttributionSourceState) meglerProps.get("ClientAttribution"); }
    public android.content.AttributionSourceState getClientAttribution(int p0, boolean p1) { return null; }
    public android.content.AttributionSourceState getClientAttribution(boolean p0) { return null; }
    public java.util.Set getConcurrentCameraIds() { return (meglerProps.get("ConcurrentCameraIds") != null ? (java.util.Set) meglerProps.get("ConcurrentCameraIds") : new java.util.HashSet()); }
    public int getDevicePolicyFromContext(android.content.Context p0) { return 0; }
    public java.util.Map getPhysicalIdToCharsMap(android.hardware.camera2.CameraCharacteristics p0) { return new java.util.HashMap(); }
    public int getTorchStrengthLevel(java.lang.String p0) { return 0; }
    public void injectCamera(java.lang.String p0, java.lang.String p1, java.lang.String p2, java.util.concurrent.Executor p3, android.hardware.camera2.CameraInjectionSession.InjectionStatusCallback p4) {}
    public void injectSessionParams(java.lang.String p0, android.hardware.camera2.CaptureRequest p1) {}
    public boolean isCameraDeviceSetupSupported(java.lang.String p0) { return false; }
    public boolean isCameraDeviceSharingSupported(java.lang.String p0) { return false; }
    public boolean isCameraServiceDisabled() { return (meglerProps.get("CameraServiceDisabled") instanceof Boolean ? (Boolean) meglerProps.get("CameraServiceDisabled") : false); }
    public boolean isConcurrentSessionConfigurationSupported(java.util.Map p0) { return false; }
    public void openCamera(java.lang.String p0, int p1, java.util.concurrent.Executor p2, android.hardware.camera2.CameraDevice.StateCallback p3) { openCamera(p0, p2, p3); }
    public void openCamera(java.lang.String p0, android.hardware.camera2.CameraDevice.StateCallback p1, android.os.Handler p2) {
        android.hardware.camera2.impl.MeglerCameraDevice.open(android.hardware.camera2.impl.MeglerCameras.index(p0), p0, p1, android.hardware.camera2.impl.MeglerCameraDevice.executorOf(p2));
    }
    public void openCamera(java.lang.String p0, java.util.concurrent.Executor p1, android.hardware.camera2.CameraDevice.StateCallback p2) {
        if (p1 == null) throw new IllegalArgumentException("executor was null");
        android.hardware.camera2.impl.MeglerCameraDevice.open(android.hardware.camera2.impl.MeglerCameras.index(p0), p0, p2, p1);
    }
    public void openCamera(java.lang.String p0, boolean p1, android.os.Handler p2, android.hardware.camera2.CameraDevice.StateCallback p3) { openCamera(p0, p3, p2); }
    public void openCameraImpl(java.lang.String p0, android.hardware.camera2.CameraDevice.StateCallback p1, java.util.concurrent.Executor p2, int p3, android.content.res.CameraCompatibilityInfo p4, boolean p5) {}
    public void openSharedCamera(java.lang.String p0, java.util.concurrent.Executor p1, android.hardware.camera2.CameraDevice.StateCallback p2) {}
    public android.hardware.camera2.CameraCharacteristics prepareCameraCharacteristics(java.lang.String p0, android.hardware.camera2.impl.CameraMetadataNative p1, android.hardware.ICameraService p2) { return null; }
    public void registerAvailabilityCallback(android.hardware.camera2.CameraManager.AvailabilityCallback p0, android.os.Handler p1) {
        registerAvailabilityCallback(android.hardware.camera2.impl.MeglerCameraDevice.executorOf(p1), p0);
    }
    /** Every camera available, said once, at once, as Android says the cameras there are to a callback newly registered. */
    public void registerAvailabilityCallback(java.util.concurrent.Executor p0, android.hardware.camera2.CameraManager.AvailabilityCallback p1) {
        if (p0 == null || p1 == null) return;
        for (java.lang.String id : android.hardware.camera2.impl.MeglerCameras.ids()) p0.execute(() -> p1.onCameraAvailable(id));
    }
    public void registerDeviceStateListener(android.hardware.camera2.CameraCharacteristics p0) {}
    public void registerTorchCallback(android.hardware.camera2.CameraManager.TorchCallback p0, android.os.Handler p1) {}
    public void registerTorchCallback(java.util.concurrent.Executor p0, android.hardware.camera2.CameraManager.TorchCallback p1) {}
    public void setTorchMode(java.lang.String p0, boolean p1) {}
    public void turnOnTorchWithStrengthLevel(java.lang.String p0, int p1) {}
    public void unregisterAvailabilityCallback(android.hardware.camera2.CameraManager.AvailabilityCallback p0) {}
    public void unregisterTorchCallback(android.hardware.camera2.CameraManager.TorchCallback p0) {}
    CameraManager() { this((android.content.Context) null); }
    public static abstract class AvailabilityCallback {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public AvailabilityCallback() {}
        public void onCameraAccessPrioritiesChanged() {}
        public void onCameraAvailable(java.lang.String p0) {}
        public void onCameraClosed(java.lang.String p0) {}
        public void onCameraOpened(java.lang.String p0, java.lang.String p1) {}
        public void onCameraUnavailable(java.lang.String p0) {}
        public void onPhysicalCameraAvailable(java.lang.String p0, java.lang.String p1) {}
        public void onPhysicalCameraUnavailable(java.lang.String p0, java.lang.String p1) {}
    }
    public interface DeviceStateListener {
        void onDeviceStateChanged(boolean p0);
    }
    public static abstract class TorchCallback {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public TorchCallback() {}
        public void onTorchModeChanged(java.lang.String p0, boolean p1) {}
        public void onTorchModeUnavailable(java.lang.String p0) {}
        public void onTorchStrengthLevelChanged(java.lang.String p0, int p1) {}
    }
}
