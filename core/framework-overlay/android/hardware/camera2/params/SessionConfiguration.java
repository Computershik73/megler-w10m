// megler-w10m overlay of Megler/src/java-framework/gen-src/android/hardware/camera2/params/SessionConfiguration.java, written by
// tools/compat/camera2-overlays.py from that generated stub (run it again rather than editing this file).
// It differs in keeping its type, outputs, executor and callback, which the stub's constructors dropped.
package android.hardware.camera2.params;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class SessionConfiguration implements android.os.Parcelable {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    public static android.os.Parcelable.Creator CREATOR;
    public static final int SESSION_HIGH_SPEED = 1;
    public static final int SESSION_REGULAR = 0;
    public static final int SESSION_SHARED = 2;
    public static final int SESSION_VENDOR_START = 32768;
    public SessionConfiguration(int p0, java.util.List p1) { meglerProps.put("SessionType", Integer.valueOf(p0)); meglerProps.put("OutputConfigurations", p1 != null ? new java.util.ArrayList<>(p1) : new java.util.ArrayList<>()); }
    public SessionConfiguration(int p0, java.util.List p1, java.util.concurrent.Executor p2, android.hardware.camera2.CameraCaptureSession.StateCallback p3) { this(p0, p1); meglerProps.put("Executor", p2); meglerProps.put("StateCallback", p3); }
    public void clearColorSpace() {}
    public int describeContents() { return 0; }
    public android.graphics.ColorSpace getColorSpace() { return (android.graphics.ColorSpace) meglerProps.get("ColorSpace"); }
    public java.util.concurrent.Executor getExecutor() { return (java.util.concurrent.Executor) meglerProps.get("Executor"); }
    public android.hardware.camera2.params.InputConfiguration getInputConfiguration() { return (android.hardware.camera2.params.InputConfiguration) meglerProps.get("InputConfiguration"); }
    public java.util.List getOutputConfigurations() { return (meglerProps.get("OutputConfigurations") != null ? (java.util.List) meglerProps.get("OutputConfigurations") : new java.util.ArrayList()); }
    public android.hardware.camera2.CaptureRequest getSessionParameters() { return (android.hardware.camera2.CaptureRequest) meglerProps.get("SessionParameters"); }
    public int getSessionType() { return (meglerProps.get("SessionType") instanceof Integer ? (Integer) meglerProps.get("SessionType") : 0); }
    public android.hardware.camera2.CameraCaptureSession.StateCallback getStateCallback() { return (android.hardware.camera2.CameraCaptureSession.StateCallback) meglerProps.get("StateCallback"); }
    public void setColorSpace(android.graphics.ColorSpace.Named p0) { meglerProps.put("ColorSpace", p0); }
    public void setInputConfiguration(android.hardware.camera2.params.InputConfiguration p0) { meglerProps.put("InputConfiguration", p0); }
    public void setSessionParameters(android.hardware.camera2.CaptureRequest p0) { meglerProps.put("SessionParameters", p0); }
    public void setStateCallback(java.util.concurrent.Executor p0, android.hardware.camera2.CameraCaptureSession.StateCallback p1) { meglerProps.put("Executor", p0); meglerProps.put("StateCallback", p1); }
    public void writeToParcel(android.os.Parcel p0, int p1) {}
    SessionConfiguration() { this((int) 0, (java.util.List) null); }
}
