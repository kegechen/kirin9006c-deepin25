#!/usr/bin/env bash
# 不用 Gradle，直接用 build-tools 出 APK。在 Git Bash 下运行：
#   ./build.sh            编译 → build/phone-unlock.apk
#   ./build.sh install    编译并 adb install -r
#   ./build.sh run        编译、安装并启动
#   ./build.sh selftest   跑纯 JVM 自测（同 ./selftest.sh）
#   ./build.sh clean      删掉 build/
# 路径都可以用环境变量覆盖：SDK_ROOT ANDROID_JAR BUILD_TOOLS JDK ZXING_JAR KEYSTORE KEY_ALIAS KS_PASS ADB ADB_SERIAL
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")"; pwd -W 2>/dev/null || pwd)
B="$HERE/build"

SDK_ROOT=${SDK_ROOT:-D:/android-platforms}
ANDROID_JAR=${ANDROID_JAR:-$SDK_ROOT/platforms/android-35/android.jar}
BUILD_TOOLS=${BUILD_TOOLS:-$SDK_ROOT/build-tools/35.0.0}
JDK=${JDK:-$SDK_ROOT/jdk-17}
ZXING_JAR=${ZXING_JAR:-C:/Users/kechen12.IFLYTEK/.gradle/caches/modules-2/files-2.1/com.google.zxing/core/3.5.2/baab0f4a47b2052c9393af6af7c47a9dd8d89297/core-3.5.2.jar}
KEYSTORE=${KEYSTORE:-C:/Users/kechen12.IFLYTEK/.android/debug.keystore}
KEY_ALIAS=${KEY_ALIAS:-androiddebugkey}
KS_PASS_DEFAULT='android'
KS_PASS=${KS_PASS:-$KS_PASS_DEFAULT}
ADB=${ADB:-D:/ZhcMoveData/Downloads/scrcpy-win64-v3.3.3/scrcpy-win64-v3.3.3/adb.exe}
ADB_SERIAL=${ADB_SERIAL:-}

EXE=""
if [ -f "$BUILD_TOOLS/aapt2.exe" ]; then EXE=".exe"; fi
AAPT2="$BUILD_TOOLS/aapt2$EXE"
AAPT="$BUILD_TOOLS/aapt$EXE"
ZIPALIGN="$BUILD_TOOLS/zipalign$EXE"
JAVA="$JDK/bin/java$EXE"
JAVAC="$JDK/bin/javac$EXE"
JAR="$JDK/bin/jar$EXE"
CPSEP=":"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) CPSEP=";" ;; esac

adb_() {
    if [ -n "$ADB_SERIAL" ]; then "$ADB" -s "$ADB_SERIAL" "$@"; else "$ADB" "$@"; fi
}

need() {
    if [ ! -e "$1" ]; then
        echo "找不到 $1（$2）" >&2
        exit 1
    fi
}

build() {
    need "$ANDROID_JAR" "ANDROID_JAR"
    need "$AAPT2" "BUILD_TOOLS"
    need "$JAVAC" "JDK"
    need "$ZXING_JAR" "ZXING_JAR"
    need "$KEYSTORE" "KEYSTORE"

    rm -rf "$B/gen" "$B/classes" "$B/dex" "$B/res.zip" "$B"/*.apk "$B/classes.jar" "$B/sources.txt"
    mkdir -p "$B/gen" "$B/classes" "$B/dex"

    echo "== aapt2 编译资源"
    "$AAPT2" compile --dir "$HERE/res" -o "$B/res.zip"
    "$AAPT2" link -o "$B/base.apk" -I "$ANDROID_JAR" \
        --manifest "$HERE/AndroidManifest.xml" --java "$B/gen" \
        "$B/res.zip"

    echo "== javac"
    find "$HERE/src" "$B/gen" -name '*.java' > "$B/sources.txt"
    "$JAVAC" -J-Duser.language=en -J-Duser.country=US \
        -encoding UTF-8 -source 8 -target 8 -Xlint:-options -nowarn \
        -bootclasspath "$ANDROID_JAR" -classpath "$ZXING_JAR" \
        -d "$B/classes" @"$B/sources.txt"
    "$JAR" cf "$B/classes.jar" -C "$B/classes" .

    echo "== d8"
    "$JAVA" -cp "$BUILD_TOOLS/lib/d8.jar" com.android.tools.r8.D8 \
        --release --min-api 28 --lib "$ANDROID_JAR" \
        --output "$B/dex" "$B/classes.jar" "$ZXING_JAR"

    echo "== 打包 / 对齐 / 签名"
    cp "$B/base.apk" "$B/unsigned.apk"
    (cd "$B/dex"; "$AAPT" add "$B/unsigned.apk" classes.dex > /dev/null)
    "$ZIPALIGN" -f -p 4 "$B/unsigned.apk" "$B/aligned.apk"
    "$JAVA" -jar "$BUILD_TOOLS/lib/apksigner.jar" sign \
        --ks "$KEYSTORE" --ks-key-alias "$KEY_ALIAS" \
        --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
        --out "$B/phone-unlock.apk" "$B/aligned.apk"
    "$JAVA" -jar "$BUILD_TOOLS/lib/apksigner.jar" verify "$B/phone-unlock.apk"
    rm -f "$B/base.apk" "$B/unsigned.apk" "$B/aligned.apk" "$B/phone-unlock.apk.idsig"
    echo "== 完成：$B/phone-unlock.apk（$(wc -c < "$B/phone-unlock.apk") 字节）"
}

case "${1:-build}" in
    build)
        build
        ;;
    install)
        build
        adb_ install -r "$B/phone-unlock.apk"
        ;;
    run)
        build
        adb_ install -r "$B/phone-unlock.apk"
        adb_ shell am start -n org.hisi.phoneunlock/.MainActivity
        ;;
    selftest)
        exec bash "$HERE/selftest.sh"
        ;;
    clean)
        rm -rf "$B"
        ;;
    *)
        echo "用法：$0 [build|install|run|selftest|clean]" >&2
        exit 2
        ;;
esac
