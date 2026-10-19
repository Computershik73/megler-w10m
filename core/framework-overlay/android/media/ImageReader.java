// megler-w10m overlay of Megler/src/java-framework/gen-src/android/media/ImageReader.java (tools/build-framework.sh), in place of
// the generated stub, which had no images: getSurface() was null and acquireLatestImage() null.
//
// Why. CameraX analyses a camera's pictures through an ImageReader (ImageAnalysis, YUV_420_888): urent's QR scanner hands
// each image to Huawei's ScanKit (MLFrame.fromMediaImage). The camera (android.hardware.camera2.impl.MeglerCameraDevice) writes
// a picture into a buffer the reader gives it (meglerDequeue, NV21's bytes: the luma, then V and U interleaved) and queues it
// (meglerQueue); the reader's listener hears of it, on its handler or executor, as on Android.
//
// What it is: Android's reader as an app sees it. Its surface is a Surface that names it (Surface.meglerImageReader, added by
// this overlay's Surface.java.members). An image is the reader's size, its timestamp the camera's, and YUV_420_888's three
// planes over the NV21 buffer as Android's devices give them: Y with a pixel stride of 1, U and V with 2, every row stride
// the width, V's buffer one byte before U's. At most maxImages are acquired and not yet closed (acquireNextImage and
// acquireLatestImage then throw IllegalStateException, as Android's do); a picture that comes while every buffer is taken
// takes the oldest queued one's place, as a camera that keeps streaming does. Closed images' buffers are used again.
//
// Not done: formats other than YUV_420_888 and NV21 (an image of another format has no planes), hardware buffers, images
// detached or attached, and multi-resolution readers' parents.
package android.media;

import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.concurrent.Executor;

@SuppressWarnings({"unchecked", "rawtypes", "deprecation"})
public class ImageReader implements java.lang.AutoCloseable {
    private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
    private final int mWidth, mHeight, mFormat, mMaxImages;
    private final long mUsage;
    private final Surface mSurface;
    private final Object mLock = new Object();
    private final ArrayDeque<MeglerImage> mQueued = new ArrayDeque<>();
    private final ArrayList<byte[]> mFree = new ArrayList<>();
    private int mAcquired, mDequeued;
    private OnImageAvailableListener mListener;
    private Executor mListenerExecutor;
    private boolean mClosed;

    public static android.media.ImageReader.ImagePlane[] initializeImagePlanes(int p0, android.graphics.GraphicBuffer p1, int p2, int p3, long p4, int p5, int p6, android.graphics.Rect p7) { return null; }
    public static android.media.ImageReader newInstance(int width, int height, int format, int maxImages) { return new ImageReader(width, height, format, maxImages, 3L); }
    public static android.media.ImageReader newInstance(int width, int height, int format, int maxImages, long usage) { return new ImageReader(width, height, format, maxImages, usage); }
    public static android.media.ImageReader newInstance(int width, int height, int format, int maxImages, android.hardware.camera2.MultiResolutionImageReader parent) { return new ImageReader(width, height, format, maxImages, 3L); }
    public static void unlockGraphicBuffer(android.graphics.GraphicBuffer p0) {}

    protected ImageReader() { this(1, 1, 35, 1, 3L); }

    private ImageReader(int width, int height, int format, int maxImages, long usage) {
        if (width < 1 || height < 1) throw new IllegalArgumentException("The image dimensions must be positive");
        if (maxImages < 1) throw new IllegalArgumentException("Maximum outstanding image count must be at least 1");
        mWidth = width;
        mHeight = height;
        mFormat = format;
        mMaxImages = maxImages;
        mUsage = usage;
        mSurface = new Surface();
        mSurface.meglerImageReader = this;
    }

    public int getWidth() { return mWidth; }
    public int getHeight() { return mHeight; }
    public int getImageFormat() { return mFormat; }
    public int getMaxImages() { return mMaxImages; }
    public long getUsage() { return mUsage; }
    public int getDataSpace() { return 0; }
    public int getHardwareBufferFormat() { return mFormat; }
    public android.view.Surface getSurface() { return mSurface; }

