/*
 * treeland 锁屏界面上的指纹 / 人脸入口（华为擎云 L540 / deepin 25，treeland 0.11）。
 *
 * LD_PRELOAD 进 treeland，不改 treeland 的包文件：
 *   - 往 Qt 资源系统里先登记一份 :/qt/qml/LockScreen/qmldir 和 qml/UserInputHisi.qml（src/lockbio/）。
 *     Qt 按登记顺序查资源、先到先得，锁屏插件 liblockscreen.so 是 treeland 起来以后才 dlopen 的，
 *     所以这份模块清单会盖过插件自带的那份：LockScreen 模块里的 UserInput 换成带指纹 / 人脸图标和提示的版本，
 *     其余组件照旧用插件里 AOT 编译好的（引用到 UserInput 的 LockView 等会因依赖变了改从源码编译）；
 *   - 注册 QML 单例 HisiBio.BioUnlock：从 system bus 上的 hisi-bio-unlock（org.hisi.BioUnlock1）取状态，
 *     点人脸图标时让它开摄像头。认证和解锁仍由 hisi-bio-unlock（root）完成，这里只是界面。
 * 保护：
 *   - 只在 /usr/bin/treeland 里生效，并把自己从 LD_PRELOAD 里摘掉，不传给 Xwayland 等子进程；--try-exec 预检不处理；
 *   - 锁屏插件的 sha256 不在 lockbio.known 里（treeland 升级了）就不启用，锁屏回到原样，后台指纹 / 人脸照常可用；
 *     画图标要用的 QtQuick.Shapes（qml6-module-qtquick-shapes）没装也不启用；
 *   - treeland 建锁屏组件失败会直接 qFatal：上一次启动没撑过 60 秒就不启用（标记文件在 dde 的家目录），
 *     免得 systemd 在 30 秒内连撞两次后彻底放弃；删掉标记文件后重启 treeland 再试；
 *   - 环境变量 HISI_LOCKBIO_DISABLE=1 关闭。
 * 编译：见 Makefile（moc、rcc --binary，链接 Qt6Core / Qt6DBus / Qt6Qml）。
 */
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QLibraryInfo>
#include <QLoggingCategory>
#include <QResource>
#include <QTimer>
#include <QtQml/qqml.h>

#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#define TREELAND_EXE  "/usr/bin/treeland"
#define KNOWN_LIST    "/usr/local/lib/hisi-treeland/lockbio.known"
#define GUARD_NAME    ".hisi-lockbio-starting"
#define GUARD_SECS    60

/* multiarch 三元组由 Makefile 传入（aarch64-linux-gnu / x86_64-linux-gnu） */
#ifndef HISI_MULTIARCH
#error "需要 -DHISI_MULTIARCH=\"<multiarch>\"（见 Makefile）"
#endif
#define LOCKSCREEN_SO "/usr/lib/" HISI_MULTIARCH "/treeland/plugins/liblockscreen.so"

#define BIO_SVC   "org.hisi.BioUnlock1"
#define BIO_PATH  "/org/hisi/BioUnlock1"
#define BIO_IFACE "org.hisi.BioUnlock1"

/* src/lockbio/lockbio.qrc 经 rcc --binary 编出的数据，原样嵌进 .rodata */
#ifndef HISI_LOCKBIO_RCC
#error "需要 -DHISI_LOCKBIO_RCC=\"<lockbio.rcc 的路径>\"（见 Makefile）"
#endif
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden hisi_lockbio_rcc\n"
    "hisi_lockbio_rcc:\n"
    ".incbin \"" HISI_LOCKBIO_RCC "\"\n"
    ".previous\n");
extern "C" const unsigned char hisi_lockbio_rcc[];

Q_LOGGING_CATEGORY(lcBio, "hisi.lockbio")

class BioUnlock : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString user READ user NOTIFY changed)
    Q_PROPERTY(int fingerprint READ fingerprint NOTIFY changed)
    Q_PROPERTY(int face READ face NOTIFY changed)
    // 已连上电脑的手机（名字由 hisi-bio-unlock 经 GetPhone 给，信号里不带）；phoneUser 是它属于哪个用户
    Q_PROPERTY(QString phoneUser READ phoneUser NOTIFY phoneChanged)
    Q_PROPERTY(QString phoneName READ phoneName NOTIFY phoneChanged)

