// Overlay of core/framework/src/android/accounts/AccountManager.java (tools/build-framework.sh).
// It differs in having accounts. The original answered every lookup with none and failed every request: no app could keep
// an account, and microG, whose Google accounts are AccountManager's, could neither add one nor give an app a token for one.
//
// Now it does what Android's AccountManager and AccountManagerService do between them, in the app's process: the accounts
// are megler.Accounts' (one file all apps share, in the services' folder), and a request that needs an account type's
// authenticator (addAccount, getAuthToken, confirmCredentials, updateCredentials, hasFeatures, removeAccount, ...) binds
// that authenticator's service -- microG's for com.google, or the app's own -- and calls it off the main thread, with the
// caller's package, uid and pid in the options, as the system adds them. Its answer is handled as Android's session handles
// it: an intent to start (the sign-in) is started when the caller gave an activity, or else is the result; an error code is
// the exception Android throws for it; a token is kept unless the authenticator keeps its own (customTokens). The futures
// block as Android's do, and refuse to on the main thread while they are not done.
//
// What is not here: the system's account chooser (newChooseAccountIntent stays the cancelled screen it was), visibility
// (every app sees every account), and the session API (startAddAccountSession and its kin fail as unsupported).
package android.accounts;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.RemoteException;
import android.os.UserHandle;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

@SuppressWarnings({"unchecked", "rawtypes"})
public class AccountManager {
    public static final String ACCOUNT_ACCESS_TOKEN_TYPE = "com.android.AccountManager.ACCOUNT_ACCESS_TOKEN_TYPE";
    public static final String ACTION_ACCOUNT_REMOVED = "android.accounts.action.ACCOUNT_REMOVED";
    public static final String ACTION_AUTHENTICATOR_INTENT = "android.accounts.AccountAuthenticator";
    public static final String ACTION_VISIBLE_ACCOUNTS_CHANGED = "android.accounts.action.VISIBLE_ACCOUNTS_CHANGED";
    public static final String AUTHENTICATOR_ATTRIBUTES_NAME = "account-authenticator";
    public static final String AUTHENTICATOR_META_DATA_NAME = "android.accounts.AccountAuthenticator";
    public static final int CACHE_ACCOUNTS_DATA_SIZE = 4;
    public static final String CACHE_KEY_ACCOUNTS_DATA_PROPERTY = "cache_key.system_server.accounts_data";
    public static final String CACHE_KEY_USER_DATA_PROPERTY = "cache_key.system_server.account_user_data";
    public static final int CACHE_USER_DATA_SIZE = 32;
    public static final int ERROR_CODE_BAD_ARGUMENTS = 7;
    public static final int ERROR_CODE_BAD_AUTHENTICATION = 9;
    public static final int ERROR_CODE_BAD_REQUEST = 8;
    public static final int ERROR_CODE_CANCELED = 4;
    public static final int ERROR_CODE_INVALID_RESPONSE = 5;
    public static final int ERROR_CODE_MANAGEMENT_DISABLED_FOR_ACCOUNT_TYPE = 101;
    public static final int ERROR_CODE_NETWORK_ERROR = 3;
    public static final int ERROR_CODE_REMOTE_EXCEPTION = 1;
    public static final int ERROR_CODE_UNSUPPORTED_OPERATION = 6;
    public static final int ERROR_CODE_USER_RESTRICTED = 100;
    public static final String KEY_ACCOUNTS = "accounts";
    public static final String KEY_ACCOUNT_ACCESS_ID = "accountAccessId";
    public static final String KEY_ACCOUNT_AUTHENTICATOR_RESPONSE = "accountAuthenticatorResponse";
    public static final String KEY_ACCOUNT_MANAGER_RESPONSE = "accountManagerResponse";
    public static final String KEY_ACCOUNT_NAME = "authAccount";
    public static final String KEY_ACCOUNT_SESSION_BUNDLE = "accountSessionBundle";
    public static final String KEY_ACCOUNT_STATUS_TOKEN = "accountStatusToken";
    public static final String KEY_ACCOUNT_TYPE = "accountType";
    public static final String KEY_ANDROID_PACKAGE_NAME = "androidPackageName";
    public static final String KEY_AUTHENTICATOR_TYPES = "authenticator_types";
    public static final String KEY_AUTHTOKEN = "authtoken";
    public static final String KEY_AUTH_FAILED_MESSAGE = "authFailedMessage";
    public static final String KEY_AUTH_TOKEN_LABEL = "authTokenLabelKey";
    public static final String KEY_BOOLEAN_RESULT = "booleanResult";
    public static final String KEY_CALLER_PID = "callerPid";
    public static final String KEY_CALLER_UID = "callerUid";
    public static final String KEY_ERROR_CODE = "errorCode";
    public static final String KEY_ERROR_MESSAGE = "errorMessage";
    public static final String KEY_INTENT = "intent";
    public static final String KEY_LAST_AUTHENTICATED_TIME = "lastAuthenticatedTime";
    public static final String KEY_NOTIFY_ON_FAILURE = "notifyOnAuthFailure";
    public static final String KEY_PASSWORD = "password";
    public static final String KEY_USERDATA = "userdata";
    public static final String LOGIN_ACCOUNTS_CHANGED_ACTION = "android.accounts.LOGIN_ACCOUNTS_CHANGED";
    public static final String PACKAGE_NAME_KEY_LEGACY_NOT_VISIBLE = "android:accounts:key_legacy_not_visible";
    public static final String PACKAGE_NAME_KEY_LEGACY_VISIBLE = "android:accounts:key_legacy_visible";
    public static final int VISIBILITY_NOT_VISIBLE = 3;
    public static final int VISIBILITY_UNDEFINED = 0;
    public static final int VISIBILITY_USER_MANAGED_NOT_VISIBLE = 4;
    public static final int VISIBILITY_USER_MANAGED_VISIBLE = 2;
    public static final int VISIBILITY_VISIBLE = 1;

