// For the compiler only (tools/build-test-app.sh, stubs/): BluetoothGatt as Android 13 declares what the test calls of it --
// writeCharacteristic and writeDescriptor with the value, which return BluetoothStatusCodes, as MeshCore's flutter_blue_plus
// calls them on Android 13 and later. Not in the APK: the platform's class is the framework's.
package android.bluetooth;

import java.util.List;
import java.util.UUID;

public final class BluetoothGatt implements BluetoothProfile {
    public static final int GATT_SUCCESS = 0, GATT_READ_NOT_PERMITTED = 2, GATT_WRITE_NOT_PERMITTED = 3, GATT_INSUFFICIENT_AUTHENTICATION = 5,
            GATT_INSUFFICIENT_ENCRYPTION = 15, GATT_FAILURE = 257;
    public static final int CONNECTION_PRIORITY_BALANCED = 0, CONNECTION_PRIORITY_HIGH = 1, CONNECTION_PRIORITY_LOW_POWER = 2;
    BluetoothGatt() {}
    public BluetoothDevice getDevice() { return null; }
    public boolean connect() { return false; }
    public void disconnect() {}
    public void close() {}
    public boolean discoverServices() { return false; }
    public List<BluetoothGattService> getServices() { return null; }
    public BluetoothGattService getService(UUID uuid) { return null; }
    public boolean readCharacteristic(BluetoothGattCharacteristic characteristic) { return false; }
    public boolean writeCharacteristic(BluetoothGattCharacteristic characteristic) { return false; }
    public int writeCharacteristic(BluetoothGattCharacteristic characteristic, byte[] value, int writeType) { return 0; }
    public boolean readDescriptor(BluetoothGattDescriptor descriptor) { return false; }
    public boolean writeDescriptor(BluetoothGattDescriptor descriptor) { return false; }
    public int writeDescriptor(BluetoothGattDescriptor descriptor, byte[] value) { return 0; }
    public boolean setCharacteristicNotification(BluetoothGattCharacteristic characteristic, boolean enable) { return false; }
    public boolean requestMtu(int mtu) { return false; }
    public boolean readRemoteRssi() { return false; }
    public boolean requestConnectionPriority(int connectionPriority) { return false; }
    public int getConnectionState(BluetoothDevice device) { return 0; }
    public List<BluetoothDevice> getConnectedDevices() { return null; }
    public List<BluetoothDevice> getDevicesMatchingConnectionStates(int[] states) { return null; }
}
