#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QPainter>
#include <QMouseEvent>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QTime>
#include <QLinearGradient>
#include <QFileInfo>
#include <QSet>
#include <functional>
#include <QScroller>
#include <QPushButton>
#include <QSlider>
#include <QtGlobal>
#include <cmath>
#include <QImage>
#include <QColor>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QSequentialAnimationGroup>
#include <QParallelAnimationGroup>
#include <QEasingCurve>
#include <QSettings>
#include <QPixmapCache>
#include <QBuffer>
#include <QKeyEvent>
#include <QShowEvent>
#include <QMap>
#include <algorithm>
#include <QPropertyAnimation>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusReply>

// ──────────────────────────────  Helper: read file
static QString readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return "";
    return QString::fromUtf8(f.readAll()).trimmed();
}

// ──────────────────────────────  Pixmap tint helper (now unused, but kept)
static QPixmap tintPixmap(const QPixmap &src, const QColor &color) {
    if (src.isNull()) return src;

    QPixmap result(src.size());
    result.fill(Qt::transparent);

    QPainter p(&result);
    p.drawPixmap(0, 0, src);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(result.rect(), color);
    p.end();

    return result;
}

// ──────────────────────────────  Wi-Fi detection
static QString detectWifiInterface() {
    QDir dir("/sys/class/net");
    QStringList nets = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList wifiPrefixes = { "wl", "wlan", "wifi" };

    for (const QString &iface : nets) {
        for (const QString &p : wifiPrefixes) {
            if (iface.startsWith(p))
                return iface;
        }
    }
    return "wlan0";
}

static bool wifiOn_nm(const QString &iface = QString()) {
    Q_UNUSED(iface);
    QProcess p;
    p.start("bash", {"-c", "nmcli radio wifi"});
    p.waitForFinished(200);
    QString out = QString::fromUtf8(p.readAll()).trimmed();
    return (out == "enabled");
}

static int wifiQualityPercent(const QString &iface) {
    QFile f("/proc/net/wireless");
    if (!f.open(QIODevice::ReadOnly)) return -1;

    QString content = QString::fromUtf8(f.readAll());
    const QStringList lines = content.split('\n');
    for (const QString &line : lines) {
        if (!line.contains(iface + ":")) continue;
        QString l = line.trimmed();
        QStringList parts = l.split(' ', Qt::SkipEmptyParts);
        if (parts.size() < 3) return -1;
        bool ok = false;
        double link = parts[2].remove('.').toDouble(&ok);
        if (!ok) return -1;
        int perc = int(link * 100.0 / 70.0);
        return qBound(0, perc, 100);
    }
    return -1;
}

static QString detectWifiSsid(const QString &iface) {
    Q_UNUSED(iface);

    // Try iwgetid first
    {
        QProcess p;
        p.start("bash", {"-c", "iwgetid -r 2>/dev/null"});
        p.waitForFinished(600);
        QString ssid = QString::fromUtf8(p.readAll()).trimmed();
        if (!ssid.isEmpty()) return ssid;
    }

    // Try iw dev
    {
        QString cmd =
            "if command -v iw >/dev/null 2>&1; then "
            "iw dev wlan0 link | awk -F'ssid ' '/SSID/ {print $2}'; "
            "fi";

        QProcess p;
        p.start("bash", {"-c", cmd});
        p.waitForFinished(600);
        QString ssid = QString::fromUtf8(p.readAll()).trimmed();
        if (!ssid.isEmpty()) return ssid;
    }

    // Try wpa_cli
    {
        QString cmd =
            "if command -v wpa_cli >/dev/null 2>&1; then "
            "wpa_cli status | awk -F= '/^ssid=/ {print $2}'; "
            "fi";

        QProcess p;
        p.start("bash", {"-c", cmd});
        p.waitForFinished(600);
        QString ssid = QString::fromUtf8(p.readAll()).trimmed();
        if (!ssid.isEmpty()) return ssid;
    }

    // Fallback: wpa_supplicant.conf
    {
        QProcess p;
        p.start("bash", {"-c",
            "grep -m1 '^ssid=' /etc/wpa_supplicant/wpa_supplicant.conf "
            "2>/dev/null | cut -d'\"' -f2"});
        p.waitForFinished(300);
        QString ssid = QString::fromUtf8(p.readAll()).trimmed();
        if (!ssid.isEmpty()) return ssid;
    }

    return QString();
}

static QString wifiInfo(const QString &iface) {
    if (!wifiOn_nm())
        return "🔴";

    int perc = wifiQualityPercent(iface);
    QString ssid = detectWifiSsid(iface);

    QString line1 = (perc >= 0)
        ? QString("🟢 %1%").arg(perc)
        : QString("🟢 ON");

    if (!ssid.isEmpty())
        return line1 + "\n" + ssid;

    return line1;
}

static void toggleWifi_nm() {
    bool on = wifiOn_nm();
    QString cmd = on
        ? "nmcli radio wifi off"
        : "nmcli radio wifi on";
    QProcess::startDetached("bash", {"-c", cmd});
}

// ────────────────────────────── Ethernet
static QString detectEthernetInterface() {
    QDir dir("/sys/class/net");
    QStringList nets = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QStringList ethPrefixes = { "eth", "en", "eno", "enp" };

    for (const QString &iface : nets) {
        if (iface == "lo") continue;
        for (const QString &p : ethPrefixes)
            if (iface.startsWith(p))
                return iface;
    }
    return "eth0";
}

static bool ethOn(const QString &iface) {
    if (iface.isEmpty()) return false;
    QString carrier = readFile("/sys/class/net/" + iface + "/carrier");
    return (carrier == "1");
}

