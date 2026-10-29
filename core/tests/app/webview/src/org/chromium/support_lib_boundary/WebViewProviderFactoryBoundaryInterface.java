package org.chromium.support_lib_boundary;

/**
 * The boundary interface androidx.webkit carries in an app, its copy of the WebView's: through a method of it the app asks
 * the WebView's support-library glue, which answers with its own copy's (the glue's invocation handler finds the method of
 * that name in the WebView's class loader). Only the method the test asks for.
 */
public interface WebViewProviderFactoryBoundaryInterface {
    String[] getSupportedFeatures();
}
