package meglet.test.demos;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.View;

/** Started by an Intent with extras; says where its button is, and answers with a result and finishes when the button is pressed. */
public class Second extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(R.layout.second);
        final int number = getIntent().getIntExtra("number", -1);
        Log.i("DemoTest", "second: created with number=" + number + " text=" + getIntent().getStringExtra("text"));
        final View.OnClickListener done = new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                setResult(RESULT_OK, new Intent().putExtra("answer", number * 2));
                finish();
            }
        };
        findViewById(R.id.back).setOnClickListener(done);
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (!focus) return;
        View back = findViewById(R.id.back);
        int[] at = new int[2];
        back.getLocationOnScreen(at);
        Log.i("DemoTest", "second: shown back=" + at[0] + "," + at[1] + "," + back.getWidth() + "x" + back.getHeight());
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        Log.i("DemoTest", "second: destroyed");
    }
}
