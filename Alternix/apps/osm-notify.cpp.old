#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QScreen>
#include <QTimer>
#include <QSocketNotifier>
#include <QCryptographicHash>
#include <QDebug>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <QPainter>
#include <QMouseEvent>
#include <QTouchEvent>
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
#include <QStandardPaths>
#include <time.h>
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


// Keep Xlib AFTER every Qt and C++ header. Xlib exports macros such as
// None, Status and Unsorted that corrupt Qt5's QUrl/QTextStream/QDir enums
// when Qt headers are parsed after Xlib (Devuan/XLibre build failure).
#include <X11/Xlib.h>
// These are not used by this overlay; don't leak them into Qt expressions.
#ifdef None
#  undef None
#endif
#ifdef Status
#  undef Status
#endif
#ifdef Unsorted
#  undef Unsorted
#endif

// Use one X11 connection for reading the live root geometry and ensuring
// the transparent swipe zones remain in front of Qtile's top bar.  A Qt
// stays-on-top hint alone cannot reliably order bypass-WM windows after a
// bar redraw, WM restart, or display rotation.
static Display *alternixX11Display() {
    static Display *dpy = XOpenDisplay(nullptr);
    return dpy;
}

// XLibre/XRandR can change the X11 root size before Qt5 publishes the new
// QScreen geometry.  Query the live server root for a single-display tablet;
// preserve Qt's per-monitor geometry on multi-display desktops.
static QRect alternixActiveScreenGeometry() {
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) return QRect();
    const QRect qtGeometry = screen->geometry();
    if (QGuiApplication::screens().size() != 1) return qtGeometry;
    Display *dpy = alternixX11Display();
    if (!dpy) return qtGeometry;
    Window returnedRoot = 0;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, border = 0, depth = 0;
    if (XGetGeometry(dpy, DefaultRootWindow(dpy), &returnedRoot,
                     &x, &y, &width, &height, &border, &depth) &&
        width > 0 && height > 0) {
        return QRect(0, 0, int(width), int(height));
    }
    return qtGeometry;
}

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
// ──────────────────────────────  Brightness controller
// Keep hardware brightness separate from XRandR's software gamma adjustment.
// The resident osm-notify process must survive display rotation, suspend and
// GPU/output re-enumeration without keeping a stale output name.
class AlternixBrightnessController : public QObject {
public:
    AlternixBrightnessController(QSlider *slider, QLabel *caption, QObject *parent)
        : QObject(parent), slider_(slider), caption_(caption),
          target_(qBound(20, slider->value(), 100)) {
        applyTimer_.setSingleShot(true);
        saveTimer_.setSingleShot(true);
        commandDeadline_.setSingleShot(true);
        healthTimer_.setInterval(2500);
        brightnessctl_ = QStandardPaths::findExecutable("brightnessctl");
        sleepOffsetMs_ = bootMinusMonotonicMs();

        connect(&applyTimer_, &QTimer::timeout, this, [this]() { applyBrightness(); });
        connect(&saveTimer_, &QTimer::timeout, this, [this]() {
            QSettings settings("Alternix", "osm-notify");
            settings.setValue("brightness", target_);
            settings.sync();
            if (settings.status() != QSettings::NoError)
                qWarning() << "osm-notify: unable to save brightness preference";
        });
        connect(slider_, &QSlider::valueChanged, this, [this](int value) {
            target_ = qBound(20, value, 100);
            // Coalesce changes to roughly 20 writes/sec, but keep applying
            // during a long drag rather than waiting for the drag to stop.
            if (!applyTimer_.isActive()) applyTimer_.start(50);
            saveTimer_.start(350);
        });
        connect(&command_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int rc, QProcess::ExitStatus status) {
            finishCommand(rc == 0 && status == QProcess::NormalExit,
                          QString::fromUtf8(command_.readAll()).trimmed());
        });
        connect(&command_, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                QTimer::singleShot(0, this, [this]() {
                    if (operation_ != Operation::None &&
                        command_.state() == QProcess::NotRunning)
                        finishCommand(false, command_.errorString());
                });
        });
        connect(&commandDeadline_, &QTimer::timeout, this, [this]() {
            timedOut_ = true;
            if (command_.state() != QProcess::NotRunning) command_.kill();
        });
        connect(&healthTimer_, &QTimer::timeout, this, [this]() { checkHealth(); });
        healthTimer_.start();

        // Apply the saved preference after startup and again after delayed
        // device enumeration, without blocking the launcher/overlay animation.
        applyTimer_.start(200);
        QTimer::singleShot(1800, this, [this]() { scheduleRestore(); });
        QTimer::singleShot(5000, this, [this]() { scheduleRestore(); });
    }

    void panelOpened() {
        // Device hotplug or XRandR recovery may have happened while hidden.
        checkHealth();
        if (device_.isEmpty() || softwareOnly_) {
            // Re-query when opened: output names can change without a geometry change.
            output_.clear();
            scheduleRestore();
        } else if (target_ < 97 && backlightPercent(device_) >= 97) {
            scheduleRestore();
        }
    }

    void displayChanged() {
        // A changed XRandR geometry can also mean a changed connector name.
        output_.clear();
        if (device_.isEmpty() || softwareOnly_) scheduleRestore();
    }

