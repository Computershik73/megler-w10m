// Overlay of core/framework/src/android/content/pm/ConfigurationInfo.java (tools/build-framework.sh).
// It differs in one statement: the OpenGL ES version the device offers (ActivityManager.getDeviceConfigurationInfo) is the
// one this process can make a context of (meglet.EGLNative.glesVersion: 3.0 where ANGLE's display has it, 2.0 at Direct3D's
// feature level 9_3), not 3.2, which the original says for its ANGLE on iOS. An app that believes 3.2 asks for 3.1's
// functions, which ANGLE on these phones does not have; AIDA64 shows the version on its Display page.
package android.content.pm;

/** What the device offers: the GL ES version above all (what ANGLE gives here), a touch screen, no keyboard or navigation keys. */
public class ConfigurationInfo implements android.os.Parcelable {
    public static final int GL_ES_VERSION_UNDEFINED = 0;
    public static final int INPUT_FEATURE_FIVE_WAY_NAV = 2;
    public static final int INPUT_FEATURE_HARD_KEYBOARD = 1;
    public int reqGlEsVersion, reqInputFeatures, reqKeyboardType, reqNavigation, reqTouchScreen;
    public ConfigurationInfo() {}
    public ConfigurationInfo(ConfigurationInfo o) {
        reqGlEsVersion = o.reqGlEsVersion; reqInputFeatures = o.reqInputFeatures; reqKeyboardType = o.reqKeyboardType;
        reqNavigation = o.reqNavigation; reqTouchScreen = o.reqTouchScreen;
    }
    public String getGlEsVersion() { return ((reqGlEsVersion & 0xffff0000) >> 16) + "." + (reqGlEsVersion & 0x0000ffff); }
    public int describeContents() { return 0; }
    public void writeToParcel(android.os.Parcel dest, int flags) {
        dest.writeInt(reqTouchScreen); dest.writeInt(reqKeyboardType); dest.writeInt(reqNavigation); dest.writeInt(reqInputFeatures); dest.writeInt(reqGlEsVersion);
    }
    public static final Creator<ConfigurationInfo> CREATOR = new Creator<ConfigurationInfo>() {
        public ConfigurationInfo createFromParcel(android.os.Parcel s) {
            ConfigurationInfo c = new ConfigurationInfo();
            c.reqTouchScreen = s.readInt(); c.reqKeyboardType = s.readInt(); c.reqNavigation = s.readInt(); c.reqInputFeatures = s.readInt(); c.reqGlEsVersion = s.readInt();
            return c;
        }
        public ConfigurationInfo[] newArray(int size) { return new ConfigurationInfo[size]; }
    };
    /** Meglet's device. */
    public static ConfigurationInfo megletDevice() {
        ConfigurationInfo c = new ConfigurationInfo();
        c.reqGlEsVersion = meglet.EGLNative.glesVersion();
        c.reqTouchScreen = android.content.res.Configuration.TOUCHSCREEN_FINGER;
        c.reqKeyboardType = android.content.res.Configuration.KEYBOARD_NOKEYS;
        c.reqNavigation = android.content.res.Configuration.NAVIGATION_NONAV;
        return c;
    }
}
