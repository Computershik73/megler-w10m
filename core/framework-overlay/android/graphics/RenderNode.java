// Overlay of core/framework/src/android/graphics/RenderNode.java (tools/build-framework.sh).
// It differs in what a node is: a display list kept by the core and the properties it is drawn with, as on Android, where
// the original drew the node's content into a bitmap of its size when it was recorded and drew that bitmap where the node
// was. Compose makes a node for every graphics layer, dozens, many as large as the screen or larger, and each cost a
// bitmap in the app's memory and a target, a read-back and an upload on the graphics processor: Spotify ended with
// java.lang.OutOfMemoryError: Bitmap 1650x2560 thrown from beginRecording, after Direct2D had run out of memory.
//
// What changed, against the original:
//   - beginRecording hands out a RecordingCanvas over the core's recording canvas (megler.Gfx.cvRecord), which writes down
//     what it is given; endRecording makes that the node's list (rnEnd); discardDisplayList drops it (rnDiscard). No
//     bitmap. computeApproximateMemoryUsage is the list's size.
//   - meglerDraw (Canvas.drawRenderNode) is megler.Gfx.cvDrawNode: on a recording canvas the node itself is written to the
//     outer list, and is drawn with its list and properties as they are when that list is drawn; on any other canvas the
//     node is drawn now, by the core, as Android's RenderNodeDrawable draws it (mg_java_gfx.c, node_play).
//   - The properties stay in this class's fields, and every setter that changes one marks the node; before anything is
//     drawn on a canvas that is not recording, every marked node hands its properties to the core in one call (meglerSync,
//     meglerPush, megler.Gfx.rnSet). The matrix is the original's getMatrix, unchanged.
//   - What drawing needs and the original kept by reference is copied when set, as Android copies it: the outline (with
//     its path) and the layer paint.
//   - A layer, as Android makes one: for a translucent node, or one asked to be composited (setUseCompositingLayer(true),
//     setLayerType other than none); its paint's mode and alpha are the layer's. The original put every node on with its
//     layer paint, layer or not.
//   - The generated create(String, AnimationHost), which returned null, makes a node.
// Everything else is the original's.
package android.graphics;

/**
 * A display list with its own position and transform, as Compose's graphics layers and View's hardware layers use it. Its
 * content is recorded by the core (megler.Gfx.cvRecord) and drawn again wherever the node is drawn, with the properties it has
 * then: translation, scale, rotation about the pivot, the animation matrix, alpha, clips, outline and layer.
 */
public final class RenderNode {
    public static final int USAGE_BACKGROUND = 1;
    public static final int USAGE_UNKNOWN = 0;
    private static long sNextId = 1;
    private final String mName;
    private final long mId;
    private int mLeft, mTop, mRight, mBottom;
    private float mAlpha = 1f, mTx, mTy, mTz, mSx = 1f, mSy = 1f, mRx, mRy, mRz, mPx, mPy, mCamera = 8f, mElevation;
    private boolean mPivotSet, mClipToBounds = true, mClipToOutline, mOverlapping = true, mCompositing, mForceDark = true, mProjBackwards, mProjReceiver;
    private int mAmbient = 0xff000000, mSpot = 0xff000000, mUsage, mLayerType;
    private Matrix mAnimation, mStatic;
    private Rect mClipRect;
    private Outline mOutline;
    private Paint mLayerPaint;
    private RenderEffect mEffect;
    private RecordingCanvas mCanvas;
    private boolean mHasDisplayList;
    public long mNativeRenderNode;
    /** Megler: the core's node (megler.Gfx.rnNew): the list and the properties it was last handed. */
    private long mMeglerNode;
    /** Megler: a property changed since the core was last told (meglerChanged). */
    private boolean mMeglerDirty;
    private final float[] mMeglerF = new float[20];
    private final int[] mMeglerI = new int[5];
    private Matrix mMeglerMatrix;
    private float[] mMeglerValues;
    /** Megler: the nodes whose properties changed, to be handed to the core before anything is drawn (meglerSync). */
    private static final java.util.ArrayList<RenderNode> sMeglerDirty = new java.util.ArrayList<>();

