#!/usr/bin/env python3
"""
Writes the overlays of android.hardware.camera2's metadata classes and parameters (core/framework-overlay/android/hardware/
camera2) from the framework's generated stubs (core/framework/gen-src), which have every member's signature and no
behaviour: a key field there is null, get() answers null, a session's StateCallback has no methods at all.

What is made:
  CameraCharacteristics, CaptureRequest, CaptureResult: each key field a real Key, named as Android names it
    (LENS_FACING is "android.lens.facing", SENSOR_INFO_ACTIVE_ARRAY_SIZE "android.sensor.info.activeArraySize"), equal to
    another of the same name; the values kept by name; get, getKeys, a request's Builder (set, targets, tag, build), and
    methods of this layer's own (megletSet, megletFrom) through which the camera (camera2/impl/MegletCameraDevice) fills them.
  CameraCaptureSession: the StateCallback's methods, which the stub has not.
  params.StreamConfigurationMap, OutputConfiguration, SessionConfiguration: what was given kept and said back.
Every other member is the stub's, unchanged. Run again after the stubs change:

    python tools/compat/camera2-overlays.py
"""
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FW = os.path.join(ROOT, "core", "framework")
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
    lines = ["// Overlay of core/framework/gen-src/android/hardware/camera2/%s, written by" % rel,
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

VALUES = """    // ---- Meglet: the values, by their keys' names, and the keys themselves in the order they were set.
    private final java.util.LinkedHashMap<java.lang.String, java.lang.Object> mMegletValues = new java.util.LinkedHashMap<>();
    private final java.util.LinkedHashMap<java.lang.String, Key> mMegletKeys = new java.util.LinkedHashMap<>();
    /** This layer's: a key's value set (null takes it away). */
    public void megletSet(Key key, java.lang.Object value) {
        if (key == null) return;
        if (value == null) {
            mMegletValues.remove(key.getName());
            mMegletKeys.remove(key.getName());
        } else {
            mMegletValues.put(key.getName(), value);
            mMegletKeys.put(key.getName(), key);
        }
    }
    /** This layer's: the value of the key of this name, or null. */
    public java.lang.Object megletGet(java.lang.String name) { return mMegletValues.get(name); }
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
    i = text.index("\n", text.index("megletProps = new java.util.HashMap<>();", i)) + 1
    text = text[:i] + VALUES + extra_fields + text[i:]
    text = text.replace("// Generated by tools/compat/genstubs.py from the platform's framework.jar: signatures and constants only.\n", header(rel, why))
    write(rel, text)


GET = "public java.lang.Object get(android.hardware.camera2.%s.Key p0)"
COMMON = lambda cls: [
    (GET % cls, "    public java.lang.Object get(android.hardware.camera2.%s.Key p0) { return p0 == null ? null : mMegletValues.get(p0.getName()); }" % cls),
    ("public java.util.List getKeys()", "    public java.util.List getKeys() { return java.util.Collections.unmodifiableList(new java.util.ArrayList<>(mMegletKeys.values())); }"),
    ("public java.lang.Class getKeyClass()", "    public java.lang.Class getKeyClass() { return Key.class; }"),
    ("protected java.lang.Object getProtected(android.hardware.camera2.%s.Key p0)" % cls,
     "    protected java.lang.Object getProtected(android.hardware.camera2.%s.Key p0) { return get(p0); }" % cls),
    ("public java.lang.Object getProtected(java.lang.Object p0)", "    public java.lang.Object getProtected(java.lang.Object p0) { return p0 instanceof Key ? get((Key) p0) : null; }"),
]

# ------------------------------------------------------------------ CameraCharacteristics
metadata_class("CameraCharacteristics", "CameraCharacteristics.java", """    private java.util.List<CaptureRequest.Key> mMegletRequestKeys = new java.util.ArrayList<>();
    private java.util.List<CaptureResult.Key> mMegletResultKeys = new java.util.ArrayList<>();
    /** This layer's: the request and result keys the camera takes and says (getAvailableCaptureRequestKeys, ...ResultKeys). */
    public void megletSetAvailableKeys(java.util.List<CaptureRequest.Key> request, java.util.List<CaptureResult.Key> result) {
        mMegletRequestKeys = request;
        mMegletResultKeys = result;
    }
""", COMMON("CameraCharacteristics") + [
    ("public java.util.List getAvailableCaptureRequestKeys()", "    public java.util.List getAvailableCaptureRequestKeys() { return java.util.Collections.unmodifiableList(mMegletRequestKeys); }"),
    ("public java.util.List getAvailableCaptureResultKeys()", "    public java.util.List getAvailableCaptureResultKeys() { return java.util.Collections.unmodifiableList(mMegletResultKeys); }"),
    ("public java.util.List getAvailableSessionKeys()", "    public java.util.List getAvailableSessionKeys() { return java.util.Collections.emptyList(); }"),
], ["It differs in having keys and values: a camera's characteristics as the phone's camera has them (CameraManager, the",
    "overlay's, fills them), which CameraX reads before it opens a camera."])

# ------------------------------------------------------------------ CaptureRequest
BUILDER = """    /** A request being made: its values and its target surfaces, kept until build(). */
    public static final class Builder {
        private final java.util.HashMap<String, Object> megletProps = new java.util.HashMap<>();
        private final CaptureRequest mRequest = new CaptureRequest();
        public Builder(android.hardware.camera2.impl.CameraMetadataNative p0, boolean p1, int p2, java.lang.String p3, java.util.Set p4) {}
        public void addTarget(android.view.Surface p0) {
            if (p0 == null) throw new IllegalArgumentException("Null target surface");
            mRequest.mMegletTargets.add(p0);
        }
        public android.hardware.camera2.CaptureRequest build() { return new CaptureRequest(mRequest); }
        public java.lang.Object get(android.hardware.camera2.CaptureRequest.Key p0) { return mRequest.get(p0); }
        public java.lang.Object getPhysicalCameraKey(android.hardware.camera2.CaptureRequest.Key p0, java.lang.String p1) { return mRequest.get(p0); }
        public boolean isEmpty() { return mRequest.mMegletValues.isEmpty() && mRequest.mMegletTargets.isEmpty(); }
        public void removeTarget(android.view.Surface p0) { mRequest.mMegletTargets.remove(p0); }
        public void set(android.hardware.camera2.CaptureRequest.Key p0, java.lang.Object p1) {
            if (p0 == null) throw new IllegalArgumentException("key must not be null");
            mRequest.megletSet(p0, p1);
        }
        public void setPartOfCHSRequestList(boolean p0) { megletProps.put("PartOfCHSRequestList", Boolean.valueOf(p0)); }
        public android.hardware.camera2.CaptureRequest.Builder setPhysicalCameraKey(android.hardware.camera2.CaptureRequest.Key p0, java.lang.Object p1, java.lang.String p2) { set(p0, p1); return this; }
        public void setTag(java.lang.Object p0) { mRequest.mMegletTag = p0; }
        Builder() { this((android.hardware.camera2.impl.CameraMetadataNative) null, (boolean) false, (int) 0, (java.lang.String) null, (java.util.Set) null); }
    }
"""
metadata_class("CaptureRequest", "CaptureRequest.java", """    final java.util.LinkedHashSet<android.view.Surface> mMegletTargets = new java.util.LinkedHashSet<>();
    java.lang.Object mMegletTag;
    private CaptureRequest(CaptureRequest from) {
        super();
        mMegletValues.putAll(from.mMegletValues);
        mMegletKeys.putAll(from.mMegletKeys);
        mMegletTargets.addAll(from.mMegletTargets);
        mMegletTag = from.mMegletTag;
    }
    /** This layer's: the surfaces this request's pictures go to, in the order they were added. */
    public java.util.List<android.view.Surface> megletTargets() { return new java.util.ArrayList<>(mMegletTargets); }
""", COMMON("CaptureRequest") + [
    ("public boolean containsTarget(android.view.Surface p0)", "    public boolean containsTarget(android.view.Surface p0) { return mMegletTargets.contains(p0); }"),
    ("public java.lang.Object getTag()", "    public java.lang.Object getTag() { return mMegletTag; }"),
    ("public java.util.Collection getTargets()", "    public java.util.Collection getTargets() { return java.util.Collections.unmodifiableCollection(mMegletTargets); }"),
    ("public boolean isReprocess()", "    public boolean isReprocess() { return false; }"),
    ("BLOCK:public static final class Builder {", BUILDER),
], ["It differs in having keys, values and targets: a request built by its Builder keeps what was set and the surfaces its",
    "pictures go to, which the camera (camera2/impl/MegletCameraDevice) reads."])

# ------------------------------------------------------------------ CaptureResult
metadata_class("CaptureResult", "CaptureResult.java", """    private android.hardware.camera2.CaptureRequest mMegletRequest;
    private long mMegletFrameNumber;
    private int mMegletSequenceId;
    private java.lang.String mMegletCameraId;
    /** This layer's: the camera, request, picture and sequence this result is of. */
    public void megletFrom(java.lang.String cameraId, android.hardware.camera2.CaptureRequest request, long frameNumber, int sequenceId) {
        mMegletCameraId = cameraId;
        mMegletRequest = request;
        mMegletFrameNumber = frameNumber;
        mMegletSequenceId = sequenceId;
    }
""", COMMON("CaptureResult") + [
    ("public java.lang.String getCameraId()", "    public java.lang.String getCameraId() { return mMegletCameraId; }"),
    ("public long getFrameNumber()", "    public long getFrameNumber() { return mMegletFrameNumber; }"),
    ("public android.hardware.camera2.CaptureRequest getRequest()", "    public android.hardware.camera2.CaptureRequest getRequest() { return mMegletRequest; }"),
    ("public int getSequenceId()", "    public int getSequenceId() { return mMegletSequenceId; }"),
], ["It differs in having keys and values: a picture's result as the camera (camera2/impl/MegletCameraDevice) fills it, the",
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
    ("public int[] getOutputFormats()", "    public int[] getOutputFormats() { return MEGLET_FORMATS.clone(); }"),
    ("public long getOutputMinFrameDuration(int p0, android.util.Size p1)", "    public long getOutputMinFrameDuration(int p0, android.util.Size p1) { return megletHas(p1) ? mMegletMinFrameNs : 0L; }"),
    ("public long getOutputMinFrameDuration(java.lang.Class p0, android.util.Size p1)", "    public long getOutputMinFrameDuration(java.lang.Class p0, android.util.Size p1) { return megletHas(p1) ? mMegletMinFrameNs : 0L; }"),
    ("public android.util.Size[] getOutputSizes(int p0)", "    public android.util.Size[] getOutputSizes(int p0) { return isOutputSupportedFor(p0) ? mMegletSizes.clone() : null; }"),
    ("public android.util.Size[] getOutputSizes(java.lang.Class p0)", "    public android.util.Size[] getOutputSizes(java.lang.Class p0) { return isOutputSupportedFor(p0) ? mMegletSizes.clone() : null; }"),
    ("public long getOutputStallDuration(int p0, android.util.Size p1)", "    public long getOutputStallDuration(int p0, android.util.Size p1) { return 0L; }"),
    ("public long getOutputStallDuration(java.lang.Class p0, android.util.Size p1)", "    public long getOutputStallDuration(java.lang.Class p0, android.util.Size p1) { return 0L; }"),
    ("public int[] getValidOutputFormatsForInput(int p0)", "    public int[] getValidOutputFormatsForInput(int p0) { return new int[0]; }"),
    ("public boolean isOutputSupportedFor(int p0)", "    public boolean isOutputSupportedFor(int p0) { for (int f : MEGLET_FORMATS) if (f == p0) return true; return false; }"),
    ("public boolean isOutputSupportedFor(android.util.Size p0, int p1)", "    public boolean isOutputSupportedFor(android.util.Size p0, int p1) { return isOutputSupportedFor(p1) && megletHas(p0); }"),
    ("public boolean isOutputSupportedFor(android.view.Surface p0)", "    public boolean isOutputSupportedFor(android.view.Surface p0) { return p0 != null; }"),
]:
    text = replace_line(text, start, new)
i = text.index("\n", text.index("megletProps = new java.util.HashMap<>();")) + 1
text = text[:i] + """    // ---- Meglet: the camera's sizes, the largest first, the same for every format it gives; and its fastest picture.
    /** YUV_420_888, PRIVATE (a SurfaceTexture's or a SurfaceView's), JPEG (said, so that a size is there to choose; not taken) and NV21. */
    private static final int[] MEGLET_FORMATS = { 35, 34, 256, 17 };
    private android.util.Size[] mMegletSizes = new android.util.Size[0];
    private long mMegletMinFrameNs;
    /** This layer's: a map of these sizes in every format it gives, each at most 1e9 / `maxFps` ns a picture. */
    public static StreamConfigurationMap megletOf(android.util.Size[] sizes, int maxFps) {
        StreamConfigurationMap m = new StreamConfigurationMap();
        m.mMegletSizes = sizes.clone();
        m.mMegletMinFrameNs = 1000000000L / (maxFps > 0 ? maxFps : 30);
        return m;
    }
    private boolean megletHas(android.util.Size s) {
        for (android.util.Size x : mMegletSizes) if (x.equals(s)) return true;
        return false;
    }
""" + text[i:]
text = text.replace("// Generated by tools/compat/genstubs.py from the platform's framework.jar: signatures and constants only.\n",
                    header("params/StreamConfigurationMap.java", ["It differs in saying the camera's sizes (megletOf), for YUV_420_888, PRIVATE, JPEG and NV21 alike, where the",
                                                                  "stub said null, which CameraX takes for a camera it cannot use."]))
write("params/StreamConfigurationMap.java", text)

# ------------------------------------------------------------------ params.OutputConfiguration
text = read("params/OutputConfiguration.java")
SURFACES = 'java.util.ArrayList<android.view.Surface> l = new java.util.ArrayList<>(); if (p%s != null) l.add(p%s); megletProps.put("Surfaces", l); megletProps.put("Surface", p%s); megletProps.put("SurfaceGroupId", Integer.valueOf(%s));'
for start, new in [
    ("public OutputConfiguration(int p0, android.view.Surface p1) {}", "    public OutputConfiguration(int p0, android.view.Surface p1) { %s }" % (SURFACES % (1, 1, 1, "p0"))),
    ("public OutputConfiguration(int p0, android.view.Surface p1, int p2) {}", "    public OutputConfiguration(int p0, android.view.Surface p1, int p2) { %s megletProps.put(\"Rotation\", Integer.valueOf(p2)); }" % (SURFACES % (1, 1, 1, "p0"))),
    ("public OutputConfiguration(android.view.Surface p0) {}", "    public OutputConfiguration(android.view.Surface p0) { %s }" % (SURFACES % (0, 0, 0, "SURFACE_GROUP_ID_NONE"))),
    ("public OutputConfiguration(android.view.Surface p0, int p1) {}", "    public OutputConfiguration(android.view.Surface p0, int p1) { %s megletProps.put(\"Rotation\", Integer.valueOf(p1)); }" % (SURFACES % (0, 0, 0, "SURFACE_GROUP_ID_NONE"))),
    ("public OutputConfiguration(android.util.Size p0, java.lang.Class p1) {}",
     '    public OutputConfiguration(android.util.Size p0, java.lang.Class p1) { megletProps.put("Surfaces", new java.util.ArrayList<android.view.Surface>()); megletProps.put("ConfiguredSize", p0); megletProps.put("DeferredConfiguration", Boolean.TRUE); megletProps.put("SurfaceGroupId", Integer.valueOf(SURFACE_GROUP_ID_NONE)); }'),
    ("public OutputConfiguration(android.hardware.camera2.params.OutputConfiguration p0) {}",
     '    public OutputConfiguration(android.hardware.camera2.params.OutputConfiguration p0) { if (p0 != null) { megletProps.putAll(p0.megletProps); megletProps.put("Surfaces", new java.util.ArrayList<>(p0.getSurfaces())); } else { megletProps.put("Surfaces", new java.util.ArrayList<android.view.Surface>()); } }'),
    ("public void addSurface(android.view.Surface p0) {}",
     '    public void addSurface(android.view.Surface p0) { if (p0 == null) throw new IllegalArgumentException("Surface must not be null"); java.util.List l = getSurfaces(); if (!l.contains(p0)) l.add(p0); megletProps.put("Surfaces", l); if (megletProps.get("Surface") == null) megletProps.put("Surface", p0); }'),
    ("public void removeSurface(android.view.Surface p0) {}",
     '    public void removeSurface(android.view.Surface p0) { java.util.List l = getSurfaces(); l.remove(p0); megletProps.put("Surfaces", l); if (p0 == megletProps.get("Surface")) megletProps.put("Surface", l.isEmpty() ? null : l.get(0)); }'),
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
     '    public SessionConfiguration(int p0, java.util.List p1) { megletProps.put("SessionType", Integer.valueOf(p0)); megletProps.put("OutputConfigurations", p1 != null ? new java.util.ArrayList<>(p1) : new java.util.ArrayList<>()); }'),
    ("public SessionConfiguration(int p0, java.util.List p1, java.util.concurrent.Executor p2, android.hardware.camera2.CameraCaptureSession.StateCallback p3) {}",
     '    public SessionConfiguration(int p0, java.util.List p1, java.util.concurrent.Executor p2, android.hardware.camera2.CameraCaptureSession.StateCallback p3) { this(p0, p1); megletProps.put("Executor", p2); megletProps.put("StateCallback", p3); }'),
    ("public void setStateCallback(java.util.concurrent.Executor p0, android.hardware.camera2.CameraCaptureSession.StateCallback p1) {}",
     '    public void setStateCallback(java.util.concurrent.Executor p0, android.hardware.camera2.CameraCaptureSession.StateCallback p1) { megletProps.put("Executor", p0); megletProps.put("StateCallback", p1); }'),
]:
    text = replace_line(text, start, new)
text = text.replace("// Generated by tools/compat/genstubs.py from the platform's framework.jar: signatures and constants only.\n",
                    header("params/SessionConfiguration.java", ["It differs in keeping its type, outputs, executor and callback, which the stub's constructors dropped."]))
write("params/SessionConfiguration.java", text)
