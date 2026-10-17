// megler-w10m overlay of Megler/src/java-framework/src/android/media/MediaCodecList.java (tools/build-framework.sh).
// It differs in infos(): every decoder is listed only when megler.MediaCodecNative.supported says its type can be decoded;
// the original listed H.264, AAC and MP3 always and asked only for HEVC.
//
// Why. The list must be what MediaCodec can make, and here that is whatever the shell's decoders are (megler_codec_host in
// megler.h; core/src/drivers/hk_java_media.c asks it in supported()), which a phone may lack one of. With none the list is
// empty, which Android allows: an app finds no decoder and createDecoderByType throws IOException, as on a phone without
// one. Before, AIDA64's Codecs page listed three decoders that could not be made. Everything else is the original's,
// unchanged.
package android.media;

import java.util.ArrayList;

/** The decoders Megler has (see MediaCodecInfo): H.264 and HEVC video, AAC and MP3 audio. No encoders. */
public final class MediaCodecList {
    public static final int REGULAR_CODECS = 0, ALL_CODECS = 1;
    private static MediaCodecInfo[] sInfos;
    static synchronized MediaCodecInfo[] infos() {
        if (sInfos == null) {
            int[][] avc = {
                { MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedBaseline, MediaCodecInfo.CodecProfileLevel.AVCLevel52 },
                { MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline, MediaCodecInfo.CodecProfileLevel.AVCLevel52 },
                { MediaCodecInfo.CodecProfileLevel.AVCProfileMain, MediaCodecInfo.CodecProfileLevel.AVCLevel52 },
                { MediaCodecInfo.CodecProfileLevel.AVCProfileConstrainedHigh, MediaCodecInfo.CodecProfileLevel.AVCLevel52 },
                { MediaCodecInfo.CodecProfileLevel.AVCProfileHigh, MediaCodecInfo.CodecProfileLevel.AVCLevel52 },
            };
            int[][] hevc = {
                { MediaCodecInfo.CodecProfileLevel.HEVCProfileMain, MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel51 },
                { MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10, MediaCodecInfo.CodecProfileLevel.HEVCMainTierLevel51 },
            };
            ArrayList<MediaCodecInfo> l = new ArrayList<>();
            if (megler.MediaCodecNative.supported(MediaFormat.MIMETYPE_VIDEO_AVC))
                l.add(new MediaCodecInfo("c2.megler.avc.decoder", false, MediaCodecInfo.CodecCapabilities.video(MediaFormat.MIMETYPE_VIDEO_AVC, avc)));
            if (megler.MediaCodecNative.supported(MediaFormat.MIMETYPE_VIDEO_HEVC))
                l.add(new MediaCodecInfo("c2.megler.hevc.decoder", false, MediaCodecInfo.CodecCapabilities.video(MediaFormat.MIMETYPE_VIDEO_HEVC, hevc)));
            if (megler.MediaCodecNative.supported(MediaFormat.MIMETYPE_AUDIO_AAC))
                l.add(new MediaCodecInfo("c2.megler.aac.decoder", false, MediaCodecInfo.CodecCapabilities.audio(MediaFormat.MIMETYPE_AUDIO_AAC, 8,
                    new int[] { MediaCodecInfo.CodecProfileLevel.AACObjectLC, MediaCodecInfo.CodecProfileLevel.AACObjectHE, MediaCodecInfo.CodecProfileLevel.AACObjectHE_PS })));
            if (megler.MediaCodecNative.supported(MediaFormat.MIMETYPE_AUDIO_MPEG))
                l.add(new MediaCodecInfo("c2.megler.mp3.decoder", false, MediaCodecInfo.CodecCapabilities.audio(MediaFormat.MIMETYPE_AUDIO_MPEG, 2, new int[0])));
            sInfos = l.toArray(new MediaCodecInfo[0]);
        }
        return sInfos;
    }
    public MediaCodecList(int kind) {}
    public static int getCodecCount() { return infos().length; }
    public static MediaCodecInfo getCodecInfoAt(int i) { MediaCodecInfo[] a = infos(); if (i < 0 || i >= a.length) throw new IllegalArgumentException(); return a[i]; }
    public static MediaCodecInfo getInfoFor(String name) { for (MediaCodecInfo i : infos()) if (i.getName().equals(name)) return i; return null; }
    public MediaCodecInfo[] getCodecInfos() { return infos().clone(); }
    public String findDecoderForFormat(MediaFormat f) {
        String mime = f.getString(MediaFormat.KEY_MIME);
        if (mime == null) return null;
        for (MediaCodecInfo i : infos()) {
            for (String t : i.getSupportedTypes()) if (t.equalsIgnoreCase(mime) && i.getCapabilitiesForType(t).isFormatSupported(f)) return i.getName();
        }
        return null;
    }
    public String findEncoderForFormat(MediaFormat f) { return null; }
}
