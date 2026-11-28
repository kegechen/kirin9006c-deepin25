/*
 * 控制中心（deepin 25 的 dde-control-center 6.1，插件格式 v1.0）“账户 → 登录方式 → 手机解锁”插件的数据对象，
 * 在 phoneunlockMain.qml 里是 dccData。
 *
 * 只是客户端：经系统总线 org.hisi.BioUnlock1.User 找 hisi-bio-unlock。服务按调用者的 uid 只给看、只给改他自己的手机；
 * 配对新手机时服务先问 polkit（org.hisi.bio-unlock.pair-phone，要输一次自己的密码），过了才把一次性配对码给我们。
 * 配对链接和 sudo hisi-phone pair 打出来的一样（hisiphone://pair?...），二维码用 libqrencode 画（dlopen，没装就只给链接）。
 * 协议见 docs/phone-unlock.md。
 */
#include <QClipboard>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QGuiApplication>
#include <QLibrary>
#include <QUrl>
#include <QVariantMap>

#include <algorithm>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pwd.h>
#include <unistd.h>

#include "dccfactory.h"

namespace {

const QString kService = QStringLiteral("org.hisi.BioUnlock1");
const QString kPath = QStringLiteral("/org/hisi/BioUnlock1");
const QString kIface = QStringLiteral("org.hisi.BioUnlock1.User");
constexpr int kLinkPort = 47390;      /* hisi-phone-link 的 TCP/UDP 端口 */
constexpr int kPairSeconds = 180;     /* 和 sudo hisi-phone pair 一样 */
constexpr int kNameMaxBytes = 64;     /* 服务端 NAME_MAX_LEN */
constexpr int kAuthTimeoutMs = 6 * 60 * 1000;   /* 等用户在 polkit 认证框里输密码（服务那边等 5 分钟） */

/* 本机已连上的 IPv4 地址，逗号分隔（含 Tailscale 的 100.x，手机在外面也能连） */
QString localIpv4s()
{
    QStringList ips;
    struct ifaddrs *ifa;

    if (getifaddrs(&ifa) != 0)
        return {};
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
        char buf[INET_ADDRSTRLEN];

        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)
            || !(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_RUNNING))
            continue;
        if (inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in *>(i->ifa_addr)->sin_addr, buf, sizeof buf))
            ips << QString::fromLatin1(buf);
    }
    freeifaddrs(ifa);
    return ips.join(QLatin1Char(','));
}

/* 已上电的蓝牙适配器地址；没开蓝牙返回空串 */
QString btAddress()
{
    QDBusMessage m = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), QStringLiteral("/"),
                                                    QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                                    QStringLiteral("GetManagedObjects"));
    QDBusMessage r = QDBusConnection::systemBus().call(m, QDBus::Block, 3000);
    QString addr;

    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty())
        return {};
    const QDBusArgument arg = r.arguments().at(0).value<QDBusArgument>();
    arg.beginMap();
    while (!arg.atEnd()) {
        QDBusObjectPath path;

        arg.beginMapEntry();
        arg >> path;
        arg.beginMap();
        while (!arg.atEnd()) {
            QString iface;
            QVariantMap props;

            arg.beginMapEntry();
            arg >> iface >> props;
            arg.endMapEntry();
            if (addr.isEmpty() && iface == QLatin1String("org.bluez.Adapter1") && props.value(QStringLiteral("Powered")).toBool())
                addr = props.value(QStringLiteral("Address")).toString();
        }
        arg.endMap();
        arg.endMapEntry();
    }
    arg.endMap();
    return addr;
}

/* libqrencode 4 的 ABI（qrencode.h）：只用这两个函数 */
struct QRcodeRaw {
    int version;
    int width;
    unsigned char *data;   /* width*width 字节，最低位 1 = 黑 */
};
using QrEncodeFn = QRcodeRaw *(*)(const char *, int version, int level, int hint, int casesensitive);
using QrFreeFn = void (*)(QRcodeRaw *);
constexpr int kQrEcLevelM = 1;   /* QR_ECLEVEL_M */
constexpr int kQrMode8 = 2;      /* QR_MODE_8 */