private:
    enum class Operation { None, HardwareWrite, QueryOutputs, SoftwareWrite };
    QPointer<QSlider> slider_;
    QPointer<QLabel> caption_;
    QTimer applyTimer_;
    QTimer saveTimer_;
    QTimer commandDeadline_;
    QTimer healthTimer_;
    QProcess command_;
    Operation operation_ = Operation::None;
    int target_ = 80;
    int commandTarget_ = 80;
    int lastSamplePercent_ = -1;
    qint64 sleepOffsetMs_ = -1;
    qint64 lastResetRecoveryMs_ = 0;
    QString device_;
    QString output_;
    QString brightnessctl_;
    QString hardwareError_;
    bool softwareOnly_ = false;
    bool pendingApply_ = false;
    bool timedOut_ = false;

    static qint64 clockMs(clockid_t id) {
        timespec ts{};
        if (::clock_gettime(id, &ts) != 0) return -1;
        return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    }

    static qint64 bootMinusMonotonicMs() {
        // BOOTTIME includes time spent suspended; MONOTONIC does not.
        // Their offset therefore jumps after suspend without requiring QtDBus.
        const qint64 boot = clockMs(CLOCK_BOOTTIME);
        const qint64 mono = clockMs(CLOCK_MONOTONIC);
        return (boot < 0 || mono < 0) ? -1 : boot - mono;
    }

    static int backlightMaximum(const QString &name) {
        bool ok = false;
        const int maximum = readFile("/sys/class/backlight/" + name +
                                     "/max_brightness").toInt(&ok);
        return ok && maximum > 0 ? maximum : -1;
    }

    static int backlightPercent(const QString &name) {
        const int maximum = backlightMaximum(name);
        if (maximum < 1) return -1;
        const QString root = "/sys/class/backlight/" + name + "/";
        QString actual = readFile(root + "actual_brightness");
        if (actual.isEmpty()) actual = readFile(root + "brightness");
        bool ok = false;
        const int value = actual.toInt(&ok);
        return ok && value >= 0 ? qBound(0, qRound(100.0 * value / maximum), 100) : -1;
    }

    static QString preferredBacklight() {
        QDir directory("/sys/class/backlight");
        QString preferred;
        int bestScore = -100000;
        // sysfs backlight entries are symlinks to real devices.
        for (const QString &name : directory.entryList(QDir::AllEntries |
                        QDir::NoDotAndDotDot, QDir::Name)) {
            if (backlightMaximum(name) < 1) continue;
            const QString type = readFile("/sys/class/backlight/" + name + "/type");
            int score = (type == "raw") ? 300 :
                        (type == "platform") ? 200 : 100;
            if (name.startsWith("intel_backlight") || name.startsWith("amdgpu_bl") ||
                name.startsWith("radeon_bl") || name.startsWith("nvidia") ||
                name.startsWith("apple_backlight") || name.contains("pwm"))
                score += 35;
            if (name.startsWith("acpi_video")) score -= 100;
            if (score > bestScore) {
                bestScore = score;
                preferred = name;
            }
        }
        return preferred;
    }

    bool writeDirectly(const QString &name, int percent) {
        const int maximum = backlightMaximum(name);
        if (maximum < 1) return false;
        const int value = qBound(1, qRound(maximum * (percent / 100.0)), maximum);
        QFile f("/sys/class/backlight/" + name + "/brightness");
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
        const QByteArray bytes = QByteArray::number(value) + '\n';
        const bool success = f.write(bytes) == bytes.size() && f.flush();
        f.close();
        return success;
    }

    void updateBackendLabel(bool native, const QString &error = QString()) {
        if (!caption_) return;
        caption_->setText(native ? "Brightness" :
                          (!error.isEmpty() ? "Brightness (unavailable)" :
                           "Brightness (software)"));
        const QString description = native
            ? "Physical backlight: " + device_
            : "Software (XRandR) dimming; does not reduce the LCD backlight power";
        const QString reason = error.isEmpty() ? hardwareError_ : error;
        caption_->setToolTip(reason.isEmpty() ? description :
                             description + "\nError: " + reason);
        if (slider_) slider_->setToolTip(caption_->toolTip());
    }

    void scheduleRestore() {
        applyTimer_.start(100);
    }

    void resumeDetected() {
        // Drop stale device/output and retry after GPU/backlight reinitialisation.
        device_.clear();
        output_.clear();
        softwareOnly_ = false;
        hardwareError_.clear();
        lastSamplePercent_ = -1;
        scheduleRestore();
        QTimer::singleShot(900, this, [this]() { scheduleRestore(); });
        QTimer::singleShot(2500, this, [this]() { scheduleRestore(); });
        QTimer::singleShot(6500, this, [this]() { scheduleRestore(); });
    }

    void checkHealth() {
        const qint64 offset = bootMinusMonotonicMs();
        if (offset >= 0) {
            if (sleepOffsetMs_ >= 0 && offset - sleepOffsetMs_ > 1200) {
                sleepOffsetMs_ = offset;
                resumeDetected();
                return;
            }
            sleepOffsetMs_ = offset;
        }

        const QString available = preferredBacklight();
        if (available != device_) {
            device_ = available;
            output_.clear();
            softwareOnly_ = false;
            hardwareError_.clear();
            lastSamplePercent_ = -1;
            scheduleRestore();
            return;
        }
        if (device_.isEmpty() || softwareOnly_) return;
        const int actual = backlightPercent(device_);
        if (actual < 0) return;
        // Firmware frequently restores the hardware backlight to 100% during
        // GPU reset, even without an observable suspend event.
        const qint64 now = clockMs(CLOCK_BOOTTIME);
        if (target_ <= 94 && actual >= 97 && lastSamplePercent_ >= 0 &&
            lastSamplePercent_ < 95 &&
            (lastResetRecoveryMs_ == 0 || now - lastResetRecoveryMs_ > 12000)) {
            lastResetRecoveryMs_ = now;
            scheduleRestore();
        }
        lastSamplePercent_ = actual;
    }

    void beginCommand(const QString &program, const QStringList &args,
                      Operation operation) {
        if (command_.state() != QProcess::NotRunning) {
            pendingApply_ = true;
            return;
        }
        operation_ = operation;
        commandTarget_ = target_;
        timedOut_ = false;
        command_.setProcessChannelMode(QProcess::MergedChannels);
        command_.start(program, args);
        commandDeadline_.start(3000);
    }

    void applyBrightness() {
        if (operation_ != Operation::None ||
            command_.state() != QProcess::NotRunning) {
            pendingApply_ = true;
            return;
        }
        const QString available = preferredBacklight();
        if (available != device_) {
            device_ = available;
            output_.clear();
            softwareOnly_ = false;
            hardwareError_.clear();
            lastSamplePercent_ = -1;
        }
        if (!device_.isEmpty() && !softwareOnly_) {
            if (writeDirectly(device_, target_)) {
                lastSamplePercent_ = backlightPercent(device_);
                hardwareError_.clear();
                updateBackendLabel(true);
                return;
            }
            if (!brightnessctl_.isEmpty()) {
                beginCommand(brightnessctl_, {"--class=backlight",
                             "--device=" + device_, "set",
                             QString::number(target_) + "%"}, Operation::HardwareWrite);
                return;
            }
            qWarning() << "osm-notify: no permission to write hardware backlight"
                       << device_ << "and brightnessctl is not installed";
            softwareOnly_ = true;
            hardwareError_ = "Hardware backlight is not writable and brightnessctl is absent";
            updateBackendLabel(false);
        }
        if (output_.isEmpty()) {
            beginCommand("xrandr", {"--query"}, Operation::QueryOutputs);
            return;
        }
        beginCommand("xrandr", {"--output", output_, "--brightness",
                     QString::number(target_ / 100.0, 'f', 2)},
                     Operation::SoftwareWrite);
    }

    static QString chooseConnectedOutput(const QString &query) {
        QString primary;
        QString internal;
        QString first;
        const QRegularExpression connected("^([^\\s]+)\\s+connected(?:\\s|$)");
        for (const QString &line : query.split('\n')) {
            const QRegularExpressionMatch match = connected.match(line);
            if (!match.hasMatch()) continue;
            const QString name = match.captured(1);
            if (first.isEmpty()) first = name;
            if (line.contains(QRegularExpression("\\sprimary(?:\\s|$)"))) primary = name;
            if (internal.isEmpty() &&
                (name.startsWith("eDP", Qt::CaseInsensitive) ||
                 name.startsWith("DSI", Qt::CaseInsensitive) ||
                 name.startsWith("LVDS", Qt::CaseInsensitive))) internal = name;
        }
        return !primary.isEmpty() ? primary : (!internal.isEmpty() ? internal : first);
    }

    void finishCommand(bool success, QString details) {
        if (operation_ == Operation::None) return;
        commandDeadline_.stop();
        if (timedOut_) {
            success = false;
            details = "command timed out";
        }
        const Operation completed = operation_;
        operation_ = Operation::None;
        bool next = pendingApply_ || (target_ != commandTarget_);
        pendingApply_ = false;

        if (completed == Operation::HardwareWrite) {
            if (success) {
                lastSamplePercent_ = backlightPercent(device_);
                hardwareError_.clear();
                updateBackendLabel(true);
            } else {
                qWarning() << "osm-notify: hardware brightness failed for"
                           << device_ << ":" << details;
                // Never invoke osm-sudo for each slider movement. If the
                // hardware device cannot be controlled as the session user,
                // use software dimming and expose the failure in the tooltip.
                softwareOnly_ = true;
                hardwareError_ = details;
                output_.clear();
                updateBackendLabel(false);
                next = true;
            }
        } else if (completed == Operation::QueryOutputs) {
            if (success) output_ = chooseConnectedOutput(details);
            if (output_.isEmpty()) {
                qWarning() << "osm-notify: no connected XRandR output:" << details;
                updateBackendLabel(false, "No connected XRandR display found");
            } else {
                next = true;
            }
        } else if (completed == Operation::SoftwareWrite) {
            if (!success) {
                qWarning() << "osm-notify: XRandR brightness failed on" << output_
                           << ":" << details;
                output_.clear(); // Force fresh output detection on next attempt.
                updateBackendLabel(false, details);
            } else {
                updateBackendLabel(false);
            }
        }
        if (next) QTimer::singleShot(0, this, [this]() { scheduleRestore(); });
    }
};

