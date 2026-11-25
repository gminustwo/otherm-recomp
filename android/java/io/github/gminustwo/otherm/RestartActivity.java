// Restarts the game: runs in a process of its own (AndroidManifest.xml), ends the game's process (its saves are written)
// and starts the game's activity again in a fresh one.
package io.github.gminustwo.otherm;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

public class RestartActivity extends Activity {
    static final String EXTRA_PID = "pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0) Process.killProcess(pid);
        Intent intent = new Intent(this, OtherMActivity.class);
        intent.putExtra(OtherMActivity.EXTRA_SKIP_LAUNCHER, true);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(intent);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