    private static final String TAG = "AccountManager";
    /** The authenticators' calls: off the main thread, as Android's run on their binder threads. */
    private static final ExecutorService CALLS = Executors.newCachedThreadPool();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private final Context mContext;

    public AccountManager(Context context) { mContext = context; }
    public static AccountManager get(Context context) {
        if (context == null) throw new IllegalArgumentException("context is null");
        return new AccountManager(context);
    }

    private String callerPackage() {
        try { return mContext.getOpPackageName(); } catch (Throwable t) { return mContext.getPackageName(); }
    }

    // ---- the accounts, kept here

    public Account[] getAccounts() { return megler.Accounts.accounts(null); }
    public Account[] getAccountsAsUser(int userId) { return getAccounts(); }
    public Account[] getAccountsByType(String type) { return megler.Accounts.accounts(type); }
    public Account[] getAccountsByTypeAsUser(String type, UserHandle user) { return getAccountsByType(type); }
    public Account[] getAccountsByTypeForPackage(String type, String packageName) { return getAccountsByType(type); }
    public Account[] getAccountsForPackage(String packageName, int uid) { return getAccounts(); }

    public boolean addAccountExplicitly(Account account, String password, Bundle userdata) {
        if (account == null) throw new IllegalArgumentException("account is null");
        return megler.Accounts.add(account, password, userdata);
    }
    public boolean addAccountExplicitly(Account account, String password, Bundle extras, Map<String, Integer> visibility) {
        return addAccountExplicitly(account, password, extras);
    }
    public boolean removeAccountExplicitly(Account account) {
        if (account == null) throw new IllegalArgumentException("account is null");
        return megler.Accounts.remove(account);
    }
    public String getPassword(Account account) { check(account); return megler.Accounts.password(account); }
    public void setPassword(Account account, String password) { check(account); megler.Accounts.setPassword(account, password); }
    public void clearPassword(Account account) { setPassword(account, null); }
    public String getUserData(Account account, String key) {
        check(account);
        if (key == null) throw new IllegalArgumentException("key is null");
        return megler.Accounts.userData(account, key);
    }
    public void setUserData(Account account, String key, String value) {
        check(account);
        if (key == null) throw new IllegalArgumentException("key is null");
        megler.Accounts.setUserData(account, key, value);
    }
    public String peekAuthToken(Account account, String authTokenType) {
        check(account);
        if (authTokenType == null) throw new IllegalArgumentException("authTokenType is null");
        return megler.Accounts.token(account, authTokenType);
    }
    public void setAuthToken(Account account, String authTokenType, String authToken) {
        check(account);
        if (authTokenType == null) throw new IllegalArgumentException("authTokenType is null");
        megler.Accounts.setToken(account, authTokenType, authToken);
    }
    public void invalidateAuthToken(String accountType, String authToken) {
        if (accountType == null) throw new IllegalArgumentException("accountType is null");
        megler.Accounts.invalidateToken(accountType, authToken);
    }
    public String getPreviousName(Account account) { check(account); return megler.Accounts.previousName(account); }
    public boolean notifyAccountAuthenticated(Account account) { check(account); return megler.Accounts.exists(account); }
    public boolean someUserHasAccount(Account account) { return account != null && megler.Accounts.exists(account); }
    public boolean hasAccountAccess(Account account, String packageName, UserHandle userHandle) { return account != null && megler.Accounts.exists(account); }
    public int getAccountVisibility(Account account, String packageName) { check(account); return megler.Accounts.exists(account) ? VISIBILITY_VISIBLE : VISIBILITY_UNDEFINED; }
    public boolean setAccountVisibility(Account account, String packageName, int visibility) { check(account); return megler.Accounts.exists(account); }
    public Map<Account, Integer> getAccountsAndVisibilityForPackage(String packageName, String accountType) {
        HashMap<Account, Integer> out = new HashMap<>();
        for (Account a : getAccountsByType(accountType)) out.put(a, VISIBILITY_VISIBLE);
        return out;
    }
    public Map<String, Integer> getPackagesAndVisibilityForAccount(Account account) { check(account); return new HashMap<>(); }
    public void updateAppPermission(Account account, String authTokenType, int uid, boolean value) {}
    private static void check(Account account) { if (account == null) throw new IllegalArgumentException("account is null"); }

