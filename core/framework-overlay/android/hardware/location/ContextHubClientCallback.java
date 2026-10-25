package android.hardware.location;

/**
 * The callback of a client of a context hub (a low-power sensor processor), as Android's is subclassed. A Megler phone has no
 * context hub: Context.getSystemService gives no ContextHubManager, so no client is made and nothing calls one. Android's
 * class has callbacks for a hub's messages, all of which do nothing; they are left out, as no hub sends any. GmsCore's
 * location service makes one when it starts.
 */
public class ContextHubClientCallback {
    public ContextHubClientCallback() {}
}
