#!/usr/bin/env python3
"""
Writes the overlays of android.hardware.camera2's metadata classes and parameters (core/framework-overlay/android/hardware/
camera2) from the original's generated stubs (Megler/src/java-framework/gen-src), which have every member's signature and no
behaviour: a key field there is null, get() answers null, a session's StateCallback has no methods at all.

What is made:
  CameraCharacteristics, CaptureRequest, CaptureResult: each key field a real Key, named as Android names it
    (LENS_FACING is "android.lens.facing", SENSOR_INFO_ACTIVE_ARRAY_SIZE "android.sensor.info.activeArraySize"), equal to
    another of the same name; the values kept by name; get, getKeys, a request's Builder (set, targets, tag, build), and
    methods of this layer's own (meglerSet, meglerFrom) through which the camera (camera2/impl/MeglerCameraDevice) fills them.
  CameraCaptureSession: the StateCallback's methods, which the stub has not.
  params.StreamConfigurationMap, OutputConfiguration, SessionConfiguration: what was given kept and said back.
Every other member is the stub's, unchanged. Run again after the stubs change:

    python tools/compat/camera2-overlays.py [path to Megler/src/java-framework, else $MEGLER_FRAMEWORK, else ../Megler's]
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("MEGLER_FRAMEWORK") or os.path.join(os.path.dirname(ROOT), "Megler", "src", "java-framework")
STUBS = os.path.join(FW, "gen-src", "android", "hardware", "camera2")
OUT = os.path.join(ROOT, "core", "framework-overlay", "android", "hardware", "camera2")


def read(rel):
    return io.open(os.path.join(STUBS, rel), encoding="utf-8").read().replace("\r\n", "\n")


def write(rel, text):
    path = os.path.join(OUT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)
    print("wrote", os.path.relpath(path, ROOT))


def header(rel, why):
    lines = ["// megler-w10m overlay of Megler/src/java-framework/gen-src/android/hardware/camera2/%s, written by" % rel,
             "// tools/compat/camera2-overlays.py from that generated stub (run it again rather than editing this file)."]
    lines += ["// " + w for w in why]
    return "\n".join(lines) + "\n"


def key_name(field):
    """Android's name of a key field: LENS_FACING android.lens.facing, SENSOR_INFO_ACTIVE_ARRAY_SIZE android.sensor.info.activeArraySize."""
    parts = field.lower().split("_")
    section, rest = parts[0], parts[1:]
    prefix = "android." + section + "."
    if rest and rest[0] == "info" and len(rest) > 1:
        prefix += "info."
        rest = rest[1:]
    camel = rest[0] + "".join(w[:1].upper() + w[1:] for w in rest[1:]) if rest else section
    return prefix + camel


def replace_line(text, startswith, new):
    """The outer class's member (four spaces in) whose line starts `startswith`; a nested class's are deeper."""
    lines = text.split("\n")
    hits = [i for i, l in enumerate(lines) if l.startswith("    " + startswith)]
    assert len(hits) == 1, (startswith, len(hits))
    lines[hits[0]] = new
    return "\n".join(lines)


def replace_block(text, opening, new):
    """The block that begins with the line starting `opening` (stripped), to its closing brace at the same indentation."""
    lines = text.split("\n")
    hits = [i for i, l in enumerate(lines) if l.strip().startswith(opening)]
    assert len(hits) == 1, (opening, len(hits))
    i = hits[0]
    indent = len(lines[i]) - len(lines[i].lstrip())
    j = i + 1
    while not (lines[j].strip() == "}" and len(lines[j]) - len(lines[j].lstrip()) == indent):
        j += 1
    return "\n".join(lines[:i] + new.rstrip("\n").split("\n") + lines[j + 1:])


def keys_made_real(text, cls):
    pat = re.compile(r"^(\s*)public static android\.hardware\.camera2\.%s\.Key (\w+);$" % cls, re.M)
    return pat.sub(lambda m: '%spublic static final android.hardware.camera2.%s.Key %s = new android.hardware.camera2.%s.Key("%s", java.lang.Object.class);'
                   % (m.group(1), cls, m.group(2), cls, key_name(m.group(2))), text)