    public RenderNode(String name) { mName = name; synchronized (RenderNode.class) { mId = sNextId++; } mNativeRenderNode = mId; mMeglerNode = megler.Gfx.rnNew(); }
    public static RenderNode create(String name, Object animationHost) { return new RenderNode(name); }
    public static RenderNode adopt(long nativePtr) { return new RenderNode("adopted"); }
    @Override protected void finalize() throws Throwable {
        if (mMeglerNode != 0) megler.Gfx.rnFree(mMeglerNode);
        mMeglerNode = 0;
        super.finalize();
    }

    // ---- recording
    public RecordingCanvas beginRecording(int width, int height) {
        if (mCanvas != null) throw new IllegalStateException("Recording currently in progress - missing #endRecording() call?");
        mCanvas = new RecordingCanvas(this, megler.Gfx.cvRecord(width, height), width, height);
        return mCanvas;
    }
    public RecordingCanvas beginRecording() { return beginRecording(getWidth(), getHeight()); }
    public RecordingCanvas start(int width, int height) { return beginRecording(width, height); }
    public void endRecording() {
        if (mCanvas == null) throw new IllegalStateException("No recording in progress, forgot to call #beginRecording()?");
        megler.Gfx.rnEnd(mMeglerNode, mCanvas.meglerNative());
        mCanvas.meglerRelease();
        mCanvas = null;
        mHasDisplayList = true;
    }
    public void end(RecordingCanvas canvas) { endRecording(); }
    public boolean hasDisplayList() { return mHasDisplayList; }
    public void discardDisplayList() { mHasDisplayList = false; megler.Gfx.rnDiscard(mMeglerNode); }
    public boolean isAttached() { return false; }
    public long getUniqueId() { return mId; }
    public long computeApproximateMemoryUsage() { return megler.Gfx.rnBytes(mMeglerNode); }
    public long computeApproximateMemoryAllocated() { return computeApproximateMemoryUsage(); }
    public void output() {}
    public void endAllAnimators() {}
    public void forceEndAnimators() {}
    public void setUsageHint(int usage) { mUsage = usage; }
    public void setIsTextureView() {}
    public void registerVectorDrawableAnimator(android.view.NativeVectorDrawableAnimator a) {}

    /**
     * Megler: the node drawn on a canvas, as drawRenderNode does it. A recording canvas writes the node itself down; on any
     * other the nodes whose properties changed tell the core first, since this node's list may draw any of them.
     */
    void meglerDraw(Canvas c) {
        long cv = c.meglerNative();
        if (cv == 0) return;
        if (!c.meglerRecords()) meglerSync();
        megler.Gfx.cvDrawNode(cv, mMeglerNode);
        c.meglerDrawn();
    }

    /** Megler: a property that drawing uses changed; the core is told before the next drawing that is not a recording. */
    private void meglerChanged() {
        if (mMeglerDirty) return;
        mMeglerDirty = true;
        synchronized (sMeglerDirty) {
            sMeglerDirty.add(this);
            // Nodes changed and never drawn (an app that keeps nodes it no longer draws) are not let pile up.
            if (sMeglerDirty.size() >= 4096) meglerSyncLocked();
        }
    }

    /** Megler: every node whose properties changed hands them to the core. Before anything is drawn that may draw a node. */
    static void meglerSync() {
        synchronized (sMeglerDirty) {
            meglerSyncLocked();
        }
    }

    private static void meglerSyncLocked() {
        int n = sMeglerDirty.size();
        for (int i = 0; i < n; i++) sMeglerDirty.get(i).meglerPush();
        sMeglerDirty.clear();
    }