static QString ethInfo(const QString &iface) {
    if (!ethOn(iface))
        return "🔴";
    QString speed = readFile("/sys/class/net/" + iface + "/speed");
    return speed.isEmpty() ? "🟢" : "🟢" + speed + " Mb/s";
}

static void toggleEth(const QString &iface) {
    if (iface.isEmpty()) return;
    bool on = ethOn(iface);
    QString cmd = QString("ip link set %1 %2").arg(iface, on ? "down" : "up");
    QProcess::startDetached("bash", {"-c", cmd});
}

// ────────────────────────────── Bluetooth
static bool btOn_ctl() {
    QProcess p;
    p.start("bash", {"-c",
        "bluetoothctl show | grep -i 'Powered:' | sed 's/.*Powered: *//'"});
    p.waitForFinished(300);
    QString out = QString::fromUtf8(p.readAll()).trimmed();
    return (out == "yes" || out == "true" || out == "on");
}

static QString btInfo() {
    if (!btOn_ctl()) return "🔴";

    QProcess p;
    p.start("bash", {"-c",
        "bluetoothctl info | grep -i 'Name:' | sed 's/.*Name: *//'"});
    p.waitForFinished(300);
    QString name = QString::fromUtf8(p.readAll()).trimmed();
    return name.isEmpty() ? "🟢\nNo device connected"
                          : "🟢" + name;
}

static void toggleBt_ctl() {
    bool on = btOn_ctl();
    QString cmd = QString("bluetoothctl --timeout 1 power %1")
                  .arg(on ? "off" : "on");
    QProcess::startDetached("bash", {"-c", cmd});
}

// ────────────────────────────── GPS stub
static bool gpsOn() { return false; }
static QString gpsInfo() { return gpsOn() ? "🟢" : "🔴"; }

// ────────────────────────────── Auto-Rotate
// Shared config file (same one used by the Display settings page) so
// both stay in sync with a single "display_auto_rotate" key.
static QString rotateCfgPath() {
    return QDir::homePath() + "/.config/Alternix/osm-settings.conf";
}

static QString rotateScriptDir() {
    return QDir::homePath() + "/.config/Alternix/scripts";
}

static QString rotatePidPath() {
    return QDir::homePath() + "/.config/Alternix/rotate-monitor.pid";
}

static QMap<QString, QString> rotateLoadCfg() {
    QMap<QString, QString> map;
    QFile f(rotateCfgPath());
    if (!f.open(QIODevice::ReadOnly)) return map;
    QTextStream s(&f);
    while (!s.atEnd()) {
        QString line = s.readLine().trimmed();
        if (line.startsWith("#") || !line.contains("=")) continue;
        QStringList parts = line.split("=");
        if (parts.size() == 2)
            map[parts[0].trimmed()] = parts[1].trimmed();
    }
    return map;
}

static void rotateSaveCfg(const QMap<QString, QString> &map) {
    QDir().mkpath(QDir::homePath() + "/.config/Alternix");
    QFile f(rotateCfgPath());
    if (!f.open(QFile::WriteOnly | QFile::Truncate)) return;
    QTextStream s(&f);
    for (auto it = map.begin(); it != map.end(); ++it)
        s << it.key() << "=" << it.value() << "\n";
}

static bool autoRotateEnabled() {
    return rotateLoadCfg().value("display_auto_rotate", "false") == "true";
}

// The monitor script writes a pidfile while it's actively running and
// applying orientation changes; its presence is our "is it really on"
// signal rather than trusting the config flag alone.
static bool rotateMonitorRunning() {
    return QFile::exists(rotatePidPath());
}

static QString rotateInfo() {
    return rotateMonitorRunning() ? "🟢 ON" : "🔒 Locked";
}

static void toggleAutoRotate() {
    bool enabling = !autoRotateEnabled();

    QMap<QString, QString> cfg = rotateLoadCfg();
    cfg["display_auto_rotate"] = enabling ? "true" : "false";
    rotateSaveCfg(cfg);

    QString script = rotateScriptDir() + "/alternix-rotate-toggle.sh";
    QProcess::startDetached("bash", { script, enabling ? "on" : "off" });
}

// ──────────────────────────────  Battery helpers
static QString detectBatteryPath() {
    QDir dir("/sys/class/power_supply");
    QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : entries) {
        QString type = readFile("/sys/class/power_supply/" + name + "/type");
        if (type.toLower() == "battery")
            return "/sys/class/power_supply/" + name;
    }
    return "";
}

static int batteryPercent(const QString &base) {
    if (base.isEmpty()) return -1;
    bool ok = false;
    int v = readFile(base + "/capacity").toInt(&ok);
    return ok ? v : -1;
}

static QString batteryStatus(const QString &base) {
    if (base.isEmpty()) return "Unknown";
    return readFile(base + "/status");
}

static QString formatHoursMinutes(double hours) {
    if (hours <= 0 || std::isnan(hours) || std::isinf(hours))
        return "Est. time: Unknown";

    int h = int(hours);
    int m = int(std::round((hours - h) * 60.0));
    if (m == 60) { h++; m = 0; }

    return (h > 0)
        ? QString("Est. time: %1h %2m").arg(h).arg(m)
        : QString("Est. time: %1m").arg(m);
}

static QString batteryTimeText(const QString &base) {
    if (base.isEmpty()) return "No battery detected";

    QString status = batteryStatus(base);

    auto readLL = [&](const QString &p)->long long {
        bool ok = false;
        long long v = readFile(base + "/" + p).toLongLong(&ok);
        return ok ? v : -1;
    };

    long long now = -1, full = -1, rate = -1;

    if (QFile::exists(base + "/energy_now")) {
        now  = readLL("energy_now");
        full = readLL("energy_full");
        rate = readLL("power_now");
    } else if (QFile::exists(base + "/charge_now")) {
        now  = readLL("charge_now");
        full = readLL("charge_full");
        rate = readLL("current_now");
    }

    if (now <= 0 || full <= 0 || rate <= 0)
        return "Est. time: Unknown";

    double hours = 0.0;
    if (status == "Discharging")
        hours = double(now) / double(rate);
    else if (status == "Charging")
        hours = double(full - now) / double(rate);
    else
        return "Est. time: Unknown";

    return formatHoursMinutes(hours);
}