def key_class(cls, ctors):
    body = ["    /** A key: its name is what it is (two keys of one name are the same key); its type is said for the reader. */",
            "    public static final class Key<T> {",
            "        private final java.lang.String mName;",
            "        private final java.lang.Class<T> mType;"]
    for c in ctors:
        body.append("        " + c)
    body += ["        public java.lang.String getName() { return mName; }",
             "        public android.hardware.camera2.impl.CameraMetadataNative.Key getNativeKey() { return null; }",
             "        public long getVendorId() { return 0L; }",
             "        @Override public boolean equals(java.lang.Object o) { return o instanceof Key && ((Key) o).mName.equals(mName); }",
             "        @Override public int hashCode() { return mName.hashCode(); }",
             '        @Override public java.lang.String toString() { return "%s.Key(" + mName + ")"; }' % cls,
             "    }"]
    return "\n".join(body) + "\n"


KEY_CTORS = {
    "TypeReference": "public Key(java.lang.String name, android.hardware.camera2.utils.TypeReference type) { mName = name; mType = (java.lang.Class) java.lang.Object.class; }",
    "Class": "public Key(java.lang.String name, java.lang.Class<T> type) { mName = name; mType = type; }",
    "ClassVendor": "public Key(java.lang.String name, java.lang.Class<T> type, long vendorId) { mName = name; mType = type; }",
    "Fallback": "public Key(java.lang.String name, java.lang.String fallbackName, java.lang.Class<T> type) { mName = name; mType = type; }",
}

VALUES = """    // ---- megler-w10m: the values, by their keys' names, and the keys themselves in the order they were set.
    private final java.util.LinkedHashMap<java.lang.String, java.lang.Object> mMeglerValues = new java.util.LinkedHashMap<>();
    private final java.util.LinkedHashMap<java.lang.String, Key> mMeglerKeys = new java.util.LinkedHashMap<>();
    /** This layer's: a key's value set (null takes it away). */
    public void meglerSet(Key key, java.lang.Object value) {
        if (key == null) return;
        if (value == null) {
            mMeglerValues.remove(key.getName());
            mMeglerKeys.remove(key.getName());
        } else {
            mMeglerValues.put(key.getName(), value);
            mMeglerKeys.put(key.getName(), key);
        }
    }
    /** This layer's: the value of the key of this name, or null. */
    public java.lang.Object meglerGet(java.lang.String name) { return mMeglerValues.get(name); }
"""


def metadata_class(cls, rel, extra_fields, replacements, why, keep_builder=True):
    text = read(rel)
    text = keys_made_real(text, cls)
    stub_key = text[text.index("    public static final class Key {"):]
    ctors = []
    if "Key(java.lang.String p0, android.hardware.camera2.utils.TypeReference p1)" in stub_key: ctors.append(KEY_CTORS["TypeReference"])
    if "Key(java.lang.String p0, java.lang.Class p1) {}" in stub_key: ctors.append(KEY_CTORS["Class"])
    if "Key(java.lang.String p0, java.lang.Class p1, long p2)" in stub_key: ctors.append(KEY_CTORS["ClassVendor"])
    if "Key(java.lang.String p0, java.lang.String p1, java.lang.Class p2)" in stub_key: ctors.append(KEY_CTORS["Fallback"])
    text = replace_block(text, "public static final class Key {", key_class(cls, ctors))
    # (Blocks first: a nested class may have a line the outer one's replacements look for, as a Builder's get.)
    for start, new in sorted(replacements, key=lambda r: not r[0].startswith("BLOCK:")):
        if start.startswith("BLOCK:"):
            text = replace_block(text, start[6:], new)
        else:
            text = replace_line(text, start, new)
    opening = "public %sclass %s extends android.hardware.camera2.CameraMetadata" % ("final " if "final class " + cls in text else "", cls)
    i = text.index(opening)
    i = text.index("\n", text.index("meglerProps = new java.util.HashMap<>();", i)) + 1
    text = text[:i] + VALUES + extra_fields + text[i:]
    text = text.replace("// Generated by tools/compat/genstubs.py from the platform's framework.jar: signatures and constants only.\n", header(rel, why))
    write(rel, text)


