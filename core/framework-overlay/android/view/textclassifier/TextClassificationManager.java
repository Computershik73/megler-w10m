// The generated stub (core/framework/gen-src, tools/compat/genstubs.py) made to answer as Android's does, with a classifier
// that does nothing where Android has its system's: never null. Chromium's WebView logs a text selection to the session
// createTextClassificationSession makes, and the stub's null ended the app at the first selection in a WebView (a long press,
// a double click, a paste into Google's sign-in page) on a NullPointerException in TextClassifier.onSelectionEvent.
package android.view.textclassifier;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class TextClassificationManager {
    private TextClassifier custom;
    private TextClassificationSessionFactory sessions;

    public TextClassificationManager(android.content.Context p0) {}
    TextClassificationManager() { this((android.content.Context) null); }

    public static android.view.textclassifier.TextClassificationConstants getSettings(android.content.Context p0) { return null; }

    /** The app's own classifier where it set one, else Android's NO_OP (no system classifier here). */
    public android.view.textclassifier.TextClassifier getTextClassifier() { return custom != null ? custom : TextClassifier.NO_OP; }
    public android.view.textclassifier.TextClassifier getTextClassifier(int type) { return TextClassifier.NO_OP; }
    public android.view.textclassifier.TextClassifier getClassifier(int type) { return TextClassifier.NO_OP; }
    public void setTextClassifier(android.view.textclassifier.TextClassifier classifier) { custom = classifier; }

    /** The app's factory's session where it set one, else the classifier itself (Android wraps it in a session that logs). */
    public android.view.textclassifier.TextClassifier createTextClassificationSession(android.view.textclassifier.TextClassificationContext context) {
        final TextClassifier made = sessions != null ? sessions.createTextClassificationSession(context) : null;
        return made != null ? made : getTextClassifier();
    }
    public android.view.textclassifier.TextClassifier createTextClassificationSession(android.view.textclassifier.TextClassificationContext context,
                                                                                      android.view.textclassifier.TextClassifier classifier) {
        return classifier != null ? classifier : TextClassifier.NO_OP;
    }
    public void setTextClassificationSessionFactory(android.view.textclassifier.TextClassificationSessionFactory factory) { sessions = factory; }
}
