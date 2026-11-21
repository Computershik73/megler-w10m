// Megler: Android's FloatMath, which the stub (gen-src) answered 0 for: cocos2d-x 2.x sizes its label bitmaps with ceil.
/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.util;

/**
 * Math routines similar to those found in {@link java.lang.Math}. Gone from the SDK since API 23, but still in Android 9's
 * framework, where older apps call it: cocos2d-x 2.x's Cocos2dxBitmap.computeTextProperty sizes every label's bitmap with
 * ceil. The generated stub (gen-src) answered 0 for all of these, and Metal Slug 3's buttons drew no text. As Android's
 * own: each the float of java.lang.Math's double result.
 */
public class FloatMath {

    /** Prevents instantiation. */
    private FloatMath() {}

    public static float floor(float value) { return (float) Math.floor(value); }

    public static float ceil(float value) { return (float) Math.ceil(value); }

    public static float sin(float angle) { return (float) Math.sin(angle); }

    public static float cos(float angle) { return (float) Math.cos(angle); }

    public static float sqrt(float value) { return (float) Math.sqrt(value); }

    public static float exp(float value) { return (float) Math.exp(value); }

    public static float pow(float x, float y) { return (float) Math.pow(x, y); }

    public static float hypot(float x, float y) { return (float) Math.hypot(x, y); }
}