GET = "public java.lang.Object get(android.hardware.camera2.%s.Key p0)"
COMMON = lambda cls: [
    (GET % cls, "    public java.lang.Object get(android.hardware.camera2.%s.Key p0) { return p0 == null ? null : mMeglerValues.get(p0.getName()); }" % cls),
    ("public java.util.List getKeys()", "    public java.util.List getKeys() { return java.util.Collections.unmodifiableList(new java.util.ArrayList<>(mMeglerKeys.values())); }"),
    ("public java.lang.Class getKeyClass()", "    public java.lang.Class getKeyClass() { return Key.class; }"),
    ("protected java.lang.Object getProtected(android.hardware.camera2.%s.Key p0)" % cls,
     "    protected java.lang.Object getProtected(android.hardware.camera2.%s.Key p0) { return get(p0); }" % cls),
    ("public java.lang.Object getProtected(java.lang.Object p0)", "    public java.lang.Object getProtected(java.lang.Object p0) { return p0 instanceof Key ? get((Key) p0) : null; }"),
]

# ------------------------------------------------------------------ CameraCharacteristics
metadata_class("CameraCharacteristics", "CameraCharacteristics.java", """    private java.util.List<CaptureRequest.Key> mMeglerRequestKeys = new java.util.ArrayList<>();
    private java.util.List<CaptureResult.Key> mMeglerResultKeys = new java.util.ArrayList<>();
    /** This layer's: the request and result keys the camera takes and says (getAvailableCaptureRequestKeys, ...ResultKeys). */
    public void meglerSetAvailableKeys(java.util.List<CaptureRequest.Key> request, java.util.List<CaptureResult.Key> result) {
        mMeglerRequestKeys = request;
        mMeglerResultKeys = result;
    }
""", COMMON("CameraCharacteristics") + [
    ("public java.util.List getAvailableCaptureRequestKeys()", "    public java.util.List getAvailableCaptureRequestKeys() { return java.util.Collections.unmodifiableList(mMeglerRequestKeys); }"),
    ("public java.util.List getAvailableCaptureResultKeys()", "    public java.util.List getAvailableCaptureResultKeys() { return java.util.Collections.unmodifiableList(mMeglerResultKeys); }"),
    ("public java.util.List getAvailableSessionKeys()", "    public java.util.List getAvailableSessionKeys() { return java.util.Collections.emptyList(); }"),
], ["It differs in having keys and values: a camera's characteristics as the phone's camera has them (CameraManager, the",
    "overlay's, fills them), which CameraX reads before it opens a camera."])

