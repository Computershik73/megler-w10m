package dalvik.system;

/** Android 13's optimization query on Android 9's DexFile. The dollar in this
 * top-level name gives it the original nested class's DEX descriptor while
 * leaving DexFile itself in Android 9's libcore. The constructor and accessors
 * retain Android 9's layout; Megler interprets DEX from its APK. */
public final class DexFile$OptimizationInfo {
    private final String status;
    private final String reason;

    private DexFile$OptimizationInfo(String status, String reason) {
        this.status = status;
        this.reason = reason;
    }

    public String getStatus() { return status; }
    public String getReason() { return reason; }
    public boolean isVerified() { return false; }
    public boolean isOptimized() { return false; }
    public boolean isFullyCompiled() { return false; }
}