    public AuthenticatorDescription[] getAuthenticatorTypes() {
        ArrayList<AuthenticatorDescription> out = new ArrayList<>();
        for (megler.Accounts.Authenticator a : megler.Accounts.authenticators()) out.add(a.description);
        return out.toArray(new AuthenticatorDescription[0]);
    }
    public AuthenticatorDescription[] getAuthenticatorTypesAsUser(int userId) { return getAuthenticatorTypes(); }

    // ---- who hears that the accounts changed

    private static final HashMap<OnAccountsUpdateListener, Object[]> sListeners = new HashMap<>();
    private static final Runnable CHANGED = () -> {
        ArrayList<Map.Entry<OnAccountsUpdateListener, Object[]>> all;
        synchronized (sListeners) { all = new ArrayList<>(sListeners.entrySet()); }
        for (Map.Entry<OnAccountsUpdateListener, Object[]> e : all) tell(e.getKey(), (Handler) e.getValue()[0], (String[]) e.getValue()[1]);
    };
    private static void tell(OnAccountsUpdateListener l, Handler h, String[] types) {
        (h != null ? h : MAIN).post(() -> {
            ArrayList<Account> list = new ArrayList<>();
            for (Account a : megler.Accounts.accounts(null)) {
                if (types == null) { list.add(a); continue; }
                for (String t : types) if (t.equals(a.type)) { list.add(a); break; }
            }
            try { l.onAccountsUpdated(list.toArray(new Account[0])); } catch (Throwable t) { android.util.Log.w(TAG, "a listener failed", t); }
        });
    }
    public void addOnAccountsUpdatedListener(OnAccountsUpdateListener listener, Handler handler, boolean updateImmediately) {
        addOnAccountsUpdatedListener(listener, handler, updateImmediately, null);
    }
    public void addOnAccountsUpdatedListener(OnAccountsUpdateListener listener, Handler handler, boolean updateImmediately, String[] accountTypes) {
        if (listener == null) throw new IllegalArgumentException("the listener is null");
        synchronized (sListeners) {
            if (sListeners.containsKey(listener)) throw new IllegalStateException("this listener is already added");
            sListeners.put(listener, new Object[] { handler, accountTypes });
        }
        megler.Accounts.listen(CHANGED, true);
        if (updateImmediately) tell(listener, handler, accountTypes);
    }
    public void removeOnAccountsUpdatedListener(OnAccountsUpdateListener listener) {
        if (listener == null) throw new IllegalArgumentException("listener is null");
        boolean none;
        synchronized (sListeners) { sListeners.remove(listener); none = sListeners.isEmpty(); }
        if (none) megler.Accounts.listen(CHANGED, false);
    }

    // ---- futures

    /** A request's future, as Android's AmsTask and Future2Task are to their callers. */
    private static class Result<V> implements AccountManagerFuture<V> {
        private final CountDownLatch mDone = new CountDownLatch(1);
        private final AccountManagerCallback<V> mCallback;
        private final Handler mHandler;
        private V mValue;
        private Exception mError;
        private boolean mCancelled;
        private final ArrayList<Runnable> mThen = new ArrayList<>();
        Result(AccountManagerCallback<V> callback, Handler handler) { mCallback = callback; mHandler = handler; }
        void set(V value) { finish(value, null); }
        void fail(Exception error) { finish(null, error); }
        private void finish(V value, Exception error) {
            ArrayList<Runnable> then;
            synchronized (this) {
                if (mDone.getCount() == 0) return;
                mValue = value;
                mError = error;
                mDone.countDown();
                then = new ArrayList<>(mThen);
            }
            for (Runnable r : then) r.run();
            if (mCallback != null) (mHandler != null ? mHandler : MAIN).post(() -> mCallback.run(this));
        }
        /** Runs once the result is known: at once if it is. */
        void then(Runnable r) {
            synchronized (this) { if (mDone.getCount() != 0) { mThen.add(r); return; } }
            r.run();
        }
        public boolean cancel(boolean mayInterruptIfRunning) {
            synchronized (this) { if (mDone.getCount() == 0) return false; mCancelled = true; }
            fail(new OperationCanceledException());
            return true;
        }
        public boolean isCancelled() { return mCancelled; }
        public boolean isDone() { return mDone.getCount() == 0; }
        public V getResult() throws OperationCanceledException, IOException, AuthenticatorException { return getResult(-1, null); }
        public V getResult(long timeout, TimeUnit unit) throws OperationCanceledException, IOException, AuthenticatorException {
            if (!isDone() && Looper.myLooper() == Looper.getMainLooper())
                throw new IllegalStateException("calling this from your main thread can lead to deadlock");
            try {
                if (unit == null) mDone.await();
                else if (!mDone.await(timeout, unit)) { cancel(true); throw new OperationCanceledException("the request took too long"); }
            } catch (InterruptedException e) {
                cancel(true);
                throw new OperationCanceledException(e);
            }
            Exception e = mError;
            if (e == null) return mValue;
            if (e instanceof OperationCanceledException) throw (OperationCanceledException) e;
            if (e instanceof IOException) throw (IOException) e;
            if (e instanceof AuthenticatorException) throw (AuthenticatorException) e;
            if (e instanceof RuntimeException) throw (RuntimeException) e;
            throw new AuthenticatorException(e);
        }
        V valueOrNull() { return mError == null ? mValue : null; }
        Exception error() { return mError; }
    }