public:
    explicit BioUnlock(QObject *parent = nullptr)
        : QObject(parent)
    {
        QDBusConnection bus = QDBusConnection::systemBus();
        bus.connect(BIO_SVC, BIO_PATH, BIO_IFACE, "StateChanged", this,
                    SLOT(onSignal(QString, int, int)));
        bus.connect(BIO_SVC, BIO_PATH, BIO_IFACE, "PhoneChanged", this, SLOT(onPhoneSignal()));
        auto *watcher = new QDBusServiceWatcher(BIO_SVC, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
        connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
                [this](const QString &, const QString &, const QString &owner) {
                    if (owner.isEmpty()) {
                        apply(QString(), 0, 0);   // hisi-bio-unlock 退出了
                        applyPhone(QString(), QString());
                    } else {
                        refresh();
                        refreshPhone();
                    }
                });
        refresh();
        refreshPhone();
    }

    QString user() const { return m_user; }
    int fingerprint() const { return m_fp; }
    int face() const { return m_face; }
    QString phoneUser() const { return m_phoneUser; }
    QString phoneName() const { return m_phoneName; }

    Q_INVOKABLE void startFace()
    {
        QDBusConnection::systemBus().asyncCall(
            QDBusMessage::createMethodCall(BIO_SVC, BIO_PATH, BIO_IFACE, "StartFace"));
    }

Q_SIGNALS:
    void changed();
    void phoneChanged();

private Q_SLOTS:
    void onSignal(const QString &user, int fp, int face)
    {
        m_serial++;
        apply(user, fp, face);
    }

    void onPhoneSignal()
    {
        refreshPhone();
    }

private:
    void applyPhone(const QString &user, const QString &name)
    {
        if (user == m_phoneUser && name == m_phoneName)
            return;
        m_phoneUser = user;
        m_phoneName = name;
        Q_EMIT phoneChanged();
    }

    /* 信号只说“变了”：每次都重取，以最后一次回复为准（回复按发出顺序到达） */
    void refreshPhone()
    {
        auto *w = new QDBusPendingCallWatcher(
            QDBusConnection::systemBus().asyncCall(
                QDBusMessage::createMethodCall(BIO_SVC, BIO_PATH, BIO_IFACE, "GetPhone")),
            this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<QString, QString> r = *w;
            if (r.isValid())
                applyPhone(r.argumentAt<0>(), r.argumentAt<1>());
            w->deleteLater();
        });
    }

    void apply(const QString &user, int fp, int face)
    {
        if (user == m_user && fp == m_fp && face == m_face)
            return;
        m_user = user;
        m_fp = fp;
        m_face = face;
        Q_EMIT changed();
    }

    /* 取一次当前状态；回复到达前要是已经收到过新信号，就以信号为准 */
    void refresh()
    {
        const quint64 serial = m_serial;
        auto *w = new QDBusPendingCallWatcher(
            QDBusConnection::systemBus().asyncCall(
                QDBusMessage::createMethodCall(BIO_SVC, BIO_PATH, BIO_IFACE, "GetState")),
            this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, serial](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<QString, int, int> r = *w;
            if (r.isValid() && serial == m_serial)
                apply(r.argumentAt<0>(), r.argumentAt<1>(), r.argumentAt<2>());
            w->deleteLater();
        });
    }

    QString m_phoneUser;
    QString m_phoneName;
    QString m_user;
    int m_fp = 0;
    int m_face = 0;
    quint64 m_serial = 0;
};

static bool g_target;   // 本进程是正式运行的 treeland（或测试模式）

static bool env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0");
}

/* 从 LD_PRELOAD 里去掉自己，子进程不再预加载（其余项原样保留） */
static void drop_self_from_preload()
{
    const char *val = getenv("LD_PRELOAD");
    if (!val || !strstr(val, "libtl-lockbio"))
        return;
    char *copy = strdup(val), *out = static_cast<char *>(calloc(1, strlen(val) + 1)), *save = nullptr;
    if (copy && out) {
        for (char *tok = strtok_r(copy, " :", &save); tok; tok = strtok_r(nullptr, " :", &save)) {
            if (strstr(tok, "libtl-lockbio"))
                continue;
            if (*out)
                strcat(out, " ");
            strcat(out, tok);
        }
        if (*out)
            setenv("LD_PRELOAD", out, 1);
        else
            unsetenv("LD_PRELOAD");
    }
    free(copy);
    free(out);
}

/* 命令行里有没有某个参数（/proc/self/cmdline，参数之间是 \0） */
static bool has_arg(const char *arg)
{
    char buf[4096];
    int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (n <= 0)
        return false;
    buf[n] = 0;
    for (char *p = buf; p < buf + n; p += strlen(p) + 1)
        if (!strcmp(p, arg))
            return true;
    return false;
}

