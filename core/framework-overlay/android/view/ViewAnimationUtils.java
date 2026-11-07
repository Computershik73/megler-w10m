// Overlay of core/framework/gen-src/android/view/ViewAnimationUtils.java (tools/build-framework.sh).
// It differs in createCircularReveal, which gives an animator of the circle's radius, from the start radius to the end
// one, that clips the view to that circle while it runs (View.setRevealClip) and lets the clip go at its end, as Android's
// RevealAnimator does. The generated stub returned null: Jamfish (package 570) plays a reveal with another animator in an
// AnimatorSet as each song begins, and stopped there with a NullPointerException in AnimatorSet.startChild.
package android.view;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public final class ViewAnimationUtils {
    public static android.animation.Animator createCircularReveal(final View view, final int centerX, final int centerY, float startRadius, float endRadius) {
        android.animation.ValueAnimator a = android.animation.ValueAnimator.ofFloat(startRadius, endRadius);
        a.addUpdateListener(u -> view.setRevealClip(true, centerX, centerY, (Float) u.getAnimatedValue()));
        a.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(android.animation.Animator e) { view.setRevealClip(false, 0, 0, 0); }
        });
        return a;
    }
    protected ViewAnimationUtils() {}
}