# ------------------------------------------------------------------ CaptureRequest
BUILDER = """    /** A request being made: its values and its target surfaces, kept until build(). */
    public static final class Builder {
        private final java.util.HashMap<String, Object> meglerProps = new java.util.HashMap<>();
        private final CaptureRequest mRequest = new CaptureRequest();
        public Builder(android.hardware.camera2.impl.CameraMetadataNative p0, boolean p1, int p2, java.lang.String p3, java.util.Set p4) {}
        public void addTarget(android.view.Surface p0) {
            if (p0 == null) throw new IllegalArgumentException("Null target surface");
            mRequest.mMeglerTargets.add(p0);
        }
        public android.hardware.camera2.CaptureRequest build() { return new CaptureRequest(mRequest); }
        public java.lang.Object get(android.hardware.camera2.CaptureRequest.Key p0) { return mRequest.get(p0); }
        public java.lang.Object getPhysicalCameraKey(android.hardware.camera2.CaptureRequest.Key p0, java.lang.String p1) { return mRequest.get(p0); }
        public boolean isEmpty() { return mRequest.mMeglerValues.isEmpty() && mRequest.mMeglerTargets.isEmpty(); }
        public void removeTarget(android.view.Surface p0) { mRequest.mMeglerTargets.remove(p0); }
        public void set(android.hardware.camera2.CaptureRequest.Key p0, java.lang.Object p1) {
            if (p0 == null) throw new IllegalArgumentException("key must not be null");
            mRequest.meglerSet(p0, p1);
        }
        public void setPartOfCHSRequestList(boolean p0) { meglerProps.put("PartOfCHSRequestList", Boolean.valueOf(p0)); }
        public android.hardware.camera2.CaptureRequest.Builder setPhysicalCameraKey(android.hardware.camera2.CaptureRequest.Key p0, java.lang.Object p1, java.lang.String p2) { set(p0, p1); return this; }
        public void setTag(java.lang.Object p0) { mRequest.mMeglerTag = p0; }
        Builder() { this((android.hardware.camera2.impl.CameraMetadataNative) null, (boolean) false, (int) 0, (java.lang.String) null, (java.util.Set) null); }
    }
"""
metadata_class("CaptureRequest", "CaptureRequest.java", """    final java.util.LinkedHashSet<android.view.Surface> mMeglerTargets = new java.util.LinkedHashSet<>();
    java.lang.Object mMeglerTag;
    private CaptureRequest(CaptureRequest from) {
        super();
        mMeglerValues.putAll(from.mMeglerValues);
        mMeglerKeys.putAll(from.mMeglerKeys);
        mMeglerTargets.addAll(from.mMeglerTargets);
        mMeglerTag = from.mMeglerTag;
    }
    /** This layer's: the surfaces this request's pictures go to, in the order they were added. */
    public java.util.List<android.view.Surface> meglerTargets() { return new java.util.ArrayList<>(mMeglerTargets); }
""", COMMON("CaptureRequest") + [
    ("public boolean containsTarget(android.view.Surface p0)", "    public boolean containsTarget(android.view.Surface p0) { return mMeglerTargets.contains(p0); }"),
    ("public java.lang.Object getTag()", "    public java.lang.Object getTag() { return mMeglerTag; }"),
    ("public java.util.Collection getTargets()", "    public java.util.Collection getTargets() { return java.util.Collections.unmodifiableCollection(mMeglerTargets); }"),
    ("public boolean isReprocess()", "    public boolean isReprocess() { return false; }"),
    ("BLOCK:public static final class Builder {", BUILDER),
], ["It differs in having keys, values and targets: a request built by its Builder keeps what was set and the surfaces its",
    "pictures go to, which the camera (camera2/impl/MeglerCameraDevice) reads."])

# ------------------------------------------------------------------ CaptureResult
metadata_class("CaptureResult", "CaptureResult.java", """    private android.hardware.camera2.CaptureRequest mMeglerRequest;
    private long mMeglerFrameNumber;
    private int mMeglerSequenceId;
    private java.lang.String mMeglerCameraId;
    /** This layer's: the camera, request, picture and sequence this result is of. */
    public void meglerFrom(java.lang.String cameraId, android.hardware.camera2.CaptureRequest request, long frameNumber, int sequenceId) {
        mMeglerCameraId = cameraId;
        mMeglerRequest = request;
        mMeglerFrameNumber = frameNumber;
        mMeglerSequenceId = sequenceId;
    }
""", COMMON("CaptureResult") + [
    ("public java.lang.String getCameraId()", "    public java.lang.String getCameraId() { return mMeglerCameraId; }"),
    ("public long getFrameNumber()", "    public long getFrameNumber() { return mMeglerFrameNumber; }"),
    ("public android.hardware.camera2.CaptureRequest getRequest()", "    public android.hardware.camera2.CaptureRequest getRequest() { return mMeglerRequest; }"),
    ("public int getSequenceId()", "    public int getSequenceId() { return mMeglerSequenceId; }"),
], ["It differs in having keys and values: a picture's result as the camera (camera2/impl/MeglerCameraDevice) fills it, the",
    "request's values and the picture's own (its timestamp, the state of focus and exposure)."])

