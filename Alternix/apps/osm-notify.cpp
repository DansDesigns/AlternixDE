#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QScreen>
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
#include <functional>
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
#include <QSet>
#include <QRegularExpression>
#include <QPointer>
#include <QDateTime>
#include <memory>
#include <algorithm>
#include <QPropertyAnimation>


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

// All quick-settings cards use the same authority as their Settings pages:
// NetworkManager (Wi-Fi), BlueZ (Bluetooth), kernel IFF_UP (Ethernet),
// ModemManager (cellular), and the existing Alternix settings for Location
// and screen rotation. Never infer a power state from cable carrier / GPS fix.
static QString alternixSettingsPath() {
    return QDir::homePath() + "/.config/Alternix/osm-settings.conf";
}

static QString detectEthernetInterface() {
    QDir dir("/sys/class/net");
    const QStringList names = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        if (!(name.startsWith("en") || name.startsWith("eth"))) continue;
        const QString base = "/sys/class/net/" + name;
        if (QFileInfo::exists(base + "/wireless") ||
            QFileInfo::exists(base + "/phy80211")) continue;
        if (readFile(base + "/type") == "1") return name;
    }
    return {};
}

static QString detectWifiInterface() {
    const QDir dir("/sys/class/net");
    for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString root = "/sys/class/net/" + name;
        if (QFileInfo::exists(root + "/wireless") ||
            QFileInfo::exists(root + "/phy80211")) return name;
    }
    return {};
}

// Preserve the signal percentage shown by the original notification card.
static int wifiSignalPercent() {
    const QString iface = detectWifiInterface();
    QFile f("/proc/net/wireless");
    if (iface.isEmpty() || !f.open(QIODevice::ReadOnly)) return -1;
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    for (const QString &line : lines) {
        if (!line.trimmed().startsWith(iface + ":")) continue;
        const QStringList cols = line.trimmed().split(QRegularExpression("\\s+"),
                                                     Qt::SkipEmptyParts);
        if (cols.size() < 3) return -1;
        QString level = cols.at(2);
        level.remove('.');
        bool ok = false;
        const double link = level.toDouble(&ok);
        return ok ? qBound(0, int(link * 100.0 / 70.0), 100) : -1;
    }
    return -1;
}

// IFF_UP (bit zero) means administratively enabled, even with no cable.
static bool ethernetAdministrativelyUp(const QString &iface, bool *known = nullptr) {
    const QString flags = readFile("/sys/class/net/" + iface + "/flags");
    bool ok = false;
    const uint mask = flags.toUInt(&ok, 0);
    if (known) *known = ok;
    return ok && (mask & 1U);
}

static QString rotateScriptPath() {
    return QDir::homePath() + "/.config/Alternix/scripts/alternix-rotate-toggle.sh";
}
static QString rotatePidPath() {
    return QDir::homePath() + "/.config/Alternix/rotate-monitor.pid";
}
static bool autoRotateEnabled() {
    QSettings s(alternixSettingsPath(), QSettings::IniFormat);
    return s.value("display_auto_rotate", false).toBool();
}

