# gsplat-viewer on Android

A NativeActivity app (C++ only, no Java/Kotlin) that runs the same renderer as the desktop
viewer on OpenGL ES 3.1+. Tested target: Samsung Galaxy S23 Ultra (Adreno 740).

## Install

1. On the phone: *Settings → About phone → Software information*, tap *Build number* 7 times,
   then *Developer options → USB debugging* on.
2. Connect the phone, then either
   - `F:\tools\android\sdk\platform-tools\adb.exe install -r dist\gsplat-viewer-<version>-android.apk`, or
   - copy the APK to the phone and open it (allow "install unknown apps" for the file manager).

## Emulator (no phone needed)

`F:\tools\android\emulator.cmd` starts the test AVD `gsv_test` (Android 15, x86_64, host GPU
through WHPX); `tools\test_android.ps1` then runs the CPU tests on it and installs the app (the APK
carries x86_64 and arm64-v8a libraries). The emulator exposes OpenGL ES 3.1, which is why the
shaders target GLSL ES 3.10. Inside the emulator the PC is `10.0.2.2`: *Find PC* falls back to it
when the broadcast gets no answer. Android Studio (zip, no installer) is in
`F:\tools\android\android-studio`; start it with `F:\tools\android\studio.cmd` so it uses the SDK,
the AVDs and a data folder on F:. Frame rates in the emulator are the PC's GPU, not a phone's.

## Scenes

The app lists every `.ply`, `.splat` and `.spz` under its own folder:

```
Internal storage/Android/data/com.gsplatviewer.app/files/
```

Copy a scene there over USB (or `adb push scene.spz /sdcard/Android/data/com.gsplatviewer.app/files/`).
For a scene captured with `tools/capture.ps1`, copy the whole scene folder including
`dataset/sparse/0`: the app then levels the scene with the training cameras and opens it at the
first camera, like the desktop viewer. `.spz` files are 5–10× smaller than `.ply`.

## Phone video → 3D scene (PC does the training)

1. On the PC: `build\gsplat_server.exe` (once: allow it in the Windows firewall prompt for
   private networks). It prints the port (8765) and lists the scenes already trained.
2. Film with the phone's camera app: 1080p or 4K, 30 fps, main 1× lens, walk slowly around the
   subject (see [CAPTURE.md](CAPTURE.md)).
3. In the phone's browser open `http://<PC IP>:8765`, type a scene name, pick the video, *Upload
   and train*. The page shows the upload progress and then the training state.
4. In the app, *PC capture → Find PC* (UDP broadcast on the same Wi-Fi), then *Download* next to the
   finished scene: the PLY and the COLMAP cameras go to the app folder and the scene opens at the
   first training camera.

Measured on the PC (RTX 3060): a 112 s room video takes about 12 minutes (frames 2 s, COLMAP
2.5 min, Brush 30 000 steps 9.5 min); the 55 MB result downloads in a few seconds over Wi-Fi.

## Controls

| Touch | Action |
|---|---|
| Finger down in the bottom-left area | Move joystick (forward/back, sideways) |
| Finger down in the bottom-right area | Look joystick (turn around the eye) |
| One finger elsewhere | Orbit |
| Two fingers | Pinch zoom + pan |

The panel (top left) shows FPS and GPU times and has the scene list, render resolution
(default 0.5× of the 3088×1440 screen), SH degree and blending precision.

## Build

Requirements (all under `F:\tools\android`, nothing on C:): JDK 17, Android SDK command-line tools,
`platforms;android-35`, `build-tools;35.0.1`, `ndk;27.3.13750724`, `platform-tools`.

```powershell
powershell -ExecutionPolicy Bypass -File tools\build_android.ps1          # -> dist\...-android.apk
powershell -ExecutionPolicy Bypass -File tools\build_android.ps1 -Install # + adb install
powershell -ExecutionPolicy Bypass -File tools\check_shaders_es.ps1       # shaders as GLSL ES 3.10
```

No Gradle: CMake (NDK toolchain) builds `libgsplat_android.so`, `aapt2` packages the manifest and
the shaders, `aapt` adds the library, `zipalign` and `apksigner` (debug key) finish the APK.

## Logs

`adb logcat -s gsplat` shows the GPU name, OpenGL ES version and load errors.