# ------------------------------------------------------------------ CameraCaptureSession
text = read("CameraCaptureSession.java")
text = replace_block(text, "public static abstract class StateCallback {", """    /** What becomes of a session: configured or not, ready, active, closed. */
    public static abstract class StateCallback {
        public StateCallback() {}
        public abstract void onConfigured(android.hardware.camera2.CameraCaptureSession session);
        public abstract void onConfigureFailed(android.hardware.camera2.CameraCaptureSession session);
        public void onReady(android.hardware.camera2.CameraCaptureSession session) {}
        public void onActive(android.hardware.camera2.CameraCaptureSession session) {}
        public void onCaptureQueueEmpty(android.hardware.camera2.CameraCaptureSession session) {}
        public void onClosed(android.hardware.camera2.CameraCaptureSession session) {}
        public void onSurfacePrepared(android.hardware.camera2.CameraCaptureSession session, android.view.Surface surface) {}
    }
""")
text = text.replace("// Generated by tools/compat/genstubs.py from the platform's framework.jar: signatures and constants only.\n",
                    header("CameraCaptureSession.java", ["It differs in its StateCallback, which the stub left without methods: a session's onConfigured, onReady,",
                                                         "onClosed and the rest, which the camera calls and an app overrides."]))
write("CameraCaptureSession.java", text)

# ------------------------------------------------------------------ params.StreamConfigurationMap
text = read("params/StreamConfigurationMap.java")
for start, new in [
    ("public static boolean isOutputSupportedFor(java.lang.Class p0)",
     "    public static boolean isOutputSupportedFor(java.lang.Class p0) { return p0 == android.graphics.SurfaceTexture.class || p0 == android.view.SurfaceHolder.class || p0 == android.media.ImageReader.class; }"),
    ("public android.util.Size[] getHighResolutionOutputSizes(int p0)", "    public android.util.Size[] getHighResolutionOutputSizes(int p0) { return null; }"),
    ("public android.util.Range[] getHighSpeedVideoFpsRanges()", "    public android.util.Range[] getHighSpeedVideoFpsRanges() { return new android.util.Range[0]; }"),
    ("public android.util.Size[] getHighSpeedVideoSizes()", "    public android.util.Size[] getHighSpeedVideoSizes() { return new android.util.Size[0]; }"),
    ("public int[] getInputFormats()", "    public int[] getInputFormats() { return new int[0]; }"),
    ("public android.util.Size[] getInputSizes(int p0)", "    public android.util.Size[] getInputSizes(int p0) { return null; }"),
    ("public int[] getOutputFormats()", "    public int[] getOutputFormats() { return MEGLER_FORMATS.clone(); }"),
    ("public long getOutputMinFrameDuration(int p0, android.util.Size p1)", "    public long getOutputMinFrameDuration(int p0, android.util.Size p1) { return meglerHas(p1) ? mMeglerMinFrameNs : 0L; }"),
    ("public long getOutputMinFrameDuration(java.lang.Class p0, android.util.Size p1)", "    public long getOutputMinFrameDuration(java.lang.Class p0, android.util.Size p1) { return meglerHas(p1) ? mMeglerMinFrameNs : 0L; }"),
    ("public android.util.Size[] getOutputSizes(int p0)", "    public android.util.Size[] getOutputSizes(int p0) { return isOutputSupportedFor(p0) ? mMeglerSizes.clone() : null; }"),
    ("public android.util.Size[] getOutputSizes(java.lang.Class p0)", "    public android.util.Size[] getOutputSizes(java.lang.Class p0) { return isOutputSupportedFor(p0) ? mMeglerSizes.clone() : null; }"),
    ("public long getOutputStallDuration(int p0, android.util.Size p1)", "    public long getOutputStallDuration(int p0, android.util.Size p1) { return 0L; }"),
    ("public long getOutputStallDuration(java.lang.Class p0, android.util.Size p1)", "    public long getOutputStallDuration(java.lang.Class p0, android.util.Size p1) { return 0L; }"),
    ("public int[] getValidOutputFormatsForInput(int p0)", "    public int[] getValidOutputFormatsForInput(int p0) { return new int[0]; }"),
    ("public boolean isOutputSupportedFor(int p0)", "    public boolean isOutputSupportedFor(int p0) { for (int f : MEGLER_FORMATS) if (f == p0) return true; return false; }"),
    ("public boolean isOutputSupportedFor(android.util.Size p0, int p1)", "    public boolean isOutputSupportedFor(android.util.Size p0, int p1) { return isOutputSupportedFor(p1) && meglerHas(p0); }"),
    ("public boolean isOutputSupportedFor(android.view.Surface p0)", "    public boolean isOutputSupportedFor(android.view.Surface p0) { return p0 != null; }"),
]:
    text = replace_line(text, start, new)
