#!/usr/bin/env bash
# 纯 JVM 自测（不需要 Android SDK）。
#   ./selftest.sh            打印测试向量 + 解析测试 + 与本进程 FakePc 走一遍 TCP/UDP 协议
#   ./selftest.sh fakepc ... 启动假电脑（默认 127.0.0.1:47390），给手机经 adb reverse 调试用；参数见 test/FakePc.java
#   ./selftest.sh realpc '<配对链接>' [host[:port]] [shot.jpg]   对真实电脑端互通（软件密钥），见 test/RealPc.java
# 环境变量：JDK（默认 D:/android-platforms/jdk-17，找不到就用 PATH 里的 javac/java）
set -eu

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")"; pwd -W 2>/dev/null || pwd)
JDK=${JDK:-D:/android-platforms/jdk-17}
if [ -x "$JDK/bin/javac.exe" ] || [ -x "$JDK/bin/javac" ]; then
    JAVAC="$JDK/bin/javac"
    JAVA="$JDK/bin/java"
else
    JAVAC=javac
    JAVA=java
fi

OUT="$HERE/build/selftest"
rm -rf "$OUT"
mkdir -p "$OUT"

# 只编 proto 包 + test：proto 里要是混进了 android.* 这里就会编不过
"$JAVAC" -encoding UTF-8 --release 8 -Xlint:-options -d "$OUT" \
    "$HERE"/src/org/hisi/phoneunlock/proto/*.java "$HERE"/test/*.java

case "${1:-}" in
    fakepc)
        shift
        exec "$JAVA" -Dfile.encoding=UTF-8 -Dstdout.encoding=UTF-8 -Dstderr.encoding=UTF-8 -cp "$OUT" FakePc "$@"
        ;;
    realpc)
        # 用协议层 + 软件密钥对真实电脑端互通：./selftest.sh realpc '<配对链接>' [host[:port]] [shot.jpg] [--no-unlock]
        shift
        exec "$JAVA" -Dfile.encoding=UTF-8 -Dstdout.encoding=UTF-8 -Dstderr.encoding=UTF-8 -cp "$OUT" RealPc "$@"
        ;;
    *)
        exec "$JAVA" -Dfile.encoding=UTF-8 -Dstdout.encoding=UTF-8 -Dstderr.encoding=UTF-8 -cp "$OUT" SelfTest
        ;;
esac