    private static <V> Result<V> failed(Exception e, AccountManagerCallback<V> cb, Handler h) {
        Result<V> r = new Result<>(cb, h);
        r.fail(e);
        return r;
    }
    private static <V> Result<V> done(V value, AccountManagerCallback<V> cb, Handler h) {
        Result<V> r = new Result<>(cb, h);
        r.set(value);
        return r;
    }

    /** The exception Android throws for an authenticator's error code (AccountManager.convertErrorToException). */
    private static Exception error(int code, String message) {
        if (code == ERROR_CODE_NETWORK_ERROR) return new IOException(message);
        if (code == ERROR_CODE_UNSUPPORTED_OPERATION) return new UnsupportedOperationException(message);
        if (code == ERROR_CODE_INVALID_RESPONSE) return new AuthenticatorException(message);
        if (code == ERROR_CODE_BAD_ARGUMENTS) return new IllegalArgumentException(message);
        if (code == ERROR_CODE_CANCELED) return new OperationCanceledException(message);
        return new AuthenticatorException(message);
    }

    // ---- sessions with an authenticator

    /** What a session asks of the authenticator: a bundle now, or null when it answers the response later. */
    private interface Call { Bundle run(AbstractAccountAuthenticator a, AccountAuthenticatorResponse response) throws Exception; }
    /** What a session does with a final answer before the caller has it: a token kept, an account removed. */
    private interface Finish { void run(Bundle result); }

