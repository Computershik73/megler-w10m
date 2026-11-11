// Megler's android.test.base (tools/build-shared-libraries.sh): the interface Android's has, written here.
package com.android.internal.util;

/** A test of an object: true where it holds. (Android's own code used it before java.util.function.) */
@Deprecated
public interface Predicate<T> {
    boolean apply(T t);
}
