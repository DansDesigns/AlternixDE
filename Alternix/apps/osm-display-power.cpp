// AlternixOS session display/lid power backend (Qt5Core + Qt5DBus + X11/Xext).
// Build: g++ -O2 -std=c++17 -fPIC osm-display-power.cpp -o osm-display-power \
//          $(pkg-config --cflags --libs Qt5Core Qt5DBus x11 xext)
// Run once per graphical session; XDG autostart/Qtile's startup hook should start it.
// No root required; a low-level elogind inhibitor is held ONLY while this process runs.
#include <QCoreApplication>
#include <QTimer>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QDir>
#include <QDateTime>
#include <QMap>
#include <QLockFile>
#include <QStandardPaths>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QVariantMap>
#include <QDebug>
#include <X11/Xlib.h>
#include <X11/extensions/dpms.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>

namespace {
QString configDir() { return QDir::homePath() + "/.config/Alternix"; }
QString settingsPath() { return configDir() + "/osm-settings.conf"; }
QString statusPath() { return configDir() + "/display-power.status"; }

QMap<QString, QString> readConfig()
{
    QMap<QString, QString> values;
    QFile f(settingsPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return values;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        const int eq = line.indexOf('=');
        if (eq > 0 && !line.startsWith('#'))
            values.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
    }
    return values;
}

int timeoutSeconds(int state)
{
    switch (state) {
    case 0: return 5;
    case 1: return 10;
    case 2: return 15;
    case 3: return 30;
    case 4: return 60;
    case 5: return 0; // Never
    default: return 5;
    }
}

struct LidReading {
    bool found = false;
    bool closed = false;
    QString source = "No readable lid switch";
};

LidReading viaProcAcpi()
{
    const QDir lidDir("/proc/acpi/button/lid");
    for (const QString &name : lidDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile f(lidDir.filePath(name + "/state"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString data = QString::fromUtf8(f.readAll()).toLower();
        if (data.contains("closed")) return {true, true, "ACPI /proc (" + name + ")"};
        if (data.contains("open")) return {true, false, "ACPI /proc (" + name + ")"};
    }
    return {};
}

// EV_SW/SW_LID is the generic Linux input kernel interface. It may be denied
// to an ordinary session by device permissions; UPower/ACPI then take priority.
LidReading viaEvdev()
{
    const QDir dir("/dev/input");
    for (const QString &name : dir.entryList({"event*"}, QDir::System | QDir::Files)) {
        const QString path = dir.filePath(name);
        const QByteArray bytes = QFile::encodeName(path);
        const int fd = ::open(bytes.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        unsigned char capable[(SW_MAX / 8) + 1] = {};
        unsigned char state[(SW_MAX / 8) + 1] = {};
        const bool hasCapabilities = ioctl(fd, EVIOCGBIT(EV_SW, sizeof capable), capable) >= 0;
        const bool isLid = hasCapabilities && (capable[SW_LID / 8] & (1u << (SW_LID % 8)));
        const bool hasState = isLid && ioctl(fd, EVIOCGSW(sizeof state), state) >= 0;
        ::close(fd);
        if (hasState)
            return {true, (state[SW_LID / 8] & (1u << (SW_LID % 8))) != 0,
                    "Kernel input " + name};
    }
    return {};
}

LidReading viaUPower()
{
    QDBusInterface properties("org.freedesktop.UPower", "/org/freedesktop/UPower",
                              "org.freedesktop.DBus.Properties", QDBusConnection::systemBus());
    properties.setTimeout(650);
    const QDBusReply<QVariantMap> result = properties.call("GetAll", "org.freedesktop.UPower");
    if (!result.isValid()) return {};
    const QVariantMap values = result.value();
    if (!values.value("LidIsPresent").toBool()) return {};
    if (!values.contains("LidIsClosed")) return {};
    return {true, values.value("LidIsClosed").toBool(), "UPower (D-Bus)"};
}

struct ScreenResult { bool connected = false; bool dpms = false; QString error; };

ScreenResult applyScreenTimeout(int seconds, bool onlyIfChanged)
{
    ScreenResult result;
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) { result.error = "No X11 display; unable to change screen timeout"; return result; }
    result.connected = true;
    int currentTimeout = 0, interval = 0, prefer = 0, exposures = 0;
    XGetScreenSaver(dpy, &currentTimeout, &interval, &prefer, &exposures);
    int eventBase = 0, errorBase = 0;
    const bool dpms = DPMSQueryExtension(dpy, &eventBase, &errorBase) && DPMSCapable(dpy);
    result.dpms = dpms;
    CARD16 standby = 0, suspend = 0, off = 0;
    bool matches = currentTimeout == seconds;
    if (dpms) {
        DPMSGetTimeouts(dpy, &standby, &suspend, &off);
        CARD16 level = DPMSModeOn;
        BOOL enabled = False;
        DPMSInfo(dpy, &level, &enabled);
        matches = matches && standby == seconds && suspend == seconds && off == seconds
                  && (seconds == 0 || enabled);
    }
    if (!onlyIfChanged || !matches) {
        // 0 disables the idle blanker and all DPMS timers ("Never").
        XSetScreenSaver(dpy, seconds, seconds ? std::max(60, seconds) : 0,
                        PreferBlanking, AllowExposures);
        if (dpms) {
            if (seconds) DPMSEnable(dpy);
            DPMSSetTimeouts(dpy, static_cast<CARD16>(seconds),
                            static_cast<CARD16>(seconds), static_cast<CARD16>(seconds));
        }
        XSync(dpy, False);
    }
    XCloseDisplay(dpy);
    return result;
}

bool setDisplayPower(bool on)
{
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;
    int eventBase = 0, errorBase = 0;
    const bool capable = DPMSQueryExtension(dpy, &eventBase, &errorBase) && DPMSCapable(dpy);
    if (capable) {
        DPMSEnable(dpy);
        CARD16 level = DPMSModeOn;
        BOOL enabled = False;
        DPMSInfo(dpy, &level, &enabled);
        if (level != (on ? DPMSModeOn : DPMSModeOff)) {
            DPMSForceLevel(dpy, on ? DPMSModeOn : DPMSModeOff);
            XSync(dpy, False);
        }
    }
    XCloseDisplay(dpy);
    return capable;
}

void writeStatus(const QMap<QString, QString> &status)
{
    QDir().mkpath(configDir());
    QSaveFile f(statusPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&f);
    for (auto it = status.cbegin(); it != status.cend(); ++it)
        out << it.key() << '=' << it.value().simplified() << '\n';
    f.commit();
}
} // namespace

class DisplayPowerService : public QObject {
public:
    explicit DisplayPowerService(QObject *parent = nullptr) : QObject(parent)
    {
        m_acpiListen = new QProcess(this);
        connect(m_acpiListen, &QProcess::readyReadStandardOutput, this, [this] {
            m_acpiBuffer += m_acpiListen->readAllStandardOutput();
            int newline = -1;
            while ((newline = m_acpiBuffer.indexOf('\n')) >= 0) {
                const QString line = QString::fromUtf8(m_acpiBuffer.left(newline)).toLower();
                m_acpiBuffer.remove(0, newline + 1);
                if (!line.contains("button/lid") && !line.contains("button lid")) continue;
                if (line.contains("close")) {
                    m_acpiEventKnown = true;
                    m_acpiEventClosed = true;
                    m_acpiEventPending = true;
                } else if (line.contains("open")) {
                    m_acpiEventKnown = true;
                    m_acpiEventClosed = false;
                    m_acpiEventPending = true;
                }
            }
            if (m_acpiBuffer.size() > 4096) m_acpiBuffer.clear();
        });
        m_pollTimer.setInterval(1600);
        connect(&m_pollTimer, &QTimer::timeout, this, [this] { poll(); });
        poll();
        m_pollTimer.start();
    }

private:
    void ensureAcpiListener()
    {
        if (m_acpiListen->state() != QProcess::NotRunning) return;
        if (m_cycles % 20 != 1) return;
        const QString listener = QStandardPaths::findExecutable("acpi_listen");
        if (!listener.isEmpty()) m_acpiListen->start(listener, {});
    }

    LidReading detectLid()
    {
        // UPower is seat-aware and can read input switches without adding
        // users to the security-sensitive 'input' group.
        LidReading s = viaUPower();
        if (s.found) return s;
        s = viaProcAcpi();
        if (s.found) return s;
        s = viaEvdev();
        if (s.found) return s;
        if (m_acpiEventKnown)
            return {true, m_acpiEventClosed, "acpid event socket"};
        return {};
    }

    void acquireInhibitor()
    {
        if (m_lidInhibitor.isValid()) return;
        if (m_cycles - m_lastInhibitTry < 8) return;
        m_lastInhibitTry = m_cycles;
        QDBusInterface manager("org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", QDBusConnection::systemBus());
        manager.setTimeout(1300);
        QDBusReply<QDBusUnixFileDescriptor> r = manager.call(
            "Inhibit", "handle-lid-switch", "AlternixOS Display",
            "Apply selected lid-switch behaviour", "block");
        if (r.isValid() && r.value().isValid()) {
            m_lidInhibitor = r.value();
            m_inhibitError.clear();
        } else {
            m_inhibitError = r.isValid() ? "elogind returned no inhibitor file descriptor"
                                         : r.error().message();
        }
    }

    void requestSuspend()
    {
        // Only a genuine OPEN -> CLOSED transition is eligible. No surprise
        // suspend when launching the program with a lid already shut.
        QDBusMessage req = QDBusMessage::createMethodCall(
            "org.freedesktop.login1", "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager", "Suspend");
        req << false; // polkit: use current authorised graphical session
        auto *pending = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(req, 9000), this);
        connect(pending, &QDBusPendingCallWatcher::finished, this,
                [this](QDBusPendingCallWatcher *watcher) {
            const QDBusPendingReply<void> reply = *watcher;
            m_lastAction = reply.isError()
                ? "Suspend failed: " + reply.error().message()
                : "Suspend requested";
            watcher->deleteLater();
            publish(true);
        });
        m_lastAction = "Requesting suspend from elogind";
    }

    void poll()
    {
        ++m_cycles;
        ensureAcpiListener();
        const QMap<QString, QString> cfg = readConfig();
        int state = cfg.value("display_sleep_timeout", "0").toInt();
        if (state < 0 || state > 5) state = 0;
        QString action = cfg.value("display_lid_action", "sleep").toLower();
        if (action != "screen_off" && action != "ignore") action = "sleep";
        const bool settingsChanged = (m_timeoutState != state || m_action != action);
        m_timeoutState = state;
        m_action = action;
        if (m_screenOffHeld && action != "screen_off") {
            setDisplayPower(true);
            m_screenOffHeld = false;
        }

        if (m_cycles == 1 || settingsChanged || m_cycles % 12 == 0) {
            const ScreenResult screen = applyScreenTimeout(timeoutSeconds(state), true);
            m_hasXDisplay = screen.connected;
            m_hasDpms = screen.dpms;
            m_displayError = screen.error;
            // Do not retain a machine-wide lid inhibitor after the graphical
            // X session has disappeared (e.g. the user logs out).
            if (!screen.connected && ++m_missingDisplayChecks >= 3) {
                QCoreApplication::quit();
                return;
            }
            if (screen.connected) m_missingDisplayChecks = 0;
        }
        m_lid = detectLid();
        if (m_lid.found) acquireInhibitor();

        if (m_lid.found) {
            if (!m_hadReading) {
                // Establish the initial state without firing an action. An
                // acpid event is different: acpi_listen only reports a real
                // switch transition, so its first observed close is actionable.
                const bool firstAcpiClose = m_lid.source == "acpid event socket"
                    && m_acpiEventPending && m_lid.closed;
                m_prevClosed = m_lid.closed;
                m_hadReading = true;
                if (firstAcpiClose) {
                    if (m_lidInhibitor.isValid()) {
                        if (m_action == "sleep") requestSuspend();
                        else if (m_action == "ignore") m_lastAction = "Lid closed: ignored";
                        else if (m_hasDpms) {
                            m_screenOffHeld = setDisplayPower(false);
                            m_lastAction = "Lid closed: screen powered off";
                        } else m_lastAction = "Cannot turn off screen: X11 DPMS is not available";
                    } else {
                        m_lastAction = "Lid action not controlled: elogind inhibitor unavailable";
                    }
                }
            } else if (m_prevClosed != m_lid.closed) {
                m_prevClosed = m_lid.closed;
                if (m_lid.closed && m_lidInhibitor.isValid()) {
                    if (m_action == "sleep") requestSuspend();
                    else if (m_action == "ignore") m_lastAction = "Lid closed: ignored";
                    else if (m_hasDpms) {
                        m_screenOffHeld = setDisplayPower(false);
                        m_lastAction = "Lid closed: screen powered off";
                    } else m_lastAction = "Cannot turn off screen: X11 DPMS is not available";
                } else if (m_lid.closed) {
                    m_lastAction = "Lid action not controlled: elogind inhibitor unavailable";
                } else {
                    if (m_screenOffHeld && m_hasDpms) setDisplayPower(true);
                    m_screenOffHeld = false;
                    m_lastAction = "Lid opened";
                }
            }
            // Pointer/pen activity can wake a DPMS-blanked display while
            // closed; keep it off without continuously forcing screen redraws.
            if (m_lid.closed && m_action == "screen_off" && m_lidInhibitor.isValid()
                && m_hasDpms) m_screenOffHeld = setDisplayPower(false);
            m_acpiEventPending = false;
        } else {
            m_hadReading = false;
        }
        publish(settingsChanged);
    }

    void publish(bool force = false)
    {
        QMap<QString, QString> status;
        status["pid"] = QString::number(QCoreApplication::applicationPid());
        status["lid_detected"] = m_lid.found ? "yes" : "no";
        status["lid_state"] = !m_lid.found ? "unknown" : (m_lid.closed ? "closed" : "open");
        status["lid_source"] = m_lid.source;
        status["lid_action"] = m_action;
        status["inhibitor"] = m_lidInhibitor.isValid() ? "active" : "unavailable";
        status["inhibitor_error"] = m_inhibitError;
        status["screen_timeout_seconds"] = QString::number(timeoutSeconds(m_timeoutState));
        status["x11"] = m_hasXDisplay ? "yes" : "no";
        status["dpms"] = m_hasDpms ? "yes" : "no";
        status["display_error"] = m_displayError;
        status["last_action"] = m_lastAction;
        if (force || status != m_lastPublished) {
            writeStatus(status);
            m_lastPublished = status;
        }
    }

    QTimer m_pollTimer;
    QProcess *m_acpiListen = nullptr;
    QByteArray m_acpiBuffer;
    QDBusUnixFileDescriptor m_lidInhibitor;
    QMap<QString, QString> m_lastPublished;
    LidReading m_lid;
    int m_cycles = 0;
    int m_lastInhibitTry = -100;
    int m_timeoutState = -1;
    int m_missingDisplayChecks = 0;
    bool m_hadReading = false;
    bool m_prevClosed = false;
    bool m_acpiEventKnown = false;
    bool m_acpiEventClosed = false;
    bool m_acpiEventPending = false;
    bool m_hasXDisplay = false;
    bool m_hasDpms = false;
    bool m_screenOffHeld = false;
    QString m_action;
    QString m_inhibitError;
    QString m_lastAction;
    QString m_displayError;
};

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("osm-display-power");
    if (app.arguments().contains("--probe")) {
        LidReading lid = viaUPower();
        if (!lid.found) lid = viaProcAcpi();
        if (!lid.found) lid = viaEvdev();
        QTextStream out(stdout);
        out << "Detected: " << (lid.found ? "yes" : "no") << '\n'
            << "State: " << (lid.found ? (lid.closed ? "closed" : "open") : "unknown") << '\n'
            << "Source: " << lid.source << '\n';
        return lid.found ? 0 : 2;
    }
    if (qEnvironmentVariableIsEmpty("DISPLAY")) {
        qWarning() << "No X11 DISPLAY in this session; not taking a lid inhibitor";
        return 3;
    }
    Display *checkDisplay = XOpenDisplay(nullptr);
    if (!checkDisplay) {
        qWarning() << "X11 display unavailable; not taking a lid inhibitor";
        return 3;
    }
    XCloseDisplay(checkDisplay);
    const QString lockBase = configDir() + "/runtime";
    QDir().mkpath(lockBase);
    QLockFile lock(lockBase + "/alternix-display-power.lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) {
        qWarning() << "osm-display-power is already running in this user session";
        return 0;
    }
    DisplayPowerService service;
    return app.exec();
}