// ──────────────────────────────  Main overlay widget
class NotificationOverlay : public QWidget {
public:
    QLabel *clockLabel;

    QWidget *topCardWidget;
    QWidget *sysRowWidget;
    QWidget *brightnessCardWidget;
    AlternixBrightnessController *brightnessController = nullptr;
    QWidget *batteryCardWidget;

    QLabel *batteryInfoLabel;
    QLabel *batteryTimeLabel;
    QLabel *batteryStatusLabel;
    QLabel *batteryIconLabel;

    QList<QWidget*> orderedWidgets;
    QList<QWidget*> systemCards;
    QGridLayout *systemGrid = nullptr;
    QRect lastScreenGeo;
    QMap<QWidget*, QRect> finalGeomMap;
    QPointer<QSequentialAnimationGroup> panelAnimation;
    QString lastBatteryIconName;
    QProcess *powerSaverQuery = nullptr;
    QString powerSaverExecutable = QStandardPaths::findExecutable("powerprofilesctl");
    bool cachedPowerSaver = false;
    qint64 nextPowerSaverProbe = 0;
    bool closing;
    std::function<void()> onPanelClosed;

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
    // A single slow/empty status query is not evidence of disconnection.
    QMap<QString, int> failedStatusReads;
    QMap<QString, int> emptyConnectionReads;
    QString lastWifiSsid;
    int lastWifiSignal = -1;
    int zeroWifiSignalReads = 0;
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