static QString batteryMainText(const QString &base) {
    if (base.isEmpty()) return "No battery detected";
    int pct = batteryPercent(base);
    return (pct < 0) ? "Unknown" : QString("%1%").arg(pct);
}

static QString batteryStatusLine(const QString &base) {
    if (base.isEmpty()) return "";
    QString status = batteryStatus(base).trimmed();
    if (status.startsWith("Charging", Qt::CaseInsensitive)) return "Charging";
    if (status.startsWith("Full", Qt::CaseInsensitive)) return "Full";
    return "";
}

// choose icon name based on percentage, charging, power saver
static QString selectBatteryIconName(int pct, const QString &statusRaw, bool saver) {
    QString status = statusRaw.trimmed();

    // Charging overrides everything
    if (status.startsWith("Charging", Qt::CaseInsensitive)) {
        return "battery_charge.png";
    }

    // Power-saver icon overrides level (when not charging)
    if (saver) {
        return "battery_saver.png";
    }

    // Otherwise choose level icon
    if (pct < 0) {
        return "battery_low.png";
    } else if (pct < 20) {
        return "battery_low.png";
    } else if (pct < 33) {
        return "battery25.png";
    } else if (pct < 66) {
        return "battery50.png";
    } else {
        return "battery.png";
    }
}

static void enablePowerSaver() {
    QProcess::startDetached("bash", {"-c",
        "if command -v powerprofilesctl >/dev/null 2>&1; then "
        "powerprofilesctl set power-saver; fi"});
}

static bool isPowerSaver() {
    QProcess p;
    p.start("bash", {"-c",
        "if command -v powerprofilesctl >/dev/null 2>&1; then "
        "powerprofilesctl get; else echo unknown; fi"});
    p.waitForFinished(222);
    return (QString::fromUtf8(p.readAll()).trimmed() == "power-saver");
}

static void togglePowerSaver() {
    QProcess::startDetached("bash", {"-c",
        "if command -v powerprofilesctl >/dev/null 2>&1; then "
        "cur=$(powerprofilesctl get); "
        "if [ \"$cur\" = power-saver ]; then "
        "powerprofilesctl set balanced; "
        "else powerprofilesctl set power-saver; fi; fi"});
}

// ──────────────────────────────  Tray items
struct TrayEntry {
    QString serviceName;
    QString label;
};

static QList<TrayEntry> listTrayItems() {
    QList<TrayEntry> items;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) return items;

    QDBusInterface dbusIface(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        bus
    );

    QDBusReply<QStringList> reply = dbusIface.call("ListNames");
    if (!reply.isValid()) return items;

    const QStringList names = reply.value();
    for (const QString &name : names) {
        if (!name.contains("StatusNotifierItem")) continue;

        QDBusInterface sni(
            name,
            "/StatusNotifierItem",
            "org.kde.StatusNotifierItem",
            bus
        );
        if (!sni.isValid()) continue;

        QString label = sni.property("Title").toString();
        if (label.isEmpty()) label = sni.property("Id").toString();
        if (label.isEmpty()) label = name;

        items.append({ name, label });
    }

    return items;
}

// ──────────────────────────────  Clickable icon
class ClickIcon : public QLabel {
public:
    QString cmd;
    std::function<void()> closeFunc;

    ClickIcon(const QString &path, const QString &command,
              int size = 48, QWidget *parent = nullptr)
        : QLabel(parent), cmd(command) {
        QPixmap px(path);
        if (!px.isNull())
            setPixmap(px.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            setText("?");
        setAlignment(Qt::AlignCenter);
        setStyleSheet("padding: 4px;");
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (!cmd.isEmpty())
            QProcess::startDetached(cmd, QStringList());
        if (closeFunc) closeFunc();
        QLabel::mousePressEvent(e);
    }
};

// ──────────────────────────────  Clickable label (tray)
class ClickLabel : public QLabel {
public:
    QString serviceName;
    std::function<void()> onClick;

    ClickLabel(const QString &text, const QString &service,
               QWidget *parent = nullptr)
        : QLabel(text, parent), serviceName(service) {
        setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        setStyleSheet("color:white; font-size:14pt; padding:0px 4px;");
        setFixedHeight(28);
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (onClick) onClick();
        QLabel::mousePressEvent(e);
    }
};

// ──────────────────────────────  Clickable card frame
class ClickableCard : public QFrame {
public:
    std::function<void()> onClick;
    using QFrame::QFrame;

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (onClick) onClick();
        QFrame::mousePressEvent(e);
    }
};

// ──────────────────────────────  Card creation helper
static QFrame* createCard(QWidget *parent,
                          QLayout *innerLayout,
                          bool hover = true,
                          std::function<void()> onClick = nullptr) {
    ClickableCard *frame = new ClickableCard(parent);
    frame->onClick = onClick;
    frame->setAttribute(Qt::WA_TranslucentBackground);
    frame->setStyleSheet("background:transparent;");

    QWidget *box = new QWidget(frame);
    box->setObjectName("box");

    if (hover)
        box->setStyleSheet(
            "#box { background:#80708099; border-radius:20px; }"
            "#box:hover { background-color:#80708099; border:1px solid #ffffff; }"
        );
    else
        box->setStyleSheet("#box { background:#80708099; border-radius:20px; }");

    innerLayout->setContentsMargins(10,10,10,10);
    innerLayout->setSpacing(6);
    box->setLayout(innerLayout);

    QVBoxLayout *outer = new QVBoxLayout(frame);
    outer->setContentsMargins(0,0,0,0);
    outer->addWidget(box);

    return frame;
}