    public void setOnImageAvailableListener(OnImageAvailableListener listener, Handler handler) {
        Handler h = handler != null ? handler : listener == null ? null : new Handler(Looper.myLooper() != null ? Looper.myLooper() : Looper.getMainLooper());
        synchronized (mLock) {
            mListener = listener;
            mListenerExecutor = h == null ? null : h::post;
        }
    }

    public void setOnImageAvailableListenerWithExecutor(OnImageAvailableListener listener, Executor executor) {
        if (listener != null && executor == null) throw new IllegalArgumentException("executor must not be null");
        synchronized (mLock) {
            mListener = listener;
            mListenerExecutor = executor;
        }
    }

    public android.media.Image acquireNextImage() {
        synchronized (mLock) {
            MeglerImage image = mQueued.poll();
            if (image == null) return null;
            if (mAcquired >= mMaxImages) {
                mQueued.addFirst(image);
                throw new IllegalStateException(String.format("maxImages (%d) has already been acquired, call #close before acquiring more.", mMaxImages));
            }
            mAcquired++;
            image.mIsImageValid = true;
            return image;
        }
    }

    public android.media.Image acquireNextImageNoThrowISE() {
        try {
            return acquireNextImage();
        } catch (IllegalStateException e) {
            return null;
        }
    }

    /** The newest image, the older queued ones dropped (their buffers used again). */
    public android.media.Image acquireLatestImage() {
        synchronized (mLock) {
            while (mQueued.size() > 1) mFree.add(mQueued.poll().mData);
        }
        return acquireNextImage();
    }

    public void close() {
        synchronized (mLock) {
            if (mClosed) return;
            mClosed = true;
            mListener = null;
            mQueued.clear();
            mFree.clear();
        }
        mSurface.release();
    }

    public void detachImage(android.media.Image p0) { throw new UnsupportedOperationException("detaching images is not supported here"); }
    public void discardFreeBuffers() { synchronized (mLock) { mFree.clear(); } }
    protected void finalize() {}

    // ---- megler-w10m: the camera's side (android.hardware.camera2.impl.MeglerCameraDevice).

    /**
     * A buffer for one picture of this reader's size in NV21 (width x height x 3 / 2 bytes), or null when the reader is
     * closed or its format takes no YUV picture. When every buffer is taken (images acquired and queued) the oldest queued
     * picture is dropped and its buffer given, as a camera that keeps streaming overwrites what was not read.
     */
    public byte[] meglerDequeue() {
        synchronized (mLock) {
            if (mClosed || (mFormat != 35 && mFormat != 17)) return null;
            if (mAcquired + mQueued.size() + mDequeued >= mMaxImages + 1) {
                MeglerImage oldest = mQueued.poll();
                if (oldest == null) return null;
                mDequeued++;
                return oldest.mData;
            }
            mDequeued++;
            int bytes = mWidth * mHeight * 3 / 2;
            for (int i = mFree.size() - 1; i >= 0; i--) {
                byte[] b = mFree.remove(i);
                if (b.length == bytes) return b;
            }
            return new byte[bytes];
        }
    }

    /** A buffer from meglerDequeue given back without a picture. */
    public void meglerCancel(byte[] data) {
        synchronized (mLock) {
            mDequeued--;
            if (!mClosed) mFree.add(data);
        }
    }

    /** A buffer from meglerDequeue filled with a picture taken at `timestamp`: queued, and the listener told. */
    public void meglerQueue(byte[] data, long timestamp) {
        OnImageAvailableListener l;
        Executor e;
        synchronized (mLock) {
            mDequeued--;
            if (mClosed) return;
            mQueued.add(new MeglerImage(this, data, timestamp));
            l = mListener;
            e = mListenerExecutor;
        }
        if (l != null && e != null) e.execute(() -> l.onImageAvailable(this));
    }

    private void released(MeglerImage image) {
        synchronized (mLock) {
            mAcquired--;
            if (!mClosed) mFree.add(image.mData);
        }
    }