        QRect g = alternixActiveScreenGeometry();
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
            QObject::connect(t, &QTimer::timeout, this, [this]() {
                if (!isVisible()) return;
                const QString value = QTime::currentTime().toString("HH:mm:ss");
                if (clockLabel->text() != value) clockLabel->setText(value);
            });
            t->start(1000);
        }

        // ───────── System Row (Wi-Fi, BT, Ethernet, GPS, Battery)
        int sysCols;
        if (screenW < 520) sysCols = 2;
        else if (screenW < 900) sysCols = 3;
        else sysCols = 5;

        {
            QWidget *row = new QWidget(this);
            QGridLayout *grid = new QGridLayout(row);
            systemGrid = grid;
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
                systemCards.append(card);

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
                    nextPowerSaverProbe = 0; // refresh after requested profile change

                    // Give powerprofilesctl time to update:
                    //  - first quick refresh at 150ms
                    //  - second more relaxed refresh at 500ms
                    QTimer::singleShot(100, this, refreshBatteryUI);
                    QTimer::singleShot(500, this, refreshBatteryUI);
                    QTimer::singleShot(1000, this, refreshBatteryUI);
                };

                QFrame *batCard = createCard(this, inner, true, onClick);
                batteryCardWidget = batCard;
                systemCards.append(batCard);

                int r = toggleIndex / sysCols;
                int c = toggleIndex % sysCols;
                grid->addWidget(batCard, r, c);
                toggleIndex++;
            }

            sysRowWidget = row;
            main->addWidget(row);

            updateBatteryIconColor();
        }

        // ───────── Brightness Card (native backlight, XRandR fallback)
        {
            QVBoxLayout *bInner = new QVBoxLayout();
            bInner->setAlignment(Qt::AlignCenter);

            QLabel *bLabel = new QLabel("Brightness", this);
            bLabel->setStyleSheet("color:white; font-size:18pt; font-weight:bold;");
            bLabel->setAlignment(Qt::AlignCenter);

            QSettings brightnessSettings("Alternix", "osm-notify");
            const int savedBrightness = qBound(20,
                brightnessSettings.value("brightness", 80).toInt(), 100);
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

            // A single long-lived controller owns command serialization,
            // rescanning and resume recovery even when the panel is hidden.
            brightnessController = new AlternixBrightnessController(slider, bLabel, this);
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
        refreshTimer->setInterval(1800);  // icons/text only need periodic updates
        QObject::connect(refreshTimer, &QTimer::timeout, this, [=]() {
            if (!refreshEnabled) return;
            if (!this->isVisible()) return;

            if (!batPath.isEmpty()) {
                const QString mainText = batteryMainText(batPath);
                const QString timeText = batteryTimeText(batPath);
                const QString statusText = batteryStatusLine(batPath);
                if (batteryInfoLabel && batteryInfoLabel->text() != mainText)
                    batteryInfoLabel->setText(mainText);
                if (batteryTimeLabel && batteryTimeLabel->text() != timeText)
                    batteryTimeLabel->setText(timeText);
                if (batteryStatusLabel) {
                    if (batteryStatusLabel->text() != statusText)
                        batteryStatusLabel->setText(statusText);
                    if (batteryStatusLabel->isVisible() != !statusText.isEmpty())
                        batteryStatusLabel->setVisible(!statusText.isEmpty());
                }
            }
            updateBatteryIconColor();
        });

        // Device probes are asynchronous. They cannot stall animation or
        // trigger authentication merely by querying status.
        deviceTimer = new QTimer(this);
        deviceTimer->setInterval(4000);  // initial probe also occurs as panel opens
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
                const QString rendered = detail.isEmpty() ? text : text + "\n" + detail;
                // Avoid forcing repaints on every refresh when nothing changed.
                if (label->text() != rendered) label->setText(rendered);
                if (label->toolTip() != detail) label->setToolTip(detail);
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

        // Two-phase Wi-Fi refresh: never replace a confirmed SSID/signal with
        // a temporary "Radio enabled" message while a second nmcli query runs.
        pollCommand("wifi", "nmcli", {"radio", "wifi"},
            [this](const QString &out, bool ok) {
                if (togglesInFlight.contains("wifi")) return;
                const QString value = out.trimmed().toLower();
                if (!ok || (value != "enabled" && value != "disabled")) {
                    // A timed-out nmcli query must not flash 'off' or erase a
                    // connected network. Only mark it unavailable if repeated.
                    if (++failedStatusReads["wifi"] == 3)
                        setDeviceState("wifi", false, false, "NetworkManager status unavailable");
                    return;
                }
                failedStatusReads["wifi"] = 0;
                if (value == "disabled") {
                    emptyConnectionReads["wifi"] = 0;
                    lastWifiSsid.clear();
                    lastWifiSignal = -1;
                    zeroWifiSignalReads = 0;
                    setDeviceState("wifi", true, false, "Radio disabled");
                    return;
                }

                // Only show the generic message on the first reading or a
                // genuine off -> on transition, not at every polling interval.
                const DeviceState previous = devices.value("wifi");
                if (!previous.known || !previous.enabled)
                    setDeviceState("wifi", true, true, "Radio enabled");

                pollCommand("wifi-ssid", "nmcli",
                    {"-t", "-e", "no", "-f", "IN-USE,SSID,SIGNAL",
                     "device", "wifi", "list", "--rescan", "no"},
                    [this](const QString &ssids, bool valid) {
                        if (!valid || togglesInFlight.contains("wifi") ||
                            !devices.value("wifi").enabled) return;
                        for (const QString &line : ssids.split('\n')) {
                            if (!line.startsWith("*:")) continue;
                            const QString inUse = line.mid(2);
                            const int separator = inUse.lastIndexOf(':');
                            const QString ssid = (separator >= 0 ? inUse.left(separator)
                                                                  : inUse).trimmed();
                            if (ssid.isEmpty()) continue;
                            bool numeric = false;
                            int signal = (separator >= 0 ? inUse.mid(separator + 1).trimmed()
                                                         : QString()).toInt(&numeric);
                            if (!numeric || signal < 0 || signal > 100)
                                signal = wifiSignalPercent();
                            if (ssid != lastWifiSsid) {
                                lastWifiSsid = ssid;
                                lastWifiSignal = -1;
                                zeroWifiSignalReads = 0;
                            }
                            // NM may briefly report 0 or an unknown signal
                            // during an AP refresh. Retain the last confirmed
                            // reading until the 0% value is seen twice.
                            if (signal > 0) {
                                lastWifiSignal = signal;
                                zeroWifiSignalReads = 0;
                            } else if (signal == 0) {
                                if (++zeroWifiSignalReads >= 2) lastWifiSignal = 0;
                            }
                            emptyConnectionReads["wifi"] = 0;
                            // A single zero reading is not proof of lost signal.
                            // The card may show the SSID alone until confirmed.
                            const int percent = lastWifiSignal >= 0 ? lastWifiSignal :
                                                (signal == 0 ? -1 : signal);
                            const QString detail = (percent < 0)
                                ? ssid : QString("%1% · %2").arg(percent).arg(ssid);
                            if (devices.value("wifi").detail != detail)
                                setDeviceState("wifi", true, true, detail);
                            return;
                        }
                        // Do not interpret one empty/partially updated AP list
                        // as loss of the current connection.
                        // The AP list can be empty while NetworkManager is
                        // updating its cache. An associated kernel interface
                        // independently confirms that the Wi-Fi link still exists.
                        const QString iface = detectWifiInterface();
                        const QString carrier = iface.isEmpty() ? QString() :
                            readFile("/sys/class/net/" + iface + "/carrier");
                        if (carrier == "1") {
                            emptyConnectionReads["wifi"] = 0;
                            return;
                        }
                        if (++emptyConnectionReads["wifi"] >= 2) {
                            lastWifiSsid.clear();
                            lastWifiSignal = -1;
                            zeroWifiSignalReads = 0;
                            if (devices.value("wifi").detail != "Not connected")
                                setDeviceState("wifi", true, true, "Not connected");
                        }
                    });
            });

        // Keep the last confirmed connected-device name while checking power.
        // Bluetooth power-off is applied immediately once BlueZ confirms it.
        pollCommand("bluetooth", "bluetoothctl", {"show"},
            [this](const QString &out, bool ok) {
                if (togglesInFlight.contains("bluetooth")) return;
                QRegularExpression re("(?:^|\\n)\\s*Powered:\\s*(yes|no)",
                                      QRegularExpression::CaseInsensitiveOption);
                auto m = re.match(out);
                if (!ok || !m.hasMatch()) {
                    if (++failedStatusReads["bluetooth"] == 3)
                        setDeviceState("bluetooth", false, false,
                            out.contains("No default controller", Qt::CaseInsensitive)
                                ? "No usable Bluetooth adapter" : "Bluetooth status unavailable");
                    return;
                }
                failedStatusReads["bluetooth"] = 0;
                const bool powered = m.captured(1).compare("yes", Qt::CaseInsensitive) == 0;
                if (!powered) {
                    emptyConnectionReads["bluetooth"] = 0;
                    setDeviceState("bluetooth", true, false, "Adapter switched off");
                    return;
                }
                const DeviceState previous = devices.value("bluetooth");
                if (!previous.known || !previous.enabled)
                    setDeviceState("bluetooth", true, true, "Adapter powered");

                pollCommand("bluetooth-connected", "bluetoothctl",
                    {"devices", "Connected"},
                    [this](const QString &devicesText, bool valid) {
                        if (!valid || togglesInFlight.contains("bluetooth") ||
                            !devices.value("bluetooth").enabled ||
                            devicesText.contains("Invalid command", Qt::CaseInsensitive) ||
                            devicesText.contains("Too many arguments", Qt::CaseInsensitive) ||
                            devicesText.contains("Error:", Qt::CaseInsensitive)) return;
                        for (const QString &line : devicesText.split('\n')) {
                            if (!line.startsWith("Device ")) continue;
                            const QString name = line.mid(7).section(' ', 1).trimmed();
                            if (name.isEmpty()) continue;
                            emptyConnectionReads["bluetooth"] = 0;
                            if (devices.value("bluetooth").detail != name)
                                setDeviceState("bluetooth", true, true, name);
                            return;
                        }
                        // BlueZ sometimes returns a transient empty list.
                        if (++emptyConnectionReads["bluetooth"] >= 2 &&
                            devices.value("bluetooth").detail != "No device connected")
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

    void syncScreenGeometry(const QRect &g) {
        if (!g.isValid() || g.isEmpty()) return;
        if (lastScreenGeo == g && geometry() == g) return;
        lastScreenGeo = g;
        if (brightnessController) brightnessController->displayChanged();
        if (geometry() != g) setGeometry(g);
        if (!systemGrid) return;
        // A portrait display needs a different card layout from landscape.
        const int cols = g.width() < 520 ? 2 : (g.width() < 900 ? 3 : 5);
        for (QWidget *card : systemCards) systemGrid->removeWidget(card);
        for (int i = 0; i < systemCards.size(); ++i)
            systemGrid->addWidget(systemCards.at(i), i / cols, i % cols);
        if (layout()) layout()->invalidate();
        updateGeometry();
    }

    void openPanel() {
        if (closing || isVisible()) return;
        syncScreenGeometry(alternixActiveScreenGeometry());
        if (brightnessController) brightnessController->panelOpened();
        if (refreshTimer) { refreshEnabled = false; refreshTimer->stop(); }
        if (deviceTimer) deviceTimer->stop();
        if (panelAnimation) { panelAnimation->stop(); panelAnimation->deleteLater(); panelAnimation = nullptr; }

        // Keep the fullscreen-sized geometry without toggling EWMH fullscreen
        // state on every swipe (and without an unnecessary hide/show pair).
        for (QWidget *w : orderedWidgets) if (w) w->hide();
        clockLabel->setText(QTime::currentTime().toString("HH:mm:ss"));
        show();
        raise();
        // Network probes are intentionally delayed until the slide-in has
        // finished, avoiding subprocess startup during animation frames.
    }

    void playOpenAnimation() {
        if (!isVisible() || closing) return;
        if (panelAnimation) { panelAnimation->stop(); panelAnimation->deleteLater(); panelAnimation = nullptr; }
        finalGeomMap.clear();
        for (QWidget *w : orderedWidgets) {
            if (w) w->show();
        }
        // Give the layout one opportunity to settle at the final geometry.
        if (layout()) layout()->activate();
        for (QWidget *w : orderedWidgets)
            if (w) finalGeomMap.insert(w, w->geometry());

        // Move intact widgets instead of simultaneously resizing every card
        // and rasterising each entire child subtree through an opacity effect.
        // The original combination saturated Xorg/Picom on low-end tablets.
        auto *seq = new QSequentialAnimationGroup(this);
        panelAnimation = seq;
        for (QWidget *w : orderedWidgets) {
            if (!w || !finalGeomMap.contains(w)) continue;
            const QPoint destination = finalGeomMap.value(w).topLeft();
            const QPoint initial = destination + QPoint(0, 28);
            w->move(initial);
            auto *anim = new QPropertyAnimation(w, "pos", seq);
            anim->setDuration(90);
            anim->setStartValue(initial);
            anim->setEndValue(destination);
            anim->setEasingCurve(QEasingCurve::OutCubic);
            seq->addAnimation(anim);
            seq->addPause(8);
        }
        QObject::connect(seq, &QSequentialAnimationGroup::finished,
                         this, [this, seq]() {
            if (panelAnimation != seq || closing || !isVisible()) return;
            panelAnimation = nullptr;
            for (QWidget *w : orderedWidgets)
                if (w && finalGeomMap.contains(w)) w->setGeometry(finalGeomMap.value(w));
            if (refreshTimer) { refreshEnabled = true; refreshTimer->start(); }
            if (deviceTimer) deviceTimer->start();
            refreshDeviceStates();
            updateBatteryIconColor();
        });
        seq->start(QAbstractAnimation::DeleteWhenStopped);
    }

    void animatedClose() {
        if (closing || !isVisible()) return;
        closing = true;
        if (refreshTimer) { refreshEnabled = false; refreshTimer->stop(); }
        if (deviceTimer) deviceTimer->stop();
        if (panelAnimation) { panelAnimation->stop(); panelAnimation->deleteLater(); panelAnimation = nullptr; }

        // The close runs backwards; no graphics opacity layers or 30%-size
        // relayouts need to be recomputed for every animation frame.
        QList<QWidget*> closeOrder = orderedWidgets;
        std::reverse(closeOrder.begin(), closeOrder.end());
        auto *seq = new QSequentialAnimationGroup(this);
        panelAnimation = seq;
        for (QWidget *w : closeOrder) {
            if (!w) continue;
            const QPoint initial = w->pos();
            const QPoint destination = initial + QPoint(0, -28);
            auto *anim = new QPropertyAnimation(w, "pos", seq);
            anim->setDuration(85);
            anim->setStartValue(initial);
            anim->setEndValue(destination);
            anim->setEasingCurve(QEasingCurve::InCubic);
            seq->addAnimation(anim);
            seq->addPause(8);
        }
        QObject::connect(seq, &QSequentialAnimationGroup::finished,
                         this, [this, seq]() {
            if (panelAnimation != seq) return;
            panelAnimation = nullptr;
            for (QWidget *w : orderedWidgets) {
                if (!w) continue;
                if (finalGeomMap.contains(w)) w->setGeometry(finalGeomMap.value(w));
                w->hide();
            }
            hide();
            lower();
            closing = false;
            if (onPanelClosed) onPanelClosed();
            // Do not repaint() an already-hidden fullscreen ARGB window.
        });
        seq->start(QAbstractAnimation::DeleteWhenStopped);
    }

    // ──────────────────────────────  Update battery icon from icons
    void updateBatteryIconColor() {
        if (!batteryIconLabel) return;
        const QString batPath = detectBatteryPath();
        if (batPath.isEmpty()) return;

        // powerprofilesctl can hang for 222ms in the old implementation,
        // blocking animations and dragging on the GUI thread. Query it
        // asynchronously and at most once every 5s.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (!powerSaverExecutable.isEmpty() && !powerSaverQuery &&
            now >= nextPowerSaverProbe) {
            nextPowerSaverProbe = now + 5000;
            auto *p = new QProcess(this);
            powerSaverQuery = p;
            connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    this, [this, p](int code, QProcess::ExitStatus status) {
                if (powerSaverQuery == p) powerSaverQuery = nullptr;
                const bool saver = status == QProcess::NormalExit && code == 0 &&
                    QString::fromUtf8(p->readAllStandardOutput()).trimmed() == "power-saver";
                p->deleteLater();
                if (cachedPowerSaver != saver) {
                    cachedPowerSaver = saver;
                    updateBatteryIconColor();
                }
            });
            connect(p, &QProcess::errorOccurred, this,
                    [this, p](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart) {
                    if (powerSaverQuery == p) powerSaverQuery = nullptr;
                    p->deleteLater();
                }
            });
            p->start(powerSaverExecutable, {"get"});
            QTimer::singleShot(1200, p, [p]() {
                if (p->state() != QProcess::NotRunning) p->kill();
            });
        }

        const int pct = batteryPercent(batPath);
        const QString rawStatus = batteryStatus(batPath);
        const QString iconName = selectBatteryIconName(pct, rawStatus, cachedPowerSaver);
        if (iconName == lastBatteryIconName) return;
        lastBatteryIconName = iconName;
        const QString fullPath = QDir::homePath() + "/.config/qtile/images/" + iconName;
        QPixmap px(fullPath);
        if (px.isNull()) batteryIconLabel->setText("🔋");
        else batteryIconLabel->setPixmap(
            px.scaled(42, 42, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
            if (isVisible() && !closing) playOpenAnimation();
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
        // Keep an actual X11 input window: transparent pixels must remain
        // touchable, rather than making the window click-through.
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_AcceptTouchEvents, true);

        setGeometry(x, y, w, h);
        // Intentionally no stylesheet or painted fill: the window is fully
        // invisible, but its native input region still accepts swipes.
        show();
        ensureInFront();
    }

    bool syncEdgeGeometry(int x, int y, int w, int h) {
        const QRect target(x, y, qMax(1, w), qMax(1, h));
        if (geometry() == target) return false;
        setGeometry(target);
        return true;
    }

    void ensureInFront() {
        // Do not put the activation zones in front of the open notification
        // panel; the zones are only supposed to capture gestures on Qtile.
        if (!isVisible() || (overlay && overlay->isVisible())) return;
        if (Display *dpy = alternixX11Display()) {
            XRaiseWindow(dpy, static_cast<Window>(winId()));
            XFlush(dpy);
        } else {
            raise();
        }
    }

protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::TouchBegin ||
            event->type() == QEvent::TouchUpdate ||
            event->type() == QEvent::TouchEnd ||
            event->type() == QEvent::TouchCancel) {
            auto *touch = static_cast<QTouchEvent *>(event);
            if (event->type() == QEvent::TouchBegin) {
                if (!touch->touchPoints().isEmpty()) {
                    pressPos = touch->touchPoints().first().screenPos().toPoint();
                    dragging = true;
                }
            } else if (event->type() == QEvent::TouchUpdate && dragging &&
                       !touch->touchPoints().isEmpty()) {
                if (touch->touchPoints().first().screenPos().y() - pressPos.y() > 12) {
                    if (overlay) overlay->openPanel();
                    dragging = false;
                }
            } else if (event->type() != QEvent::TouchUpdate) {
                dragging = false;
            }
            event->accept();
            return true;
        }
        return QWidget::event(event);
    }

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

// ──────────────────────────────  Activation IPC (X11/Qtile, Linux)
// Qtile can keep launching plain `osm-notify` as before: the first instance
// stays hidden and owns the top-edge strips. A subsequent `osm-notify` or
// `osm-notify --show` signals the resident instance to display its panel.
//
// An AF_UNIX abstract socket is tied to the lifetime of the first process.
// It needs no lock file, no extra Qt module, no signal handler, and can't
// leave behind stale socket paths after a crash or logout.
static sockaddr_un notifyAddress(socklen_t &length) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const QByteArray identity = QByteArray("alternix.osm-notify.") +
        QByteArray::number(::getuid()) + "." +
        QCryptographicHash::hash(qgetenv("DISPLAY"),
                                 QCryptographicHash::Sha256).toHex().left(16);
    // Abstract address: a leading NUL byte, not a filesystem path.
    addr.sun_path[0] = '\0';
    static_assert(sizeof(addr.sun_path) > 48, "short sockaddr_un path");
    const int count = qMin(int(sizeof(addr.sun_path) - 1), identity.size());
    std::memcpy(addr.sun_path + 1, identity.constData(), size_t(count));
    length = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + count);
    return addr;
}