    /** Megler: the properties as megler.Gfx.rnSet takes them (mg_java_gfx.c, G_rnSet, says each place). */
    private void meglerPush() {
        mMeglerDirty = false;
        float[] f = mMeglerF;
        int[] k = mMeglerI;
        int flags = 0, kind = 0, fill = 0, count = 0;
        float[] path = null;
        f[0] = mLeft; f[1] = mTop; f[2] = mRight; f[3] = mBottom;
        if (hasIdentityMatrix()) {
            flags |= 1;
        } else {
            if (mMeglerMatrix == null) { mMeglerMatrix = new Matrix(); mMeglerValues = new float[9]; }
            getMatrix(mMeglerMatrix);
            mMeglerMatrix.getValues(mMeglerValues);
            System.arraycopy(mMeglerValues, 0, f, 4, 6);
        }
        f[10] = mAlpha;
        if (mClipToBounds) flags |= 2;
        if (mClipRect != null) { flags |= 4; f[11] = mClipRect.left; f[12] = mClipRect.top; f[13] = mClipRect.right; f[14] = mClipRect.bottom; }
        if (mClipToOutline) flags |= 8;
        if (mOutline != null && mOutline.mPath != null) {
            kind = 2; path = mOutline.mPath.ops; count = mOutline.mPath.n; fill = mOutline.mPath.meglerFill();
        } else if (mOutline != null && mOutline.mRect != null) {
            kind = 1; f[15] = mOutline.mRect.left; f[16] = mOutline.mRect.top; f[17] = mOutline.mRect.right; f[18] = mOutline.mRect.bottom; f[19] = mOutline.mRadius;
        }
        boolean layer = mCompositing || mLayerType != 0 || mAlpha < 1f;
        int[] paint = null;
        if (layer) flags |= 16;
        if (layer && mLayerPaint != null) { flags |= 32; paint = mLayerPaint.mP; }
        k[0] = flags;
        k[1] = kind;
        k[2] = layer ? Math.round(mAlpha * (mLayerPaint != null ? mLayerPaint.getAlpha() : 255)) & 0xFF : 255;
        k[3] = fill;
        k[4] = count;
        megler.Gfx.rnSet(mMeglerNode, f, k, path, paint);
    }

    // ---- position
    public boolean setPosition(int left, int top, int right, int bottom) {
        boolean changed = left != mLeft || top != mTop || right != mRight || bottom != mBottom;
        mLeft = left; mTop = top; mRight = right; mBottom = bottom;
        if (changed) meglerChanged();
        return changed;
    }
    public boolean setPosition(Rect r) { return setPosition(r.left, r.top, r.right, r.bottom); }
    public boolean setLeftTopRightBottom(int l, int t, int r, int b) { return setPosition(l, t, r, b); }
    public boolean setLeft(int v) { boolean c = v != mLeft; mLeft = v; if (c) meglerChanged(); return c; }
    public boolean setTop(int v) { boolean c = v != mTop; mTop = v; if (c) meglerChanged(); return c; }
    public boolean setRight(int v) { boolean c = v != mRight; mRight = v; if (c) meglerChanged(); return c; }
    public boolean setBottom(int v) { boolean c = v != mBottom; mBottom = v; if (c) meglerChanged(); return c; }
    public int getLeft() { return mLeft; }
    public int getTop() { return mTop; }
    public int getRight() { return mRight; }
    public int getBottom() { return mBottom; }
    public int getWidth() { return mRight - mLeft; }
    public int getHeight() { return mBottom - mTop; }
    public boolean offsetLeftAndRight(int d) { if (d == 0) return false; mLeft += d; mRight += d; meglerChanged(); return true; }
    public boolean offsetTopAndBottom(int d) { if (d == 0) return false; mTop += d; mBottom += d; meglerChanged(); return true; }

