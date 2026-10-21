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
    private int connections;
    private IBinder first;
    private TextView text;
    private static void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); }
    private void fail(Throwable t) { done = true; Log.e("MicroGTest", "FAILED", t); text.setText("FAILED: " + t); }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        text = new TextView(this); text.setText("Checking microG..."); setContentView(text);
        try {
            PackageInfo own = getPackageManager().getPackageInfo(getPackageName(), PackageManager.GET_META_DATA);
            check(own.applicationInfo != null && getPackageName().equals(own.applicationInfo.packageName), "own package metadata");
            PackageInfo pi = getPackageManager().getPackageInfo(GMS, PackageManager.GET_SERVICES);
            check(pi.applicationInfo.sourceDir.endsWith("/GmsCore/GmsCore.apk"), "real APK metadata");
            check(pi.services != null && pi.services.length > 20, "manifest services");
            Context gms = createPackageContext(GMS, CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY);
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
                        accounts();
                        runOnUiThread(() -> {
                            done = true;
                            Log.i("MicroGTest", "PASS isolated GmsCore broker, duplicate classes, reflection, arrays, advertising ID, storage and resources, dynamite modules, accounts");
                            text.setText("microG isolated services: PASS");
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
        android.accounts.Account account = new android.accounts.Account(name, type);
        check(am.addAccountExplicitly(account, null, null), "account added");
        check(!am.addAccountExplicitly(account, null, null), "account added once");
        check(java.util.Arrays.asList(am.getAccountsByType(type)).contains(account), "account listed");
        check(heard.await(10, java.util.concurrent.TimeUnit.SECONDS), "listener told of the account");
        am.removeOnAccountsUpdatedListener(listener);
        check(new java.io.File("/data/megler-services/accounts/accounts.json").isFile(), "accounts kept in the services' shared folder");
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