// ──────────────────────────────  main
int main(int argc, char **argv) {
    QApplication a(argc, argv);
    const bool forceShow = a.arguments().contains("--show");

    const int notifySocket = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (notifySocket < 0) {
        qWarning() << "osm-notify: cannot create activation socket:" << std::strerror(errno);
        return 1;
    }
    socklen_t addressLength = 0;
    const sockaddr_un addr = notifyAddress(addressLength);
    if (::bind(notifySocket, reinterpret_cast<const sockaddr *>(&addr), addressLength) != 0) {
        const int bindError = errno;
        if (bindError == EADDRINUSE) {
            // Existing instance: all legacy invocations mean "show".
            // There is no need to spawn another panel or install new edge bars.
            const char command[] = "show";
            const ssize_t sent = ::sendto(notifySocket, command, sizeof(command) - 1, 0,
                reinterpret_cast<const sockaddr *>(&addr), addressLength);
            const int sendError = errno;
            ::close(notifySocket);
            if (sent == ssize_t(sizeof(command) - 1)) return 0;
            qWarning() << "osm-notify: existing instance could not be activated:"
                       << std::strerror(sendError);
            return 1;
        }
        ::close(notifySocket);
        qWarning() << "osm-notify: cannot bind activation socket:"
                   << std::strerror(bindError);
        return 1;
    }

    NotificationOverlay overlay;
    // The registered activation endpoint is serviced on Qt's event loop,
    // avoiding signals and polling. No new link libraries are necessary.
    QSocketNotifier showRequests(notifySocket, QSocketNotifier::Read, &overlay);
    // Qt 5.15 replaced activated(int) with a QSocketDescriptor/Type signal.
    // Connect to the visible overload directly: some Qt 5.15 headers hide
    // the obsolete int overload and include a private trailing signal token.
    // The receiving lambda can ignore that trailing token safely.
    auto processShowRequests = [&overlay, notifySocket]() {
        char command[32];
        while (true) {
            const ssize_t n = ::recv(notifySocket, command, sizeof(command), MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 4 && std::memcmp(command, "show", 4) == 0 &&
                !overlay.isVisible() && !overlay.closing)
                overlay.openPanel();
        }
    };
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QObject::connect(&showRequests, &QSocketNotifier::activated, &overlay,
        [processShowRequests](QSocketDescriptor, QSocketNotifier::Type) {
            processShowRequests();
        });
#else
    QObject::connect(&showRequests, &QSocketNotifier::activated, &overlay,
        [processShowRequests](int) { processShowRequests(); });
#endif
    QObject::connect(&a, &QCoreApplication::aboutToQuit, &overlay,
                     [notifySocket]() { ::close(notifySocket); });

    // Preserve the original Qtile autostart: plain `osm-notify` stays hidden.
    // Only explicit `--show` opens immediately if no instance is running.
    if (forceShow)
        QTimer::singleShot(0, &overlay, [&overlay]() { overlay.openPanel(); });

    QScreen *screen = QApplication::primaryScreen();
    // Two distinct strips keep the existing top-left/top-right swipe behavior.
    // They must be repositioned when XRandR rotates or resizes the display.
    ActivationEdgeBar leftBar(&overlay, 0, 0, 1, 1);
    ActivationEdgeBar rightBar(&overlay, 0, 0, 1, 1);
    auto updateScreen = [&overlay, &leftBar, &rightBar](const QRect &g) {
        if (!g.isValid() || g.isEmpty()) return;
        overlay.syncScreenGeometry(g);
        const int W = g.width();
        int edgeExclusion = qMin(int(W * 0.18), W / 3);
        int centerExclusion = qMin(int(W * 0.14), W / 3);
        int usable = W - 2 * edgeExclusion - centerExclusion;
        if (usable < 24) {
            edgeExclusion = 0;
            centerExclusion = 0;
            usable = W;
        }
        const int leftWidth = qMax(1, usable / 2);
        const int rightWidth = qMax(1, usable - leftWidth);
        const int barHeight = qMin(50, g.height());
        const bool leftChanged = leftBar.syncEdgeGeometry(
            g.x() + edgeExclusion, g.y(), leftWidth, barHeight);
        const bool rightChanged = rightBar.syncEdgeGeometry(
            g.x() + edgeExclusion + leftWidth + centerExclusion,
            g.y(), rightWidth, barHeight);
        // Restack only after a real resize/rotation. The old 750ms timer
        // issued two unconditional XRaiseWindow/XFlush calls forever.
        if (leftChanged) leftBar.ensureInFront();
        if (rightChanged) rightBar.ensureInFront();
    };
    overlay.onPanelClosed = [&leftBar, &rightBar]() {
        leftBar.ensureInFront();
        rightBar.ensureInFront();
    };
    QObject::connect(screen, &QScreen::geometryChanged, &overlay,
                     [updateScreen](const QRect &) {
        updateScreen(alternixActiveScreenGeometry());
    });
    QTimer geometryPoll(&a);
    geometryPoll.setInterval(750);
    QObject::connect(&geometryPoll, &QTimer::timeout, &overlay, [updateScreen]() {
        updateScreen(alternixActiveScreenGeometry());
    });
    geometryPoll.start();
    updateScreen(alternixActiveScreenGeometry());

    return a.exec();
}
