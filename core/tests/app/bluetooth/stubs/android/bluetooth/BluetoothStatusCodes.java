// For the compiler only (tools/build-test-app.sh, stubs/): Android 12's BluetoothStatusCodes, which Android 13's GATT writes
// return. Not in the APK: its constants are compiled into the test.
package android.bluetooth;

public final class BluetoothStatusCodes {
    private BluetoothStatusCodes() {}
    public static final int SUCCESS = 0;
    public static final int ERROR_DEVICE_NOT_CONNECTED = 4;
    public static final int ERROR_GATT_WRITE_NOT_ALLOWED = 200;
    public static final int ERROR_GATT_WRITE_REQUEST_BUSY = 201;
}