/* 二维码按行展开成 '0'/'1' 串，QML 的 Canvas 照着画。*width 是边长（模块数），失败返回空串 */
QString encodeQr(const QByteArray &text, int *width)
{
    static QLibrary lib(QStringLiteral("qrencode"), 4);
    static QrEncodeFn encode;
    static QrFreeFn release;
    QString bits;

    *width = 0;
    if (!encode) {
        if (!lib.load()) {
            qWarning("phoneunlock: 加载 libqrencode.so.4 失败（%s），只能显示配对链接", qPrintable(lib.errorString()));
            return {};
        }
        encode = reinterpret_cast<QrEncodeFn>(lib.resolve("QRcode_encodeString"));
        release = reinterpret_cast<QrFreeFn>(lib.resolve("QRcode_free"));
        if (!encode || !release) {
            encode = nullptr;
            return {};
        }
    }
    QRcodeRaw *q = encode(text.constData(), 0, kQrEcLevelM, kQrMode8, 1);
    if (!q)
        return {};
    bits.reserve(q->width * q->width);
    for (int i = 0; i < q->width * q->width; i++)
        bits += (q->data[i] & 1) ? QLatin1Char('1') : QLatin1Char('0');
    *width = q->width;
    release(q);
    return bits;
}

QString userName()
{
    struct passwd *pw = getpwuid(getuid());
    return pw ? QString::fromLocal8Bit(pw->pw_name) : QString();
}

QString errorText(const QDBusError &e)
{
    const QString n = e.name();

    if (n == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
        || n == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner"))
        return QStringLiteral("手机解锁服务没在运行（hisi-bio-unlock.service）");
    if (n == QLatin1String("org.freedesktop.DBus.Error.UnknownInterface")
        || n == QLatin1String("org.freedesktop.DBus.Error.UnknownMethod")
        || n == QLatin1String("org.freedesktop.DBus.Error.UnknownObject"))
        return QStringLiteral("手机解锁服务版本太旧，更新后重启 hisi-bio-unlock.service");
    if (n == QLatin1String("org.freedesktop.DBus.Error.AccessDenied") && e.message().contains(QLatin1String("policy")))
        return QStringLiteral("系统总线不让调用手机解锁服务（D-Bus 策略没装或没 reload）");
    if (n == QLatin1String("org.freedesktop.DBus.Error.NoReply") || n == QLatin1String("org.freedesktop.DBus.Error.Timeout"))
        return QStringLiteral("手机解锁服务没响应");
    return e.message().isEmpty() ? n : e.message();
}

class PhoneUnlock : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList phones READ phones NOTIFY phonesChanged)
    Q_PROPERTY(int maxPhones READ maxPhones NOTIFY phonesChanged)
    Q_PROPERTY(bool serviceOk READ serviceOk NOTIFY phonesChanged)
    Q_PROPERTY(QString serviceError READ serviceError NOTIFY phonesChanged)
    /* 0 = 没在配对，1 = 等 polkit 认证，2 = 正在显示二维码 */
    Q_PROPERTY(int pairState READ pairState NOTIFY pairChanged)
    Q_PROPERTY(QString pairLink READ pairLink NOTIFY pairChanged)
    Q_PROPERTY(QString qrBits READ qrBits NOTIFY pairChanged)
    Q_PROPERTY(int qrWidth READ qrWidth NOTIFY pairChanged)
    Q_PROPERTY(QString pairHost READ pairHost NOTIFY pairChanged)
    Q_PROPERTY(QString pairIps READ pairIps NOTIFY pairChanged)
    Q_PROPERTY(QString pairBt READ pairBt NOTIFY pairChanged)
    Q_PROPERTY(double pairDeadline READ pairDeadline NOTIFY pairChanged)   /* 二维码作废的时刻（毫秒，同 Date.now()） */
    Q_PROPERTY(int nameMaxBytes READ nameMaxBytes CONSTANT)

