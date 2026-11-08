package android.graphics.drawable;

public class StateListDrawable extends DrawableContainer {
    private final java.util.ArrayList<int[]> mSets = new java.util.ArrayList<>();
    public StateListDrawable() {}
    public void addState(int[] set, Drawable d) { mSets.add(set); addChild(d); onStateChange(getState()); }
    public int getStateCount() { return mSets.size(); }
    public int[] getStateSet(int i) { return mSets.get(i); }
    public Drawable getStateDrawable(int i) { return mChildren.get(i); }
    public int findStateDrawableIndex(int[] s) { for (int i = 0; i < mSets.size(); i++) if (android.util.StateSet.stateSetMatches(mSets.get(i), s)) return i; return -1; }
    /* A copy (DrawableContainer's constant state): a StateListDrawable with the same state sets, as Android's StateListState makes. */
    @Override DrawableContainer meglerNewEmpty() { return new StateListDrawable(); }
    @Override void meglerAddCopy(DrawableContainer to, int i, Drawable child) { ((StateListDrawable) to).addState(mSets.get(i), child); }
    @Override protected boolean onStateChange(int[] s) {
        int i = findStateDrawableIndex(s);
        boolean ch = selectDrawable(i);
        return super.onStateChange(s) || ch;
    }
}
