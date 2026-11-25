// SDL's drawing surface, telling aurora (the game's renderer, in libmain.so) when it can draw: it presents only while
// the surface is usable (aurora/lib/window.cpp set_surface_ready), and SDL's own Java has no hook for that.
package io.github.gminustwo.otherm;

import android.content.Context;
import android.view.SurfaceHolder;

import org.libsdl.app.SDLSurface;

public class OtherMSurface extends SDLSurface {
    private static native void nativeSetSurfaceReady(boolean ready);

    public OtherMSurface(Context context) {
        super(context);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeSetSurfaceReady(false);  // before SDL pauses the game
        super.surfaceDestroyed(holder);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        super.surfaceChanged(holder, format, width, height);
        if (mIsSurfaceReady) nativeSetSurfaceReady(true);
    }
}