public:
    explicit PhoneUnlock(QObject *parent = nullptr)
        : QObject(parent)
    {
        QDBusConnection bus = QDBusConnection::systemBus();

        bus.connect(kService, kPath, kIface, QStringLiteral("Changed"), this, SLOT(refresh()));
        bus.connect(kService, kPath, kIface, QStringLiteral("PairEnded"), this, SLOT(onPairEnded(QString,QString)));
        auto *w = new QDBusServiceWatcher(kService, bus,
                                          QDBusServiceWatcher::WatchForRegistration | QDBusServiceWatcher::WatchForUnregistration,
                                          this);
        connect(w, &QDBusServiceWatcher::serviceRegistered, this, &PhoneUnlock::refresh);
        connect(w, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
            refresh();
            if (m_pairState) {
                clearPair();
                Q_EMIT pairFinished(QStringLiteral("service_gone"), QString());
            }
        });
    }

    QVariantList phones() const { return m_phones; }
    int maxPhones() const { return m_max; }
    bool serviceOk() const { return m_serviceError.isEmpty(); }
    QString serviceError() const { return m_serviceError; }
    int pairState() const { return m_pairState; }
    QString pairLink() const { return m_link; }
    QString qrBits() const { return m_qrBits; }
    int qrWidth() const { return m_qrWidth; }
    QString pairHost() const { return m_host; }
    QString pairIps() const { return m_ips; }
    QString pairBt() const { return m_bt; }
    double pairDeadline() const { return double(m_deadline); }
    int nameMaxBytes() const { return kNameMaxBytes; }

    Q_INVOKABLE int utf8Bytes(const QString &s) const { return s.toUtf8().size(); }

    /* 设置别名：手机上报名保留不动，有别名优先显示别名；空串 = 清除别名。
     * 成功返回空串，否则返回给用户看的原因 */
    Q_INVOKABLE QString rename(const QString &id, const QString &name)
    {
        const QString n = name.trimmed();

        if (n.toUtf8().size() > kNameMaxBytes)
            return QStringLiteral("别名太长（最多 %1 字节，一个汉字算 3 字节）").arg(kNameMaxBytes);
        QDBusMessage r = call(QStringLiteral("Rename"), { id, n });
        if (r.type() != QDBusMessage::ReplyMessage)
            return errorText(QDBusError(r));
        refresh();
        return r.arguments().value(0).toBool() ? QString() : QStringLiteral("找不到这台手机（可能已经删掉了）");
    }

    Q_INVOKABLE QString unpair(const QString &id)
    {
        QDBusMessage r = call(QStringLiteral("Unpair"), { id });

        if (r.type() != QDBusMessage::ReplyMessage)
            return errorText(QDBusError(r));
        refresh();
        return r.arguments().value(0).toBool() ? QString() : QStringLiteral("找不到这台手机（可能已经删掉了）");
    }

    /* 锁屏试试：成功返回空串 */
    Q_INVOKABLE QString lockNow()
    {
        QDBusMessage r = call(QStringLiteral("Lock"));

        if (r.type() != QDBusMessage::ReplyMessage)
            return errorText(QDBusError(r));
        switch (r.arguments().value(0).toInt()) {
        case 0:
            return {};
        case 2:
            return QStringLiteral("没找到你的桌面会话");
        default:
            return QStringLiteral("没锁上（状态 %1），看 journalctl -u hisi-bio-unlock").arg(r.arguments().value(0).toInt());
        }
    }

    /* 开始配对：服务先弹 polkit 认证框，过了才进 pairState 2。认证没过/取消了回到 0（取消不报错）；
     * 其它失败发 pairFailed */
    Q_INVOKABLE void startPair()
    {
        if (m_pairState)
            return;
        QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("PairStart"));
        m.setArguments({ QVariant::fromValue(uint(kPairSeconds)) });
        m_pairState = 1;
        Q_EMIT pairChanged();
        auto *w = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(m, kAuthTimeoutMs), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, &PhoneUnlock::onPairStarted);
    }

    Q_INVOKABLE void stopPair()
    {
        if (m_pairState == 2) {
            QDBusConnection::systemBus().asyncCall(
                QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("PairStop")));
        }
        if (m_pairState)
            clearPair();   /* 1（还在认证）时：认证回来发现已取消，会顺手 PairStop */
    }

    Q_INVOKABLE void copyPairLink()
    {
        if (!m_link.isEmpty())
            QGuiApplication::clipboard()->setText(m_link);
    }

