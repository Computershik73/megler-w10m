package megler.test.crossapp;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.RemoteException;
import android.util.Log;
import android.widget.TextView;

import java.io.File;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

import megler.test.shared.Note;
import megler.test.shared.Tag;

/**
 * One app working with another the phone has (the test app plugin), as Total Commander works with its plug-ins. A thread of
 * the app's goes through the steps and says how each went; what the other app got of them the test sees in that app's own
 * run, which follows this one (core/tests/mg_java_tests.c: run_crossapp, run_plugin). The other app's service it binds runs
 * inside this app (megler.HostedApps), and says so in this run; and so do that app's screens asked for once it does, which
 * is why the activity for the other app's own run is asked for before the service is bound.
 */
public class Main extends Activity {
    private static final String T = "CrossAppTest";
    private static final String PLUGIN = "megler.test.plugin";
    /* Where the intents for the other app wait for it, in the folder every app has (megler.PhoneApps). */
    private static final File INTENTS = new File("/data/megler-services/intents/" + PLUGIN);
    /* The other app's service: its class, its action, and its interface's name (megler.test.plugin.PluginService). */
    private static final String SERVICE = PLUGIN + ".PluginService", IPLUGIN = PLUGIN + ".IPlugin";
    private int ok, failed;
    private String detail = "";
    private Conn first, second;
    /* The result of the other app's screen that ran here (request 78), and whether this screen is in front. */
    private volatile CountDownLatch answered;
    private volatile Intent answer;
    private volatile int answerCode;
    private volatile boolean front;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i(T, "onCreate");
        TextView v = new TextView(this);
        v.setText("Cross-app");
        setContentView(v);
        new Thread(this::steps, "steps").start();
    }

    @Override
    protected void onResume() {
        super.onResume();
        front = true;
    }

    @Override
    protected void onPause() {
        front = false;
        super.onPause();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        Log.i(T, "result of " + request + ": " + result);
        if (request != 78) return;
        answerCode = result;
        answer = data;
        CountDownLatch l = answered;
        if (l != null) l.countDown();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i(T, "onDestroy");
    }

    private interface Step { boolean run() throws Exception; }

    private void step(String name, Step s) {
        boolean good;
        detail = "";
        try {
            good = s.run();
        } catch (Throwable e) {
            Log.e(T, name + " threw", e);
            good = false;
        }
        if (good) ok++;
        else failed++;
        Log.i(T, name + (good ? " ok" : " FAILED " + detail));
    }

    /* On the main thread, as an app's screen does it; false when it threw or did not run within a minute. */
    private boolean onMain(Runnable r) throws InterruptedException {
        CountDownLatch done = new CountDownLatch(1);
        boolean[] good = { false };
        runOnUiThread(() -> {
            try {
                r.run();
                good[0] = true;
            } catch (Throwable e) {
                Log.e(T, "on the main thread", e);
                detail += " threw " + e;
            }
            done.countDown();
        });
        return done.await(60, TimeUnit.SECONDS) && good[0];
    }

    private static int waiting() {
        String[] names = INTENTS.list();
        return names == null ? 0 : names.length;
    }

    private static Uri uri(String path) { return Uri.parse("content://megler.test.crossapp/" + path); }

    /* A binding: what the connection heard, and on which thread. */
    private static final class Conn implements ServiceConnection {
        private final CountDownLatch connected = new CountDownLatch(1);
        volatile IBinder binder;
        volatile ComponentName name;
        volatile boolean main;

        @Override
        public void onServiceConnected(ComponentName n, IBinder b) {
            name = n;
            binder = b;
            main = Looper.myLooper() == Looper.getMainLooper();
            connected.countDown();
        }

        @Override
        public void onServiceDisconnected(ComponentName n) {}

        IBinder await() throws InterruptedException {
            connected.await(30, TimeUnit.SECONDS);
            return binder;
        }
    }

    /* What the other app's service says of where it runs: its first transaction. */
    private static final class Said {
        String pkg, label, files, app;
        int apps, made, own;

        @Override
        public String toString() { return pkg + " '" + label + "' " + files + " " + app + " apps=" + apps + " made=" + made + " own=" + own; }
    }

    private static Said describe(IBinder b) throws RemoteException {
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        try {
            Said s = new Said();
            d.writeInterfaceToken(IPLUGIN);
            if (!b.transact(IBinder.FIRST_CALL_TRANSACTION, d, r, 0)) return s;
            r.readException();
            s.pkg = r.readString();
            s.label = r.readString();
            s.files = r.readString();
            s.app = r.readString();
            s.apps = r.readInt();
            s.made = r.readInt();
            s.own = r.readInt();
            return s;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /* What the other app's service says of its screens: what it heard of them, how many of each were made, what its own one said. */
    private static String[] state(IBinder b) throws RemoteException {
        Parcel d = Parcel.obtain(), r = Parcel.obtain();
        try {
            d.writeInterfaceToken(IPLUGIN);
            if (!b.transact(IBinder.FIRST_CALL_TRANSACTION + 2, d, r, 0)) return new String[] { null, "-1", "-1", null };
            r.readException();
            return new String[] { r.readString(), String.valueOf(r.readInt()), String.valueOf(r.readInt()), r.readString() };
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    private boolean inFront() throws InterruptedException {
        for (int n = 0; n < 100 && !front; n++) Thread.sleep(100);
        return front;
    }

    private void steps() {
        PackageManager pm = getPackageManager();
        /* The other app is installed: its package's version and label, and its launcher's intent. */
        step("installed", () -> {
            PackageInfo p = pm.getPackageInfo(PLUGIN, 0);
            ApplicationInfo a = pm.getApplicationInfo(PLUGIN, 0);
            Intent launch = pm.getLaunchIntentForPackage(PLUGIN);
            detail = "version=" + p.versionName + "/" + p.versionCode + " label=" + pm.getApplicationLabel(a) + " launch=" + launch;
            return "1.7".equals(p.versionName) && p.versionCode == 7 && "Megler Plug-in".contentEquals(pm.getApplicationLabel(a)) && launch != null
                && (PLUGIN + ".Main").equals(launch.getComponent().getClassName());
        });
        /* An intent that names the package and what to do resolves to the activity whose filter takes it, and to none that takes it not. */
        step("resolve", () -> {
            Intent show = new Intent(PLUGIN + ".SHOW").setPackage(PLUGIN).setDataAndType(uri("notes/1"), "text/plain");
            ResolveInfo r = pm.resolveActivity(show, 0);
            ResolveInfo none = pm.resolveActivity(new Intent(PLUGIN + ".SHOW").setPackage(PLUGIN).setDataAndType(uri("notes/1"), "image/png"), 0);
            detail = "resolved=" + (r == null ? null : r.activityInfo.packageName + "/" + r.activityInfo.name) + " none=" + none;
            return r != null && PLUGIN.equals(r.activityInfo.packageName) && (PLUGIN + ".Shown").equals(r.activityInfo.name) && none == null;
        });
        /* Such an intent started: the other app is asked for that activity, and the intent waits for it, written out. */
        step("send-implicit", () -> {
            int before = waiting();
            Intent show = new Intent(PLUGIN + ".SHOW").setPackage(PLUGIN).setDataAndType(uri("notes/1"), "text/plain").putExtra("text", "first");
            boolean sent = onMain(() -> startActivity(show));
            detail += " sent=" + sent + " waiting=" + before + "->" + waiting();
            return sent && before == 0 && waiting() == 1;
        });
        /*
         * The other app's activity by its class, for a result, with an Intent of everything: an action, data with a type, two
         * categories, flags, and extras -- strings, numbers, arrays, a Bundle, a list of Uris (what Total Commander's Wi-Fi
         * plug-in gets the files to send in), a Parcelable and a Serializable of classes both apps have, an Intent. A
         * Parcelable this app alone has goes and is left out where it is read; a Binder does not go at all.
         */
        step("send", () -> {
            Intent i = new Intent(PLUGIN + ".SHOW").setClassName(PLUGIN, PLUGIN + ".Shown").setDataAndType(uri("notes/7?x=1#part"), "text/plain")
                .addCategory(Intent.CATEGORY_DEFAULT).addCategory("megler.test.category.EXTRA").addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            i.putExtra("text", "hello, plug-in").putExtra("wide", "é中😀").putExtra("count", 42).putExtra("big", 1234567890123L)
                .putExtra("flag", true).putExtra("ratio", 2.5).putExtra("longs", new long[] { 1, -2, 3000000000L }).putExtra("bytes", new byte[] { 1, -2, 127 })
                .putExtra("strings", new String[] { "a", null, "c" });
            Bundle nested = new Bundle();
            nested.putString("inner", "in");
            nested.putInt("n", 5);
            nested.putStringArrayList("list", new ArrayList<>(Arrays.asList("x", "y")));
            i.putExtra("nested", nested);
            ArrayList<Uri> uris = new ArrayList<>(Arrays.asList(uri("files/a.txt"), uri("files/b%20c.txt")));
            i.putParcelableArrayListExtra("uris", uris);
            i.putExtra(Intent.EXTRA_STREAM, uris.get(0));
            Bundle more = new Bundle();
            more.putString("k", "v");
            i.putExtra("note", new Note(7, "seven", uri("notes/7"), more, new Note(8, "eight", null, null, null), "t1", "t2"));
            i.putExtra("notes", new Parcelable[] { new Note(1, "one", null, null, null), new Note(2, "two", null, null, null) });
            i.putExtra("tag", new Tag("blue", 3));
            i.putExtra("only", new Only(5));
            i.putExtra("inner", new Intent("megler.test.INNER").putExtra("deep", "down"));
            Bundle binder = new Bundle();
            binder.putBinder("binder", new Binder());
            i.putExtras(binder);
            boolean sent = onMain(() -> startActivityForResult(i, 77));
            String[] names = INTENTS.list();
            detail += " sent=" + sent + " waiting=" + Arrays.toString(names);
            if (names == null || names.length != 2) return false;
            for (String name : names) if (!name.matches("[0-9a-f-]+")) return false;
            return sent;
        });
        /*
         * The other app's services another app may bind, found as Total Commander finds its plug-ins: by the action alone,
         * whichever package has it. The exported one with that action is found, with its own label; the package's services
         * are told of, and the one service by its class. The one for the plug-in alone is nobody else's, and the one behind
         * a permission this app does not ask for is told of but not found: what is found can be bound.
         */
        step("services", () -> {
            List<ResolveInfo> found = pm.queryIntentServices(new Intent(IPLUGIN), 0);
            ResolveInfo one = pm.resolveService(new Intent(IPLUGIN).setPackage(PLUGIN), 0);
            ServiceInfo byClass = pm.getServiceInfo(new ComponentName(PLUGIN, SERVICE), 0);
            ArrayList<String> names = new ArrayList<>();
            PackageInfo p = pm.getPackageInfo(PLUGIN, PackageManager.GET_SERVICES);
            if (p.services != null) for (ServiceInfo s : p.services) names.add(s.name.substring(s.name.lastIndexOf('.') + 1));
            int others = pm.queryIntentServices(new Intent(PLUGIN + ".PRIVATE"), 0).size() + pm.queryIntentServices(new Intent(PLUGIN + ".GUARDED").setPackage(PLUGIN), 0).size()
                + pm.queryIntentServices(new Intent().setComponent(new ComponentName(PLUGIN, PLUGIN + ".Private")), 0).size();
            detail = "found=" + found.size() + " one=" + (one == null ? null : one.serviceInfo.packageName + "/" + one.serviceInfo.name) + " names=" + names + " others=" + others;
            if (found.size() != 1 || one == null || byClass == null) return false;
            ServiceInfo s = found.get(0).serviceInfo;
            CharSequence label = found.get(0).loadLabel(pm);
            one.loadIcon(pm);
            detail += " label=" + label + " exported=" + s.exported + " byClass=" + byClass.name;
            return PLUGIN.equals(s.packageName) && SERVICE.equals(s.name) && s.exported && "Plug-in service".contentEquals(label) && SERVICE.equals(one.serviceInfo.name)
                && SERVICE.equals(byClass.name) && PLUGIN.equals(byClass.packageName) && PLUGIN.equals(byClass.applicationInfo.packageName)
                && names.contains("PluginService") && names.contains("Guarded") && !names.contains("Private") && others == 0;
        });
        /* That service bound as Total Commander binds a plug-in's, by the action and the class: the connection hears of it on the main thread. */
        step("bind", () -> {
            first = new Conn();
            boolean bound = bindService(new Intent(IPLUGIN).setComponent(new ComponentName(PLUGIN, SERVICE)), first, Context.BIND_AUTO_CREATE);
            IBinder b = first.await();
            detail = "bound=" + bound + " name=" + first.name + " binder=" + (b == null ? null : b.getClass().getName()) + " main=" + first.main;
            return bound && b != null && first.main && new ComponentName(PLUGIN, SERVICE).equals(first.name);
        });
        /*
         * A call of it, a transaction as an AIDL proxy makes: the service is the other app's in everything that tells one app
         * from another -- its package, its resources, its Application, made once, a files folder of its own in the folder
         * every app has -- inside this app, which stays itself. And the service has bound a service of its own, in its own
         * classes.
         */
        step("call", () -> {
            Said s = describe(first.binder);
            for (int n = 0; n < 50 && s.own != 1; n++) {
                Thread.sleep(100);
                s = describe(first.binder);
            }
            boolean served = s.files != null && new File(s.files, "served.txt").isFile();
            detail = s + " served=" + served + " mine=" + getPackageName() + " '" + getString(R.string.app_name) + "' " + getApplication().getClass().getName();
            return PLUGIN.equals(s.pkg) && "Megler Plug-in".equals(s.label) && ("/data/megler-services/hosted/" + PLUGIN + "/files").equals(s.files)
                && (PLUGIN + ".App").equals(s.app) && s.apps == 1 && s.made == 1 && s.own == 1 && served
                && "megler.test.crossapp".equals(getPackageName()) && "Megler Cross-app".equals(getString(R.string.app_name));
        });
        /* A Binder of this app's handed to the service in a call, and called back by it before the call returns. */
        step("callback", () -> {
            String[] heard = { null };
            Binder callback = new Binder() {
                @Override
                protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
                    if (code != IBinder.FIRST_CALL_TRANSACTION) return super.onTransact(code, data, reply, flags);
                    data.enforceInterface(PLUGIN + ".ICallback");
                    heard[0] = data.readString();
                    reply.writeNoException();
                    reply.writeInt(41);
                    return true;
                }
            };
            Parcel d = Parcel.obtain(), r = Parcel.obtain();
            int answer;
            try {
                d.writeInterfaceToken(IPLUGIN);
                d.writeStrongBinder(callback);
                d.writeString("hello");
                boolean taken = first.binder.transact(IBinder.FIRST_CALL_TRANSACTION + 1, d, r, 0);
                r.readException();
                answer = r.readInt();
                detail = "taken=" + taken + " heard=" + heard[0] + " answer=" + answer;
                return taken && "plugin heard: hello".equals(heard[0]) && answer == 42;
            } finally {
                d.recycle();
                r.recycle();
            }
        });
        /* Bound again by the action within the package: the same service, the same Binder, and nothing made twice. */
        step("bind-action", () -> {
            second = new Conn();
            boolean bound = bindService(new Intent(IPLUGIN).setPackage(PLUGIN), second, Context.BIND_AUTO_CREATE);
            IBinder b = second.await();
            Said s = b == null ? new Said() : describe(b);
            detail = "bound=" + bound + " same=" + (b == first.binder) + " name=" + second.name + " " + s;
            return bound && b != null && b == first.binder && new ComponentName(PLUGIN, SERVICE).equals(second.name) && s.apps == 1 && s.made == 1;
        });
        /* Both bindings ended: the service is destroyed, and bound again it is made again, in the Application that was made once. */
        step("unbind", () -> {
            unbindService(second);
            unbindService(first);
            first = new Conn();
            boolean bound = bindService(new Intent(IPLUGIN).setComponent(new ComponentName(PLUGIN, SERVICE)), first, Context.BIND_AUTO_CREATE);
            IBinder b = first.await();
            Said s = b == null ? new Said() : describe(b);
            detail = "bound=" + bound + " " + s;
            return bound && b != null && s.apps == 1 && s.made == 2;
        });
        /*
         * What is not bound, and answers false as Android does for a service that is not there: the other app's service for
         * itself alone, the one behind a permission this app does not ask for, a service of an app the phone has not, and
         * an action with no package.
         */
        step("refused", () -> {
            Conn none = new Conn();
            boolean own = bindService(new Intent().setComponent(new ComponentName(PLUGIN, PLUGIN + ".Private")), none, Context.BIND_AUTO_CREATE);
            boolean guarded = bindService(new Intent(PLUGIN + ".GUARDED").setPackage(PLUGIN), none, Context.BIND_AUTO_CREATE);
            boolean absent = bindService(new Intent("megler.test.absent.ACTION").setComponent(new ComponentName("megler.test.absent", "megler.test.absent.Service")), none, Context.BIND_AUTO_CREATE);
            boolean nowhere = bindService(new Intent(IPLUGIN), none, Context.BIND_AUTO_CREATE);
            Thread.sleep(300);
            detail = "own=" + own + " guarded=" + guarded + " absent=" + absent + " nowhere=" + nowhere + " heard=" + none.binder;
            return !own && !guarded && !absent && !nowhere && none.binder == null;
        });
        /*
         * Megler's own way to it, which the bindings above go through and its other classes may use (megler.HostedApps.binder:
         * a service of a package by an Intent, in this process): the Binder, here from a thread that is not the main one; null
         * for what is refused.
         */
        step("entry", () -> {
            Method binder = Class.forName("megler.HostedApps").getMethod("binder", Context.class, Intent.class);
            IBinder b = (IBinder) binder.invoke(null, this, new Intent(IPLUGIN).setPackage(PLUGIN));
            Object refused = binder.invoke(null, this, new Intent(PLUGIN + ".GUARDED").setPackage(PLUGIN));
            Object absent = binder.invoke(null, this, new Intent(IPLUGIN).setPackage("megler.test.absent"));
            Said s = b == null ? new Said() : describe(b);
            detail = "same=" + (b == first.binder) + " refused=" + refused + " absent=" + absent + " " + s;
            return b != null && b == first.binder && refused == null && absent == null && PLUGIN.equals(s.pkg) && s.apps == 1 && s.made == 2;
        });
        /*
         * A handler for a package (megler.HostedApps.handle), which is how Megler answers for a service of a package itself:
         * it is asked before the phone's apps are looked at, its Binder is what a binding and the entry give, and what it
         * leaves (null) goes on as without it. Taken away, the package has no service again.
         */
        step("handler", () -> {
            String handled = "megler.test.handled";
            Class<?> hosted = Class.forName("megler.HostedApps"), type = Class.forName("megler.HostedApps$PackageHandler");
            Method handle = hosted.getMethod("handle", String.class, type), binder = hosted.getMethod("binder", Context.class, Intent.class);
            Binder answer = new Binder();
            Object handler = java.lang.reflect.Proxy.newProxyInstance(getClassLoader(), new Class<?>[] { type }, (proxy, method, args) ->
                "bind".equals(method.getName()) && (handled + ".ACTION").equals(((Intent) args[1]).getAction()) ? answer : null);
            handle.invoke(null, handled, handler);
            Conn c = new Conn(), other = new Conn();
            boolean bound = bindService(new Intent(handled + ".ACTION").setPackage(handled), c, Context.BIND_AUTO_CREATE);
            IBinder got = c.await();
            boolean left = bindService(new Intent(handled + ".OTHER").setPackage(handled), other, Context.BIND_AUTO_CREATE);
            Object direct = binder.invoke(null, this, new Intent(handled + ".ACTION").setPackage(handled));
            handle.invoke(null, handled, null);
            boolean after = bindService(new Intent(handled + ".ACTION").setPackage(handled), other, Context.BIND_AUTO_CREATE);
            detail = "bound=" + bound + " got=" + (got == answer) + " name=" + c.name + " main=" + c.main + " left=" + left + " direct=" + (direct == answer) + " after=" + after;
            return bound && got == answer && c.main && new ComponentName(handled, handled + ".ACTION").equals(c.name) && !left && direct == answer && !after;
        });
        /*
         * A screen of the other app asked for now that its service runs here, by an action within the package and for a
         * result, as Total Commander opens its LAN plug-in's "New server": it runs here too (megler.HostedApps.activity), as
         * the other app's -- its package, its label for a title, its Application, its service's own instance -- with the
         * extra made by its own class, and its result comes back with one made by this app's. The service hears the
         * broadcast the screen sends within its package. Nothing is asked of the phone: no intent waits for the other app
         * but the two from before.
         */
        step("activity", () -> {
            int before = waiting();
            answered = new CountDownLatch(1);
            Intent panel = new Intent(PLUGIN + ".PANEL").setPackage(PLUGIN).putExtra("note", new Note(9, "nine", null, null, null));
            boolean sent = onMain(() -> startActivityForResult(panel, 78));
            boolean came = answered.await(30, TimeUnit.SECONDS);
            Intent data = answer;
            Object note = data == null ? null : data.getParcelableExtra("note");
            String where = data == null ? null : data.getStringExtra("where");
            String[] s = state(first.binder);
            for (int n = 0; n < 50 && s[0] == null; n++) {
                Thread.sleep(100);
                s = state(first.binder);
            }
            boolean back = inFront();
            detail += " sent=" + sent + " came=" + came + " code=" + answerCode + " action=" + (data == null ? null : data.getAction()) + " note=" + note + " mine=" + (note instanceof Note)
                + " where=" + where + " state=" + Arrays.toString(s) + " back=" + back + " waiting=" + before + "->" + waiting();
            return sent && came && answerCode == RESULT_OK && data != null && (PLUGIN + ".PANEL_DONE").equals(data.getAction()) && note instanceof Note && ((Note) note).id == 10
                && (PLUGIN + "|Megler Plug-in|" + PLUGIN + "/.Panel|true").equals(where) && ("panel 1 in " + PLUGIN).equals(s[0]) && "1".equals(s[1]) && "0".equals(s[2]) && back
                && before == 2 && waiting() == 2;
        });
        /*
         * The other app's screen for itself alone, which its service opens (a call asks it to): made here as the plug-in's
         * own. It opens the first screen again by an action with no package, which here is the plug-in's, not this app's nor
         * the phone's, and has its result, of the plug-in's own classes; then this app's screen is in front again.
         */
        step("activity-own", () -> {
            Parcel d = Parcel.obtain(), r = Parcel.obtain();
            boolean taken;
            try {
                d.writeInterfaceToken(IPLUGIN);
                taken = first.binder.transact(IBinder.FIRST_CALL_TRANSACTION + 3, d, r, 0);
                r.readException();
            } finally {
                d.recycle();
                r.recycle();
            }
            String want = PLUGIN + "|Inner|true|5/-1/" + PLUGIN + ".PANEL_DONE/Note(10, ten, null, [], null)", heard = "panel 2 in " + PLUGIN;
            String[] s = state(first.binder);
            for (int n = 0; n < 100 && !(want.equals(s[3]) && heard.equals(s[0])); n++) {
                Thread.sleep(100);
                s = state(first.binder);
            }
            Thread.sleep(300);
            boolean back = inFront();
            detail = "taken=" + taken + " state=" + Arrays.toString(s) + " back=" + back + " waiting=" + waiting();
            return taken && want.equals(s[3]) && heard.equals(s[0]) && "2".equals(s[1]) && "1".equals(s[2]) && back && waiting() == 2;
        });
        /*
         * What does not open here (megler.HostedApps.activity answers none, and the request goes on to the phone as before):
         * the other app's screen for itself alone when this app asks for it, its launcher's intent, which is that app
         * itself, its action with no package, an app that does not run here. What another app may open does, by its class.
         */
        step("activity-refused", () -> {
            Method activity = Class.forName("megler.HostedApps").getMethod("activity", Context.class, Intent.class);
            Object inner = activity.invoke(null, this, new Intent().setClassName(PLUGIN, PLUGIN + ".Inner"));
            Object launcher = activity.invoke(null, this, pm.getLaunchIntentForPackage(PLUGIN));
            Object nowhere = activity.invoke(null, this, new Intent(PLUGIN + ".PANEL"));
            Object absent = activity.invoke(null, this, new Intent(PLUGIN + ".PANEL").setPackage("megler.test.absent"));
            Object panel = activity.invoke(null, this, new Intent().setClassName(PLUGIN, PLUGIN + ".Panel"));
            detail = "inner=" + inner + " launcher=" + launcher + " nowhere=" + nowhere + " absent=" + absent + " panel=" + (panel != null);
            return inner == null && launcher == null && nowhere == null && absent == null && panel != null;
        });
        Log.i(T, "done ok=" + ok + " failed=" + failed);
    }
}