i = text.index("\n", text.index("meglerProps = new java.util.HashMap<>();")) + 1
text = text[:i] + """    // ---- megler-w10m: the camera's sizes, the largest first, the same for every format it gives; and its fastest picture.
    /** YUV_420_888, PRIVATE (a SurfaceTexture's or a SurfaceView's), JPEG (said, so that a size is there to choose; not taken) and NV21. */
    private static final int[] MEGLER_FORMATS = { 35, 34, 256, 17 };
    private android.util.Size[] mMeglerSizes = new android.util.Size[0];
    private long mMeglerMinFrameNs;
    /** This layer's: a map of these sizes in every format it gives, each at most 1e9 / `maxFps` ns a picture. */
    public static StreamConfigurationMap meglerOf(android.util.Size[] sizes, int maxFps) {
        StreamConfigurationMap m = new StreamConfigurationMap();
        m.mMeglerSizes = sizes.clone();
        m.mMeglerMinFrameNs = 1000000000L / (maxFps > 0 ? maxFps : 30);
        return m;
    }
    private boolean meglerHas(android.util.Size s) {
        for (android.util.Size x : mMeglerSizes) if (x.equals(s)) return true;
        return false;
    }
""" + text[i:]
text = text.replace("// Generated by tools/compat/genstubs.py from the platform's framework.jar: signatures and constants only.\n",
                    header("params/StreamConfigurationMap.java", ["It differs in saying the camera's sizes (meglerOf), for YUV_420_888, PRIVATE, JPEG and NV21 alike, where the",
                                                                  "stub said null, which CameraX takes for a camera it cannot use."]))
write("params/StreamConfigurationMap.java", text)