    /**
     * A request of the authenticator of `type`: its service bound (microG's as the system binds it), `call` run off the main
     * thread, and its answer handled as AccountManagerService's sessions handle one. The future is the caller's.
     */
    private Result<Bundle> session(String type, Activity activity, AccountManagerCallback<Bundle> cb, Handler h, Call call, Finish finish) {
        return session(type, activity, cb, h, call, finish, false);
    }
    /**
     * With `added`, the request is to add an account: when the authenticator's screen runs in its own app (microG's sign-in,
     * started in microG's app by the shell), its answer cannot come back to this process, and the request ends instead when
     * an account of the type that was not there appears among the accounts, which all apps share.
     */
    private Result<Bundle> session(String type, Activity activity, AccountManagerCallback<Bundle> cb, Handler h, Call call, Finish finish, boolean added) {
        final Result<Bundle> result = new Result<>(cb, h);
        final java.util.HashSet<Account> before = new java.util.HashSet<>(java.util.Arrays.asList(added ? getAccountsByType(type) : new Account[0]));
        final megler.Accounts.Authenticator auth = megler.Accounts.authenticator(type);
        if (auth == null) {
            result.fail(new IllegalArgumentException("no authenticator for the account type " + type));
            return result;
        }
        final ServiceConnection[] bound = new ServiceConnection[1];
        final IAccountAuthenticatorResponse response = new IAccountAuthenticatorResponse.Stub() {
            public void onResult(Bundle value) {
                if (value == null) { onError(ERROR_CODE_INVALID_RESPONSE, "null bundle returned"); return; }
                Intent intent = value.getParcelable(KEY_INTENT);
                if (intent != null) {
                    if (activity == null) { result.set(value); return; }
                    // The authenticator's screen (the sign-in), started for the caller; the request goes on until it answers.
                    MAIN.post(() -> {
                        try { activity.startActivity(intent); }
                        catch (Throwable t) { result.fail(new AuthenticatorException("the authenticator's screen cannot be started: " + t, t)); }
                    });
                    if (added) {
                        final Runnable[] watch = new Runnable[1];
                        watch[0] = () -> {
                            for (Account a : getAccountsByType(type)) if (!before.contains(a)) {
                                Bundle b = new Bundle();
                                b.putString(KEY_ACCOUNT_NAME, a.name);
                                b.putString(KEY_ACCOUNT_TYPE, a.type);
                                result.set(b);
                                return;
                            }
                        };
                        megler.Accounts.listen(watch[0], true);
                        result.then(() -> megler.Accounts.listen(watch[0], false));
                    }
                    return;
                }
                if (value.containsKey(KEY_ERROR_CODE)) { onError(value.getInt(KEY_ERROR_CODE), value.getString(KEY_ERROR_MESSAGE)); return; }
                try { if (finish != null) finish.run(value); } catch (Throwable t) { android.util.Log.w(TAG, "after " + type + "'s answer", t); }
                result.set(value);
            }
            public void onRequestContinued() {}
            public void onError(int code, String message) {
                android.util.Log.w(TAG, type + "'s authenticator: error " + code + ": " + message);
                result.fail(error(code, message));
            }
        };
        final AccountAuthenticatorResponse wrapped = new AccountAuthenticatorResponse(response);
        ServiceConnection conn = new ServiceConnection() {
            public void onServiceConnected(ComponentName name, IBinder binder) {
                AbstractAccountAuthenticator a = AbstractAccountAuthenticator.of(binder);
                if (a == null) { result.fail(new AuthenticatorException("the service " + name + " is not an authenticator")); return; }
                CALLS.execute(() -> {
                    try {
                        Bundle now = call.run(a, wrapped);
                        if (now != null) wrapped.onResult(now);
                    } catch (NetworkErrorException e) {
                        wrapped.onError(ERROR_CODE_NETWORK_ERROR, e.getMessage());
                    } catch (UnsupportedOperationException e) {
                        wrapped.onError(ERROR_CODE_UNSUPPORTED_OPERATION, "not supported: " + e.getMessage());
                    } catch (IllegalArgumentException e) {
                        wrapped.onError(ERROR_CODE_BAD_ARGUMENTS, "not supported: " + e.getMessage());
                    } catch (Throwable t) {
                        android.util.Log.w(TAG, type + "'s authenticator failed", t);
                        wrapped.onError(ERROR_CODE_REMOTE_EXCEPTION, t.toString());
                    }
                });
            }
            public void onServiceDisconnected(ComponentName name) { result.fail(new AuthenticatorException("the authenticator went: " + name)); }
            public void onNullBinding(ComponentName name) { result.fail(new AuthenticatorException("the authenticator " + name + " gave no binder")); }
        };
        bound[0] = conn;
        boolean ok;
        try { ok = auth.microG ? megler.MicroG.bindAsSystem(auth.intent, conn) : megler.Services.bind(mContext, auth.intent, conn); }
        catch (Throwable t) { ok = false; android.util.Log.w(TAG, "binding " + auth.intent, t); }
        if (!ok) { result.fail(new AuthenticatorException("bind failure: " + auth.intent)); return result; }
        result.then(() -> MAIN.post(() -> {
            try { if (auth.microG) megler.MicroG.unbind(bound[0]); else megler.Services.unbind(bound[0]); } catch (Throwable t) { }
        }));
        return result;
    }

    /** The options as the system passes them on: the caller's package, uid and pid added. */
    private Bundle options(Bundle given) {
        Bundle o = given == null ? new Bundle() : new Bundle(given);
        o.putString(KEY_ANDROID_PACKAGE_NAME, callerPackage());
        o.putInt(KEY_CALLER_UID, android.os.Process.myUid());
        o.putInt(KEY_CALLER_PID, android.os.Process.myPid());
        return o;
    }

    // ---- requests of authenticators

    public AccountManagerFuture<Bundle> addAccount(String accountType, String authTokenType, String[] requiredFeatures, Bundle addAccountOptions,
            Activity activity, AccountManagerCallback<Bundle> callback, Handler handler) {
        if (accountType == null) throw new IllegalArgumentException("accountType is null");
        final Bundle o = options(addAccountOptions);
        return session(accountType, activity, callback, handler, (a, r) -> a.addAccount(r, accountType, authTokenType, requiredFeatures, o), null, true);
    }
    public AccountManagerFuture<Bundle> addAccountAsUser(String accountType, String authTokenType, String[] requiredFeatures, Bundle addAccountOptions,
            Activity activity, AccountManagerCallback<Bundle> callback, Handler handler, UserHandle userHandle) {
        return addAccount(accountType, authTokenType, requiredFeatures, addAccountOptions, activity, callback, handler);
    }

