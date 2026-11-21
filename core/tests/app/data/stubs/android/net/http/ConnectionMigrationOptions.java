package android.net.http;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the steps http-engine and sdk-extensions call of Android 14's class. */
public class ConnectionMigrationOptions {
    public static final int MIGRATION_OPTION_ENABLED = 1;
    public static final int MIGRATION_OPTION_DISABLED = 2;
    public static final class Builder {
        public Builder() {}
        public Builder setDefaultNetworkMigration(int option) { return this; }
        public Builder setPathDegradationMigration(int option) { return this; }
        public Builder setAllowNonDefaultNetworkUsage(int option) { return this; }
        public ConnectionMigrationOptions build() { return null; }
    }
}