// Forward declaration
class NotificationOverlay;
// ──────────────────────────────  Main overlay widget
class NotificationOverlay : public QWidget {
public:
    QLabel *clockLabel;
    QScrollArea *notifScroll;

    QWidget *topCardWidget;
    QWidget *sysRowWidget;
    QWidget *notifCardWidget;
    QWidget *brightnessCardWidget;
    QWidget *batteryCardWidget;

    QLabel *batteryInfoLabel;
    QLabel *batteryTimeLabel;
    QLabel *batteryStatusLabel;
    QLabel *batteryIconLabel;

    QVBoxLayout *notifListLayout;
    QMap<ClickLabel*, QList<QWidget*>> submenuMap;

    QList<QWidget*> orderedWidgets;
    QMap<QWidget*, QRect> finalGeomMap;
    QMap<QWidget*, QGraphicsOpacityEffect*> opacityMap;
    bool closing;

    QTimer *refreshTimer;
    bool refreshEnabled;

    NotificationOverlay()
        : QWidget(),
          clockLabel(nullptr),
          notifScroll(nullptr),
          topCardWidget(nullptr),
          sysRowWidget(nullptr),
          notifCardWidget(nullptr),
          brightnessCardWidget(nullptr),
          batteryCardWidget(nullptr),
          batteryInfoLabel(nullptr),
          batteryTimeLabel(nullptr),
          batteryStatusLabel(nullptr),
          batteryIconLabel(nullptr),
          notifListLayout(nullptr),
          closing(false),
          refreshTimer(nullptr),
          refreshEnabled(false)
    {
        setWindowFlags(Qt::FramelessWindowHint
                       | Qt::WindowStaysOnTopHint
                       | Qt::BypassWindowManagerHint
                       | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);

        QRect g = QApplication::primaryScreen()->geometry();
        setGeometry(g);

        int screenW = g.width();
        QVBoxLayout *main = new QVBoxLayout(this);
        main->setContentsMargins(20, 5, 20, 5);
        main->setSpacing(16);

        QString imgBase = QDir::homePath() + "/.config/qtile/images/";

        QString wifiIF = detectWifiInterface();
        QString ethIF  = detectEthernetInterface();
        QString batPath = detectBatteryPath();

        auto scheduleRefresh = [=](QLabel *infoLabel,
                                   std::function<QString()> infoFunc)
        {
            auto performUpdate = [=]() {
                if (infoLabel && infoFunc)
                    infoLabel->setText(infoFunc());

                if (!batPath.isEmpty()) {
                    if (batteryInfoLabel)
                        batteryInfoLabel->setText(batteryMainText(batPath));
                    if (batteryTimeLabel)
                        batteryTimeLabel->setText(batteryTimeText(batPath));
                    if (batteryStatusLabel) {
                        QString st = batteryStatusLine(batPath);
                        batteryStatusLabel->setText(st);
                        batteryStatusLabel->setVisible(!st.isEmpty());
                    }
                }
                const_cast<NotificationOverlay*>(this)->updateBatteryIconColor();
            };

            QTimer::singleShot(100,  this, performUpdate);
            QTimer::singleShot(500,  this, performUpdate);
            QTimer::singleShot(1500, this, performUpdate);
        };

        // ───────── Clock Card
        {
            QHBoxLayout *topInner = new QHBoxLayout();
            topInner->setSpacing(10);

            auto *wall = new ClickIcon(imgBase + "osm-power.png",
                                       "osm-power", 60, this);
            auto *set  = new ClickIcon(imgBase + "osm-settings.png",
                                       "osm-settings", 60, this);

            wall->closeFunc = [this]() { animatedClose(); };
            set->closeFunc  = [this]() { animatedClose(); };

            clockLabel = new QLabel("--:--:--", this);
            clockLabel->setStyleSheet("color:white; font-size:28pt;");
            clockLabel->setAlignment(Qt::AlignCenter);

            topInner->addWidget(wall);
            topInner->addStretch();
            topInner->addWidget(clockLabel);
            topInner->addStretch();
            topInner->addWidget(set);

            QFrame *topCard = createCard(this, topInner, false);
            topCard->setFixedHeight(90);
            topCardWidget = topCard;

            main->addWidget(topCard);

            QTimer *t = new QTimer(this);
            QObject::connect(t, &QTimer::timeout, [this]() {
                clockLabel->setText(QTime::currentTime().toString("HH:mm:ss"));
            });
            t->start(100);
        }

        // ───────── System Row (Wi-Fi, BT, Ethernet, GPS, Battery)
        int sysCols;
        if (screenW < 520) sysCols = 2;
        else if (screenW < 900) sysCols = 3;
        else sysCols = 5;

        {
            QWidget *row = new QWidget(this);
            QGridLayout *grid = new QGridLayout(row);
            grid->setSpacing(12);
            grid->setContentsMargins(0,0,0,0);

            int toggleIndex = 0;

            auto addToggle =
                [&](const QString &icon, const QString &labelText,
                    std::function<QString()> infoFunc,
                    std::function<void()> toggleFunc)
            {
                QVBoxLayout *inner = new QVBoxLayout();
                inner->setAlignment(Qt::AlignCenter);

                auto *ic = new ClickIcon(imgBase + icon, "", 42, this);
                ic->closeFunc = nullptr;

                QLabel *lbl = new QLabel(labelText, this);
                lbl->setStyleSheet("color:white; font-size:18pt;");
                lbl->setAlignment(Qt::AlignCenter);

                QLabel *info = new QLabel(infoFunc(), this);
                info->setStyleSheet("color:#dddddd; font-size:14pt;");
                info->setAlignment(Qt::AlignCenter);

                inner->addWidget(ic);
                inner->addWidget(lbl);
                inner->addWidget(info);

                auto onClick = [=]() {
                    if (toggleFunc) toggleFunc();
                    info->setText(infoFunc());
                    scheduleRefresh(info, infoFunc);
                };

                QFrame *card = createCard(this, inner, true, onClick);

                int r = toggleIndex / sysCols;
                int c = toggleIndex % sysCols;
                grid->addWidget(card, r, c);
                toggleIndex++;
            };

            // Wi-Fi
            addToggle("wifi.png", "Wi-Fi",
                      [wifiIF]() { return wifiInfo(wifiIF); },
                      []() { toggleWifi_nm(); });

            // Bluetooth
            addToggle("bt.png", "Bluetooth",
                      []() { return btInfo(); },
                      []() { toggleBt_ctl(); });

            // Ethernet
            addToggle("enet.png", "Ethernet",
                      [ethIF]() { return ethInfo(ethIF); },
                      [ethIF]() { toggleEth(ethIF); });

            // GPS
            addToggle("gps.png", "GPS",
                      []() { return gpsInfo(); },
                      []() {});

            // Auto-Rotate (uses the rotation sensor via iio-sensor-proxy;
            // driver setup and sensor scanning happen from the Display
            // settings page — this is just the on/off switch)
            addToggle("rotate.png", "Auto-Rotate",
                      []() { return rotateInfo(); },
                      []() { toggleAutoRotate(); });

            // ───────── Battery (icon based on status + saver)
            {
                QVBoxLayout *inner = new QVBoxLayout();
                inner->setAlignment(Qt::AlignCenter);

                batteryIconLabel = new QLabel(this);
                batteryIconLabel->setAlignment(Qt::AlignCenter);
                batteryIconLabel->setText("🔋"); // will be replaced by updateBatteryIconColor()

                QLabel *lbl = new QLabel("Battery", this);
                lbl->setStyleSheet("color:white; font-size:18pt;");
                lbl->setAlignment(Qt::AlignCenter);

                batteryInfoLabel = new QLabel(batteryMainText(batPath), this);
                batteryInfoLabel->setStyleSheet("color:#dddddd; font-size:14pt;");
                batteryInfoLabel->setAlignment(Qt::AlignCenter);

                batteryTimeLabel = new QLabel(batteryTimeText(batPath), this);
                batteryTimeLabel->setStyleSheet("color:#cccccc; font-size:14pt;");
                batteryTimeLabel->setAlignment(Qt::AlignCenter);

                batteryStatusLabel = new QLabel(batteryStatusLine(batPath), this);
                batteryStatusLabel->setStyleSheet("color:#cccccc; font-size:14pt;");
                batteryStatusLabel->setAlignment(Qt::AlignCenter);
                batteryStatusLabel->setVisible(!batteryStatusLine(batPath).isEmpty());

                inner->addWidget(batteryIconLabel);
                inner->addWidget(lbl);
                inner->addWidget(batteryInfoLabel);
                inner->addWidget(batteryTimeLabel);
                inner->addWidget(batteryStatusLabel);

                // shared updater for delayed refresh after toggling saver
                auto refreshBatteryUI = [this, batPath]() {
                    if (!batPath.isEmpty()) {
                        if (batteryInfoLabel)
                            batteryInfoLabel->setText(batteryMainText(batPath));
                        if (batteryTimeLabel)
                            batteryTimeLabel->setText(batteryTimeText(batPath));
                        if (batteryStatusLabel) {
                            QString st = batteryStatusLine(batPath);
                            batteryStatusLabel->setText(st);
                            batteryStatusLabel->setVisible(!st.isEmpty());
                        }
                    }
                    updateBatteryIconColor();
                };

                auto onClick = [=]() {
                    // Toggle power saver profile
                    togglePowerSaver();

                    // Give powerprofilesctl time to update:
                    //  - first quick refresh at 150ms
                    //  - second more relaxed refresh at 500ms
                    QTimer::singleShot(100, this, refreshBatteryUI);
                    QTimer::singleShot(500, this, refreshBatteryUI);
                    QTimer::singleShot(1000, this, refreshBatteryUI);
                };

                QFrame *batCard = createCard(this, inner, true, onClick);
                batteryCardWidget = batCard;

                int r = toggleIndex / sysCols;
                int c = toggleIndex % sysCols;
                grid->addWidget(batCard, r, c);
                toggleIndex++;
            }

            sysRowWidget = row;
            main->addWidget(row);

            updateBatteryIconColor();
        }

        // ───────── Brightness Card  (XRANDR VERSION)
        {
            QVBoxLayout *bInner = new QVBoxLayout();
            bInner->setAlignment(Qt::AlignCenter);

            QLabel *bLabel = new QLabel("Brightness", this);
            bLabel->setStyleSheet("color:white; font-size:18pt; font-weight:bold;");
            bLabel->setAlignment(Qt::AlignCenter);

            auto detectPrimaryOutput = []() -> QString {
                QProcess p;
                p.start("bash", {"-c",
                    "xrandr | awk '/ primary/{print $1; exit}'"});
                p.waitForFinished(100);
                QString out = QString::fromUtf8(p.readAll()).trimmed();
                if (out.isEmpty()) {
                    p.start("bash", {"-c",
                        "xrandr | awk '/ connected/{print $1; exit}'"});
                    p.waitForFinished(100);
                    out = QString::fromUtf8(p.readAll()).trimmed();
                }
                return out;
            };

            QString output = detectPrimaryOutput();
            if (output.isEmpty()) output = "HDMI-1";

            int savedBrightness = 80;
            {
                QSettings s("Alternix", "osm-notify");
                savedBrightness = s.value("brightness", 80).toInt();
            }

            QSlider *slider = new QSlider(Qt::Horizontal, this);
            slider->setRange(20, 100);
            slider->setValue(savedBrightness);
            slider->setFixedHeight(32);
            slider->setStyleSheet(
                "QSlider::groove:horizontal { height: 12px; background: #505050; border-radius: 6px; }"
                "QSlider::handle:horizontal { width: 32px; height: 32px; "
                " background-color:#ffffff; border-radius: 16px; margin: -10px 0; "
                " outline:none; border:0px solid transparent; }"
                "QSlider::handle:horizontal:hover { background-color: #3a3a3a; border-radius: 16px; "
                " outline:none; border:0px solid transparent; }"
            );

            {
                double factor = savedBrightness / 100.0;
                QString cmd = QString("xrandr --output %1 --brightness %2")
                                .arg(output)
                                .arg(QString::number(factor, 'f', 2));
                QProcess::startDetached("bash", {"-c", cmd});
            }

            QObject::connect(slider, &QSlider::valueChanged, this,
                             [output](int v) {
                double factor = v / 100.0;
                QString cmd = QString("xrandr --output %1 --brightness %2")
                                .arg(output)
                                .arg(QString::number(factor, 'f', 2));
                QProcess::startDetached("bash", {"-c", cmd});

                QSettings s("Alternix", "osm-notify");
                s.setValue("brightness", v);
            });

            bInner->addWidget(bLabel);
            bInner->addWidget(slider);

            QFrame *brightnessCard = createCard(this, bInner, false);
            brightnessCardWidget = brightnessCard;
            main->addWidget(brightnessCard);
        }

        // ───────── Notifications Card
        {
            QVBoxLayout *notifInner = new QVBoxLayout();
            notifInner->setAlignment(Qt::AlignTop);

            QLabel *title = new QLabel("Notifications", this);
            title->setStyleSheet("color:white; font-size:18pt; font-weight:bold;");
            title->setAlignment(Qt::AlignCenter);
            notifInner->addWidget(title);

            notifScroll = new QScrollArea(this);
            notifScroll->setWidgetResizable(true);
            notifScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            notifScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            notifScroll->setStyleSheet("background:transparent; border:none;");
            QScroller::grabGesture(notifScroll->viewport(), QScroller::TouchGesture);

            QWidget *scrollContent = new QWidget(this);
            scrollContent->setAttribute(Qt::WA_TranslucentBackground);
            scrollContent->setStyleSheet("background:transparent;");

            notifListLayout = new QVBoxLayout(scrollContent);
            notifListLayout->setContentsMargins(2,2,2,2);
            notifListLayout->setSpacing(0);

            notifScroll->setWidget(scrollContent);

            QList<TrayEntry> trayItems = listTrayItems();
            bool hasEntries = !trayItems.isEmpty();

            for (const TrayEntry &te : trayItems) {
                ClickLabel *row = new ClickLabel(
                    QString("• %1").arg(te.label),
                    te.serviceName,
                    scrollContent
                );

                row->onClick = [this, row]() {
                    toggleSubmenu(row);
                };

                notifListLayout->addWidget(row);
            }

            if (!hasEntries)
                notifScroll->hide();

            notifInner->addWidget(notifScroll);

            QFrame *card = createCard(this, notifInner, true);
            notifCardWidget = card;
            main->addWidget(card);
        }

        main->addStretch();

        orderedWidgets.clear();
        orderedWidgets << topCardWidget
                       << sysRowWidget
                       << brightnessCardWidget
                       << notifCardWidget;

        for (QWidget *w : orderedWidgets) {
            if (w) w->setVisible(false);
        }

        // ───────── Controlled periodic refresh (battery etc.)
        refreshTimer = new QTimer(this);
        refreshTimer->setInterval(500);   // 500ms global refresh
        QObject::connect(refreshTimer, &QTimer::timeout, this, [=]() {
            if (!refreshEnabled) return;
            if (!this->isVisible()) return;

            if (!batPath.isEmpty()) {
                if (batteryInfoLabel)
                    batteryInfoLabel->setText(batteryMainText(batPath));
                if (batteryTimeLabel)
                    batteryTimeLabel->setText(batteryTimeText(batPath));
                if (batteryStatusLabel) {
                    QString st = batteryStatusLine(batPath);
                    batteryStatusLabel->setText(st);
                    batteryStatusLabel->setVisible(!st.isEmpty());
                }
            }
            updateBatteryIconColor();
        });
    }