    /**
     * A token: the one kept when the authenticator lets the system keep them and one is, else the authenticator's, kept in
     * its turn -- as GetAuthTokenSession does.
     */
    private AccountManagerFuture<Bundle> authToken(Account account, String authTokenType, Bundle given, Activity activity, boolean notify,
            AccountManagerCallback<Bundle> callback, Handler handler) {
        check(account);
        if (authTokenType == null) throw new IllegalArgumentException("authTokenType is null");
        megler.Accounts.Authenticator auth = megler.Accounts.authenticator(account.type);
        boolean custom = auth != null && auth.description.customTokens;
        if (!custom) {
            String kept = megler.Accounts.token(account, authTokenType);
            if (kept != null) {
                Bundle b = new Bundle();
                b.putString(KEY_AUTHTOKEN, kept);
                b.putString(KEY_ACCOUNT_NAME, account.name);
                b.putString(KEY_ACCOUNT_TYPE, account.type);
                return done(b, callback, handler);
            }
        }
        final Bundle o = options(given);
        if (notify) o.putBoolean(KEY_NOTIFY_ON_FAILURE, true);
        return session(account.type, activity, callback, handler, (a, r) -> a.getAuthToken(r, account, authTokenType, o), result -> {
            String token = result.getString(KEY_AUTHTOKEN);
            if (token != null && !custom) megler.Accounts.setToken(account, authTokenType, token);
        });
    }
    public AccountManagerFuture<Bundle> getAuthToken(Account account, String authTokenType, Bundle options, Activity activity,
            AccountManagerCallback<Bundle> callback, Handler handler) {
        return authToken(account, authTokenType, options, activity, false, callback, handler);
    }
    public AccountManagerFuture<Bundle> getAuthToken(Account account, String authTokenType, Bundle options, boolean notifyAuthFailure,
            AccountManagerCallback<Bundle> callback, Handler handler) {
        return authToken(account, authTokenType, options, null, notifyAuthFailure, callback, handler);
    }
    public AccountManagerFuture<Bundle> getAuthToken(Account account, String authTokenType, boolean notifyAuthFailure,
            AccountManagerCallback<Bundle> callback, Handler handler) {
        return authToken(account, authTokenType, null, null, notifyAuthFailure, callback, handler);
    }
    public String blockingGetAuthToken(Account account, String authTokenType, boolean notifyAuthFailure)
            throws OperationCanceledException, IOException, AuthenticatorException {
        Bundle b = getAuthToken(account, authTokenType, notifyAuthFailure, null, null).getResult();
        if (b == null) return null;
        return b.getString(KEY_AUTHTOKEN);
    }

    public AccountManagerFuture<Bundle> confirmCredentials(Account account, Bundle options, Activity activity, AccountManagerCallback<Bundle> callback,
            Handler handler) {
        check(account);
        final Bundle o = options(options);
        return session(account.type, activity, callback, handler, (a, r) -> a.confirmCredentials(r, account, o), null);
    }
    public AccountManagerFuture<Bundle> confirmCredentialsAsUser(Account account, Bundle options, Activity activity, AccountManagerCallback<Bundle> callback,
            Handler handler, UserHandle userHandle) {
        return confirmCredentials(account, options, activity, callback, handler);
    }
    public AccountManagerFuture<Bundle> updateCredentials(Account account, String authTokenType, Bundle options, Activity activity,
            AccountManagerCallback<Bundle> callback, Handler handler) {
        check(account);
        final Bundle o = options(options);
        return session(account.type, activity, callback, handler, (a, r) -> a.updateCredentials(r, account, authTokenType, o), null);
    }
    public AccountManagerFuture<Bundle> editProperties(String accountType, Activity activity, AccountManagerCallback<Bundle> callback, Handler handler) {
        if (accountType == null) throw new IllegalArgumentException("accountType is null");
        return session(accountType, activity, callback, handler, (a, r) -> a.editProperties(r, accountType), null);
    }

    /** A request whose answer is one value of the authenticator's bundle (or of the request), as a Future2Task's. */
    private interface Pick<V> { V from(Bundle b) throws Exception; }
    private <V> Result<V> map(Result<Bundle> inner, AccountManagerCallback<V> callback, Handler handler, Pick<V> pick) {
        Result<V> outer = new Result<>(callback, handler);
        inner.then(() -> {
            if (inner.error() != null) { outer.fail(inner.error()); return; }
            try { outer.set(pick.from(inner.valueOrNull())); } catch (Exception e) { outer.fail(e); }
        });
        return outer;
    }

    public AccountManagerFuture<Boolean> hasFeatures(Account account, String[] features, AccountManagerCallback<Boolean> callback, Handler handler) {
        check(account);
        if (features == null) throw new IllegalArgumentException("features is null");
        Result<Bundle> inner = session(account.type, null, null, null, (a, r) -> a.hasFeatures(r, account, features), null);
        return map(inner, callback, handler, b -> b.getBoolean(KEY_BOOLEAN_RESULT));
    }
    public AccountManagerFuture<String> getAuthTokenLabel(String accountType, String authTokenType, AccountManagerCallback<String> callback, Handler handler) {
        if (accountType == null) throw new IllegalArgumentException("accountType is null");
        if (authTokenType == null) throw new IllegalArgumentException("authTokenType is null");
        Result<Bundle> inner = session(accountType, null, null, null, (a, r) -> {
            Bundle b = new Bundle();
            b.putString(KEY_AUTH_TOKEN_LABEL, a.getAuthTokenLabel(authTokenType));
            return b;
        }, null);
        return map(inner, callback, handler, b -> b.getString(KEY_AUTH_TOKEN_LABEL));
    }
    public AccountManagerFuture<Boolean> isCredentialsUpdateSuggested(Account account, String statusToken, AccountManagerCallback<Boolean> callback,
            Handler handler) {
        check(account);
        Result<Bundle> inner = session(account.type, null, null, null, (a, r) -> a.isCredentialsUpdateSuggested(r, account, statusToken), null);
        return map(inner, callback, handler, b -> b.getBoolean(KEY_BOOLEAN_RESULT));
    }

