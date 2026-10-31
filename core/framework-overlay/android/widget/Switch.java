// Overlay of core/framework/src/android/widget/Switch.java (tools/build-framework.sh).
// It differs in how wide a switch with a thumb and a track drawable is, and where the two are then drawn: onMeasure and the
// first branch of onDraw, which are Android's sums now.
//
// Why. Android's Switch is as wide as two thumbs and the track's padding (Switch.onMeasure: "switchWidth = Math.max(
// mSwitchMinWidth, 2 * mThumbWidth + paddingLeft + paddingRight)", the thumb's width being its drawable's less that
// drawable's own padding), and as high as the taller of the two. The original took the track drawable's intrinsic width.
// Android's track is a nine-patch a few pixels wide that is meant to be stretched (switch_track_material: 13 pixels at 280
// dpi), so a switch measured 13 pixels wide, its parent gave it that, and the thumb, drawn at its own size from the left of
// those 13 pixels, hung out over the edge: in a preference row, whose switch is the last thing in it, off the edge of the
// screen. Apktool M's preference screens showed a sliver of each switch. Everything else is the original's, unchanged.
package android.widget;

import android.content.Context;
import android.content.res.ColorStateList;
import android.content.res.TypedArray;
import android.graphics.*;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.view.MotionEvent;

