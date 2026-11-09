package megler.test.microg;

import android.app.Activity;
import android.content.*;
import android.content.pm.*;
import android.os.*;
import android.util.Log;
import android.widget.TextView;

/** Integration test: transactions go to the official GmsCore APK, not a Megler binder. */
public class Main extends Activity {
    private static final String GMS = "com.google.android.gms";
    private static final String ACTION = "com.google.android.gms.ads.identifier.service.START";
    private static final String DESCRIPTOR = "com.google.android.gms.ads.identifier.internal.IAdvertisingIdService";
    private boolean done;
    private boolean gmsBroadcastContext;
    private boolean gmsFlaggedBroadcastContext;
    private int connections;
    private IBinder first;
    private TextView text;
    private static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    private void fail(Throwable t) { done = true; Log.e("MicroGTest", "FAILED", t); text.setText("FAILED: " + t); }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this); text.setText("Checking microG..."); setContentView(text);
        try {
            Context device = createDeviceProtectedStorageContext();
            java.lang.reflect.Method isCredential = Context.class.getMethod("isCredentialProtectedStorage");
            java.lang.reflect.Method credentialContext = Context.class.getMethod("createCredentialProtectedStorageContext");
            check(Boolean.TRUE.equals(isCredential.invoke(this)) && !isDeviceProtectedStorage(), "app uses credential protected storage");
            check(device.isDeviceProtectedStorage() && Boolean.FALSE.equals(isCredential.invoke(device))
                    && !device.getDataDir().equals(getDataDir()), "device protected context has separate data");
            check(((Context) credentialContext.invoke(device)).getDataDir().equals(getDataDir()),
                    "device context returns to credential protected storage");
            PackageInfo own = getPackageManager().getPackageInfo(getPackageName(), PackageManager.GET_META_DATA);
            check(own.applicationInfo != null && getPackageName().equals(own.applicationInfo.packageName), "own package metadata");
            PackageInfo pi = getPackageManager().getPackageInfo(GMS, PackageManager.GET_SERVICES);
            check(pi.applicationInfo.sourceDir.endsWith("/GmsCore/GmsCore.apk"), "real APK metadata");
            check(pi.services != null && pi.services.length > 20, "manifest services");
            PackageInfo archive = getPackageManager().getPackageArchiveInfo(pi.applicationInfo.sourceDir, 0);
            check(archive != null && GMS.equals(archive.packageName) && archive.versionCode == pi.versionCode
                    && pi.applicationInfo.sourceDir.equals(archive.applicationInfo.sourceDir), "archive package metadata");
            check(getPackageManager().getPackageArchiveInfo(pi.applicationInfo.sourceDir + ".missing", 0) == null,
                    "missing archive has no package metadata");
            android.os.UserManager users = (android.os.UserManager) getSystemService(Context.USER_SERVICE);
            check(users.isUserUnlocked() && users.isUserUnlocked(android.os.Process.myUserHandle()), "current user is unlocked");
            check(android.os.UserManager.class.getMethod("get", Context.class).invoke(null, this) == users
                    && ((java.util.List) android.os.UserManager.class.getMethod("getProfiles", int.class).invoke(users, 0)).size() == 1,
                    "one unlocked owner profile");
            android.app.usage.UsageStatsManager usage = (android.app.usage.UsageStatsManager) getSystemService(Context.USAGE_STATS_SERVICE);
            check(usage.queryEvents(0, System.currentTimeMillis()) != null, "usage events query is an empty result, not null");
            if (System.getenv("MG_JAVA_GSF_APK") != null) {
                PackageInfo gsfInfo = getPackageManager().getPackageInfo("com.google.android.gsf", PackageManager.GET_PROVIDERS);
                check(gsfInfo.providers != null && gsfInfo.providers.length > 0, "shared GSF APK metadata");
                // GServices is microG's own here, as on a phone with microG: the Android ID of its check-in is in it, which
                // Play Store reads (GSF's, mounted beside it for Google's own Play services, never had one).
                ProviderInfo gservices = getPackageManager().resolveContentProvider("com.google.android.gsf.gservices", 0);
                check(gservices != null && GMS.equals(gservices.packageName) && gservices.name.startsWith("org.microg."), "microG's GServices provider resolves");
                Context gsf = createPackageContext("com.google.android.gsf", CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY);
                check("com.google.android.gsf".equals(gsf.getPackageName()) && gsf.getClassLoader() != getClassLoader(),
                        "GSF has its own service class loader");
                check(gsf.getApplicationContext() == gsf && gsf.getSystemService(android.os.UserManager.class) != null,
                        "GSF application context has user services");
                android.database.Cursor gservicesValues = getContentResolver().query(android.net.Uri.parse("content://com.google.android.gsf.gservices"),
                        null, null, new String[] {"android_id"}, null);
                check(gservicesValues != null, "microG's GServices provider answers a query");
                gservicesValues.close();
            }
            // microG's account provider, asked for the Google accounts as an app's GoogleAuthUtil asks it: it holds the caller's
            // package (ContentProvider.getCallingPackage) against the one its pid names, the app's here. The provider's own
            // package was said, and it refused Classroom every time ("UID [10100] is not related to packageName").
            String accountsSaid;
            try {
                android.os.Bundle got = getContentResolver().call(android.net.Uri.parse("content://com.google.android.gms.auth.accounts"),
                        "get_accounts", "com.google", null);
                accountsSaid = got == null ? "null" : "accounts " + java.util.Arrays.toString(got.getParcelableArray("accounts"));
            } catch (RuntimeException e) {
                // (A SecurityException is what the check said; past it microG reads the caller's certificate, which this
                // test app, unsigned, has none of: "CertData to small". An app from a store is signed.)
                accountsSaid = e.getClass().getName() + ": " + e.getMessage();
            }
            android.util.Log.i("MicroGTest", "account provider: " + accountsSaid);
            check(!accountsSaid.contains("is not related to packageName"), "microG's account provider takes the app for its caller: " + accountsSaid);
            Context gms = createPackageContext(GMS, CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY);
            check(gms.getSystemService(android.os.UserManager.class) != null, "GMS service context has user services");
            Context gmsDevice = gms.createDeviceProtectedStorageContext();
            check(gmsDevice.isDeviceProtectedStorage() && !gms.isDeviceProtectedStorage()
                    && !gmsDevice.getDataDir().equals(gms.getDataDir())
                    && gmsDevice.getDataDir().equals(new java.io.File(gms.getDataDir(), "device_protected")),
                    "GMS service separates device protected data");
            check(((Context) credentialContext.invoke(gmsDevice)).getDataDir().equals(gms.getDataDir()),
                    "GMS service returns to credential protected data");
            int settingsTheme = gms.getResources().getIdentifier("common.Theme.GoogleSettings", "style", GMS);
            Log.i("ThemeProbe", "fixture settingsTheme=0x" + Integer.toHexString(settingsTheme));
            if (settingsTheme != 0) {
                int actionBar = gms.getResources().getIdentifier("windowActionBar", "attr", GMS);
                Log.i("ThemeProbe", "fixture actionBar=0x" + Integer.toHexString(actionBar));
                android.content.res.Resources.Theme theme = gms.getResources().newTheme();
                theme.applyStyle(settingsTheme, true);
                check(actionBar != 0 && theme.resolveAttribute(actionBar, new android.util.TypedValue(), true),
                        "GMS settings theme inherits AppCompat attributes");
                android.content.res.Configuration phoneConfig = new android.content.res.Configuration(gms.getResources().getConfiguration());
                phoneConfig.setLocale(new java.util.Locale("ru", "RU"));
                phoneConfig.uiMode = android.content.res.Configuration.UI_MODE_TYPE_NORMAL | android.content.res.Configuration.UI_MODE_NIGHT_YES;
                phoneConfig.screenWidthDp = 411;
                phoneConfig.screenHeightDp = 731;
                android.util.DisplayMetrics phoneMetrics = new android.util.DisplayMetrics();
                phoneMetrics.setTo(gms.getResources().getDisplayMetrics());
                phoneMetrics.densityDpi = 560;
                phoneMetrics.density = 3.5f;
                android.content.res.Resources phoneResources = new android.content.res.Resources(gms.getAssets(),
                        phoneMetrics, phoneConfig);
                android.content.res.Resources.Theme phoneTheme = phoneResources.newTheme();
                phoneTheme.applyStyle(settingsTheme, true);
                check(phoneTheme.resolveAttribute(actionBar, new android.util.TypedValue(), true),
                        "GMS settings theme inherits AppCompat attributes in Russian night mode");
            }
            String partialResources = pi.applicationInfo.sourceDir + ".partial";
            if (new java.io.File(partialResources).isFile()) {
                check(settingsTheme != 0, "GMS settings style exists in the base APK");
                int cookie = (Integer) android.content.res.AssetManager.class
                        .getMethod("addAssetPathAsSharedLibrary", String.class).invoke(gms.getAssets(), partialResources);
                check(cookie != 0,
                        "same-package partial resources loaded");
                check(gms.getResources().getIdentifier("common.Theme.GoogleSettings", "style", GMS) == settingsTheme,
                        "partial locale resources do not hide the base GMS style");
                Log.i("MicroGTest", "PASS same-package partial resources preserve GMS AppCompat style");
                done = true;
                return;
            }
            String testAction = "megler.test.microg.REGISTERED_RECEIVER_CONTEXT";
            gms.registerReceiver(new BroadcastReceiver() {
                @Override public void onReceive(Context context, Intent intent) {
                    try {
                        gms.unregisterReceiver(this);
                        check(GMS.equals(context.getPackageName()), "microG receiver registration context");
                        gmsBroadcastContext = true;
                    } catch (Throwable t) { fail(t); }
                }
            }, new IntentFilter(testAction));
            sendBroadcast(new Intent(testAction));
            String flaggedAction = "megler.test.microg.FLAGGED_RECEIVER_CONTEXT";
            gms.registerReceiver(new BroadcastReceiver() {
                @Override public void onReceive(Context context, Intent intent) {
                    try {
                        gms.unregisterReceiver(this);
                        check(GMS.equals(context.getPackageName()), "microG flagged receiver registration context");
                        gmsFlaggedBroadcastContext = true;
                    } catch (Throwable t) { fail(t); }
                }
            }, new IntentFilter(flaggedAction), 0);
            sendBroadcast(new Intent(flaggedAction));
            /*
             * The receiver of Google Play services' SMS Retriever, registered at run time with the permission only Google Play
             * services holds, as a bank's app waits for its code (SmsRetriever.SMS_RETRIEVED_ACTION, SEND_PERMISSION), and its
             * manifest saying nothing of it: the report finds it in this code (the test's C side, start()).
             */
            BroadcastReceiver codeReceiver = new BroadcastReceiver() {
                @Override public void onReceive(Context context, Intent intent) {}
            };
            registerReceiver(codeReceiver, new IntentFilter("com.google.android.gms.auth.api.phone.SMS_RETRIEVED"),
                    "com.google.android.gms.auth.api.phone.permission.SEND", null);
            unregisterReceiver(codeReceiver);
            if ("1".equals(System.getenv("MG_MICROG_DROIDGUARD_HANDLE_TEST"))) {
                ClassLoader loader = gms.getClassLoader();
                Class<?> prefs = Class.forName("org.microg.gms.droidguard.core.DroidGuardPreferences", true, loader);
                prefs.getMethod("setEnabled", Context.class, boolean.class).invoke(null, gms, true);
                Class<?> factoryClass = Class.forName("org.microg.gms.droidguard.core.NetworkHandleProxyFactory", true, loader);
                java.lang.reflect.Constructor<?> ctor = factoryClass.getDeclaredConstructor(Context.class);
                ctor.setAccessible(true);
                Object factory = ctor.newInstance(gms);
                Class<?> callback = Class.forName("org.microg.gms.droidguard.GuardCallback", true, loader);
                Class<?> request = Class.forName("com.google.android.gms.droidguard.internal.DroidGuardResultsRequest", true, loader);
                Object guardCallback = callback.getDeclaredConstructor(Context.class, String.class).newInstance(gms, GMS);
                Object handle = factoryClass.getMethod("createHandle", String.class, String.class, callback, request)
                    .invoke(factory, GMS, "recaptcha-android-frame", guardCallback, null);
                check(handle != null, "DroidGuard VM handle");
                check(Boolean.TRUE.equals(handle.getClass().getMethod("init").invoke(handle)), "DroidGuard VM initializes");
                byte[] verdict = (byte[]) handle.getClass().getMethod("run", java.util.Map.class)
                    .invoke(handle, new java.util.HashMap<String, Object>());
                check(verdict != null && verdict.length > 0, "DroidGuard VM returns bytes");
                handle.getClass().getMethod("close").invoke(handle);
                Log.i("MicroGTest", "DROIDGUARD HANDLE PASS");
            }
            if ("1".equals(System.getenv("MG_MICROG_DROIDGUARD_TEST"))) {
                // A service's local Binder call may enter DroidGuard on the app's main thread.
                // Its synchronous Volley future must complete even while that looper is busy.
                Class<?> factoryClass = Class.forName("org.microg.gms.droidguard.core.NetworkHandleProxyFactory", true, gms.getClassLoader());
                java.lang.reflect.Constructor<?> ctor = factoryClass.getDeclaredConstructor(Context.class);
                ctor.setAccessible(true);
                Object factory = ctor.newInstance(gms);
                Log.i("MicroGTest", "DROIDGUARD request to Google begins on main thread");
                Object result = factoryClass.getMethod("fetchFromServer", String.class, String.class)
                    .invoke(factory, "recaptcha-android-frame", GMS);
                check(result != null && "kotlin.Triple".equals(result.getClass().getName()), "DroidGuard server response");
                Log.i("MicroGTest", "DROIDGUARD PASS signed Google response on main thread");
            }
            if ("1".equals(System.getenv("MG_MICROG_POTOKEN_VM_TEST"))) {
                ClassLoader loader = gms.getClassLoader();
                Class<?> prefs = Class.forName("org.microg.gms.droidguard.core.DroidGuardPreferences", true, loader);
                prefs.getMethod("setEnabled", Context.class, boolean.class).invoke(null, gms, true);
                Class<?> helperClass = Class.forName("org.microg.gms.potokens.PoTokenHelper", true, loader);
                java.lang.reflect.Constructor<?> helperCtor = helperClass.getDeclaredConstructor(Context.class);
                helperCtor.setAccessible(true);
                Object helper = helperCtor.newInstance(gms);
                java.lang.reflect.Method buildKeySet = helperClass.getDeclaredMethod("buildKeySet");
                buildKeySet.setAccessible(true);
                Object keySet = buildKeySet.invoke(helper);
                byte[] encodedKeySet = (byte[]) keySet.getClass().getMethod("encode").invoke(keySet);
                Class<?> requestClass = Class.forName("com.google.android.gms.droidguard.internal.DroidGuardResultsRequest", true, loader);
                Object request = requestClass.newInstance();
                Bundle bundle = (Bundle) requestClass.getField("bundle").get(request);
                bundle.putByteArray("po-fast-key", encodedKeySet);
                java.util.ArrayList<String> fallback = new java.util.ArrayList<String>();
                fallback.add("po-fast-key");
                bundle.putStringArrayList("extraKeysRetainedInFallback", fallback);
                Class<?> factoryClass = Class.forName("org.microg.gms.droidguard.core.NetworkHandleProxyFactory", true, loader);
                java.lang.reflect.Constructor<?> factoryCtor = factoryClass.getDeclaredConstructor(Context.class);
                factoryCtor.setAccessible(true);
                Object factory = factoryCtor.newInstance(gms);
                Class<?> callbackClass = Class.forName("org.microg.gms.droidguard.GuardCallback", true, loader);
                Object callback = callbackClass.getDeclaredConstructor(Context.class, String.class).newInstance(gms, GMS);
                Object handle = factoryClass.getMethod("createHandle", String.class, String.class, callbackClass, requestClass)
                    .invoke(factory, GMS, "po-token-fast", callback, request);
                check(handle != null, "PoToken DroidGuard VM handle");
                check(Boolean.TRUE.equals(handle.getClass().getMethod("init").invoke(handle)), "PoToken DroidGuard VM initializes");
                byte[] nonce = new byte[32];
                new java.security.SecureRandom().nextBytes(nonce);
                java.util.Map<String, String> input = new java.util.HashMap<String, String>();
                input.put("b", android.util.Base64.encodeToString(nonce, android.util.Base64.NO_WRAP));
                byte[] result = (byte[]) handle.getClass().getMethod("run", java.util.Map.class).invoke(handle, input);
                handle.getClass().getMethod("close").invoke(handle);
                check(result != null && result.length > 0, "PoToken DroidGuard VM returns bytes");
                Log.i("MicroGTest", "POTOKEN VM PASS bytes=" + result.length);
            }
            check(GMS.equals(gms.getPackageName()), "package context");
            Class<?> client = com.google.android.gms.common.internal.GetServiceRequest.class;
            Class<?> service = Class.forName(client.getName(), false, gms.getClassLoader());
            check(service != client, "distinct definitions of GetServiceRequest");
            check(service.getClassLoader() == gms.getClassLoader(), "defining loader");
            check(service.getPackage() != null, "service package metadata");
            check(com.google.android.gms.org.conscrypt.NativeCrypto.CLIENT_MARKER == 731, "client native class loaded first");
            Class<?> provider = Class.forName("com.google.android.gms.org.conscrypt.OpenSSLProvider", true, gms.getClassLoader());
            check(provider.newInstance() instanceof java.security.Provider, "microG Conscrypt provider initializes");
            check(com.google.android.gms.org.conscrypt.NativeCrypto.CLIENT_MARKER == 731, "JNI registration preserves client class");
            check(service.getField("CREATOR").getType() != int.class, "service CREATOR type");
            check(client.getField("CREATOR").getInt(null) == 1729, "client CREATOR unchanged");
            check(java.lang.reflect.Array.newInstance(service, 1).getClass().getComponentType() == service, "array component loader");
            check(Class.forName("android.os.Binder", false, gms.getClassLoader()) == Binder.class, "shared platform");
            check(!gms.getFilesDir().equals(getFilesDir()), "separate files");
            check("/data/megler-services/com.google.android.gms".equals(gms.getDataDir().getPath()), "microG's state in the services' shared folder");
            check(!gms.getDatabasePath("x").equals(getDatabasePath("x")), "separate database");
            gms.getSharedPreferences("microg-probe", 0).edit().putString("owner", "microg").commit();
            check(!getSharedPreferences("microg-probe", 0).contains("owner"), "separate preferences");
            int label = gms.getResources().getIdentifier("gms_app_name", "string", GMS);
            if (label == 0) label = gms.getResources().getIdentifier("app_name", "string", GMS);
            check(label != 0, "package resources found");
            check(gms.getResources().getString(label).length() > 0, "package resource value");
            String authority = "com.google.android.gms.microg.settings";
            ProviderInfo settings = getPackageManager().resolveContentProvider(authority, 0);
            check(settings != null && settings.name.startsWith("org.microg.gms."), "microG settings provider resolves");
            android.database.Cursor checkin = getContentResolver().query(
                android.net.Uri.parse("content://" + authority + "/check-in"),
                new String[] { "androidId", "digest", "lastCheckin", "securityToken", "versionInfo", "deviceDataVersionInfo" },
                null, null, null);
            check(checkin != null, "microG check-in provider answers");
            checkin.close();
            android.net.Uri global = android.provider.Settings.Global.CONTENT_URI;
            android.content.ContentValues setting = new android.content.ContentValues();
            setting.put("name", "megler_checkin_probe");
            setting.put("value", "from-gsf");
            check(getContentResolver().insert(global, setting) != null, "GSF can write global settings");
            android.database.Cursor saved = getContentResolver().query(
                android.net.Uri.withAppendedPath(global, "megler_checkin_probe"), new String[] { "value" }, null, null, null);
            check(saved != null && saved.moveToFirst() && "from-gsf".equals(saved.getString(0)),
                "GSF can read global settings by URI");
            saved.close();
            Class<?> precision = Class.forName("android.icu.number.Precision");
            Object twoPlaces = precision.getMethod("fixedFraction", int.class).invoke(null, 2);
            Object formatter = Class.forName("android.icu.number.NumberFormatter").getMethod("with").invoke(null);
            formatter = formatter.getClass().getMethod("precision", precision).invoke(formatter, twoPlaces);
            formatter = formatter.getClass().getMethod("locale", java.util.Locale.class).invoke(formatter, java.util.Locale.US);
            Object formatted = formatter.getClass().getMethod("format", double.class).invoke(formatter, 12.3);
            check("12.30".equals(formatted.toString()), "Android 30 number formatter rounds and pads");
            Class<?> numbers = Class.forName("android.icu.number.NumberFormatter");
            Object rounded = numbers.getMethod("with").invoke(null);
            rounded = rounded.getClass().getMethod("precision", precision).invoke(rounded,
                precision.getMethod("fixedFraction", int.class).invoke(null, 0));
            rounded = rounded.getClass().getMethod("roundingMode", java.math.RoundingMode.class).invoke(rounded,
                java.math.RoundingMode.HALF_UP);
            rounded = rounded.getClass().getMethod("locale", java.util.Locale.class).invoke(rounded, java.util.Locale.US);
            check("13".equals(rounded.getClass().getMethod("format", double.class).invoke(rounded, 12.5).toString()),
                "Android 30 number formatter rounding mode");
            Class<?> unit = Class.forName("android.icu.util.MeasureUnit");
            Object usd = Class.forName("android.icu.util.Currency").getMethod("getInstance", String.class).invoke(null, "USD");
            Object money = numbers.getMethod("with").invoke(null);
            money = money.getClass().getMethod("unit", unit).invoke(money, usd);
            money = money.getClass().getMethod("locale", java.util.Locale.class).invoke(money, java.util.Locale.US);
            check(money.getClass().getMethod("format", double.class).invoke(money, 12.3).toString().contains("$"),
                "Android 30 number formatter currency");
            Intent i = new Intent(ACTION).setPackage(GMS);
            ResolveInfo r = getPackageManager().resolveService(i, 0);
            check(r != null && r.serviceInfo.name.equals("org.microg.gms.ads.identifier.AdvertisingIdService"), "real service resolution");
            check(getPackageManager().resolveService(new Intent("com.google.android.gms.NO_SUCH_SERVICE").setPackage(GMS), 0) == null, "unknown service is absent");
            check(!bindService(new Intent(ACTION).setPackage("wrong.package"), connection, BIND_AUTO_CREATE), "package routing");
            ServiceConnection cancelled = new ServiceConnection() {
                public void onServiceConnected(ComponentName n, IBinder b) { fail(new AssertionError("callback after immediate unbind")); }
                public void onServiceDisconnected(ComponentName n) { fail(new AssertionError("cancelled binding disconnected")); }
                public void onNullBinding(ComponentName n) { fail(new AssertionError("cancelled null binding")); }
            };
            check(bindService(i, cancelled, BIND_AUTO_CREATE), "pending bind accepted");
            unbindService(cancelled);
            check(bindService(i, connection, BIND_AUTO_CREATE), "bind accepted");
            new Handler().postDelayed(() -> { if (!done) fail(new AssertionError("binding timeout")); }, 120000);
        } catch (Throwable t) { fail(t); }
    }
    private final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder b) {
            try {
                check(GMS.equals(name.getPackageName()), "callback package");
                check(DESCRIPTOR.equals(b.getInterfaceDescriptor()), "binder descriptor");
                Parcel data = Parcel.obtain(), reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR);
                    check(b.transact(1, data, reply, 0), "get ID transaction"); reply.readException();
                    check("00000000-0000-0000-0000-000000000000".equals(reply.readString()), "microG default limited identifier");
                } finally { data.recycle(); reply.recycle(); }
                data = Parcel.obtain(); reply = Parcel.obtain();
                try {
                    data.writeInterfaceToken(DESCRIPTOR); data.writeInt(1);
                    check(b.transact(2, data, reply, 0), "tracking limit transaction"); reply.readException();
                    check(reply.readInt() != 0, "tracking limited by default");
                } finally { data.recycle(); reply.recycle(); }
                if (++connections == 1) {
                    first = b; unbindService(this);
                    new Handler().post(() -> {
                        try { check(bindService(new Intent(ACTION).setPackage(GMS), connection, BIND_AUTO_CREATE), "rebind accepted"); }
                        catch (Throwable t) { fail(t); }
                    });
                } else {
                    check(first != b, "service destroyed after last unbind");
                    unbindService(this);
                    check(bindService(new Intent("com.google.android.gms.common.service.START").setPackage(GMS), brokerConnection, BIND_AUTO_CREATE), "broker bind");
                }
            } catch (Throwable t) { fail(t); }
        }
        @Override public void onServiceDisconnected(ComponentName n) { fail(new AssertionError("disconnected: " + n)); }
        @Override public void onNullBinding(ComponentName n) { fail(new AssertionError("null binding: " + n)); }
    };
    private final ServiceConnection brokerConnection = new ServiceConnection() {
        public void onServiceConnected(ComponentName name, IBinder binder) {
            Parcel data = Parcel.obtain(), reply = Parcel.obtain();
            try {
                check("com.google.android.gms.common.internal.IGmsServiceBroker".equals(binder.getInterfaceDescriptor()), "broker descriptor");
                final boolean[] called = {false};
                Binder callback = new Binder() {
                    @Override protected boolean onTransact(int code, Parcel in, Parcel out, int flags) throws RemoteException {
                        if (code != 1) return super.onTransact(code, in, out, flags);
                        in.enforceInterface("com.google.android.gms.common.internal.IGmsCallbacks");
                        check(in.readInt() == 0, "broker success status");
                        IBinder service = in.readStrongBinder();
                        check(service != null && "com.google.android.gms.common.internal.service.ICommonService".equals(service.getInterfaceDescriptor()), "broker service binder");
                        called[0] = true; out.writeNoException(); return true;
                    }
                };
                data.writeInterfaceToken("com.google.android.gms.common.internal.IGmsServiceBroker");
                data.writeStrongBinder(callback); data.writeInt(1);
                // SafeParcelable GetServiceRequest, encoded on the client side without loading microG classes.
                data.writeInt(0xffff4f45); int sizeAt = data.dataPosition(); data.writeInt(0); int start = data.dataPosition();
                data.writeInt(0x40001); data.writeInt(6);
                data.writeInt(0x40002); data.writeInt(39);
                data.writeInt(0x40003); data.writeInt(252432034);
                data.writeInt(0xffff0004); int fieldAt = data.dataPosition(); data.writeInt(0); int fieldStart = data.dataPosition();
                data.writeString(getPackageName()); int fieldEnd = data.dataPosition();
                data.setDataPosition(fieldAt); data.writeInt(fieldEnd - fieldStart); data.setDataPosition(fieldEnd);
                data.writeInt(0xffff0007); fieldAt = data.dataPosition(); data.writeInt(0); fieldStart = data.dataPosition();
                data.writeBundle(new Bundle()); fieldEnd = data.dataPosition();
                data.setDataPosition(fieldAt); data.writeInt(fieldEnd - fieldStart); data.setDataPosition(fieldEnd);
                // Field 11, the API's features: an array of the client's own Feature, as the client library writes it. microG's
                // broker reads it with its own Feature's creator, and must get its own Feature objects (android.os.Parcel).
                data.writeInt(0xffff000b); fieldAt = data.dataPosition(); data.writeInt(0); fieldStart = data.dataPosition();
                data.writeTypedArray(new com.google.android.gms.common.Feature[] { new com.google.android.gms.common.Feature("megler_probe", 7) }, 0);
                fieldEnd = data.dataPosition();
                data.setDataPosition(fieldAt); data.writeInt(fieldEnd - fieldStart); data.setDataPosition(fieldEnd);
                // Field 10, the default features, as the client library's SafeParcelWriter writes a typed array itself, in
                // Android's layout: the count, then each item after its size, written out in place (Play Store's are so).
                data.writeInt(0xffff000a); fieldAt = data.dataPosition(); data.writeInt(0); fieldStart = data.dataPosition();
                data.writeInt(1);
                int itemAt = data.dataPosition(); data.writeInt(0); int itemStart = data.dataPosition();
                new com.google.android.gms.common.Feature("megler_inline", 9).writeToParcel(data, 0);
                int itemEnd = data.dataPosition();
                data.setDataPosition(itemAt); data.writeInt(itemEnd - itemStart); data.setDataPosition(itemEnd);
                fieldEnd = data.dataPosition();
                data.setDataPosition(fieldAt); data.writeInt(fieldEnd - fieldStart); data.setDataPosition(fieldEnd);
                data.setDataPosition(sizeAt); data.writeInt(fieldEnd - start); data.setDataPosition(fieldEnd);
                check(binder.transact(46, data, reply, 0), "getService transaction"); reply.readException();
                check(called[0], "broker callback delivered");
                check(com.google.android.gms.common.internal.GetServiceRequest.CREATOR == 1729, "client survives broker call");
                dynamiteModule();
                unbindService(this);
                // AccountManager blocks for its answers, which it may not do on the main thread.
                new Thread(() -> {
                    try {
                        sharedPreferences();
                        accounts();
                        runOnUiThread(() -> {
                            try { check(bindService(new Intent("com.google.android.c2dm.intent.REGISTER").setPackage(GMS), gcmConnection, BIND_AUTO_CREATE), "GCM registration bind"); }
                            catch (Throwable t) { fail(t); }
                        });
                    } catch (Throwable t) { runOnUiThread(() -> fail(t)); }
                }, "accounts").start();
            } catch (Throwable t) { fail(t); }
            finally { data.recycle(); reply.recycle(); }
        }
        public void onServiceDisconnected(ComponentName n) { fail(new AssertionError("broker disconnected")); }
        public void onNullBinding(ComponentName n) { fail(new AssertionError("broker null binding")); }
    };

    /**
     * microG's GCM registration service, as Firebase's MessengerIpcClient binds it for an app's FCM token: its binder is a
     * Messenger's, which says it is android.os.IMessenger (Firebase takes no other), and a Messenger over it is that one.
     */
    private final ServiceConnection gcmConnection = new ServiceConnection() {
        public void onServiceConnected(ComponentName name, IBinder binder) {
            try {
                check(name.getClassName().startsWith("org.microg.gms.gcm."), "GCM registration service: " + name.getClassName());
                check("android.os.IMessenger".equals(binder.getInterfaceDescriptor()), "GCM registration binder descriptor: " + binder.getInterfaceDescriptor());
                check(new Messenger(binder).getBinder() == binder, "Messenger over the registration binder");
                check(gmsBroadcastContext, "microG receiver got its registration context");
                check(gmsFlaggedBroadcastContext, "microG flagged receiver got its registration context");
                // GmsCore's two storages' folders in its ApplicationInfo, as Android gives them (by reflection: hidden in the SDK)
                android.content.pm.ApplicationInfo gmsInfo = getPackageManager().getApplicationInfo("com.google.android.gms", 0);
                Object credential = android.content.pm.ApplicationInfo.class.getField("credentialProtectedDataDir").get(gmsInfo);
                Object device = android.content.pm.ApplicationInfo.class.getField("deviceProtectedDataDir").get(gmsInfo);
                check(gmsInfo.dataDir.equals(credential) && device != null && !device.equals(credential), "storage folders: " + credential + " / " + device);
                // bindings of a GmsCore service share a binder as Android's Intent.filterEquals has it: told apart by their data
                // and categories as well as their action, and not by their extras (megler.MicroG.bindKey, by reflection)
                java.lang.reflect.Method key = Class.forName("megler.MicroG").getMethod("bindKey", Intent.class);
                Intent one = new Intent("com.example.API.START").setData(android.net.Uri.parse("chimera-action:one"));
                Intent two = new Intent("com.example.API.START").setData(android.net.Uri.parse("chimera-action:two"));
                Intent oneAgain = new Intent(one).putExtra("extra", 1);
                check(!key.invoke(null, one).equals(key.invoke(null, two)) && key.invoke(null, one).equals(key.invoke(null, oneAgain)),
                      "binding keys: " + key.invoke(null, one) + " / " + key.invoke(null, two));
                // the app's uid has the GmsCore it hosts among its packages, as a shared user id has (GmsCore checks it of callers)
                java.util.List<String> sharing = java.util.Arrays.asList(getPackageManager().getPackagesForUid(android.os.Process.myUid()));
                check(sharing.contains(getPackageName()) && sharing.contains("com.google.android.gms"), "the app's uid's packages: " + sharing);
                // and GmsCore's own uid is not the app's, as on Android: a service of its own it resolves is of that uid, whose
                // packages are Google's alone, GmsCore first (it asks whether Google signed the first: Instagram's Google sign-in)
                android.content.pm.ResolveInfo authenticator = getPackageManager().resolveService(
                        new Intent("android.accounts.AccountAuthenticator").setPackage("com.google.android.gms"), 0);
                String[] google = getPackageManager().getPackagesForUid(gmsInfo.uid);
                check(gmsInfo.uid != android.os.Process.myUid() && authenticator != null && authenticator.serviceInfo.applicationInfo.uid == gmsInfo.uid
                      && google != null && "com.google.android.gms".equals(google[0]) && !java.util.Arrays.asList(google).contains(getPackageName()),
                      "GmsCore's uid " + gmsInfo.uid + ": " + java.util.Arrays.toString(google));
                // microG's device registration is on unless its settings turned it off (MicroG.registrationOn, at microG's boot,
                // which its first service made here has had): Play Store has no Android ID without a check-in.
                // (Read as microG reads it: its default preferences, through its package's context -- they are in the services' shared
                // folder, or in the app's own data where there is none.)
                android.content.SharedPreferences microgPrefs = createPackageContext(GMS, CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY)
                        .getSharedPreferences(GMS + "_preferences", MODE_PRIVATE);
                check(microgPrefs.getBoolean("checkin_enable_service", false), "microG's device registration is on: " + microgPrefs.getAll());
                unbindService(this);
                done = true;
                Log.i("MicroGTest", "PASS isolated GmsCore broker, duplicate classes, reflection, arrays, advertising ID, storage and resources, dynamite modules, accounts, GCM's messenger and receiver context");
                text.setText("microG isolated services: PASS");
            } catch (Throwable t) { fail(t); }
        }
        public void onServiceDisconnected(ComponentName n) { fail(new AssertionError("GCM registration disconnected")); }
        public void onNullBinding(ComponentName n) { fail(new AssertionError("GCM registration null binding")); }
    };

    /**
     * microG's settings in the services' shared folder, as two processes have them: each its own copy of the file in memory. One
     * turns a setting on (as microG's settings turn device registration on); the other, which read the file before, then writes
     * another setting. The first setting must stay on, and the other copy must see it.
     */
    private void sharedPreferences() throws Exception {
        java.io.File file = new java.io.File("/data/megler-services/megler-test/shared_prefs/two-processes.xml");
        file.getParentFile().mkdirs();
        file.delete();
        java.lang.reflect.Constructor<?> make = Class.forName("android.content.MeglerSharedPreferences").getConstructor(java.io.File.class);
        SharedPreferences settings = (SharedPreferences) make.newInstance(file), other = (SharedPreferences) make.newInstance(file);
        check(!other.getBoolean("checkin_enable_service", false), "nothing set yet");
        check(settings.edit().putBoolean("checkin_enable_service", true).commit(), "first copy writes");
        check(other.edit().putString("profile_serial", "008741F4E8A2D8C6").commit(), "second copy writes");
        SharedPreferences fresh = (SharedPreferences) make.newInstance(file);
        check(fresh.getBoolean("checkin_enable_service", false), "a setting another copy wrote survives a stale copy's write");
        check("008741F4E8A2D8C6".equals(fresh.getString("profile_serial", null)), "the stale copy's own setting is written");
        check(other.getBoolean("checkin_enable_service", false), "a copy sees what another wrote since");
        settings.edit().remove("profile_serial").apply();
        Class.forName("android.content.MeglerSharedPreferences").getMethod("flushAll").invoke(null);
        check(other.getString("profile_serial", null) == null && other.getBoolean("checkin_enable_service", false), "a removal reaches the other copy");
    }

    /**
     * Google accounts are AccountManager's, and microG is their authenticator: its type is there, an account added is kept where
     * every app sees it, microG's login service (not exported: the system binds it) answers a request, adding an account
     * without an activity hands back microG's sign-in screen, and an account goes when removed. No network is used.
     */
    private void accounts() throws Exception {
        final String type = "com.google";
        android.accounts.AccountManager am = android.accounts.AccountManager.get(this);
        android.accounts.AuthenticatorDescription google = null;
        for (android.accounts.AuthenticatorDescription d : am.getAuthenticatorTypes()) if (type.equals(d.type)) google = d;
        check(google != null && GMS.equals(google.packageName) && google.customTokens, "microG authenticates com.google");
        final java.util.concurrent.CountDownLatch heard = new java.util.concurrent.CountDownLatch(1);
        final String name = "megler.test@example.com";
        android.accounts.OnAccountsUpdateListener listener = accounts -> {
            for (android.accounts.Account a : accounts) if (name.equals(a.name)) heard.countDown();
        };
        am.addOnAccountsUpdatedListener(listener, null, false);
        // While someone listens, the accounts' file is looked at every two seconds, on a thread of its own: the main thread goes on
        // while another thread holds the accounts' lock. (On the main thread, the look waited for the lock there: TikTok Lite's
        // main thread did so a quarter of its time while Google Play services' calls held it, 0.1.0.675.) The lock is held here
        // for longer than a look's period, and the main thread is asked for something every 50 ms meanwhile.
        {
            final android.os.Handler main = new android.os.Handler(android.os.Looper.getMainLooper());
            final java.util.concurrent.atomic.AtomicLong worst = new java.util.concurrent.atomic.AtomicLong();
            synchronized (Class.forName("megler.Accounts")) {
                long end = android.os.SystemClock.uptimeMillis() + 4500;
                while (android.os.SystemClock.uptimeMillis() < end) {
                    final long posted = android.os.SystemClock.uptimeMillis();
                    main.post(() -> worst.accumulateAndGet(android.os.SystemClock.uptimeMillis() - posted, Math::max));
                    Thread.sleep(50);
                }
            }
            final java.util.concurrent.CountDownLatch drained = new java.util.concurrent.CountDownLatch(1);
            main.post(drained::countDown);
            check(drained.await(30, java.util.concurrent.TimeUnit.SECONDS) && worst.get() < 1500,
                  "the main thread goes on while the accounts' lock is held: it answered in " + worst.get() + " ms at worst");
        }
        android.accounts.Account account = new android.accounts.Account(name, type);
        check(am.addAccountExplicitly(account, null, null), "account added");
        check(!am.addAccountExplicitly(account, null, null), "account added once");
        check(java.util.Arrays.asList(am.getAccountsByType(type)).contains(account), "account listed");
        check(heard.await(10, java.util.concurrent.TimeUnit.SECONDS), "listener told of the account");
        // An account another process adds (microG's sign-in in Google Play services' own package writes the shared file, as
        // Megler's accounts write it: beside it, then renamed over it) is heard through the file's look, and told on the main thread.
        java.io.File accountsFile = new java.io.File("/data/megler-services/accounts/accounts.json");
        final String outsider = "outside.megler.test@example.com";
        final java.util.concurrent.CountDownLatch heardOutside = new java.util.concurrent.CountDownLatch(1);
        final boolean[] toldOnMain = { false };
        android.accounts.OnAccountsUpdateListener outsideListener = accounts -> {
            for (android.accounts.Account a : accounts) if (outsider.equals(a.name)) {
                toldOnMain[0] = android.os.Looper.myLooper() == android.os.Looper.getMainLooper();
                heardOutside.countDown();
            }
        };
        am.addOnAccountsUpdatedListener(outsideListener, null, false);
        {
            byte[] bytes = new byte[(int) accountsFile.length()];
            try (java.io.FileInputStream in = new java.io.FileInputStream(accountsFile)) {
                int n = 0, got;
                while (n < bytes.length && (got = in.read(bytes, n, bytes.length - n)) > 0) n += got;
            }
            org.json.JSONObject kept = new org.json.JSONObject(new String(bytes, "UTF-8"));
            kept.getJSONArray("accounts").put(new org.json.JSONObject().put("name", outsider).put("type", type)
                    .put("data", new org.json.JSONObject()).put("tokens", new org.json.JSONObject()));
            java.io.File next = new java.io.File(accountsFile.getPath() + ".test");
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(next)) { out.write(kept.toString().getBytes("UTF-8")); }
            check(next.renameTo(accountsFile), "the accounts' file written as another process writes it");
        }
        check(heardOutside.await(15, java.util.concurrent.TimeUnit.SECONDS) && toldOnMain[0],
              "an account another process added is heard (told on the main thread: " + toldOnMain[0] + ")");
        am.removeOnAccountsUpdatedListener(outsideListener);
        check(am.removeAccountExplicitly(new android.accounts.Account(outsider, type)), "the other process's account removed");
        am.removeOnAccountsUpdatedListener(listener);
        check(accountsFile.isFile(), "accounts kept in the services' shared folder");
        am.setUserData(account, "megler", "1");
        check("1".equals(am.getUserData(account, "megler")), "user data");
        am.setAuthToken(account, "probe", "token-1");
        check("token-1".equals(am.peekAuthToken(account, "probe")), "token kept");
        am.invalidateAuthToken(type, "token-1");
        check(am.peekAuthToken(account, "probe") == null, "token invalidated");
        Bundle confirmed = am.confirmCredentials(account, null, null, null, null).getResult(30, java.util.concurrent.TimeUnit.SECONDS);
        check(confirmed.getBoolean(android.accounts.AccountManager.KEY_BOOLEAN_RESULT), "microG's login service confirms");
        Bundle add = am.addAccount(type, null, null, null, null, null, null).getResult(30, java.util.concurrent.TimeUnit.SECONDS);
        Intent signIn = add.getParcelable(android.accounts.AccountManager.KEY_INTENT);
        check(signIn != null && signIn.getComponent() != null && GMS.equals(signIn.getComponent().getPackageName())
            && signIn.getComponent().getClassName().equals("org.microg.gms.auth.login.LoginActivity"), "microG's sign-in screen to add an account");
        check(signIn.getParcelableExtra(android.accounts.AccountManager.KEY_ACCOUNT_AUTHENTICATOR_RESPONSE) != null, "sign-in screen given the response");
        check(am.removeAccount(account, null, null, null).getResult(30, java.util.concurrent.TimeUnit.SECONDS).getBoolean(android.accounts.AccountManager.KEY_BOOLEAN_RESULT), "account removed");
        check(am.getAccountsByType(type).length == 0, "no account left");
        // A type no authenticator has: the request fails as Android's does when it cannot bind one.
        try {
            am.addAccount("megler.test.no-such-type", null, null, null, null, null, null).getResult(10, java.util.concurrent.TimeUnit.SECONDS);
            check(false, "an account type without an authenticator was added");
        } catch (android.accounts.AuthenticatorException expected) { }
    }

    /**
     * A dynamite module as a client's DynamiteModule loads one: microG's own factory makes the module's context, whose class loader
     * is a PathClassLoader over microG's APK. Its classes must be microG's, not the client's of the same name, and the provider
     * installer (Sberbank's first call into microG) must load conscrypt's natives from microG's APK and put its provider first.
     */
    private void dynamiteModule() throws Exception {
        Context gms = createPackageContext(GMS, CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY);
        Class<?> factory = Class.forName("com.google.android.gms.chimera.DynamiteContextFactory", true, gms.getClassLoader());
        Context module = (Context) factory.getMethod("createDynamiteContext", String.class, Context.class)
            .invoke(null, "com.google.android.gms.providerinstaller.dynamite", this);
        check(module != null, "dynamite module context");
        ClassLoader loader = module.getClassLoader();
        check(loader != gms.getClassLoader() && "dalvik.system.PathClassLoader".equals(loader.getClass().getName()), "module loader is microG's PathClassLoader");
        Class<?> client = com.google.android.gms.common.internal.GetServiceRequest.class;
        check(loader.loadClass(client.getName()) == Class.forName(client.getName(), false, gms.getClassLoader()), "module classes are microG's");
        check(Class.forName(client.getName(), false, loader) != client, "module class by name is not the client's");
        Class<?> installer = loader.loadClass("com.google.android.gms.providerinstaller.ProviderInstallerImpl");
        installer.getMethod("insertProvider", Context.class).invoke(null, module);
        java.security.Provider first = java.security.Security.getProviders()[0];
        check("GmsCore_OpenSSL".equals(first.getName()), "provider installer puts microG's conscrypt first, not " + first.getName());
        check("GmsCore_OpenSSL".equals(javax.net.ssl.SSLContext.getDefault().getProvider().getName()), "default TLS context from microG's conscrypt");
        check(com.google.android.gms.org.conscrypt.NativeCrypto.CLIENT_MARKER == 731, "client native class after the module's natives");
    }

}