__attribute__((constructor)) static void lockbio_ctor()
{
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);

    if (env_on("HISI_LOCKBIO_TEST")) {   // 只给离线测试用：任何进程都处理，不做崩溃保护
        g_target = true;
        return;
    }
    if (n <= 0)
        return;
    exe[n] = 0;
    if (strcmp(exe, TREELAND_EXE))
        return;   // treeland.sh 等中间进程：原样放行
    drop_self_from_preload();
    g_target = !has_arg("--try-exec");
}

/* 锁屏插件是不是核对过的版本（UserInputHisi.qml 是照着它的 UserInput.qml 改的） */
static bool plugin_known()
{
    QFile so(QStringLiteral(LOCKSCREEN_SO));
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!so.open(QIODevice::ReadOnly) || !h.addData(&so)) {
        qCWarning(lcBio) << "读不了" << LOCKSCREEN_SO << "，不启用锁屏指纹 / 人脸界面";
        return false;
    }
    const QByteArray sum = h.result().toHex();
    QFile list(QStringLiteral(KNOWN_LIST));
    if (list.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!list.atEnd()) {
            const QByteArray line = list.readLine().simplified();
            if (!line.startsWith('#') && line.split(' ').value(0) == sum)
                return true;
        }
    }
    qCWarning(lcBio).noquote() << "锁屏插件 sha256" << sum << "不在" << KNOWN_LIST
                               << "里（treeland 升级了？），不启用锁屏指纹 / 人脸界面；后台指纹 / 人脸解锁不受影响";
    return false;
}

static QString guard_path()
{
    return QDir::homePath() + QStringLiteral("/" GUARD_NAME);
}

/* UserInputHisi.qml 要用 QtQuick.Shapes 画图标；缺了它锁屏组件建不出来，treeland 会直接 qFatal */
static bool qml_deps_present()
{
    const QString qmldir = QLibraryInfo::path(QLibraryInfo::QmlImportsPath) + QStringLiteral("/QtQuick/Shapes/qmldir");
    if (QFile::exists(qmldir))
        return true;
    qCWarning(lcBio).noquote() << "没有" << qmldir << "（qml6-module-qtquick-shapes 没装？），不启用锁屏指纹 / 人脸界面";
    return false;
}

/* 上一次启动没撑过 GUARD_SECS 秒（标记还在）就不启用；否则立标记，撑过去或正常退出再删 */
static bool crash_guard()
{
    const QString p = guard_path();
    if (QFile::exists(p)) {
        qCWarning(lcBio).noquote() << "上一次启动没撑过" << GUARD_SECS << "秒，这次不启用锁屏指纹 / 人脸界面；"
                                   << "排查后删掉" << p << "再重启 treeland";
        return false;
    }
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcBio) << "写不了" << p << "，不启用锁屏指纹 / 人脸界面";
        return false;
    }
    f.write(QByteArray::number(QCoreApplication::applicationPid()) + '\n');
    f.close();
    /* 正常退出、定时器到点、以及 SIGTERM（关机）/ SIGKILL 之外的崩溃都删标记；
     * 只有真崩溃（SIGSEGV 等）留下标记，下次启动才停用界面。
     * 用 Q_UNIX 的信号 handler 只做标记：清除文件不能在信号上下文里做，先交给一个瞬时定时器。 */
    QTimer::singleShot(GUARD_SECS * 1000, qApp, [] { QFile::remove(guard_path()); });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] { QFile::remove(guard_path()); });
    struct sigaction sa{};
    sa.sa_handler = [](int) { QByteArray p = guard_path().toUtf8(); ::unlink(p.constData()); };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    return true;
}

/* QCoreApplication 构造时调用，早于 treeland 加载锁屏插件 */
static void lockbio_startup()
{
    if (!g_target)
        return;
    if (env_on("HISI_LOCKBIO_DISABLE")) {
        qCInfo(lcBio) << "HISI_LOCKBIO_DISABLE 已设置，不启用锁屏指纹 / 人脸界面";
        return;
    }
    if (!plugin_known() || !qml_deps_present())
        return;
    if (!env_on("HISI_LOCKBIO_TEST") && !crash_guard())
        return;
    if (!QResource::registerResource(hisi_lockbio_rcc)) {
        qCWarning(lcBio) << "登记锁屏界面资源失败";
        return;
    }
    qmlRegisterSingletonType<BioUnlock>("HisiBio", 1, 0, "BioUnlock",
                                        [](QQmlEngine *, QJSEngine *) -> QObject * { return new BioUnlock; });
    qCInfo(lcBio) << "已启用锁屏指纹 / 人脸界面";
}
Q_COREAPP_STARTUP_FUNCTION(lockbio_startup)

#include "tl-lockbio.moc"