    // ---- transform
    public boolean hasIdentityMatrix() {
        return mTx == 0 && mTy == 0 && mSx == 1 && mSy == 1 && mRx == 0 && mRy == 0 && mRz == 0 && (mAnimation == null || mAnimation.isIdentity()) && (mStatic == null || mStatic.isIdentity());
    }
    public void getMatrix(Matrix out) {
        out.reset();
        float px = mPivotSet ? mPx : getWidth() / 2f, py = mPivotSet ? mPy : getHeight() / 2f;
        if (mStatic != null) out.set(mStatic);
        out.preTranslate(mTx, mTy);
        if (mRx != 0 || mRy != 0) {
            Camera cam = new Camera();
            cam.setLocation(0, 0, -Math.abs(mCamera) / 72f);
            cam.rotateX(mRx); cam.rotateY(mRy);
            Matrix m3 = new Matrix();
            cam.getMatrix(m3);
            m3.preTranslate(-px, -py); m3.postTranslate(px, py);
            out.preConcat(m3);
        }
        out.preRotate(mRz, px, py);
        out.preScale(mSx, mSy, px, py);
        if (mAnimation != null) out.postConcat(mAnimation);
    }
    public void getInverseMatrix(Matrix out) { getMatrix(out); out.invert(out); }
    public boolean setAnimationMatrix(Matrix m) { mAnimation = m == null ? null : new Matrix(m); meglerChanged(); return true; }
    public Matrix getAnimationMatrix() { return mAnimation; }
    public boolean setStaticMatrix(Matrix m) { mStatic = m == null ? null : new Matrix(m); meglerChanged(); return true; }
    public boolean setTranslationX(float v) { boolean c = v != mTx; mTx = v; if (c) meglerChanged(); return c; }
    public boolean setTranslationY(float v) { boolean c = v != mTy; mTy = v; if (c) meglerChanged(); return c; }
    public boolean setTranslationZ(float v) { boolean c = v != mTz; mTz = v; return c; }
    public float getTranslationX() { return mTx; }
    public float getTranslationY() { return mTy; }
    public float getTranslationZ() { return mTz; }
    public boolean setScaleX(float v) { boolean c = v != mSx; mSx = v; if (c) meglerChanged(); return c; }
    public boolean setScaleY(float v) { boolean c = v != mSy; mSy = v; if (c) meglerChanged(); return c; }
    public float getScaleX() { return mSx; }
    public float getScaleY() { return mSy; }
    public boolean setRotationX(float v) { boolean c = v != mRx; mRx = v; if (c) meglerChanged(); return c; }
    public boolean setRotationY(float v) { boolean c = v != mRy; mRy = v; if (c) meglerChanged(); return c; }
    public boolean setRotationZ(float v) { boolean c = v != mRz; mRz = v; if (c) meglerChanged(); return c; }
    public float getRotationX() { return mRx; }
    public float getRotationY() { return mRy; }
    public float getRotationZ() { return mRz; }
    public boolean setPivotX(float v) { mPx = v; mPivotSet = true; meglerChanged(); return true; }
    public boolean setPivotY(float v) { mPy = v; mPivotSet = true; meglerChanged(); return true; }
    public float getPivotX() { return mPivotSet ? mPx : getWidth() / 2f; }
    public float getPivotY() { return mPivotSet ? mPy : getHeight() / 2f; }
    public boolean isPivotExplicitlySet() { return mPivotSet; }
    public boolean resetPivot() { boolean c = mPivotSet; mPivotSet = false; if (c) meglerChanged(); return c; }
    public boolean setCameraDistance(float d) { mCamera = d; meglerChanged(); return true; }
    public float getCameraDistance() { return mCamera; }

