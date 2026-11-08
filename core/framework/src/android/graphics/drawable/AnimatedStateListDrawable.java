package android.graphics.drawable;

/** A StateListDrawable whose state changes would animate; Megler switches straight to the new state's drawable. */
public class AnimatedStateListDrawable extends StateListDrawable {
    public AnimatedStateListDrawable() { super(); }
    /* A copy (DrawableContainer's constant state) is one of these, its states StateListDrawable's. */
    @Override DrawableContainer meglerNewEmpty() { return new AnimatedStateListDrawable(); }
    public void addState(int[] stateSet, Drawable d, int id) { addState(stateSet, d); }
    public <T extends Drawable & Animatable> void addTransition(int fromId, int toId, T transition, boolean reversible) {}
}