/** A switch: its track and thumb drawn (the theme's drawables when it has them, else a plain Material one), dragged or tapped. */
public class Switch extends CompoundButton {
    private Drawable mThumb, mTrack;
    private CharSequence mTextOn, mTextOff;
    private int mSwitchMinWidth, mSwitchWidth, mSwitchHeight;
    private float mPos;
    private boolean mShowText;
    private final Paint mPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private ColorStateList mThumbTint, mTrackTint;
    public Switch(Context c) { this(c, null); }
    public Switch(Context c, AttributeSet a) { this(c, a, android.R.attr.switchStyle); }
    public Switch(Context c, AttributeSet a, int s) { this(c, a, s, 0); }
    public Switch(Context c, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
        super(c, attrs, defStyleAttr, defStyleRes);
        TypedArray a = c.obtainStyledAttributes(attrs, megler.S.Switch, defStyleAttr, defStyleRes);
        mThumb = a.getDrawable(megler.S.Switch_thumb); mTrack = a.getDrawable(megler.S.Switch_track);
        mTextOn = a.getText(megler.S.Switch_textOn); mTextOff = a.getText(megler.S.Switch_textOff);
        mShowText = a.getBoolean(megler.S.Switch_showText, false);
        mSwitchMinWidth = a.getDimensionPixelSize(megler.S.Switch_switchMinWidth, 0);
        mThumbTint = a.getColorStateList(megler.S.Switch_thumbTint); mTrackTint = a.getColorStateList(megler.S.Switch_trackTint);
        a.recycle();
        if (mThumb != null) mThumb.setCallback(this);
        if (mTrack != null) mTrack.setCallback(this);
        mPos = isChecked() ? 1 : 0;
    }
    public void setThumbDrawable(Drawable d) { mThumb = d; requestLayout(); } public Drawable getThumbDrawable() { return mThumb; }
    public void setThumbResource(int r) { setThumbDrawable(getContext().getDrawable(r)); }
    public void setTrackDrawable(Drawable d) { mTrack = d; requestLayout(); } public Drawable getTrackDrawable() { return mTrack; }
    public void setTrackResource(int r) { setTrackDrawable(getContext().getDrawable(r)); }
    public void setThumbTintList(ColorStateList t) { mThumbTint = t; invalidate(); } public void setTrackTintList(ColorStateList t) { mTrackTint = t; invalidate(); }
    public void setTextOn(CharSequence t) { mTextOn = t; } public void setTextOff(CharSequence t) { mTextOff = t; } public CharSequence getTextOn() { return mTextOn; } public CharSequence getTextOff() { return mTextOff; }
    public void setShowText(boolean s) { mShowText = s; } public boolean getShowText() { return mShowText; }
    public void setSwitchMinWidth(int w) { mSwitchMinWidth = w; requestLayout(); } public int getSwitchMinWidth() { return mSwitchMinWidth; }
    public void setSwitchPadding(int p) {} public int getSwitchPadding() { return 0; } public void setThumbTextPadding(int p) {} public void setSplitTrack(boolean s) {}
    public void setSwitchTextAppearance(Context c, int r) {} public void setSwitchTypeface(Typeface t) {}
    private float dp(float v) { return v * getResources().getDisplayMetrics().density; }
    /** The thumb as Android measures it: its drawable's width without that drawable's padding. */
    private int mThumbWidth;
    private final android.graphics.Rect mPad = new android.graphics.Rect();
    @Override protected void onMeasure(int ws, int hs) {
        if (mThumb != null && mTrack != null) {
            /* Android's Switch.onMeasure: two thumbs and the track's padding wide, the taller of the two high. */
            mThumb.getPadding(mPad);
            mThumbWidth = Math.max(0, mThumb.getIntrinsicWidth() - mPad.left - mPad.right);
            mTrack.getPadding(mPad);
            mSwitchWidth = Math.max(mSwitchMinWidth, 2 * mThumbWidth + mPad.left + mPad.right);
            mSwitchHeight = Math.max(mTrack.getIntrinsicHeight(), mThumb.getIntrinsicHeight());
        } else {
            /* The switch this class draws by itself when the theme gives it no drawables. */
            mSwitchWidth = Math.max(mSwitchMinWidth, mTrack != null && mTrack.getIntrinsicWidth() > 0 ? mTrack.getIntrinsicWidth() : (int) dp(52));
            mSwitchHeight = Math.max(mThumb != null && mThumb.getIntrinsicHeight() > 0 ? mThumb.getIntrinsicHeight() : 0, (int) dp(32));
        }
        super.onMeasure(ws, hs);
        if (getMeasuredHeight() < mSwitchHeight) setMeasuredDimension(getMeasuredWidthAndState(), mSwitchHeight);
    }
    @Override public int getCompoundPaddingRight() { return super.getCompoundPaddingRight() + mSwitchWidth + (getText() != null && getText().length() > 0 ? (int) dp(16) : 0); }
    @Override public void setChecked(boolean c) { super.setChecked(c); mPos = c ? 1 : 0; invalidate(); }
    private boolean mDragging; private float mDownX;
    @Override public boolean onTouchEvent(MotionEvent e) {
        int right = getWidth() - getPaddingRight(), left = right - mSwitchWidth;
        switch (e.getActionMasked()) {
        case MotionEvent.ACTION_DOWN: mDownX = e.getX(); mDragging = false; break;
        case MotionEvent.ACTION_MOVE: if (Math.abs(e.getX() - mDownX) > dp(8)) { mDragging = true; mPos = Math.max(0, Math.min(1, (e.getX() - left) / (float) mSwitchWidth)); getParent().requestDisallowInterceptTouchEvent(true); invalidate(); return true; } break;
        case MotionEvent.ACTION_UP: if (mDragging) { mDragging = false; boolean on = mPos > 0.5f; setPressed(false); if (on != isChecked()) setChecked(on); else { mPos = on ? 1 : 0; invalidate(); } return true; } break;
        }
        return super.onTouchEvent(e);
    }
    @Override protected void onDraw(Canvas c) {
        super.onDraw(c);
        int right = getWidth() - getPaddingRight(), left = right - mSwitchWidth, cy = getHeight() / 2;
        int[] state = getDrawableState();
        if (mTrack != null && mThumb != null) {
            /* Android's Switch.onLayout and draw: the track over the whole switch; the thumb inside the track's padding, moved over what is left of it, its own padding outside its place. */
            int top = cy - mSwitchHeight / 2, bottom = top + mSwitchHeight;
            mTrack.setBounds(left, top, right, bottom);
            if (mTrackTint != null) mTrack.setTintList(mTrackTint);
            mTrack.setState(state); mTrack.draw(c);
            mTrack.getPadding(mPad);
            int range = Math.max(0, mSwitchWidth - mThumbWidth - mPad.left - mPad.right), tx = left + mPad.left + (int) (range * mPos + 0.5f);
            mThumb.getPadding(mPad);
            mThumb.setBounds(tx - mPad.left, top, tx + mThumbWidth + mPad.right, bottom);
            if (mThumbTint != null) mThumb.setTintList(mThumbTint);
            mThumb.setState(state); mThumb.draw(c);
            return;
        }
        // Material 3 switch, drawn
        int accent = 0xFF6750A4;
        android.util.TypedValue v = new android.util.TypedValue();
        if (getContext().getTheme().resolveAttribute(android.R.attr.colorAccent, v, true) && v.type >= android.util.TypedValue.TYPE_FIRST_INT) accent = v.data;
        float h = dp(32), w = mSwitchWidth, r = h / 2;
        RectF track = new RectF(left, cy - h / 2, left + w, cy + h / 2);
        mPaint.setStyle(Paint.Style.FILL);
        mPaint.setColor(isChecked() ? accent : 0xFFE6E0E9);
        c.drawRoundRect(track, r, r, mPaint);
        if (!isChecked()) { mPaint.setStyle(Paint.Style.STROKE); mPaint.setStrokeWidth(dp(2)); mPaint.setColor(0xFF79747E); c.drawRoundRect(new RectF(track.left + dp(1), track.top + dp(1), track.right - dp(1), track.bottom - dp(1)), r, r, mPaint); mPaint.setStyle(Paint.Style.FILL); }
        float tr = isChecked() ? dp(12) : dp(8);
        float cx = left + r + (w - 2 * r) * mPos;
        mPaint.setColor(isChecked() ? 0xFFFFFFFF : 0xFF79747E);
        c.drawCircle(cx, cy, tr, mPaint);
    }
    @Override public CharSequence getAccessibilityClassName() { return Switch.class.getName(); }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    private java.util.HashMap<String, Object> meglerFill; /* made at the first set (megler.Fill) */
    protected java.lang.CharSequence getButtonStateDescription() { return null; }
    public boolean getSplitTrack() { return false; }
    public int getThumbTextPadding() { return 0; }
    public android.graphics.BlendMode getThumbTintBlendMode() { return (android.graphics.BlendMode) megler.Fill.get(meglerFill, "ThumbTintBlendMode"); }
    public android.content.res.ColorStateList getThumbTintList() { return null; }
    public android.graphics.PorterDuff.Mode getThumbTintMode() { return (android.graphics.PorterDuff.Mode) megler.Fill.get(meglerFill, "ThumbTintMode"); }
    public android.graphics.BlendMode getTrackTintBlendMode() { return (android.graphics.BlendMode) megler.Fill.get(meglerFill, "TrackTintBlendMode"); }
    public android.content.res.ColorStateList getTrackTintList() { return null; }
    public android.graphics.PorterDuff.Mode getTrackTintMode() { return (android.graphics.PorterDuff.Mode) megler.Fill.get(meglerFill, "TrackTintMode"); }
    public void onPopulateAccessibilityEventInternal(android.view.accessibility.AccessibilityEvent p0) {}
    protected void onProvideStructure(android.view.ViewStructure p0, int p1, int p2) {}
    public void setSwitchTypeface(android.graphics.Typeface p0, int p1) {}
    public void setThumbIcon(android.graphics.drawable.Icon p0) {}
    public java.lang.Runnable setThumbIconAsync(android.graphics.drawable.Icon p0) { return null; }
    public java.lang.Runnable setThumbResourceAsync(int p0) { return null; }
    public void setThumbTintBlendMode(android.graphics.BlendMode p0) { meglerFill = megler.Fill.put(meglerFill, "ThumbTintBlendMode", p0); }
    public void setThumbTintMode(android.graphics.PorterDuff.Mode p0) { meglerFill = megler.Fill.put(meglerFill, "ThumbTintMode", p0); }
    public void setTrackIcon(android.graphics.drawable.Icon p0) {}
    public java.lang.Runnable setTrackIconAsync(android.graphics.drawable.Icon p0) { return null; }
    public java.lang.Runnable setTrackResourceAsync(int p0) { return null; }
    public void setTrackTintBlendMode(android.graphics.BlendMode p0) { meglerFill = megler.Fill.put(meglerFill, "TrackTintBlendMode", p0); }
    public void setTrackTintMode(android.graphics.PorterDuff.Mode p0) { meglerFill = megler.Fill.put(meglerFill, "TrackTintMode", p0); }
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public static final class InspectionCompanion implements android.view.inspector.InspectionCompanion {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public InspectionCompanion() {}
        public void mapProperties(android.view.inspector.PropertyMapper p0) {}
        public void readProperties(android.widget.Switch p0, android.view.inspector.PropertyReader p1) {}
        public void readProperties(java.lang.Object p0, android.view.inspector.PropertyReader p1) {}
    }
    // ---- end of generated nested classes
}
