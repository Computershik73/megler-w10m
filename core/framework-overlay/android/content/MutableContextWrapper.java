// Overlay of core/framework/gen-src/android/content/MutableContextWrapper.java (tools/build-framework.sh).
// It differs in setBaseContext: the generated one kept the context as a property and the wrapper went on wrapping its old
// one. Android's makes it the wrapper's base. WhatsApp inflates a chat's views ahead of time in a PrewarmContextWrapper (a
// MutableContextWrapper) over the application and, when a chat opens, sets the Conversation activity as its base; the list of
// messages then asks its context for its activity, which the old base never led to: "The Context is not an Activity", and
// the app ended as a chat with an unread message opened.
package android.content;

public class MutableContextWrapper extends ContextWrapper {
    public MutableContextWrapper(Context base) { super(base); }
    MutableContextWrapper() { this((Context) null); }
    /** Change the base context for this ContextWrapper. All calls will then be delegated to the base context. */
    public void setBaseContext(Context base) { mBase = base; }
}