    /** The accounts of the type that have every feature, the authenticator asked of each. */
    public AccountManagerFuture<Account[]> getAccountsByTypeAndFeatures(String type, String[] features, AccountManagerCallback<Account[]> callback,
            Handler handler) {
        if (type == null) throw new IllegalArgumentException("type is null");
        final Account[] all = getAccountsByType(type);
        if (features == null || features.length == 0 || all.length == 0) return done(all, callback, handler);
        final Result<Account[]> out = new Result<>(callback, handler);
        final ArrayList<Account> have = new ArrayList<>();
        final int[] left = { all.length };
        for (Account a : all) {
            Result<Bundle> one = session(type, null, null, null, (auth, r) -> auth.hasFeatures(r, a, features), null);
            one.then(() -> {
                Bundle b = one.valueOrNull();
                synchronized (have) {
                    if (b != null && b.getBoolean(KEY_BOOLEAN_RESULT)) have.add(a);
                    if (--left[0] == 0) out.set(have.toArray(new Account[0]));
                }
            });
        }
        return out;
    }

    /**
     * An account of the type with the features, and a token for it (getAuthTokenByFeatures): the one there is, the first of
     * several (Android asks the user to choose; there is no chooser here), or one added first when there is none.
     */
    public AccountManagerFuture<Bundle> getAuthTokenByFeatures(String accountType, String authTokenType, String[] features, Activity activity,
            Bundle addAccountOptions, Bundle getAuthTokenOptions, AccountManagerCallback<Bundle> callback, Handler handler) {
        if (accountType == null) throw new IllegalArgumentException("account type is null");
        if (authTokenType == null) throw new IllegalArgumentException("authTokenType is null");
        final Result<Bundle> out = new Result<>(callback, handler);
        final Result<Account[]> found = (Result<Account[]>) getAccountsByTypeAndFeatures(accountType, features, null, null);
        found.then(() -> {
            Account[] accounts = found.valueOrNull();
            Result<Bundle> next;
            if (accounts != null && accounts.length > 0) {
                next = (Result<Bundle>) getAuthToken(accounts[0], authTokenType, getAuthTokenOptions, activity, null, null);
            } else {
                Result<Bundle> added = (Result<Bundle>) addAccount(accountType, authTokenType, features, addAccountOptions, activity, null, null);
                added.then(() -> {
                    Bundle b = added.valueOrNull();
                    if (b == null) { out.fail(added.error()); return; }
                    String name = b.getString(KEY_ACCOUNT_NAME), type = b.getString(KEY_ACCOUNT_TYPE);
                    if (name == null || type == null) { out.set(b); return; }
                    Result<Bundle> token = (Result<Bundle>) getAuthToken(new Account(name, type), authTokenType, getAuthTokenOptions, activity, null, null);
                    token.then(() -> { if (token.error() != null) out.fail(token.error()); else out.set(token.valueOrNull()); });
                });
                return;
            }
            next.then(() -> { if (next.error() != null) out.fail(next.error()); else out.set(next.valueOrNull()); });
        });
        return out;
    }

    /** removeAccount: the authenticator says whether it may go (getAccountRemovalAllowed), and then it goes. */
    private Result<Bundle> removal(Account account, Activity activity, AccountManagerCallback<Bundle> callback, Handler handler) {
        check(account);
        if (megler.Accounts.authenticator(account.type) == null) {
            Bundle b = new Bundle();
            b.putBoolean(KEY_BOOLEAN_RESULT, megler.Accounts.remove(account));
            return done(b, callback, handler);
        }
        return session(account.type, activity, callback, handler, (a, r) -> a.getAccountRemovalAllowed(r, account), result -> {
            if (result.getBoolean(KEY_BOOLEAN_RESULT, false)) result.putBoolean(KEY_BOOLEAN_RESULT, megler.Accounts.remove(account));
        });
    }
    public AccountManagerFuture<Bundle> removeAccount(Account account, Activity activity, AccountManagerCallback<Bundle> callback, Handler handler) {
        return removal(account, activity, callback, handler);
    }
    public AccountManagerFuture<Boolean> removeAccount(Account account, AccountManagerCallback<Boolean> callback, Handler handler) {
        return map(removal(account, null, null, null), callback, handler, b -> b.getBoolean(KEY_BOOLEAN_RESULT));
    }
    public AccountManagerFuture<Bundle> removeAccountAsUser(Account account, Activity activity, AccountManagerCallback<Bundle> callback, Handler handler,
            UserHandle userHandle) {
        return removeAccount(account, activity, callback, handler);
    }
    public AccountManagerFuture<Boolean> removeAccountAsUser(Account account, AccountManagerCallback<Boolean> callback, Handler handler,
            UserHandle userHandle) {
        return removeAccount(account, callback, handler);
    }

