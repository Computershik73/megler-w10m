package megler.test.crash;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;

/** Over Hub: starts Hub again, with no flag (Hub's comment), and is finished for it. */
public class Over extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Log.i("CrashTest", "over onCreate");
        new Handler().post(() -> startActivity(new Intent(this, Hub.class).putExtra("which", "again")));
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("CrashTest", "over onDestroy finishing=" + isFinishing());
    }
}
