package megler.test.video;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.media.MediaPlayer;
import android.widget.VideoView;
import android.util.Log;
import java.io.*;

/** On-device integration checks: actual JNI, source reader, mixer and Surface. */
public final class Main extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private VideoView video;
    private int phase, checks, failures;
    private boolean rendered, finished;
    private void check(boolean ok, String what) {
        checks++; if (!ok) failures++;
        Log.i("VideoChecks", (ok ? "PASS " : "FAIL ") + what);
    }
    private void finishChecks() {
        if (finished) return;
        finished = true;
        video.stopPlayback();
        Log.i("VideoChecks", "VIDEO DONE: " + checks + " checks, " + failures + " failed");
        handler.postDelayed(this::finish, 300);
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        video = new VideoView(this); setContentView(video);
        video.setOnErrorListener((p,w,e) -> { check(false,"player error " + w + "/" + e); finishChecks(); return true; });
        video.setOnInfoListener((p,w,e) -> { if(w == MediaPlayer.MEDIA_INFO_VIDEO_RENDERING_START) rendered = true; return false; });
        video.setOnPreparedListener(p -> {
            check(p.getVideoWidth()==480 && p.getVideoHeight()==270,"dimensions phase " + phase);
            check(p.getDuration()>=1500,"duration phase " + phase);
            if(phase==0) handler.postDelayed(() -> {
                if(finished || phase!=0) return;
                video.pause(); final int at=video.getCurrentPosition();
                handler.postDelayed(() -> {
                    check(Math.abs(video.getCurrentPosition()-at)<50,"paused position stable");
                    p.setOnSeekCompleteListener(mp -> {
                        check(Math.abs(mp.getCurrentPosition()-1000)<100,"paused seek completed");
                        p.setOnSeekCompleteListener(null); video.start();
                    });
                    video.seekTo(1000);
                },200);
            },400);
        });
        video.setOnCompletionListener(p -> {
            check(rendered,"Surface received frames phase " + phase);
            if(++phase<2) play(); else finishChecks();
        });
        play();
        handler.postDelayed(() -> { if(!finished) { check(false,"integration timeout"); finishChecks(); } },30000);
    }
    private void play() {
        rendered=false;
        String name=phase==0?"av.mp4":"video-only.mp4";
        try {
            File file=new File(getCacheDir(),name);
            try(InputStream in=getAssets().open(name); OutputStream out=new FileOutputStream(file)) {
                byte[] b=new byte[65536]; int n; while((n=in.read(b))!=-1)out.write(b,0,n);
            }
            Log.i("VideoChecks","starting " + name);
            video.setVideoPath(file.getPath()); video.start();
        } catch(IOException e) { check(false,"source: " + e); finishChecks(); }
    }
}