public Q_SLOTS:
    void refresh()
    {
        QDBusMessage r = call(QStringLiteral("List"));
        QVariantList list;

        if (r.type() != QDBusMessage::ReplyMessage || r.arguments().size() < 2) {
            m_serviceError = r.type() == QDBusMessage::ErrorMessage ? errorText(QDBusError(r))
                                                                    : QStringLiteral("手机解锁服务的回复不对");
        } else {
            const QDBusArgument arg = r.arguments().at(0).value<QDBusArgument>();

            arg.beginArray();
            while (!arg.atEnd()) {
                QString id, name;
                qulonglong paired = 0;
                bool online = false;
                QVariantMap p;

                arg.beginStructure();
                arg >> id >> name >> paired >> online;
                arg.endStructure();
                p[QStringLiteral("id")] = id;
                p[QStringLiteral("name")] = name;
                p[QStringLiteral("online")] = online;
                p[QStringLiteral("pairedAt")] = double(paired);
                p[QStringLiteral("paired")] = QDateTime::fromSecsSinceEpoch(qint64(paired)).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                list << p;
            }
            arg.endArray();
            std::sort(list.begin(), list.end(), [](const QVariant &a, const QVariant &b) {
                return a.toMap().value(QStringLiteral("pairedAt")).toDouble() < b.toMap().value(QStringLiteral("pairedAt")).toDouble();
            });
            m_max = int(r.arguments().at(1).toUInt());
            m_serviceError.clear();
        }
        m_phones = list;
        Q_EMIT phonesChanged();
    }

Q_SIGNALS:
    void phonesChanged();
    void pairChanged();
    void pairFailed(const QString &message);
    /* result：paired / timeout / cancelled / replaced / bad_code / service_gone；name 只在 paired 时有 */
    void pairFinished(const QString &result, const QString &name);

private Q_SLOTS:
    void onPairEnded(const QString &result, const QString &name)
    {
        if (m_pairState != 2)
            return;
        clearPair();
        Q_EMIT pairFinished(result, name);
        refresh();
    }

private:
    QDBusMessage call(const QString &method, const QVariantList &args = {})
    {
        QDBusMessage m = QDBusMessage::createMethodCall(kService, kPath, kIface, method);
        m.setArguments(args);
        return QDBusConnection::systemBus().call(m, QDBus::Block, 5000);
    }

    void onPairStarted(QDBusPendingCallWatcher *w)
    {
        QDBusPendingReply<QString, QByteArray, QString> r = *w;

        w->deleteLater();
        if (m_pairState != 1) {
            /* 等认证的时候用户已经取消了（关了页面之类）：配对窗口已经开了就关掉 */
            if (!r.isError())
                QDBusConnection::systemBus().asyncCall(
                    QDBusMessage::createMethodCall(kService, kPath, kIface, QStringLiteral("PairStop")));
            return;
        }
        if (r.isError()) {
            m_pairState = 0;
            Q_EMIT pairChanged();
            /* 认证没过也得说一声：之前静默吞掉 AuthFailed，用户看到的就是“点了没反应” */
            Q_EMIT pairFailed(r.error().name() == QLatin1String("org.freedesktop.DBus.Error.AuthFailed")
                                  ? QStringLiteral("没通过认证（密码不对，或者认证框被关掉了）")
                                  : errorText(r.error()));
            return;
        }
        const QString pcId = r.argumentAt<0>();
        QByteArray code = r.argumentAt<1>();
        m_host = r.argumentAt<2>();
        m_ips = localIpv4s();
        m_bt = btAddress();
        const QString code64 = QString::fromLatin1(code.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        code.fill('\0');
        m_link = QStringLiteral("hisiphone://pair?v=1&id=%1&c=%2&h=%3&u=%4&ip=%5&p=%6&bt=%7")
                     .arg(pcId, code64, QString::fromLatin1(QUrl::toPercentEncoding(m_host)),
                          QString::fromLatin1(QUrl::toPercentEncoding(userName())), m_ips,
                          QString::number(kLinkPort), m_bt);
        m_qrBits = encodeQr(m_link.toUtf8(), &m_qrWidth);
        m_deadline = QDateTime::currentMSecsSinceEpoch() + kPairSeconds * 1000;
        m_pairState = 2;
        Q_EMIT pairChanged();
    }

    void clearPair()
    {
        m_link.fill(QLatin1Char('\0'));
        m_link.clear();
        m_qrBits.clear();
        m_qrWidth = 0;
        m_deadline = 0;
        m_pairState = 0;
        Q_EMIT pairChanged();
    }

    QVariantList m_phones;
    int m_max = 5;
    QString m_serviceError;
    int m_pairState = 0;
    QString m_link, m_qrBits, m_host, m_ips, m_bt;
    int m_qrWidth = 0;
    qint64 m_deadline = 0;
};

class PhoneUnlockFactory : public dccV25::DccFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID DccFactory_iid)
    Q_INTERFACES(dccV25::DccFactory)
public:
    using dccV25::DccFactory::DccFactory;
    QObject *create(QObject *parent = nullptr) override { return new PhoneUnlock(parent); }
};

} // namespace

#include "phoneunlock.moc"