    void openPanel() {
        if (closing) return;

        if (refreshTimer) {
            refreshEnabled = false;
            refreshTimer->stop();
        }

        if (isVisible()) {
            hide();
            lower();
        }

        closing = false;

        for (QWidget *w : orderedWidgets) {
            if (!w) continue;
            if (auto *eff =
                    qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect())) {
                eff->setOpacity(0.0);
            }
            w->setVisible(false);
        }

        showFullScreen();
        updateBatteryIconColor();
        raise();
    }

    void playOpenAnimation() {
        updateBatteryIconColor();

        const double startScale = 0.3;

        for (QWidget *w : orderedWidgets) {
            if (w) w->setVisible(true);
        }

        finalGeomMap.clear();

        for (QWidget *w : orderedWidgets) {
            if (!w) continue;

            QRect endRect = w->geometry();
            if (!endRect.isValid()) continue;

            finalGeomMap[w] = endRect;

            QGraphicsOpacityEffect *eff =
                qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect());
            if (!eff) {
                eff = new QGraphicsOpacityEffect(w);
                w->setGraphicsEffect(eff);
            }
            eff->setOpacity(0.0);
            opacityMap[w] = eff;

            int newW = qMax(1, int(endRect.width() * startScale));
            int newH = qMax(1, int(endRect.height() * startScale));
            QPoint c = endRect.center();
            QRect startRect(c.x() - newW/2, c.y() - newH/2, newW, newH);

            w->setGeometry(startRect);
        }

        // Sequential cascade, but faster than the original
        QSequentialAnimationGroup *seq = new QSequentialAnimationGroup(this);

        for (QWidget *w : orderedWidgets) {
            if (!w) continue;

            QGraphicsOpacityEffect *eff = opacityMap.value(w, nullptr);
            if (!eff) continue;

            QParallelAnimationGroup *par = new QParallelAnimationGroup(seq);

            QPropertyAnimation *gAnim = new QPropertyAnimation(w, "geometry");
            gAnim->setDuration(80); // was 110
            gAnim->setStartValue(w->geometry());
            gAnim->setEndValue(finalGeomMap[w]);
            gAnim->setEasingCurve(QEasingCurve::OutCubic);

            QPropertyAnimation *oAnim = new QPropertyAnimation(eff, "opacity");
            oAnim->setDuration(70); // was 100
            oAnim->setStartValue(0.0);
            oAnim->setEndValue(1.0);
            oAnim->setEasingCurve(QEasingCurve::OutQuad);

            par->addAnimation(gAnim);
            par->addAnimation(oAnim);

            seq->addAnimation(par);
            seq->addPause(10); // was 20
        }

        QObject::connect(seq, &QSequentialAnimationGroup::finished,
                         this, [this]() {
            if (refreshTimer) {
                refreshEnabled = true;
                refreshTimer->start();
            }
        });

        seq->start(QAbstractAnimation::DeleteWhenStopped);
    }
    void animatedClose() {
        if (closing) return;
        closing = true;

        if (refreshTimer) {
            refreshEnabled = false;
            refreshTimer->stop();
        }

        const double endScale = 0.3;

        if (finalGeomMap.isEmpty()) {
            for (QWidget *w : orderedWidgets) {
                if (w && w->geometry().isValid())
                    finalGeomMap[w] = w->geometry();
            }
        }

        QList<QWidget*> closeOrder = orderedWidgets;
        std::reverse(closeOrder.begin(), closeOrder.end());

        // Sequential close (reverse order), faster than original
        QSequentialAnimationGroup *seq = new QSequentialAnimationGroup(this);

        for (QWidget *w : closeOrder) {
            if (!w) continue;

            QRect baseRect = finalGeomMap.value(w, w->geometry());
            if (!baseRect.isValid()) continue;

            QGraphicsOpacityEffect *eff = opacityMap.value(w, nullptr);
            if (!eff) {
                eff = new QGraphicsOpacityEffect(w);
                w->setGraphicsEffect(eff);
                eff->setOpacity(1.0);
                opacityMap[w] = eff;
            }

            int newW = qMax(1, int(baseRect.width() * endScale));
            int newH = qMax(1, int(baseRect.height() * endScale));
            QPoint c = baseRect.center();
            QRect small(c.x() - newW/2, c.y() - newH/2, newW, newH);

            QParallelAnimationGroup *par = new QParallelAnimationGroup(seq);

            QPropertyAnimation *gAnim = new QPropertyAnimation(w, "geometry");
            gAnim->setDuration(70); // was 100
            gAnim->setStartValue(w->geometry());
            gAnim->setEndValue(small);
            gAnim->setEasingCurve(QEasingCurve::InCubic);

            QPropertyAnimation *oAnim = new QPropertyAnimation(eff, "opacity");
            oAnim->setDuration(60); // was 90
            oAnim->setStartValue(eff->opacity());
            oAnim->setEndValue(0.0);
            oAnim->setEasingCurve(QEasingCurve::InQuad);

            par->addAnimation(gAnim);
            par->addAnimation(oAnim);

            seq->addAnimation(par);
            seq->addPause(8); // was 15
        }

        QObject::connect(seq, &QSequentialAnimationGroup::finished,
            this, [this]() {

            for (QWidget *w : orderedWidgets) {
                if (!w) continue;

                QRect r = finalGeomMap.value(w);
                if (r.isValid())
                    w->setGeometry(r);

                if (auto *eff = qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect()))
                    eff->setOpacity(0.0);

                w->setVisible(false);
            }

            closing = false;

            hide();
            lower();
            setVisible(false);
            setEnabled(true);

            QTimer::singleShot(10, this, [this]() {
                this->repaint();
            });
        });

        seq->start(QAbstractAnimation::DeleteWhenStopped);
    }

    // ──────────────────────────────  Update battery icon from icons
    void updateBatteryIconColor() {
        if (!batteryIconLabel) return;

        QString batPath = detectBatteryPath();
        if (batPath.isEmpty()) return;

        int pct = batteryPercent(batPath);
        QString rawStatus = batteryStatus(batPath);
        QString status = rawStatus.trimmed();
        bool saver = isPowerSaver();

        QString iconName = selectBatteryIconName(pct, status, saver);
        QString fullPath = QDir::homePath() + "/.config/qtile/images/" + iconName;

        QPixmap px(fullPath);
        if (!px.isNull()) {
            batteryIconLabel->setPixmap(
                px.scaled(42, 42, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            // fallback so we don't leave it blank
            batteryIconLabel->setText("🔋");
        }
    }

    void toggleSubmenu(ClickLabel *label) {
        if (!notifListLayout || !label) return;

        if (submenuMap.contains(label)) {
            QList<QWidget*> widgets = submenuMap.take(label);
            for (QWidget *w : widgets) {
                notifListLayout->removeWidget(w);
                w->deleteLater();
            }
            return;
        }

        int idx = -1;
        for (int i=0; i<notifListLayout->count(); ++i) {
            if (notifListLayout->itemAt(i)->widget() == label) {
                idx = i; break;
            }
        }
        if (idx < 0) return;

        QWidget *container = new QWidget(this);
        QHBoxLayout *h = new QHBoxLayout(container);
        h->setContentsMargins(32,4,4,4);
        h->setSpacing(8);

        QPushButton *btnMenu = new QPushButton("Open menu", container);
        btnMenu->setMinimumHeight(32);
        btnMenu->setStyleSheet(
            "QPushButton { background:#80708099; border-radius:16px;"
            " padding:6px 16px; color:white; font-size:12pt; }"
            "QPushButton:hover { background:#282828; border:none; }"
            "QPushButton:pressed { background:#282828; border:1px solid #ffffff; }"
        );

        h->addWidget(btnMenu);
        h->addStretch();

        notifListLayout->insertWidget(idx+1, container);
        submenuMap[label] = { container };

        QObject::connect(btnMenu, &QPushButton::clicked, this, [label]() {
            QDBusConnection bus = QDBusConnection::sessionBus();
            if (!bus.isConnected()) return;

            QDBusInterface sni(
                label->serviceName,
                "/StatusNotifierItem",
                "org.kde.StatusNotifierItem",
                bus
            );
            if (!sni.isValid()) return;

            sni.call("ContextMenu", 0, 0);
        });
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(0,0,0,140));

        int w = width();
        int h = height();
        int fadeH = 75;

        QLinearGradient top(0,0,0,fadeH*2);
        top.setColorAt(0, QColor(0,0,0,255));
        top.setColorAt(0.5, QColor(0,0,0,255));
        top.setColorAt(1, QColor(0,0,0,0));
        p.fillRect(0,0,w,fadeH*3, top);

        QLinearGradient bottom(0,h-fadeH*2,0,h);
        bottom.setColorAt(0, QColor(0,0,0,0));
        bottom.setColorAt(0.5, QColor(0,0,0,255));
        bottom.setColorAt(1, QColor(0,0,0,255));
        p.fillRect(0,h-fadeH*2,w,fadeH*2, bottom);
    }

    void mousePressEvent(QMouseEvent *e) override {
        QWidget *c = childAt(e->pos());

        if (c) {
            QWidget *w = c;
            while (w && w != this) {
                if (w == notifCardWidget ||
                    w == topCardWidget  ||
                    w == sysRowWidget   ||
                    w == brightnessCardWidget ||
                    w == batteryCardWidget) {
                    QWidget::mousePressEvent(e);
                    return;
                }
                w = w->parentWidget();
            }
        }

        animatedClose();
    }

    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Escape)
            animatedClose();
        else
            QWidget::keyPressEvent(e);
    }

    void showEvent(QShowEvent *e) override {
        QWidget::showEvent(e);
        QTimer::singleShot(0, this, [this]() {
            playOpenAnimation();
        });
    }
};