    public AccountManagerFuture<Account> renameAccount(Account account, String newName, AccountManagerCallback<Account> callback, Handler handler) {
        check(account);
        if (newName == null || newName.isEmpty()) throw new IllegalArgumentException("the new name is empty");
        Account renamed = megler.Accounts.rename(account, newName);
        return renamed != null ? done(renamed, callback, handler) : failed(new AuthenticatorException("no such account, or the name is taken"), callback, handler);
    }

    // ---- the session API (Android 8), which no authenticator here is asked for

    private static <V> AccountManagerFuture<V> unsupported(AccountManagerCallback<V> callback, Handler handler) {
        return failed(new UnsupportedOperationException("account sessions are not supported"), callback, handler);
    }
    public AccountManagerFuture<Bundle> startAddAccountSession(String accountType, String authTokenType, String[] requiredFeatures, Bundle options,
            Activity activity, AccountManagerCallback<Bundle> callback, Handler handler) { return unsupported(callback, handler); }
    public AccountManagerFuture<Bundle> startUpdateCredentialsSession(Account account, String authTokenType, Bundle options, Activity activity,
            AccountManagerCallback<Bundle> callback, Handler handler) { return unsupported(callback, handler); }
    public AccountManagerFuture<Bundle> finishSession(Bundle sessionBundle, Activity activity, AccountManagerCallback<Bundle> callback, Handler handler) {
        return unsupported(callback, handler);
    }
    public AccountManagerFuture<Bundle> finishSessionAsUser(Bundle sessionBundle, Activity activity, UserHandle userHandle,
            AccountManagerCallback<Bundle> callback, Handler handler) { return unsupported(callback, handler); }
    public AccountManagerFuture<Boolean> copyAccountToUser(Account account, UserHandle fromUser, UserHandle toUser, Handler handler,
            AccountManagerCallback<Boolean> callback) { return done(Boolean.FALSE, callback, handler); }
    public void addSharedAccountsFromParentUser(UserHandle parentUser, UserHandle user) {}
    public android.content.IntentSender createRequestAccountAccessIntentSenderAsUser(Account account, String packageName, UserHandle userHandle) { return null; }

    public static void invalidateLocalAccountUserDataCaches() {}
    public static void invalidateLocalAccountsDataCaches() {}
    public void disableLocalAccountCaches() {}
    public void disableLocalUserInfoCaches() {}

    /** The system account chooser's intent, as Android builds it (Megler has none: started for a result, it comes back cancelled). */
    public static Intent newChooseAccountIntent(Account selectedAccount, ArrayList<Account> allowableAccounts, String[] allowableAccountTypes,
            boolean alwaysPromptForAccount, String descriptionOverrideText, String addAccountAuthTokenType, String[] addAccountRequiredFeatures,
            Bundle addAccountOptions) {
        return newChooseAccountIntent(selectedAccount, (List<Account>) allowableAccounts, allowableAccountTypes, descriptionOverrideText,
            addAccountAuthTokenType, addAccountRequiredFeatures, addAccountOptions);
    }
    public static Intent newChooseAccountIntent(Account selectedAccount, List<Account> allowableAccounts, String[] allowableAccountTypes,
            String descriptionOverrideText, String addAccountAuthTokenType, String[] addAccountRequiredFeatures, Bundle addAccountOptions) {
        Intent i = new Intent();
        i.setClassName("android", "android.accounts.ChooseTypeAndAccountActivity");
        if (allowableAccounts != null) i.putParcelableArrayListExtra("allowableAccounts", new ArrayList<android.os.Parcelable>(allowableAccounts));
        i.putExtra("allowableAccountTypes", allowableAccountTypes);
        i.putExtra("addAccountOptions", addAccountOptions);
        i.putExtra("selectedAccount", selectedAccount);
        i.putExtra("descriptionTextOverride", descriptionOverrideText);
        i.putExtra("authTokenType", addAccountAuthTokenType);
        i.putExtra("addAccountRequiredFeatures", addAccountRequiredFeatures);
        return i;
    }
    public static Bundle sanitizeResult(Bundle result) {
        if (result != null && result.containsKey(KEY_AUTHTOKEN) && !android.text.TextUtils.isEmpty(result.getString(KEY_AUTHTOKEN))) {
            Bundle newResult = new Bundle(result);
            newResult.putString(KEY_AUTHTOKEN, "<omitted for logging purposes>");
            return newResult;
        }
        return result;
    }
}
