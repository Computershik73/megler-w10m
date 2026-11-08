// For the compiler only (tools/build-test-app.sh, stubs/): ScanRecord as Android 13 declares what the test calls of it --
// getAdvertisingDataMap, from which flutter_blue_plus reads a device's appearance. Not in the APK: the platform's class is
// the framework's.
package android.bluetooth.le;

import android.os.ParcelUuid;
import android.util.SparseArray;
import java.util.List;
import java.util.Map;

public final class ScanRecord {
    ScanRecord() {}
    public int getAdvertiseFlags() { return 0; }
    public List<ParcelUuid> getServiceUuids() { return null; }
    public SparseArray<byte[]> getManufacturerSpecificData() { return null; }
    public byte[] getManufacturerSpecificData(int manufacturerId) { return null; }
    public Map<ParcelUuid, byte[]> getServiceData() { return null; }
    public byte[] getServiceData(ParcelUuid serviceDataUuid) { return null; }
    public int getTxPowerLevel() { return 0; }
    public String getDeviceName() { return null; }
    public byte[] getBytes() { return null; }
    public Map<Integer, byte[]> getAdvertisingDataMap() { return null; }
}
