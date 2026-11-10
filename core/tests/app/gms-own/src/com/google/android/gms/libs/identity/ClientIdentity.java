package com.google.android.gms.libs.identity;

import android.content.Context;

/**
 * As Google Play services' own (GmsCore 26.41, ClientIdentity.c(Context)): the identity of a location request of their own is
 * made of their context's attribution tag, which their location service refuses where it is null (gerz.h, "internal location
 * usage has a forbidden attribution tag"). Megler's package context of theirs knows the class by its name (megler.MicroG):
 * the tag asked for here is the one the user is asked about.
 */
public final class ClientIdentity {
    private ClientIdentity() {}

    /** The context's attribution tag (Android 11's Context.getAttributionTag, which this SDK's android.jar has not). */
    public static String c(Context context) {
        try {
            return (String) Context.class.getMethod("getAttributionTag").invoke(context);
        } catch (Exception e) {
            return "FAILED " + e;
        }
    }
}