    // ---- appearance
    public boolean setAlpha(float a) { boolean c = a != mAlpha; mAlpha = Math.max(0f, Math.min(1f, a)); if (c) meglerChanged(); return c; }
    public float getAlpha() { return mAlpha; }
    public boolean setHasOverlappingRendering(boolean b) { mOverlapping = b; return true; }
    public boolean hasOverlappingRendering() { return mOverlapping; }
    public boolean setElevation(float e) { mElevation = e; return true; }
    public float getElevation() { return mElevation; }
    public boolean setAmbientShadowColor(int c) { mAmbient = c; return true; }
    public int getAmbientShadowColor() { return mAmbient; }
    public boolean setSpotShadowColor(int c) { mSpot = c; return true; }
    public int getSpotShadowColor() { return mSpot; }
    public boolean hasShadow() { return false; }
    public boolean setClipToBounds(boolean b) { boolean c = b != mClipToBounds; mClipToBounds = b; if (c) meglerChanged(); return true; }
    public boolean getClipToBounds() { return mClipToBounds; }
    public boolean setClipToOutline(boolean b) { boolean c = b != mClipToOutline; mClipToOutline = b; if (c) meglerChanged(); return true; }
    public boolean getClipToOutline() { return mClipToOutline; }
    /** The outline is copied, its path with it, as Android copies it: the app reuses its Outline for the next node. */
    public boolean setOutline(Outline o) {
        if (o == null) {
            mOutline = null;
        } else {
            Outline copy = new Outline(o);
            if (o.mPath != null) copy.mPath = new Path(o.mPath);
            mOutline = copy;
        }
        meglerChanged();
        return true;
    }
    public boolean setClipRect(Rect r) { mClipRect = r == null ? null : new Rect(r); meglerChanged(); return true; }
    public boolean setRevealClip(boolean shouldClip, float x, float y, float radius) { return true; }
    public boolean setUseCompositingLayer(boolean forceToLayer, Paint paint) { mCompositing = forceToLayer; mLayerPaint = paint == null ? null : new Paint(paint); meglerChanged(); return true; }
    public boolean getUseCompositingLayer() { return mCompositing; }
    public boolean setLayerType(int type) { boolean c = type != mLayerType; mLayerType = type; if (c) meglerChanged(); return true; }
    public boolean setLayerPaint(Paint paint) { mLayerPaint = paint == null ? null : new Paint(paint); meglerChanged(); return true; }
    public boolean setRenderEffect(RenderEffect e) { mEffect = e; return true; }
    public boolean setBackdropRenderEffect(RenderEffect e) { return true; }
    public boolean setForceDarkAllowed(boolean b) { mForceDark = b; return true; }
    public boolean isForceDarkAllowed() { return mForceDark; }
    public boolean setProjectBackwards(boolean b) { mProjBackwards = b; return true; }
    public boolean setProjectionReceiver(boolean b) { mProjReceiver = b; return true; }
    public boolean stretch(float vecX, float vecY, float maxStretchX, float maxStretchY) { return true; }
    public boolean clearStretch() { return true; }
    @Override public String toString() { return "RenderNode(" + mName + " " + mLeft + "," + mTop + " " + getWidth() + "x" + getHeight() + ")"; }
    // ---- generated by tools/compat/fillmembers.py: the platform's members this class does not write (signatures only)
    public static final int USAGE_NAVIGATION_BAR_BACKGROUND = 4;
    public static android.graphics.RenderNode create(java.lang.String p0, android.graphics.RenderNode.AnimationHost p1) { return new RenderNode(p0); }
    public void addPositionUpdateListener(android.graphics.RenderNode.PositionUpdateListener p0) {}
    public void removePositionUpdateListener(android.graphics.RenderNode.PositionUpdateListener p0) {}
    // ---- end of generated members
    // ---- generated by tools/compat/genstubs.py: the platform's nested classes this class does not write
    public interface AnimationHost {
        boolean isAttached();
        void registerAnimatingRenderNode(android.graphics.RenderNode p0, android.animation.Animator p1);
        void registerVectorDrawableAnimator(android.view.NativeVectorDrawableAnimator p0);
    }
    public interface PositionUpdateListener {
        static boolean callApplyStretch(java.lang.ref.WeakReference p0, long p1, float p2, float p3, float p4, float p5, float p6, float p7, float p8, float p9, float p10, float p11) { return false; }
        static boolean callPositionChanged(java.lang.ref.WeakReference p0, long p1, int p2, int p3, int p4, int p5) { return false; }
        static boolean callPositionChanged2(java.lang.ref.WeakReference p0, long p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, int p10, int p11) { return false; }
        static boolean callPositionLost(java.lang.ref.WeakReference p0, long p1) { return false; }
        default void applyStretch(long p0, float p1, float p2, float p3, float p4, float p5, float p6, float p7, float p8, float p9, float p10) {}
        void positionChanged(long p0, int p1, int p2, int p3, int p4);
        default void positionChanged(long p0, int p1, int p2, int p3, int p4, int p5, int p6, int p7, int p8, int p9, int p10) {}
        void positionLost(long p0);
    }
    // ---- end of generated nested classes
}