// The presence of a leftover PID file alone is NOT evidence of a live sensor.
static bool rotateMonitorRunning() {
    const QString pid = readFile(rotatePidPath());
    bool ok = false;
    const qint64 n = pid.toLongLong(&ok);
    if (!ok || n <= 1) return false;
    return QFileInfo::exists(QString("/proc/%1").arg(n));
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

    QWidget *topCardWidget;
    QWidget *sysRowWidget;
    QWidget *brightnessCardWidget;
    QWidget *batteryCardWidget;

    QLabel *batteryInfoLabel;
    QLabel *batteryTimeLabel;
    QLabel *batteryStatusLabel;
    QLabel *batteryIconLabel;

    QList<QWidget*> orderedWidgets;
    QMap<QWidget*, QRect> finalGeomMap;
    QMap<QWidget*, QGraphicsOpacityEffect*> opacityMap;
    bool closing;

    QTimer *refreshTimer;
    QTimer *deviceTimer = nullptr;
    bool refreshEnabled;

    struct DeviceState {
        bool known = false;
        bool enabled = false;
        QString detail;
    };
    QMap<QString, DeviceState> devices;
    QMap<QString, QLabel*> deviceLabels;
    QMap<QString, QFrame*> deviceFrames;
    QSet<QString> pollsInFlight;
    QSet<QString> togglesInFlight;
    QMap<QString, qint64> lastErrorUntil;
    QMap<QString, QString> lastErrorMessage;
    QString modemId;
    QString ethernetIface;

    NotificationOverlay()
        : QWidget(),
          clockLabel(nullptr),
          topCardWidget(nullptr),
          sysRowWidget(nullptr),
          brightnessCardWidget(nullptr),
          batteryCardWidget(nullptr),
          batteryInfoLabel(nullptr),
          batteryTimeLabel(nullptr),
          batteryStatusLabel(nullptr),
          batteryIconLabel(nullptr),
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

        ethernetIface = detectEthernetInterface();
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

        // ───────── Clock Card & corner shortcuts
        {
            QHBoxLayout *topInner = new QHBoxLayout();
            topInner->setSpacing(10);

            //-----------Left Button ---------------
            // auto *wall = new ClickIcon(imgBase + "osm-power.png", "osm-power", 60, this);
            auto *wall = new ClickIcon(imgBase + "osm-widgets.png", "osm-widgets-settings", 60, this);

            //-----------Right Button ---------------
            auto *set  = new ClickIcon(imgBase + "osm-settings.png", "osm-settings", 60, this);

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

            auto addToggle = [&](const QString &key, const QString &icon,
                                 const QString &labelText) {
                QVBoxLayout *inner = new QVBoxLayout();
                inner->setAlignment(Qt::AlignCenter);

                auto *ic = new ClickIcon(imgBase + icon, "", 42, this);
                if (key == "mobile" && QPixmap(imgBase + icon).isNull())
                    ic->setText(QString::fromUtf8("📱"));
                QLabel *lbl = new QLabel(labelText, this);
                lbl->setStyleSheet("color:white; font-size:18pt;");
                lbl->setAlignment(Qt::AlignCenter);
                QLabel *info = new QLabel("Checking…", this);
                info->setStyleSheet("color:#dddddd; font-size:14pt;");
                info->setAlignment(Qt::AlignCenter);
                info->setWordWrap(true);

                inner->addWidget(ic);
                inner->addWidget(lbl);
                inner->addWidget(info);
                QFrame *card = createCard(this, inner, true, [this, key]() {
                    toggleDevice(key);
                });
                deviceLabels.insert(key, info);
                deviceFrames.insert(key, card);

                const int r = toggleIndex / sysCols;
                const int c = toggleIndex % sysCols;
                grid->addWidget(card, r, c);
                ++toggleIndex;
            };

            addToggle("wifi", "wifi.png", "Wi-Fi");
            addToggle("bluetooth", "bt.png", "Bluetooth");
            addToggle("ethernet", "enet.png", "Ethernet");
            addToggle("mobile", "mobile.png", "Mobile");
            addToggle("gps", "gps.png", "GPS chip");
            addToggle("rotation", "rotate.png", "Auto-Rotate");

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

        main->addStretch();

        orderedWidgets.clear();
        orderedWidgets << topCardWidget
                       << sysRowWidget
                       << brightnessCardWidget;

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

        // Device probes are asynchronous. They cannot stall animation or
        // trigger authentication merely by querying status.
        deviceTimer = new QTimer(this);
        deviceTimer->setInterval(1500);
        connect(deviceTimer, &QTimer::timeout, this, [this]() {
            if (refreshEnabled && isVisible()) refreshDeviceStates();
        });
    }

    // Asynchronous QProcess with cancellation and exit-code checking. The callback
    // is delivered only while the overlay exists; no detached root commands.
    void runDeviceProcess(const QString &program, const QStringList &args,
                          int timeoutMs,
                          const std::function<void(const QString &, bool)> &done) {
        auto *p = new QProcess(this);
        p->setProcessChannelMode(QProcess::MergedChannels);
        auto *deadline = new QTimer(p);
        deadline->setSingleShot(true);
        auto finished = std::make_shared<bool>(false);
        auto complete = [p, deadline, finished, done](bool success) {
            if (*finished) return;
            *finished = true;
            deadline->stop();
            const QString result = QString::fromUtf8(p->readAll()).trimmed();
            done(result.isEmpty() && !success ? p->errorString() : result, success);
            p->deleteLater();
        };
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [complete](int rc, QProcess::ExitStatus exitStatus) {
            complete(rc == 0 && exitStatus == QProcess::NormalExit);
        });
        connect(p, &QProcess::errorOccurred, this,
                [p, complete](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart) complete(false);
        });
        connect(deadline, &QTimer::timeout, this, [p]() {
            if (p->state() != QProcess::NotRunning) p->kill();
        });
        p->start(program, args);
        deadline->start(timeoutMs);
    }

    void setDeviceState(const QString &key, bool known, bool enabled,
                        const QString &detail = QString()) {
        DeviceState next;
        next.known = known;
        next.enabled = enabled;
        next.detail = detail;
        devices[key] = next;
        if (QLabel *label = deviceLabels.value(key, nullptr)) {
            const QString text = !known ? QString::fromUtf8("⚪ Unavailable")
                                        : (enabled ? QString::fromUtf8("🟢 On")
                                                   : QString::fromUtf8("🔴 Off"));
            if (lastErrorUntil.value(key) > QDateTime::currentMSecsSinceEpoch()) {
                label->setText(QString::fromUtf8("⚠ Failed") + "\n" +
                               lastErrorMessage.value(key).left(65));
                label->setToolTip(lastErrorMessage.value(key));
            } else {
                label->setText(detail.isEmpty() ? text : text + "\n" + detail);
                label->setToolTip(detail);
            }
        }
        if (QFrame *frame = deviceFrames.value(key, nullptr)) {
            frame->setToolTip(detail);
        }
    }

    void pollCommand(const QString &key, const QString &program,
                     const QStringList &args,
                     const std::function<void(const QString &, bool)> &done) {
        if (pollsInFlight.contains(key) || togglesInFlight.contains(key)) return;
        pollsInFlight.insert(key);
        runDeviceProcess(program, args, 4000,
            [this, key, done](const QString &out, bool ok) {
                pollsInFlight.remove(key);
                done(out, ok);
            });
    }

    void refreshDeviceStates() {
        // Kernel and settings-backed statuses require no process spawning.
        ethernetIface = detectEthernetInterface();
        bool ethKnown = false;
        const bool ethUp = !ethernetIface.isEmpty() &&
                            ethernetAdministrativelyUp(ethernetIface, &ethKnown);
        if (!togglesInFlight.contains("ethernet")) {
            if (ethKnown) {
                const bool link = readFile("/sys/class/net/" + ethernetIface + "/carrier") == "1";
                const QString speed = readFile("/sys/class/net/" + ethernetIface + "/speed");
                setDeviceState("ethernet", true, ethUp,
                               !ethUp ? "Adapter disabled" :
                               (link ? (speed.isEmpty() ? "Cable connected" : speed + " Mb/s") :
                                       "Cable unplugged"));
            } else setDeviceState("ethernet", false, false, "No Ethernet adapter");
        }

        QSettings cfg(alternixSettingsPath(), QSettings::IniFormat);
        const bool locOn = cfg.value("Location/enabled", true).toBool();
        const bool gpsSource = cfg.value("Location/serial_gps_enabled", true).toBool();
        setDeviceState("gps", true, locOn && gpsSource,
                       !locOn ? "Location is off" :
                       (gpsSource ? "GPS source enabled" : "GPS source disabled"));
        const bool rotateOn = autoRotateEnabled();
        const bool monitor = rotateMonitorRunning();
        setDeviceState("rotation", QFileInfo::exists(rotateScriptPath()), rotateOn,
            rotateOn ? (monitor ? "Sensor active" : "Monitor not running") : "Rotation locked");

        pollCommand("wifi", "nmcli", {"radio", "wifi"},
            [this](const QString &out, bool ok) {
                const QString value = out.trimmed().toLower();
                if (!ok || (value != "enabled" && value != "disabled")) {
                    setDeviceState("wifi", false, false, "NetworkManager unavailable");
                    return;
                }
                const bool enabled = value == "enabled";
                const int signal = wifiSignalPercent();
                const QString signalText = (signal >= 0)
                    ? QString("%1% signal").arg(signal) : "Radio enabled";
                setDeviceState("wifi", true, enabled, enabled ? signalText : "Radio disabled");
                if (enabled) pollCommand("wifi-ssid", "nmcli",
                    {"-t", "-f", "IN-USE,SSID", "device", "wifi", "list", "--rescan", "no"},
                    [this](const QString &ssids, bool valid) {
                        if (!valid || !devices.value("wifi").enabled) return;
                        for (const QString &line : ssids.split('\n')) {
                            if (line.startsWith("*:")) {
                                const int percent = wifiSignalPercent();
                                const QString name = line.mid(2).trimmed();
                                setDeviceState("wifi", true, true,
                                    percent < 0 ? name :
                                    QString("%1% · %2").arg(percent).arg(name));
                                break;
                            }
                        }
                    });
            });

        pollCommand("bluetooth", "bluetoothctl", {"show"},
            [this](const QString &out, bool ok) {
                if (!ok || out.contains("No default controller", Qt::CaseInsensitive)) {
                    setDeviceState("bluetooth", false, false, "No usable Bluetooth adapter");
                    return;
                }
                QRegularExpression re("(?:^|\\n)\\s*Powered:\\s*(yes|no)",
                                      QRegularExpression::CaseInsensitiveOption);
                auto m = re.match(out);
                if (!m.hasMatch()) {
                    setDeviceState("bluetooth", false, false, "Cannot read adapter power");
                    return;
                }
                const bool powered = m.captured(1).compare("yes", Qt::CaseInsensitive) == 0;
                setDeviceState("bluetooth", true, powered,
                               powered ? "Adapter powered" : "Adapter switched off");
                if (powered) pollCommand("bluetooth-connected", "bluetoothctl",
                    {"devices", "Connected"},
                    [this](const QString &devicesText, bool valid) {
                        if (!valid || !devices.value("bluetooth").enabled) return;
                        for (const QString &line : devicesText.split('\n')) {
                            if (!line.startsWith("Device ")) continue;
                            const QString name = line.mid(7).section(' ', 1).trimmed();
                            if (!name.isEmpty()) {
                                setDeviceState("bluetooth", true, true, name);
                                return;
                            }
                        }
                        setDeviceState("bluetooth", true, true, "No device connected");
                    });
            });

        if (!pollsInFlight.contains("mobile") && !togglesInFlight.contains("mobile")) {
            pollsInFlight.insert("mobile");
            runDeviceProcess("mmcli", {"-L"}, 4000, [this](const QString &out, bool ok) {
                QRegularExpression re("/Modem/(\\d+)");
                auto m = re.match(out);
                if (!ok || !m.hasMatch()) {
                    modemId.clear();
                    pollsInFlight.remove("mobile");
                    setDeviceState("mobile", false, false, "No mobile modem");
                    return;
                }
                modemId = m.captured(1);
                runDeviceProcess("mmcli", {"-m", modemId}, 4000,
                    [this](const QString &text, bool valid) {
                        pollsInFlight.remove("mobile");
                        QRegularExpression stateRe("(?:^|\\n)[^\\n]*?state:\\s*'?([^'\\n]+)",
                                                   QRegularExpression::CaseInsensitiveOption);
                        auto st = stateRe.match(text);
                        if (!valid || !st.hasMatch()) {
                            setDeviceState("mobile", false, false, "Modem status unavailable");
                            return;
                        }
                        const QString value = st.captured(1).trimmed().toLower();
                        const bool powered = value.startsWith("connected") ||
                                             value.startsWith("registered") ||
                                             value.startsWith("enabled") ||
                                             value.startsWith("searching");
                        setDeviceState("mobile", true, powered, value);
                    });
            });
        }
    }

    static bool authNeeded(const QString &out) {
        return out.contains("not authorized", Qt::CaseInsensitive) ||
               out.contains("not authorised", Qt::CaseInsensitive) ||
               out.contains("permission", Qt::CaseInsensitive) ||
               out.contains("not permitted", Qt::CaseInsensitive) ||
               out.contains("unauthorized", Qt::CaseInsensitive) ||
               out.contains("notauthorized", Qt::CaseInsensitive) ||
               out.contains("access denied", Qt::CaseInsensitive) ||
               out.contains("authorization required", Qt::CaseInsensitive);
    }

    void reportToggleResult(const QString &key, const QString &output, bool success) {
        togglesInFlight.remove(key);
        if (!success) {
            const QString message = output.isEmpty() ? "Device action failed" : output;
            lastErrorUntil[key] = QDateTime::currentMSecsSinceEpoch() + 5000;
            lastErrorMessage[key] = message;
            if (QLabel *label = deviceLabels.value(key, nullptr)) {
                label->setText(QString::fromUtf8("⚠ Failed") + "\n" + message.left(65));
                label->setToolTip(message);
            }
        } else {
            lastErrorUntil.remove(key);
            lastErrorMessage.remove(key);
        }
        // Re-read the actual backend state. Never assume the click worked.
        QTimer::singleShot(150, this, [this]() { refreshDeviceStates(); });
        QTimer::singleShot(1000, this, [this]() { refreshDeviceStates(); });
    }

    void setWithAuth(const QString &key, const QString &program,
                     const QStringList &args, int commandTimeout = 12000) {
        if (togglesInFlight.contains(key)) return;
        togglesInFlight.insert(key);
        if (QLabel *label = deviceLabels.value(key, nullptr)) label->setText("Changing…");
        runDeviceProcess(program, args, commandTimeout,
            [this, key, program, args](const QString &out, bool ok) {
                if (ok && !out.contains("Failed", Qt::CaseInsensitive) &&
                    !out.contains("Error:", Qt::CaseInsensitive)) {
                    reportToggleResult(key, out, true); return;
                }
                if (!authNeeded(out)) { reportToggleResult(key, out, false); return; }
                if (!QFileInfo("/usr/local/bin/osm-sudo").isExecutable()) {
                    reportToggleResult(key, "osm-sudo is not installed: " + out, false);
                    return;
                }
                QStringList wrapped{program};
                wrapped.append(args);
                // The panel bypasses the window manager and stays on top:
                // hide it while osm-lock asks for a pattern, or it may obscure
                // the authentication UI completely.
                const bool resumePanel = isVisible();
                if (resumePanel) {
                    if (deviceTimer) deviceTimer->stop();
                    hide();
                    lower();
                }
                // osm-sudo is an authenticating command wrapper, NOT SUDO_ASKPASS.
                runDeviceProcess("/usr/local/bin/osm-sudo", wrapped, 120000,
                    [this, key, resumePanel](const QString &response, bool success) {
                        reportToggleResult(key, response, success);
                        if (resumePanel && !closing) openPanel();
                    });
            });
    }

    void toggleDevice(const QString &key) {
        if (togglesInFlight.contains(key)) return;
        lastErrorUntil.remove(key);
        lastErrorMessage.remove(key);
        if (key == "gps") {
            QSettings cfg(alternixSettingsPath(), QSettings::IniFormat);
            const bool locationOn = cfg.value("Location/enabled", true).toBool();
            const bool gpsOn = cfg.value("Location/serial_gps_enabled", true).toBool();
            if (!locationOn) {
                cfg.setValue("Location/enabled", true);
                cfg.setValue("Location/serial_gps_enabled", true);
            } else {
                cfg.setValue("Location/serial_gps_enabled", !gpsOn);
            }
            cfg.sync();
            if (cfg.status() != QSettings::NoError)
                reportToggleResult(key, "Cannot save GPS settings", false);
            refreshDeviceStates();
            return;
        }
        if (key == "rotation") {
            if (!QFileInfo::exists(rotateScriptPath())) {
                reportToggleResult(key, "Rotation toggle script missing", false);
                return;
            }
            const bool target = !autoRotateEnabled();
            QSettings cfg(alternixSettingsPath(), QSettings::IniFormat);
            cfg.setValue("display_auto_rotate", target);
            cfg.sync();
            if (cfg.status() != QSettings::NoError) {
                reportToggleResult(key, "Unable to save auto-rotation", false);
                return;
            }
            togglesInFlight.insert(key);
            runDeviceProcess("bash", {rotateScriptPath(), target ? "on" : "off"}, 10000,
                [this, target, key](const QString &out, bool ok) {
                    if (!ok) {
                        QSettings cfg(alternixSettingsPath(), QSettings::IniFormat);
                        cfg.setValue("display_auto_rotate", !target);
                        cfg.sync();
                    }
                    reportToggleResult(key, out, ok);
                });
            return;
        }
        const DeviceState current = devices.value(key);
        if (!current.known) {
            reportToggleResult(key, "Hardware or service unavailable", false);
            return;
        }
        const QString desired = current.enabled ? "off" : "on";
        if (key == "wifi") setWithAuth(key, "nmcli", {"radio", "wifi", desired});
        else if (key == "bluetooth") setWithAuth(key, "bluetoothctl", {"power", desired});
        else if (key == "ethernet") {
            if (!ethernetIface.isEmpty())
                setWithAuth(key, "ip", {"link", "set", ethernetIface,
                                         current.enabled ? "down" : "up"});
        } else if (key == "mobile") {
            if (!modemId.isEmpty())
                setWithAuth(key, "mmcli", {"-m", modemId,
                                            current.enabled ? "--disable" : "--enable"}, 20000);
        }
    }

    void openPanel() {
        if (closing) return;

        if (refreshTimer) {
            refreshEnabled = false;
            refreshTimer->stop();
        }
        if (deviceTimer) deviceTimer->stop();

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
        refreshDeviceStates();
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
            if (deviceTimer) deviceTimer->start();
            refreshDeviceStates();
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
        if (deviceTimer) deviceTimer->stop();

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
                if (w == topCardWidget  ||
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
