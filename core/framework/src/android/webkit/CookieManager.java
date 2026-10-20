package android.webkit;

import java.util.LinkedHashMap;
import java.util.Map;

/** Cookies for the app's WebViews: kept per host, name by name. */
public abstract class CookieManager {
    private static CookieManager sInstance;
    private static boolean sFileSchemeCookies;
    public CookieManager() {}
    public static synchronized CookieManager getInstance() { if (sInstance == null) sInstance = new Megler(); return sInstance; }
    public static boolean allowFileSchemeCookies() { return sFileSchemeCookies; }
    public static void setAcceptFileSchemeCookies(boolean accept) { sFileSchemeCookies = accept; }
    @Override protected Object clone() throws CloneNotSupportedException { throw new CloneNotSupportedException("doesn't implement Cloneable"); }
    public abstract void setAcceptCookie(boolean accept);
    public abstract boolean acceptCookie();
    public abstract void setAcceptThirdPartyCookies(WebView webview, boolean accept);
    public abstract boolean acceptThirdPartyCookies(WebView webview);
    public abstract void setCookie(String url, String value);
    public abstract void setCookie(String url, String value, ValueCallback<Boolean> callback);
    public abstract String getCookie(String url);
    public String getCookie(String url, boolean privateBrowsing) { return getCookie(url); }
    public synchronized String getCookie(android.net.WebAddress uri) { return getCookie(uri.toString()); }
    @Deprecated public abstract void removeSessionCookie();
    public abstract void removeSessionCookies(ValueCallback<Boolean> callback);
    @Deprecated public abstract void removeAllCookie();
    public abstract void removeAllCookies(ValueCallback<Boolean> callback);
    public abstract boolean hasCookies();
    public boolean hasCookies(boolean privateBrowsing) { return hasCookies(); }
    @Deprecated public abstract void removeExpiredCookie();
    public abstract void flush();
    protected boolean allowFileSchemeCookiesImpl() { return sFileSchemeCookies; }
    protected void setAcceptFileSchemeCookiesImpl(boolean accept) { sFileSchemeCookies = accept; }

    /** The app's WebKit data store's cookies (megler.Web), the ones its pages send and see. */
    static final class Megler extends CookieManager {
        private boolean mAccept = true;
        Megler() { megler.Web.ensureApp(); }
        public synchronized void setAcceptCookie(boolean accept) { mAccept = accept; }
        public synchronized boolean acceptCookie() { return mAccept; }
        public void setAcceptThirdPartyCookies(WebView w, boolean accept) {}
        public boolean acceptThirdPartyCookies(WebView w) { return false; }
        public void setCookie(String url, String value) { setCookie(url, value, null); }
        public void setCookie(String url, String value, ValueCallback<Boolean> cb) {
            boolean ok = url != null && value != null && megler.Web.cookieSet(url, value);
            if (cb != null) cb.onReceiveValue(ok);
        }
        public String getCookie(String url) { return url == null ? null : megler.Web.cookieGet(url); }
        public void removeSessionCookie() { megler.Web.cookieClear(true); }
        public void removeSessionCookies(ValueCallback<Boolean> cb) { boolean had = megler.Web.cookieClear(true); if (cb != null) cb.onReceiveValue(had); }
        public void removeAllCookie() { megler.Web.cookieClear(false); }
        public void removeAllCookies(ValueCallback<Boolean> cb) { boolean had = megler.Web.cookieClear(false); if (cb != null) cb.onReceiveValue(had); }
        public boolean hasCookies() { return megler.Web.cookieAny(); }
        public void removeExpiredCookie() {}
        public void flush() {}
    }
}
