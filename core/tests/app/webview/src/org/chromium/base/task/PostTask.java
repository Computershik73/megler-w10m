package org.chromium.base.task;

/**
 * An app's own class of a name the WebView's Chromium has too, as an app that carries Cronet or Chromium's base library
 * does (microG). The WebView's classes must stay the WebView's: with this one in their place, the WebView's provider failed
 * with NoSuchMethodError: org/chromium/base/task/PostTask.a(I) and no WebView could be made.
 */
public final class PostTask {
    public static final String OWNER = "megler.test.webview";
    private PostTask() {}
}