# ------------------------------------------------------------------ params.OutputConfiguration
text = read("params/OutputConfiguration.java")
SURFACES = 'java.util.ArrayList<android.view.Surface> l = new java.util.ArrayList<>(); if (p%s != null) l.add(p%s); meglerProps.put("Surfaces", l); meglerProps.put("Surface", p%s); meglerProps.put("SurfaceGroupId", Integer.valueOf(%s));'
for start, new in [
    ("public OutputConfiguration(int p0, android.view.Surface p1) {}", "    public OutputConfiguration(int p0, android.view.Surface p1) { %s }" % (SURFACES % (1, 1, 1, "p0"))),
    ("public OutputConfiguration(int p0, android.view.Surface p1, int p2) {}", "    public OutputConfiguration(int p0, android.view.Surface p1, int p2) { %s meglerProps.put(\"Rotation\", Integer.valueOf(p2)); }" % (SURFACES % (1, 1, 1, "p0"))),
    ("public OutputConfiguration(android.view.Surface p0) {}", "    public OutputConfiguration(android.view.Surface p0) { %s }" % (SURFACES % (0, 0, 0, "SURFACE_GROUP_ID_NONE"))),
    ("public OutputConfiguration(android.view.Surface p0, int p1) {}", "    public OutputConfiguration(android.view.Surface p0, int p1) { %s meglerProps.put(\"Rotation\", Integer.valueOf(p1)); }" % (SURFACES % (0, 0, 0, "SURFACE_GROUP_ID_NONE"))),
    ("public OutputConfiguration(android.util.Size p0, java.lang.Class p1) {}",
     '    public OutputConfiguration(android.util.Size p0, java.lang.Class p1) { meglerProps.put("Surfaces", new java.util.ArrayList<android.view.Surface>()); meglerProps.put("ConfiguredSize", p0); meglerProps.put("DeferredConfiguration", Boolean.TRUE); meglerProps.put("SurfaceGroupId", Integer.valueOf(SURFACE_GROUP_ID_NONE)); }'),
    ("public OutputConfiguration(android.hardware.camera2.params.OutputConfiguration p0) {}",
     '    public OutputConfiguration(android.hardware.camera2.params.OutputConfiguration p0) { if (p0 != null) { meglerProps.putAll(p0.meglerProps); meglerProps.put("Surfaces", new java.util.ArrayList<>(p0.getSurfaces())); } else { meglerProps.put("Surfaces", new java.util.ArrayList<android.view.Surface>()); } }'),
    ("public void addSurface(android.view.Surface p0) {}",
     '    public void addSurface(android.view.Surface p0) { if (p0 == null) throw new IllegalArgumentException("Surface must not be null"); java.util.List l = getSurfaces(); if (!l.contains(p0)) l.add(p0); meglerProps.put("Surfaces", l); if (meglerProps.get("Surface") == null) meglerProps.put("Surface", p0); }'),
    ("public void removeSurface(android.view.Surface p0) {}",
     '    public void removeSurface(android.view.Surface p0) { java.util.List l = getSurfaces(); l.remove(p0); meglerProps.put("Surfaces", l); if (p0 == meglerProps.get("Surface")) meglerProps.put("Surface", l.isEmpty() ? null : l.get(0)); }'),
    ("public int getMaxSharedSurfaceCount()", "    public int getMaxSharedSurfaceCount() { return 1; }"),
]:
    text = replace_line(text, start, new)
text = text.replace("// Generated by tools/compat/genstubs.py from the platform's framework.jar: signatures and constants only.\n",
                    header("params/OutputConfiguration.java", ["It differs in keeping its surfaces (or a deferred one's size) and its group, which the stub's constructors",
                                                               "dropped: a session's outputs are read from them."]))
write("params/OutputConfiguration.java", text)

# ------------------------------------------------------------------ params.SessionConfiguration
text = read("params/SessionConfiguration.java")
for start, new in [
    ("public SessionConfiguration(int p0, java.util.List p1) {}",
     '    public SessionConfiguration(int p0, java.util.List p1) { meglerProps.put("SessionType", Integer.valueOf(p0)); meglerProps.put("OutputConfigurations", p1 != null ? new java.util.ArrayList<>(p1) : new java.util.ArrayList<>()); }'),
    ("public SessionConfiguration(int p0, java.util.List p1, java.util.concurrent.Executor p2, android.hardware.camera2.CameraCaptureSession.StateCallback p3) {}",
     '    public SessionConfiguration(int p0, java.util.List p1, java.util.concurrent.Executor p2, android.hardware.camera2.CameraCaptureSession.StateCallback p3) { this(p0, p1); meglerProps.put("Executor", p2); meglerProps.put("StateCallback", p3); }'),
    ("public void setStateCallback(java.util.concurrent.Executor p0, android.hardware.camera2.CameraCaptureSession.StateCallback p1) {}",
     '    public void setStateCallback(java.util.concurrent.Executor p0, android.hardware.camera2.CameraCaptureSession.StateCallback p1) { meglerProps.put("Executor", p0); meglerProps.put("StateCallback", p1); }'),
]:
    text = replace_line(text, start, new)
text = text.replace("// Generated by tools/compat/genstubs.py from the platform's framework.jar: signatures and constants only.\n",
                    header("params/SessionConfiguration.java", ["It differs in keeping its type, outputs, executor and callback, which the stub's constructors dropped."]))
write("params/SessionConfiguration.java", text)
