package android.view;

/**
 * The root of a window's view tree, by the name Android gives it. Here that root is megler.ViewRoot, and this class is
 * the name alone, for apps that look it up: TikTok Lite's MainLooperOptService takes Class.forName of it among other
 * framework classes and, when one is missing, leaves its state unmade, which its MainActivity.onResume then met as a
 * NullPointerException. Nothing here makes one.
 */
public final class ViewRootImpl {
    private ViewRootImpl() {}
}