// ──────────────────────────────  Activation edge bar (swipe-only)
class ActivationEdgeBar : public QWidget {
public:
    NotificationOverlay *overlay;
    bool dragging;
    QPoint pressPos;

    explicit ActivationEdgeBar(NotificationOverlay *ov,
                               int x, int y, int w, int h,
                               QWidget *parent = nullptr)
        : QWidget(parent),
          overlay(ov),
          dragging(false)
    {
        setWindowFlags(Qt::FramelessWindowHint
                       | Qt::WindowStaysOnTopHint
                       | Qt::BypassWindowManagerHint
                       | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);

        setGeometry(x, y, w, h);
        setStyleSheet("background: rgba(0,0,0,0);");

        show();
        raise();
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            dragging = true;
            pressPos = e->globalPos();
        }
        QWidget::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!dragging) {
            QWidget::mouseMoveEvent(e);
            return;
        }

        int dy = e->globalY() - pressPos.y();
        if (dy > 12) {  // swipe threshold
            if (overlay) {
                overlay->openPanel();
            }
            dragging = false;
        }

        QWidget::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        dragging = false;
        QWidget::mouseReleaseEvent(e);
    }

    void paintEvent(QPaintEvent *) override {
        // fully invisible
    }
};

// ──────────────────────────────  main
int main(int argc, char **argv) {
    QApplication a(argc, argv);

    NotificationOverlay overlay;

    QRect g = QApplication::primaryScreen()->geometry();
    int W = g.width();
    int barHeight = 50;

    double edgeFrac   = 0.18;  // left & right exclusion
    double centerFrac = 0.14;  // center (clock) exclusion

    int edgeExclusion   = static_cast<int>(W * edgeFrac);
    int centerExclusion = static_cast<int>(W * centerFrac);

    int maxExcl = W / 3;
    edgeExclusion   = qMin(edgeExclusion,   maxExcl);
    centerExclusion = qMin(centerExclusion, maxExcl);

    int usableWidth = W - 2 * edgeExclusion - centerExclusion;

    if (usableWidth <= 20) {
        int barWidth = W * 0.5;
        int x = g.x() + (W - barWidth) / 2;
        new ActivationEdgeBar(&overlay, x, g.y(), barWidth, barHeight);
    } else {
        int swipeWidth = usableWidth / 2;

        int leftSwipeX = g.x() + edgeExclusion;
        new ActivationEdgeBar(&overlay, leftSwipeX, g.y(), swipeWidth, barHeight);

        int rightSwipeX = leftSwipeX + swipeWidth + centerExclusion;
        new ActivationEdgeBar(&overlay, rightSwipeX, g.y(), swipeWidth, barHeight);
    }

    return a.exec();
}
