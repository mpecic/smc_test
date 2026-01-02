# Self-modifying code test

## Intro

This is a small native (no Java, no Kotlin, no Android Studio) Android project. Some C code is compiled and packaged into an Android app, which at runtime finds out where the executable arm64 code of one of its own native function is located and attempts to change it. The target function is a simple `return 0`, and if the change is successful it becomes `return 1`.

There is a logcat version which should be monitored with adb, and a GLES version that has a GUI. Logcat version will also log the details of memory page before and after the change.

## Disclaimer

This app pokes around memory, and may therefore be considered dangerous. Use at your own risk!

## Prerequisites

1. Obtain [JDK](https://jdk.java.net/25/)
```
tar -xzf openjdk-25.0.1_linux-x64_bin.tar.gz
```

2. Obtain Android [command line tools](https://developer.android.com/studio#command-line-tools-only)
```
unzip commandlinetools-linux-13114758_latest.zip
```

3. Install Android development tools
```
export JAVA_HOME=$PWD/jdk-25.0.1
./cmdline-tools/bin/sdkmanager --list --sdk_root=$PWD
```
There will be a huge list. We need `build-tools`, `platform-tools`, `platform` and `ndk`. Use exact versions from your list!
```
./cmdline-tools/bin/sdkmanager --sdk_root=$PWD 'build-tools;36.1.0' 'platform-tools' 'platforms;android-36.1' 'ndk;29.0.14206865'
```

4. Create debug keystore. Use password `android`.
```
./jdk-25.0.1/bin/keytool -genkey -v -keystore debug.keystore -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000
```

## Building

1. Create output dir
```
mkdir -p lib/arm64-v8a
```

2.a Compile (logcat version)
```
./ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang -shared -fPIC -o ./lib/arm64-v8a/libsmc_test.so smc_test.c -landroid -llog
```

2.b Compile (GLES version)
```
./ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android35-clang -shared -fPIC -o ./lib/arm64-v8a/libsmc_test.so smc_gles.c ./ndk/29.0.14206865/sources/android/native_app_glue/android_native_app_glue.c -I ./ndk/29.0.14206865/sources/android/native_app_glue -landroid -llog -lEGL -lGLESv2
```

3. Package
```
./build-tools/36.1.0/aapt package -f -M ./AndroidManifest.xml -I ./platforms/android-36.1/android.jar -F temp_unaligned.apk
```

4. Add compiled lib
```
./jdk-25.0.1/bin/jar u0f ./temp_unaligned.apk lib/arm64-v8a/libsmc_test.so
```

5. Align apk
```
./build-tools/36.1.0/zipalign -f -p 4 temp_unaligned.apk temp.apk
```

6. Sign apk
```
./build-tools/36.1.0/apksigner sign --ks ./debug.keystore --ks-key-alias androiddebugkey --ks-pass pass:android --out smc_test.apk temp.apk
```

## Running on Android device

1. Install apk
```
./platform-tools/adb install -r smc_test.apk
```
If this is the first time device is connected, it will ask if you trust the computer running adb.

2. Start log (if running logcat version)
```
./platform-tools/adb logcat -s SMC_DIRECT
```

3. Start app normally from Android launcher

4. Clear log (optional)
```
./platform-tools/adb logcat -c
```

## Results

This is from the logcat version, running on an actual device. The code is changed and memory page is flagged as "dirty".
```
12-21 20:21:43.582 14211 14211 I SMC_DIRECT: --- STARTING DIRECT SMC TEST (NON-JIT) ---
12-21 20:21:43.582 14211 14211 I SMC_DIRECT: 1. Original function returns: 0
12-21 20:21:43.582 14211 14211 I SMC_DIRECT: 2. Target function at: 0x7189d2bcf4
12-21 20:21:43.582 14211 14211 I SMC_DIRECT:    Page start at:      0x7189d2b000
12-21 20:21:43.582 14211 14211 I SMC_DIRECT: 3. Attempting mprotect(RWX)...
12-21 20:21:43.582 14211 14211 I SMC_DIRECT: --- MEMORY PAGE DETAILS ---
12-21 20:21:43.593 14211 14211 I SMC_DIRECT: MAP: 7189d2b000-7189d2d000 r-xp 00004000 00:66 66085                          /data/app/~~8WfGc673dsBpeJiseX_rvA==/com.example.smc-Xy1HSpR3MRbK5vJ05Jn0UQ==/base.apk
12-21 20:21:43.593 14211 14211 I SMC_DIRECT:    Rss:                   8 kB
12-21 20:21:43.593 14211 14211 I SMC_DIRECT:    Shared_Clean:          4 kB
12-21 20:21:43.593 14211 14211 I SMC_DIRECT:    Shared_Dirty:          0 kB
12-21 20:21:43.593 14211 14211 I SMC_DIRECT:    Private_Clean:         4 kB
12-21 20:21:43.593 14211 14211 I SMC_DIRECT:    Private_Dirty:         0 kB
12-21 20:21:43.618 14211 14211 I SMC_DIRECT:    mprotect SUCCESS! (This is unexpected on secure devices)
12-21 20:21:43.618 14211 14211 I SMC_DIRECT: --- MEMORY PAGE DETAILS ---
12-21 20:21:43.628 14211 14211 I SMC_DIRECT: MAP: 7189d2b000-7189d2c000 rwxp 00004000 00:66 66085                          /data/app/~~8WfGc673dsBpeJiseX_rvA==/com.example.smc-Xy1HSpR3MRbK5vJ05Jn0UQ==/base.apk
12-21 20:21:43.628 14211 14211 I SMC_DIRECT:    Rss:                   4 kB
12-21 20:21:43.628 14211 14211 I SMC_DIRECT:    Shared_Clean:          4 kB
12-21 20:21:43.628 14211 14211 I SMC_DIRECT:    Shared_Dirty:          0 kB
12-21 20:21:43.628 14211 14211 I SMC_DIRECT:    Private_Clean:         0 kB
12-21 20:21:43.628 14211 14211 I SMC_DIRECT:    Private_Dirty:         0 kB
12-21 20:21:43.652 14211 14211 I SMC_DIRECT:    Found opcode at offset 0. Patching...
12-21 20:21:43.652 14211 14211 I SMC_DIRECT: 4. Permissions restored to RX.
12-21 20:21:43.652 14211 14211 I SMC_DIRECT: --- MEMORY PAGE DETAILS ---
12-21 20:21:43.662 14211 14211 I SMC_DIRECT: MAP: 7189d2b000-7189d2c000 r-xp 00004000 00:66 66085                          /data/app/~~8WfGc673dsBpeJiseX_rvA==/com.example.smc-Xy1HSpR3MRbK5vJ05Jn0UQ==/base.apk
12-21 20:21:43.662 14211 14211 I SMC_DIRECT:    Rss:                   4 kB
12-21 20:21:43.662 14211 14211 I SMC_DIRECT:    Shared_Clean:          0 kB
12-21 20:21:43.662 14211 14211 I SMC_DIRECT:    Shared_Dirty:          0 kB
12-21 20:21:43.662 14211 14211 I SMC_DIRECT:    Private_Clean:         0 kB
12-21 20:21:43.662 14211 14211 I SMC_DIRECT:    Private_Dirty:         4 kB
12-21 20:21:43.685 14211 14211 I SMC_DIRECT: 5. New value: 1
12-21 20:21:43.686 14211 14211 I SMC_DIRECT: SUCCESS: Direct memory modification worked!
```