    /** One picture: YUV_420_888's planes over NV21's bytes. */
    private static final class MeglerImage extends Image {
        private final ImageReader mReader;
        private final byte[] mData;
        private final long mTimestamp;
        private Plane[] mPlanes;
        private boolean mClosedImage;

        MeglerImage(ImageReader reader, byte[] data, long timestamp) {
            mReader = reader;
            mData = data;
            mTimestamp = timestamp;
            setCropRect(new Rect(0, 0, reader.mWidth, reader.mHeight));
        }

        private void valid() { if (mClosedImage) throw new IllegalStateException("Image is already closed"); }

        @Override public int getFormat() { valid(); return mReader.mFormat; }
        @Override public int getWidth() { valid(); return mReader.mWidth; }
        @Override public int getHeight() { valid(); return mReader.mHeight; }
        @Override public long getTimestamp() { valid(); return mTimestamp; }
        @Override public int getTransform() { return 0; }
        @Override public int getScalingMode() { return 0; }
        @Override public int getPlaneCount() { return 3; }

        @Override public Plane[] getPlanes() {
            valid();
            if (mPlanes == null) {
                int w = mReader.mWidth, h = mReader.mHeight, luma = w * h, chroma = luma / 2;
                mPlanes = new Plane[] {
                    new MeglerPlane(ByteBuffer.wrap(mData, 0, luma).slice(), w, 1),
                    new MeglerPlane(ByteBuffer.wrap(mData, luma + 1, chroma - 1).slice(), w, 2),   // U: the second of each pair
                    new MeglerPlane(ByteBuffer.wrap(mData, luma, chroma - 1).slice(), w, 2),       // V: the first
                };
            }
            return mPlanes.clone();
        }

        @Override public void close() {
            if (mClosedImage) return;
            mClosedImage = true;
            mIsImageValid = false;
            mReader.released(this);
        }
    }

    private static final class MeglerPlane extends Image.Plane {
        private final ByteBuffer mBuffer;
        private final int mRowStride, mPixelStride;

        MeglerPlane(ByteBuffer buffer, int rowStride, int pixelStride) {
            mBuffer = buffer;
            mRowStride = rowStride;
            mPixelStride = pixelStride;
        }

        @Override public ByteBuffer getBuffer() { return mBuffer; }
        @Override public int getRowStride() { return mRowStride; }
        @Override public int getPixelStride() { return mPixelStride; }
    }

    public static final class Builder {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        private final int mWidth, mHeight;
        private int mFormat = 35, mMaxImages = 1;
        private long mUsage = 3L;
        public Builder(int width, int height) { mWidth = width; mHeight = height; }
        public android.media.ImageReader build() { return new ImageReader(mWidth, mHeight, mFormat, mMaxImages, mUsage); }
        public android.media.ImageReader.Builder setDefaultDataSpace(int p0) { meglerProps.put("DefaultDataSpace", Integer.valueOf(p0)); return this; }
        public android.media.ImageReader.Builder setDefaultHardwareBufferFormat(int p0) { mFormat = p0; return this; }
        public android.media.ImageReader.Builder setImageFormat(int p0) { mFormat = p0; return this; }
        public android.media.ImageReader.Builder setMaxImages(int p0) { mMaxImages = p0; return this; }
        public android.media.ImageReader.Builder setUsage(long p0) { mUsage = p0; return this; }
        Builder() { this((int) 0, (int) 0); }
    }
    public static class ImagePlane extends android.media.Image.Plane {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        public java.nio.ByteBuffer getBuffer() { return (java.nio.ByteBuffer) meglerProps.get("Buffer"); }
        public int getPixelStride() { return (meglerProps.get("PixelStride") instanceof Integer ? (Integer) meglerProps.get("PixelStride") : 0); }
        public int getRowStride() { return (meglerProps.get("RowStride") instanceof Integer ? (Integer) meglerProps.get("RowStride") : 0); }
        protected ImagePlane() { super(); }
    }
    public interface OnImageAvailableListener {
        void onImageAvailable(android.media.ImageReader p0);
    }
}
