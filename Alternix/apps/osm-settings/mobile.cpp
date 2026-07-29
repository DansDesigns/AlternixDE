#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QFrame>
#include <QScrollArea>
#include <QScroller>
#include <QProcess>
#include <QFont>
#include <QStringList>
#include <QTimer>
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QTextEdit>
#include <QTextCursor>
#include <QEventLoop>
#include <QSettings>
#include <QDir>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QFile>
#include <functional>

// ---------------------------------------------------------
// Constants
// ---------------------------------------------------------

static const char *NM_CON_NAME = "Alternix Mobile";

// Offline APN database. This is the same file NetworkManager itself reads,
// shipped by the 'mobile-broadband-provider-info' package.
static const char *PROVIDER_DB =
    "/usr/share/mobile-broadband-provider-info/serviceproviders.xml";

// Spinner frames - identical to wifi.cpp (Braille rotation, present in
// DejaVu Sans, so no font fallback and no grey glyphs).
static const char* const SPIN_FRAMES[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"
};
static const int SPIN_COUNT = 10;

// Set by whichever page or dialog wants its spinner animated during a
// blocking command. runProc() calls it on every poll, which is what keeps
// the icon moving through a two minute tower scan. Single threaded GUI, so
// a file scope hook avoids threading a callback through every call site.
static std::function<void()> g_spinTick;

// ---------------------------------------------------------
// Helpers (same as Bluetooth)
// ---------------------------------------------------------

static QPushButton* smallBtnBT(const QString &txt) {
    QPushButton *b = new QPushButton(txt);
    b->setFixedSize(180, 60);
    b->setStyleSheet(
        "QPushButton {"
        " background:#444444;"
        " color:white;"
        " border:1px solid #222222;"
        " border-radius:16px;"
        " font-size:26px;"
        " font-weight:bold;"
        " padding:10px 24px;"
        "}"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
    );
    return b;
}

// Runs a process without blocking the UI thread.
// Uses a local event loop + polling (same pattern as wifi.cpp's
// runCmdAsRootAnimated) so the page keeps repainting during long
// operations such as --3gpp-scan.
static QString runProc(const QString &prog, const QStringList &args,
                       int timeoutMs, int *exitCode = nullptr)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(prog, args);

    if (!p.waitForStarted(3000)) {
        if (exitCode) *exitCode = -1;
        return QString("failed to start: ") + prog;
    }

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(80);   // same cadence as wifi.cpp's runCmdAnimated
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        if (g_spinTick) g_spinTick();
        if (p.state() == QProcess::NotRunning) loop.quit();
    });

    QTimer killer;
    killer.setSingleShot(true);
    QObject::connect(&killer, &QTimer::timeout, [&]() {
        p.kill();
        loop.quit();
    });

    poll.start();
    killer.start(timeoutMs);
    loop.exec();
    poll.stop();
    killer.stop();

    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished(2000);
    }

    if (exitCode) *exitCode = p.exitCode();

    QString out = QString::fromUtf8(p.readAll());

    // ANSI FIX - DO NOT REMOVE
    // mmcli colourises its output even when stdout is a pipe, so a parsed
    // field comes back as "\033[31mfailed\033[0m" rather than "failed".
    // Every startsWith("failed") / startsWith("connected") test against a
    // parsed value silently returned false because the string actually
    // begins with an escape sequence, which is why state gating did nothing
    // and the on/off button never reflected the modem. It also puts raw
    // escape codes in the setup log. Stripped centrally, for every command.
    static const QRegularExpression ansiRe("\\x1B\\[[0-9;?]*[ -/]*[@-~]");
    out.remove(ansiRe);

    return out;
}

// Same look as smallBtnBT, but tall enough for two lines of 26px text.
// QPushButton honours a literal \n in its text; "&&" renders as one "&".
static QPushButton* twoLineBtnBT(const QString &txt, int w = 240) {
    QPushButton *b = new QPushButton(txt);
    b->setFixedSize(w, 104);
    b->setStyleSheet(
        "QPushButton {"
        " background:#444444;"
        " color:white;"
        " border:1px solid #222222;"
        " border-radius:16px;"
        " font-size:26px;"
        " font-weight:bold;"
        " padding:8px 12px;"
        " text-align:center;"
        "}"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
    );
    return b;
}

static QString runCmd(const QString &cmd, int timeoutMs = 10000, int *exitCode = nullptr)
{
    return runProc("bash", {"-c", cmd}, timeoutMs, exitCode);
}

// Privileged run. NOPASSWD sudo is granted to the install user, so
// sudo -n is tried first; if sudo is unavailable the command is run
// plainly (polkit may still authorise it under a desktop session).
static QString runPriv(const QString &prog, const QStringList &args,
                       int timeoutMs, int *exitCode = nullptr)
{
    int rc = 0;
    QStringList sudoArgs;
    sudoArgs << "-n" << prog << args;
    QString out = runProc("sudo", sudoArgs, timeoutMs, &rc);

    if (rc != 0 && (out.contains("sudo:", Qt::CaseInsensitive) ||
                    out.contains("password", Qt::CaseInsensitive) ||
                    out.contains("failed to start", Qt::CaseInsensitive))) {
        out = runProc(prog, args, timeoutMs, &rc);
    }

    if (exitCode) *exitCode = rc;
    return out;
}

// ---------------------------------------------------------
// mmcli output parsing
//
// mmcli output is column formatted:
//     Status   |           state: connected
// so the field name lives after the last '|' on the line.
// ---------------------------------------------------------

static QString mmcliField(const QString &out, const QString &key)
{
    const QStringList lines = out.split('\n');
    for (const QString &raw : lines) {
        QString line = raw;
        int bar = line.lastIndexOf('|');
        if (bar >= 0) line = line.mid(bar + 1);
        line = line.trimmed();
        if (line.startsWith(key + ":", Qt::CaseInsensitive)) {
            QString v = line.mid(key.length() + 1).trimmed();
            // mmcli < 1.18 quotes its values: state: 'connected'
            if (v.length() >= 2 && v.startsWith('\'') && v.endsWith('\''))
                v = v.mid(1, v.length() - 2);
            return v.trimmed();
        }
    }
    return QString();
}

// Field name for the SIM path moved between mmcli releases.
//
// The bare "path" key must never be accepted on its own: `mmcli -m N` also
// prints "General | path: /org/freedesktop/ModemManager1/Modem/0", which is
// the modem's own object path. Matching that made a modem with no SIM look
// as though a SIM were present. Only a value under /SIM/ counts.
static QString mmcliSimPath(const QString &out)
{
    QString p = mmcliField(out, "primary sim path");
    if (p.isEmpty()) p = mmcliField(out, "sim path");
    if (p.isEmpty()) p = mmcliField(out, "path");
    if (!p.contains("/SIM", Qt::CaseInsensitive)) return QString();
    return p;
}

// ---------------------------------------------------------
// Modem discovery
// ---------------------------------------------------------

static int g_modemIndex = -1;

static bool mmcliInstalled()
{
    int rc = 0;
    runCmd("test -x /usr/bin/mmcli", 5000, &rc);
    return rc == 0;
}

// Re-reads mmcli -L and caches the modem index. Returns -1 if none.
static int detectModemIndex()
{
    if (!mmcliInstalled()) {
        g_modemIndex = -1;
        return -1;
    }

    QString out = runCmd("mmcli -L 2>&1", 10000);
    QRegularExpression re("/org/freedesktop/ModemManager1/Modem/(\\d+)");
    QRegularExpressionMatch m = re.match(out);

    g_modemIndex = m.hasMatch() ? m.captured(1).toInt() : -1;
    return g_modemIndex;
}

static int modemIndex()
{
    if (g_modemIndex < 0) detectModemIndex();
    return g_modemIndex;
}

static bool modemAvailable()
{
    return modemIndex() >= 0;
}

// One mmcli call, reused by every status field below.
static QString modemStatus()
{
    if (!modemAvailable()) return QString();
    return runCmd(QString("mmcli -m %1 2>&1").arg(g_modemIndex), 10000);
}

static bool isMobilePowered(const QString &status)
{
    QString st = mmcliField(status, "state");
    return st.startsWith("connected") ||
           st.startsWith("registered") ||
           st.startsWith("enabled") ||
           st.startsWith("searching");
}

static QString getCurrentCarrier(const QString &status)
{
    if (status.isEmpty()) return "No modem detected";
    QString name = mmcliField(status, "operator name");
    if (name.isEmpty() || name == "--") return "No carrier detected";
    return "Carrier: " + name;
}

static QString getConnectionTime(const QString &status)
{
    if (status.isEmpty()) return "Connection time: N/A";
    QString d = mmcliField(status, "duration");
    if (d.isEmpty() || d == "--") return "Connection time: Unknown";
    return "Connection time: " + d;
}

static QString getSimSummary(const QString &status)
{
    if (status.isEmpty()) return "SIM: no modem";

    QString st = mmcliField(status, "state");
    if (st.startsWith("locked")) {
        QString lock = mmcliField(status, "lock");
        return "SIM: LOCKED (" + (lock.isEmpty() ? QString("pin required") : lock) + ")";
    }

    QString simPath = mmcliSimPath(status);
    if (simPath.isEmpty() || simPath == "--" || simPath.contains("none"))
        return "SIM: not detected";

    return "SIM: present";
}

static QString modemState()
{
    return mmcliField(modemStatus(), "state");
}

// ModemManager reports a failure reason when state == failed. Without this
// the caller only sees "wrong state" and has nothing to act on.
static QString modemFailReason(const QString &status)
{
    QString r = mmcliField(status, "failed reason");
    if (r.isEmpty()) r = mmcliField(status, "state failed reason");
    if (r.isEmpty() || r == "--") r = "no reason reported";
    return r;
}

// Sleep without freezing the UI.
static void uiSleep(int ms)
{
    QEventLoop loop;
    QTimer t;
    t.setSingleShot(true);
    QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    t.start(ms);
    loop.exec();
}

// --enable/--disable return before the modem has finished the transition.
// Poll until the state is one we can act on, or until timeout.
// Returns the final state.
static QString waitForState(const QStringList &wanted, int timeoutMs)
{
    int waited = 0;
    QString st = modemState();

    while (waited < timeoutMs) {
        for (const QString &w : wanted)
            if (st.startsWith(w)) return st;
        if (st.startsWith("failed")) return st;

        uiSleep(500);
        waited += 500;
        st = modemState();
    }

    return st;
}

// ---------------------------------------------------------
// IP readout (same idea as wifi.cpp's getIP)
//
// NetworkManager owns the connection, so the device it has bound to the
// gsm profile is the authority on which kernel interface carries the data
// session. Falls back to a direct `ip -4 addr` on any wwan interface.
// ---------------------------------------------------------

static QString mobileDevice()
{
    QString out = runPriv("nmcli", {"-t", "-f", "DEVICE,TYPE", "device", "status"}, 10000);
    const QStringList lines = out.split('\n');
    for (const QString &line : lines) {
        int sep = line.lastIndexOf(':');
        if (sep < 0) continue;
        if (line.mid(sep + 1).trimmed() == "gsm")
            return line.left(sep).trimmed();
    }
    return QString();
}

static QString getMobileIP()
{
    QString dev = mobileDevice();
    if (!dev.isEmpty()) {
        QString out = runPriv("nmcli", {"-g", "IP4.ADDRESS", "device", "show", dev}, 10000);
        QString ip = out.split('\n').value(0).trimmed();
        if (!ip.isEmpty() && ip != "--") {
            int slash = ip.indexOf('/');          // strip the /24
            return slash > 0 ? ip.left(slash) : ip;
        }
    }

    // Fallback: the modem's data interface is usually wwanN / wwpNsM.
    // Matched by name explicitly - a bare "first non-loopback address"
    // would happily report the WiFi address as the mobile one.
    return runCmd("ip -o -4 addr show 2>/dev/null "
                  "| awk '$2 ~ /^(wwan|wwp|cdc-wdm|mhi)/ {print $4}' "
                  "| cut -d/ -f1 | head -n1", 8000).trimmed();
}

// ---------------------------------------------------------
// Automatic APN lookup
//
// The settings are read from the local provider database rather than
// fetched over the network, because there is no network to fetch them
// over: you cannot download an APN using a data connection that needs
// the APN to come up. This is the same file NetworkManager consults.
// If the database is absent it can be installed over WiFi via apt.
// ---------------------------------------------------------

struct ApnEntry {
    QString apn;
    QString user;
    QString pass;
    QString name;
    bool    mmsOnly = false;
};

static QString stripLeadingZeros(const QString &s)
{
    QString t = s;
    while (t.length() > 1 && t.startsWith('0')) t.remove(0, 1);
    return t;
}

// An operator id is MCC+MNC concatenated: 23430 -> mcc 234, mnc 30.
static bool splitOperatorId(const QString &id, QString &mcc, QString &mnc)
{
    QString s = id.trimmed();
    if (s.length() < 5) return false;
    mcc = s.left(3);
    mnc = s.mid(3);
    return true;
}

// Operator id from the SIM first: that works before the modem has
// registered anywhere, which is exactly when the APN is still missing.
static QString getOperatorId(QString *operatorName = nullptr)
{
    QString status  = modemStatus();
    QString simPath = mmcliSimPath(status);

    if (!simPath.isEmpty()) {
        QString simOut = runPriv("mmcli", {"-i", simPath}, 15000);
        QString id = mmcliField(simOut, "operator id");
        if (operatorName) {
            QString n = mmcliField(simOut, "operator name");
            if (!n.isEmpty() && n != "--") *operatorName = n;
        }
        if (!id.isEmpty() && id != "--") return id;
    }

    if (operatorName && operatorName->isEmpty()) {
        QString n = mmcliField(status, "operator name");
        if (!n.isEmpty() && n != "--") *operatorName = n;
    }

    QString id = mmcliField(status, "operator id");
    return (id == "--") ? QString() : id;
}

// Finds a working non-mobile connection to download over. Ethernet is
// preferred over WiFi. Returns the device name, empty if nothing is up.
static QString onlineNonMobileDevice(QString *typeOut)
{
    QString out = runPriv("nmcli", {"-t", "-f", "DEVICE,TYPE,STATE", "device", "status"}, 10000);

    QString wifiDev;
    const QStringList lines = out.split('\n');
    for (const QString &line : lines) {
        const QStringList f = line.split(':');
        if (f.size() < 3) continue;

        const QString dev   = f.at(0).trimmed();
        const QString type  = f.at(1).trimmed();
        const QString state = f.at(2).trimmed();

        if (!state.startsWith("connected")) continue;

        if (type == "ethernet") {
            if (typeOut) *typeOut = "Ethernet";
            return dev;                       // wired wins outright
        }
        if (type == "wifi" && wifiDev.isEmpty())
            wifiDev = dev;
    }

    if (!wifiDev.isEmpty()) {
        if (typeOut) *typeOut = "WiFi";
        return wifiDev;
    }

    return QString();
}

static QList<ApnEntry> lookupApns(const QString &mcc, const QString &mnc,
                                  QString *providerOut, QString *errOut)
{
    QList<ApnEntry> result;

    QFile f(PROVIDER_DB);
    if (!f.exists()) {
        if (errOut)
            *errOut = "missing-db";
        return result;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (errOut)
            *errOut = QString("Could not read %1").arg(PROVIDER_DB);
        return result;
    }

    const QString wantMnc = stripLeadingZeros(mnc);

    QXmlStreamReader xml(&f);
    QString curProvider;
    bool    idMatch = false;
    bool    inApn = false;
    QList<ApnEntry> curApns;
    ApnEntry cur;

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            const QStringRef n = xml.name();

            if (n == QLatin1String("provider")) {
                curProvider.clear();
                idMatch = false;
                inApn = false;
                curApns.clear();
            } else if (n == QLatin1String("network-id")) {
                QString m1 = xml.attributes().value("mcc").toString().trimmed();
                QString m2 = xml.attributes().value("mnc").toString().trimmed();
                if (m1 == mcc && stripLeadingZeros(m2) == wantMnc)
                    idMatch = true;
            } else if (n == QLatin1String("apn")) {
                inApn = true;
                cur = ApnEntry();
                cur.apn = xml.attributes().value("value").toString().trimmed();
            } else if (n == QLatin1String("usage")) {
                if (inApn && xml.attributes().value("type").toString() == QLatin1String("mms"))
                    cur.mmsOnly = true;
            } else if (n == QLatin1String("name")) {
                QString t = xml.readElementText().trimmed();
                if (inApn) { if (cur.name.isEmpty()) cur.name = t; }
                else if (curProvider.isEmpty()) curProvider = t;
            } else if (n == QLatin1String("username")) {
                if (inApn) cur.user = xml.readElementText().trimmed();
            } else if (n == QLatin1String("password")) {
                if (inApn) cur.pass = xml.readElementText().trimmed();
            }
        } else if (xml.isEndElement()) {
            const QStringRef n = xml.name();

            if (n == QLatin1String("apn")) {
                inApn = false;
                if (!cur.apn.isEmpty()) curApns << cur;
            } else if (n == QLatin1String("provider")) {
                if (idMatch && !curApns.isEmpty()) {
                    if (providerOut) *providerOut = curProvider;
                    // Internet entries first; an MMS-only APN is no use here.
                    for (const ApnEntry &e : curApns) if (!e.mmsOnly) result << e;
                    for (const ApnEntry &e : curApns) if (e.mmsOnly)  result << e;
                    return result;
                }
            }
        }
    }

    if (xml.hasError() && errOut)
        *errOut = "Provider database is malformed: " + xml.errorString();

    return result;
}

// Returns an empty string on success, or a human-readable error.
static QString setMobilePowered(bool on)
{
    if (!modemAvailable())
        return "No mobile adaptor found. Run Setup first.";

    QString before = modemState();
    if (before.startsWith("failed"))
        return "The modem is in a failed state (" + modemFailReason(modemStatus()) +
               "). It cannot be turned on until that is cleared.";

    if (on && before.startsWith("locked"))
        return "The SIM is locked. Enter the PIN in APN Settings first.";

    int rc = 0;
    QString out = runPriv("mmcli",
                          {QString("-m"), QString::number(g_modemIndex),
                           on ? QString("--enable") : QString("--disable")},
                          20000, &rc);

    if (rc != 0 && !out.contains("successfully", Qt::CaseInsensitive))
        return out.trimmed().isEmpty()
                   ? QString("mmcli --%1 failed with no output.").arg(on ? "enable" : "disable")
                   : out.trimmed();

    // Wait for the transition to actually happen, otherwise the caller
    // reads the old state back and the button appears to do nothing.
    QString st = on ? waitForState({"enabled", "searching", "registered", "connected"}, 15000)
                    : waitForState({"disabled"}, 10000);

    if (st.startsWith("failed"))
        return "The modem entered a failed state (" + modemFailReason(modemStatus()) + ").";

    if (on && (st.startsWith("disabled") || st.startsWith("enabling")))
        return "The modem did not come up (state is still '" + st + "').";

    return QString();
}

// ---------------------------------------------------------
// 3GPP scan parsing
//
//   3GPP scan | networks: 23430 - EE (lte, available)
//             |           23410 - O2 - UK (umts, available)
// ---------------------------------------------------------

static QStringList parseScan(const QString &out)
{
    QStringList nets;
    bool inBlock = false;

    const QStringList lines = out.split('\n');
    for (const QString &raw : lines) {
        int bar = raw.lastIndexOf('|');
        QString tail = (bar >= 0) ? raw.mid(bar + 1).trimmed() : raw.trimmed();

        if (tail.startsWith("networks:", Qt::CaseInsensitive)) {
            inBlock = true;
            tail = tail.mid(9).trimmed();
        } else if (bar < 0) {
            inBlock = false;
        }

        if (inBlock && !tail.isEmpty() && !tail.startsWith("--"))
            nets << tail;
    }

    return nets;
}

// ---------------------------------------------------------
// Stored settings (~/.config/Alternix/osm-settings.conf)
// NOTE: the APN password is NOT written here; it is stored only in
// the NetworkManager connection, which is root readable only.
// ---------------------------------------------------------

static QString settingsPath()
{
    return QDir::homePath() + "/.config/Alternix/osm-settings.conf";
}

struct MobileConf {
    QString apn;
    QString user;
    QString auth;      // auto | pap | chap
    QString iptype;    // ipv4 | ipv6 | ipv4v6
    bool    roaming = true;
};

static MobileConf loadMobileConf()
{
    QSettings s(settingsPath(), QSettings::IniFormat);
    s.beginGroup("Mobile");
    MobileConf c;
    c.apn     = s.value("APN", "").toString();
    c.user    = s.value("Username", "").toString();
    c.auth    = s.value("AuthType", "auto").toString();
    c.iptype  = s.value("IPType", "ipv4v6").toString();
    c.roaming = s.value("Roaming", true).toBool();
    s.endGroup();
    return c;
}

static void saveMobileConf(const MobileConf &c)
{
    QSettings s(settingsPath(), QSettings::IniFormat);
    s.beginGroup("Mobile");
    s.setValue("APN", c.apn);
    s.setValue("Username", c.user);
    s.setValue("AuthType", c.auth);
    s.setValue("IPType", c.iptype);
    s.setValue("Roaming", c.roaming);
    s.endGroup();
    s.sync();
}

// ---------------------------------------------------------
// NetworkManager gsm profile
//
// Built explicitly with `connection add`, never by letting nmcli
// auto-generate a profile - same reasoning as the wifi.cpp fix.
// Arguments are passed as a list, never through a shell, so an APN
// or password containing quotes/spaces cannot break the command.
// ---------------------------------------------------------

static QString applyMobileProfile(const MobileConf &c, const QString &password, int *exitCode)
{
    QString log;
    int rc = 0;

    // 1. remove any stale profile
    log += "> nmcli connection delete \"" + QString(NM_CON_NAME) + "\"\n";
    log += runPriv("nmcli", {"connection", "delete", NM_CON_NAME}, 15000, &rc);

    // 2. build the profile explicitly
    QStringList args;
    args << "connection" << "add"
         << "type" << "gsm"
         << "ifname" << "*"
         << "con-name" << NM_CON_NAME
         << "connection.autoconnect" << "yes"
         << "gsm.apn" << c.apn;

    if (!c.user.isEmpty())
        args << "gsm.username" << c.user;

    if (!password.isEmpty()) {
        args << "gsm.password" << password
             << "gsm.password-flags" << "0";
    }

    args << "gsm.home-only" << (c.roaming ? "no" : "yes");

    if (c.iptype == "ipv4") {
        args << "ipv4.method" << "auto" << "ipv6.method" << "ignore";
    } else if (c.iptype == "ipv6") {
        args << "ipv4.method" << "disabled" << "ipv6.method" << "auto";
    } else {
        args << "ipv4.method" << "auto" << "ipv6.method" << "auto";
    }

    // PPP auth selection: refuse everything except the chosen method.
    if (c.auth == "pap") {
        args << "ppp.refuse-chap" << "yes"
             << "ppp.refuse-eap" << "yes"
             << "ppp.refuse-mschap" << "yes"
             << "ppp.refuse-mschapv2" << "yes";
    } else if (c.auth == "chap") {
        args << "ppp.refuse-pap" << "yes"
             << "ppp.refuse-eap" << "yes"
             << "ppp.refuse-mschap" << "yes"
             << "ppp.refuse-mschapv2" << "yes";
    }

    log += "> nmcli connection add type gsm apn=" + c.apn + "\n";
    log += runPriv("nmcli", args, 20000, &rc);

    if (exitCode) *exitCode = rc;
    return log;
}

static bool mobileProfileExists()
{
    int rc = 0;
    runPriv("nmcli", {"-g", "connection.id", "connection", "show", NM_CON_NAME}, 15000, &rc);
    return rc == 0;
}

static QString bringUpMobileProfile(int *exitCode)
{
    int rc = 0;
    QString log = "> nmcli connection up \"" + QString(NM_CON_NAME) + "\"\n";
    log += runPriv("nmcli", {"connection", "up", NM_CON_NAME}, 60000, &rc);
    if (exitCode) *exitCode = rc;
    return log;
}

// Reads back the live profile so the dialog shows what is actually set.
static bool readMobileProfile(MobileConf &c)
{
    int rc = 0;
    QString out = runPriv("nmcli",
                          {"-g", "gsm.apn,gsm.username,gsm.home-only",
                           "connection", "show", NM_CON_NAME},
                          15000, &rc);
    if (rc != 0) return false;

    QStringList v = out.split('\n');
    if (v.size() >= 1 && !v.at(0).trimmed().isEmpty() && v.at(0).trimmed() != "--")
        c.apn = v.at(0).trimmed();
    if (v.size() >= 2 && !v.at(1).trimmed().isEmpty() && v.at(1).trimmed() != "--")
        c.user = v.at(1).trimmed();
    if (v.size() >= 3)
        c.roaming = (v.at(2).trimmed() != "yes");

    return true;
}

// ---------------------------------------------------------
// Shared dialog styling
// ---------------------------------------------------------

static void styleDialog(QDialog *d)
{
    // GREY NUMBERS FIX - DO NOT REMOVE
    // Same method as wifi.cpp: explicit class selectors in a stylesheet and
    // NO palette manipulation anywhere. Setting a palette - on the app, on
    // the dialog, or on individual input widgets - is what produced the grey
    // text in the first place. Every widget class that draws its own text
    // needs its own rule here; an unqualified "color:white" does not reach
    // them. QComboBox's popup is a separate top level window, so it needs
    // the QAbstractItemView rule as well.
    //
    // The family must be named concretely. Dialogs are parented to the main
    // window, so they inherit the application font rather than the page
    // font, and the family that was in use had no digit glyphs - fontconfig
    // substituted a separate face for 0-9, which drew in its own colour and
    // ignored every colour rule below. That is what made only the numbers
    // grey. wifi.cpp avoids this by naming DejaVu Sans outright.
    QFont df;
    df.setFamily("DejaVu Sans");
    d->setFont(df);

    d->setStyleSheet(
        "QDialog { background:#282828; font-family:'DejaVu Sans'; }"
        "QWidget { background:#282828; font-family:'DejaVu Sans'; }"
        "QScrollArea { background:#282828; font-family:'DejaVu Sans'; border:none; }"
        "QLabel { color:white; background:transparent; font-family:'DejaVu Sans'; }"
        "QMessageBox QLabel { color:white; font-family:'DejaVu Sans'; }"
        "QLineEdit { background:#3a3a3a; color:white; font-family:'DejaVu Sans'; "
        "  border:1px solid #222222; border-radius:12px; font-size:26px; "
        "  padding:10px 14px; min-height:44px; }"
        "QComboBox { background:#3a3a3a; color:white; font-family:'DejaVu Sans'; "
        "  border:1px solid #222222; border-radius:12px; font-size:26px; "
        "  padding:10px 14px; min-height:44px; }"
        "QComboBox QAbstractItemView { background:#3a3a3a; color:white; "
        "  font-family:'DejaVu Sans'; font-size:26px; selection-background-color:#555555; "
        "  selection-color:white; }"
        "QTextEdit { background:#1e1e1e; color:white; font-family:'DejaVu Sans'; "
        "  border:1px solid #222222; border-radius:16px; font-size:22px; }"
    );
}

// ---------------------------------------------------------
// Setup & scan dialog
// ---------------------------------------------------------

class MobileSetupDialog : public QDialog
{
public:
    explicit MobileSetupDialog(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle("Mobile setup");
        setModal(true);
        styleDialog(this);

        QVBoxLayout *v = new QVBoxLayout(this);
        v->setContentsMargins(30, 30, 30, 30);
        v->setSpacing(20);

        QLabel *t = new QLabel("Adaptor / SIM setup", this);
        t->setStyleSheet("font-size:34px; color:white; font-weight:bold;");
        t->setAlignment(Qt::AlignCenter);
        v->addWidget(t);

        logView = new QTextEdit(this);
        logView->setReadOnly(true);
        v->addWidget(logView, 1);

        QHBoxLayout *row = new QHBoxLayout();
        row->setSpacing(20);
        runButton   = smallBtnBT("Start");
        closeButton = smallBtnBT("Close");
        row->addWidget(runButton);
        row->addWidget(closeButton);
        v->addLayout(row);

        connect(runButton, &QPushButton::clicked, this, &MobileSetupDialog::runSetup);
        connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

        if (parent) resize(parent->width() - 120, parent->height() - 160);
        else resize(900, 700);
    }

    ~MobileSetupDialog() override { g_spinTick = nullptr; }

    QStringList towers() const { return foundTowers; }
    bool succeeded() const { return ok; }

private:
    QTextEdit *logView = nullptr;
    QPushButton *runButton = nullptr;
    QPushButton *closeButton = nullptr;
    QStringList foundTowers;
    bool ok = false;
    bool running = false;
    bool spinnerActive = false;
    int  spinnerFrame = 0;

    void say(const QString &s)      { logLine(s, "#ffffff", false); }
    void good(const QString &s)     { logLine("OK: " + s, "#7CFC00", false); }
    void bad(const QString &s)      { logLine("ERROR: " + s, "#FF5555", true); }
    void warnLine(const QString &s) { logLine("WARNING: " + s, "#FFC066", false); }

    // Any line that announces work in progress gets the spinner from
    // wifi.cpp's Refresh button on the line underneath it, animating for as
    // long as the command runs. It is removed when the next line arrives.
    void logLine(const QString &text, const QString &colour, bool bold)
    {
        spinnerStop();

        logView->append(QString("<span style=\"color:%1; %2 font-family:'DejaVu Sans';\">%3</span>")
                            .arg(colour)
                            .arg(bold ? "font-weight:bold;" : "")
                            .arg(text.toHtmlEscaped()));

        const QString lower = text.toLower();
        if (lower.contains("scanning") || lower.contains("loading") ||
            lower.contains("waiting")  || lower.contains("downloading"))
            spinnerStart();

        pump();
    }

    QString spinnerHtml(int frame) const
    {
        return QString("<span style=\"color:#66CCFF; font-family:'DejaVu Sans';\">   %1</span>")
                   .arg(QString::fromUtf8(SPIN_FRAMES[frame]));
    }

    void spinnerStart()
    {
        spinnerFrame = 0;
        spinnerActive = true;
        logView->append(spinnerHtml(0));
        logView->moveCursor(QTextCursor::End);
        g_spinTick = [this]() { spinnerTick(); };
    }

    void spinnerTick()
    {
        if (!spinnerActive) return;
        spinnerFrame = (spinnerFrame + 1) % SPIN_COUNT;
        replaceLastBlock(spinnerHtml(spinnerFrame));
    }

    void spinnerStop()
    {
        if (!spinnerActive) return;
        spinnerActive = false;
        g_spinTick = nullptr;
        removeLastBlock();
    }

    void removeLastBlock()
    {
        QTextCursor c(logView->document());
        c.movePosition(QTextCursor::End);
        c.select(QTextCursor::BlockUnderCursor);
        c.removeSelectedText();
        logView->moveCursor(QTextCursor::End);
    }

    void replaceLastBlock(const QString &html)
    {
        removeLastBlock();
        logView->append(html);
        logView->moveCursor(QTextCursor::End);
    }

    void pump()
    {
        logView->moveCursor(QTextCursor::End);
        QApplication::processEvents();
    }

    void runSetup()
    {
        if (running) return;
        running = true;
        ok = false;
        runButton->setEnabled(false);
        foundTowers.clear();

        // 1. tooling present?
        say("Checking for ModemManager tools...");
        if (!mmcliInstalled()) {
            bad("mmcli not found at /usr/bin/mmcli - the 'modemmanager' package is not installed.");
            finish();
            return;
        }
        good("mmcli present.");

        // 2. is the daemon running?
        int rc = 0;
        runCmd("pgrep -x ModemManager >/dev/null 2>&1", 5000, &rc);
        if (rc != 0) {
            warnLine("ModemManager is not running - trying to start it.");
            runPriv("service", {"ModemManager", "start"}, 20000, &rc);
            if (rc != 0)
                runPriv("service", {"modemmanager", "start"}, 20000, &rc);

            runCmd("pgrep -x ModemManager >/dev/null 2>&1", 5000, &rc);
            if (rc != 0) {
                bad("ModemManager could not be started. Mobile data will not work until it is running.");
                finish();
                return;
            }
        }
        good("ModemManager is running.");

        // 3. ask ModemManager to re-scan the hardware
        say("Scanning for mobile adaptors...");
        runPriv("mmcli", {"-S"}, 30000, &rc);

        int idx = detectModemIndex();
        if (idx < 0) {
            // give the daemon a moment on slow eMMC/USB probe paths
            QTimer t; t.setSingleShot(true);
            QEventLoop l;
            connect(&t, &QTimer::timeout, &l, &QEventLoop::quit);
            t.start(3000); l.exec();
            idx = detectModemIndex();
        }

        if (idx < 0) {
            bad("No mobile adaptor found. Check that the WWAN card is enabled in the BIOS "
                "and that the modem is not blocked (rfkill).");
            say(runCmd("rfkill list 2>&1", 5000));
            finish();
            return;
        }
        good(QString("Mobile adaptor found at modem index %1.").arg(idx));

        QString status = modemStatus();
        QString model  = mmcliField(status, "model");
        QString manu   = mmcliField(status, "manufacturer");
        if (!manu.isEmpty() || !model.isEmpty())
            say("Adaptor: " + manu + " " + model);

        // 4. SIM
        say("Checking SIM...");
        QString simPath = mmcliSimPath(status);
        QString state   = mmcliField(status, "state");

        if (simPath.isEmpty() || simPath == "--" || simPath.contains("none")) {
            bad("No SIM detected. Insert a SIM and run setup again.");
            finish();
            return;
        }
        good("SIM detected: " + simPath);

        if (state.startsWith("locked")) {
            QString lock = mmcliField(status, "lock");
            bad("SIM is locked (" + lock + "). Enter the PIN in APN Settings, then run setup again.");
            finish();
            return;
        }

        QString imsiOut = runPriv("mmcli", {"-i", simPath}, 15000, &rc);
        QString simOp = mmcliField(imsiOut, "operator name");
        if (!simOp.isEmpty() && simOp != "--")
            say("SIM operator: " + simOp);

        if (state.startsWith("failed")) {
            QString reason = modemFailReason(status);
            warnLine("The modem is in a failed state: " + reason);

            // SIM FAILURE FIX - DO NOT REMOVE
            // A reset cannot conjure a SIM. Resetting on sim-missing only
            // dropped the modem off the bus and produced a misleading
            // "modem did not come back" error on top of the real problem.
            // SIM reasons are reported and the run stops here.
            if (reason.contains("sim", Qt::CaseInsensitive)) {
                bad("No usable SIM. ModemManager reports: " + reason);
                bad("Insert a SIM, or check it is seated the right way round and "
                    "that it is in the SIM tray rather than the microSD slot. "
                    "Then run Setup again.");
                say("Nothing else can be configured until the modem sees a SIM.");
                finish();
                return;
            }

            // Non-SIM failure: a reset is worth one attempt. The user has no
            // terminal, so it has to happen here.
            say("Resetting the modem...");
            runPriv("mmcli", {"-m", QString::number(idx), "--reset"}, 30000, &rc);

            // The modem drops off the bus and re-enumerates on a new object
            // path. That takes far longer than a single short sleep, so poll
            // for it rather than checking once.
            say("Waiting for the modem to come back (up to 45 seconds)...");
            idx = -1;
            for (int waited = 0; waited < 45000 && idx < 0; waited += 3000) {
                uiSleep(3000);
                idx = detectModemIndex();
            }

            if (idx < 0) {
                bad("The modem did not come back after the reset. Power the tablet "
                    "off fully (not suspend) and try again.");
                say(runCmd("dmesg | tail -n 30 2>&1", 8000));
                finish();
                return;
            }

            good(QString("Modem is back at index %1.").arg(idx));
            status = modemStatus();
            state  = mmcliField(status, "state");
            say("State after reset: " + state);

            if (state.startsWith("failed")) {
                QString r2 = modemFailReason(status);
                bad("Still failed after a reset: " + r2 + ".");
                if (r2.contains("sim", Qt::CaseInsensitive))
                    bad("That reason points at the SIM. Check the SIM is seated the "
                        "right way round in the tray, and that the tray is the SIM "
                        "slot and not the microSD slot.");
                else
                    bad("This is a firmware or hardware fault in the WWAN card, not a "
                        "settings problem. The card may need its firmware package "
                        "installed, or re-seating.");
                say(runCmd("dmesg | tail -n 30 2>&1", 8000));
                finish();
                return;
            }

            good("Reset cleared the failed state.");
        }

        // 5. enable the modem
        //
        // WRONG STATE FIX - DO NOT REMOVE
        // ModemManager rejects an operation that does not suit the modem's
        // current state with Core.Error.WrongState. Two things caused it here:
        // --enable being issued while the modem was already enabled, and
        // --3gpp-register-home / --3gpp-scan being issued in the gap after
        // --enable returned but before the modem had actually finished
        // enabling. Every state changing call below is now gated on the real
        // state, and the code waits for the transition rather than assuming it.
        say("Enabling modem (state: " + state + ")...");

        if (state.startsWith("enabled") || state.startsWith("searching") ||
            state.startsWith("registered") || state.startsWith("connected")) {
            good("Modem is already enabled - skipping.");
        } else {
            QString out = runPriv("mmcli", {"-m", QString::number(idx), "--enable"}, 30000, &rc);
            if (rc != 0 && !out.contains("successfully", Qt::CaseInsensitive)) {
                if (out.contains("WrongState", Qt::CaseInsensitive))
                    warnLine("Modem refused --enable in state '" + state + "'. Continuing.");
                else
                    warnLine("Enable returned an error: " + out.trimmed());
            } else {
                good("Enable accepted - waiting for the modem to come up...");
            }
        }

        state = waitForState({"enabled", "searching", "registered", "connected"}, 20000);
        say("Modem state is now: " + state);

        if (state.startsWith("failed")) {
            bad("The modem failed while enabling: " + modemFailReason(modemStatus()));
            finish();
            return;
        }

        if (state.startsWith("disabled") || state.startsWith("enabling")) {
            bad("The modem never reached the enabled state (stuck at '" + state +
                "'). Registration and scanning cannot run from here.");
            finish();
            return;
        }

        // 6. automatic network selection (home network / automatic registration)
        QString out;
        if (state.startsWith("registered") || state.startsWith("connected")) {
            good("Already registered - automatic selection not needed.");
        } else {
            say("Selecting network automatically...");
            out = runPriv("mmcli", {"-m", QString::number(idx), "--3gpp-register-home"}, 60000, &rc);
            if (rc != 0 && !out.contains("successfully", Qt::CaseInsensitive)) {
                if (out.contains("WrongState", Qt::CaseInsensitive))
                    warnLine("Modem is not ready for registration yet (state '" + state +
                             "'). It will normally register on its own once it finds a network.");
                else
                    warnLine("Automatic registration returned an error: " + out.trimmed());
            } else {
                good("Automatic network selection requested.");
            }
            state = waitForState({"registered", "connected"}, 30000);
        }

        status = modemStatus();
        QString carrier = mmcliField(status, "operator name");
        if (!carrier.isEmpty() && carrier != "--")
            good("Registered on: " + carrier);
        else
            warnLine("Not registered on any network yet - signal may be weak, or the "
                     "SIM may not be provisioned for this network.");

        // 7. tower scan (slow - this is why it is behind a button)
        //
        // A manual scan is only legal once the modem is enabled, and many
        // modems refuse it outright while a data session is up.
        if (state.startsWith("connected")) {
            warnLine("Skipping the tower scan: the modem has an active data connection "
                     "and most modems refuse a manual scan in that state. Turn mobile "
                     "data off first if you need the tower list.");
            ok = true;
            say("Setup finished.");
            finish();
            return;
        }

        say("Scanning for towers - this can take up to 2 minutes...");
        out = runPriv("mmcli",
                      {"--timeout=180", "-m", QString::number(idx), "--3gpp-scan"},
                      190000, &rc);

        foundTowers = parseScan(out);
        if (foundTowers.isEmpty()) {
            if (out.contains("WrongState", Qt::CaseInsensitive))
                warnLine("The modem refused the scan in state '" + modemState() + "'.");
            else if (out.contains("Unsupported", Qt::CaseInsensitive))
                warnLine("This modem does not support scanning for towers.");
            else
                warnLine("No towers reported: " + out.trimmed());
        } else {
            good(QString("%1 network(s) found:").arg(foundTowers.size()));
            for (const QString &t : foundTowers)
                say("   " + t);
        }

        ok = true;
        say("Setup finished. Set your APN in APN Settings if mobile data does not connect.");
        finish();
    }

    void finish()
    {
        spinnerStop();
        g_spinTick = nullptr;
        running = false;
        runButton->setEnabled(true);
        runButton->setText("Run again");
    }
};

// ---------------------------------------------------------
// Mobile data settings dialog (APN / username / password / PIN)
// ---------------------------------------------------------

class MobileSettingsDialog : public QDialog
{
public:
    explicit MobileSettingsDialog(QWidget *parent = nullptr)
        : QDialog(parent)
    {
        setWindowTitle("Mobile data settings");
        setModal(true);
        styleDialog(this);

        conf = loadMobileConf();
        readMobileProfile(conf);   // live profile wins if it exists

        QVBoxLayout *outer = new QVBoxLayout(this);
        outer->setContentsMargins(30, 30, 30, 30);
        outer->setSpacing(20);

        QLabel *t = new QLabel("APN Settings", this);
        t->setStyleSheet("font-size:34px; color:white; font-weight:bold;");
        t->setAlignment(Qt::AlignCenter);
        outer->addWidget(t);

        QScrollArea *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

        QWidget *body = new QWidget(scroll);
        body->setStyleSheet("QWidget { background:#282828; font-family:'DejaVu Sans'; }");
        QVBoxLayout *v = new QVBoxLayout(body);
        v->setContentsMargins(10, 10, 10, 10);
        v->setSpacing(16);

        v->addWidget(mkLabel("APN"));
        apnEdit = new QLineEdit(conf.apn, body);
        apnEdit->setPlaceholderText("e.g. everywhere");
        v->addWidget(apnEdit);

        autoApnButton = twoLineBtnBT("Get APN\nautomatically");
        v->addWidget(autoApnButton, 0, Qt::AlignLeft);

        v->addWidget(mkLabel("Username (optional)"));
        userEdit = new QLineEdit(conf.user, body);
        v->addWidget(userEdit);

        v->addWidget(mkLabel("Password (optional)"));
        QHBoxLayout *pwRow = new QHBoxLayout();
        pwRow->setSpacing(16);
        passEdit = new QLineEdit(body);
        passEdit->setEchoMode(QLineEdit::Password);
        passEdit->setPlaceholderText("leave blank to keep current");
        showPassButton = smallBtnBT("Show");
        pwRow->addWidget(passEdit, 1);
        pwRow->addWidget(showPassButton, 0);
        v->addLayout(pwRow);

        v->addWidget(mkLabel("Authentication"));
        authBox = new QComboBox(body);
        authBox->addItem("Automatic", "auto");
        authBox->addItem("PAP", "pap");
        authBox->addItem("CHAP", "chap");
        authBox->setCurrentIndex(qMax(0, authBox->findData(conf.auth)));
        v->addWidget(authBox);

        v->addWidget(mkLabel("IP type"));
        ipBox = new QComboBox(body);
        ipBox->addItem("IPv4 and IPv6", "ipv4v6");
        ipBox->addItem("IPv4 only", "ipv4");
        ipBox->addItem("IPv6 only", "ipv6");
        ipBox->setCurrentIndex(qMax(0, ipBox->findData(conf.iptype)));
        v->addWidget(ipBox);

        v->addWidget(mkLabel("Data roaming"));
        roamButton = smallBtnBT(conf.roaming ? "Allowed" : "Blocked");
        roamButton->setFixedWidth(260);
        v->addWidget(roamButton, 0, Qt::AlignLeft);

        v->addWidget(mkLabel("SIM PIN (only if the SIM is locked)"));
        QHBoxLayout *pinRow = new QHBoxLayout();
        pinRow->setSpacing(16);
        pinEdit = new QLineEdit(body);
        pinEdit->setEchoMode(QLineEdit::Password);
        pinEdit->setPlaceholderText("4-8 digits");
        unlockButton = smallBtnBT("Unlock");
        pinRow->addWidget(pinEdit, 1);
        pinRow->addWidget(unlockButton, 0);
        v->addLayout(pinRow);


        resultLabel = new QLabel("", body);
        resultLabel->setWordWrap(true);
        resultLabel->setStyleSheet("font-size:24px; color:#FFC066;");
        v->addWidget(resultLabel);

        v->addStretch();
        scroll->setWidget(body);
        outer->addWidget(scroll, 1);

        QHBoxLayout *btns = new QHBoxLayout();
        btns->setSpacing(20);
        saveButton    = smallBtnBT("Save");
        connectButton = smallBtnBT("Connect");
        cancelButton  = smallBtnBT("Cancel");
        btns->addWidget(saveButton);
        btns->addWidget(connectButton);
        btns->addWidget(cancelButton);
        outer->addLayout(btns);

        connect(showPassButton, &QPushButton::clicked, this, [this]() {
            bool hidden = (passEdit->echoMode() == QLineEdit::Password);
            passEdit->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
            showPassButton->setText(hidden ? "Hide" : "Show");
        });

        connect(roamButton, &QPushButton::clicked, this, [this]() {
            conf.roaming = !conf.roaming;
            roamButton->setText(conf.roaming ? "Allowed" : "Blocked");
        });

        connect(autoApnButton, &QPushButton::clicked, this, [this]() { autoFillApn(true); });
        connect(unlockButton, &QPushButton::clicked, this, &MobileSettingsDialog::unlockSim);
        connect(saveButton, &QPushButton::clicked, this, [this]() { apply(false); });
        connect(connectButton, &QPushButton::clicked, this, [this]() { apply(true); });
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

        if (parent) resize(parent->width() - 120, parent->height() - 120);
        else resize(900, 800);

        // Deferred so the dialog is painted first: the lookup runs mmcli and
        // would otherwise happen behind an unshown window with no spinner.
        QTimer::singleShot(50, this, [this]() {
            if (apnEdit->text().trimmed().isEmpty())
                autoFillApn(false);
        });
    }

    // The spin hook captures `this`; a stale lambda called from a later
    // runProc would dereference a dead dialog.
    ~MobileSettingsDialog() override { g_spinTick = nullptr; }

private:
    MobileConf conf;

    QLineEdit *apnEdit = nullptr;
    QLineEdit *userEdit = nullptr;
    QLineEdit *passEdit = nullptr;
    QLineEdit *pinEdit = nullptr;
    QComboBox *authBox = nullptr;
    QComboBox *ipBox = nullptr;
    QPushButton *roamButton = nullptr;
    QPushButton *showPassButton = nullptr;
    QPushButton *unlockButton = nullptr;
    QPushButton *saveButton = nullptr;
    QPushButton *connectButton = nullptr;
    QPushButton *cancelButton = nullptr;
    QPushButton *autoApnButton = nullptr;
    QLabel *resultLabel = nullptr;

    QString busyMsg;
    int spinFrame = 0;

    QLabel* mkLabel(const QString &txt)
    {
        QLabel *l = new QLabel(txt, this);
        l->setStyleSheet("font-size:24px; color:#ffffff; background:transparent;");
        return l;
    }

    void showError(const QString &msg)
    {
        clearBusy();
        resultLabel->setStyleSheet("font-size:24px; color:#FF5555; font-weight:bold;");
        resultLabel->setText("ERROR: " + msg);
    }

    void showOk(const QString &msg)
    {
        clearBusy();
        resultLabel->setStyleSheet("font-size:24px; color:#7CFC00;");
        resultLabel->setText(msg);
    }

    // Same spinner as the setup log and wifi.cpp's Refresh button, on the
    // line underneath the message.
    void showBusy(const QString &msg)
    {
        busyMsg = msg;
        spinFrame = 0;
        resultLabel->setStyleSheet("font-size:24px; color:#66CCFF;");
        resultLabel->setText(msg + "\n" + QString::fromUtf8(SPIN_FRAMES[0]));
        g_spinTick = [this]() {
            spinFrame = (spinFrame + 1) % SPIN_COUNT;
            resultLabel->setText(busyMsg + "\n" + QString::fromUtf8(SPIN_FRAMES[spinFrame]));
        };
        QApplication::processEvents();
    }

    void clearBusy()
    {
        g_spinTick = nullptr;
        busyMsg.clear();
    }

    // Fills the fields from the local provider database, keyed on the SIM's
    // MCC/MNC. announce=false is the silent pass done when the dialog opens
    // with no APN set yet.
    void autoFillApn(bool announce)
    {
        QString opName;
        showBusy("Loading APN settings for this SIM...");
        QString opId = getOperatorId(&opName);

        if (opId.isEmpty()) {
            clearBusy();
            if (announce)
                showError("No operator id available from the SIM. The modem must be "
                          "on and a SIM present before the APN can be looked up.");
            return;
        }

        QString mcc, mnc;
        if (!splitOperatorId(opId, mcc, mnc)) {
            clearBusy();
            if (announce)
                showError("Could not read the network code from the SIM (got '" + opId + "').");
            return;
        }

        QString provider, err;
        QList<ApnEntry> list = lookupApns(mcc, mnc, &provider, &err);

        // The database is a package, so if it is missing it can be fetched
        // over WiFi. This is the only part of the lookup that touches the
        // network, and it is not needed at all on a normal install.
        if (list.isEmpty() && err == "missing-db") {
            QString linkType;
            QString link = onlineNonMobileDevice(&linkType);

            if (link.isEmpty()) {
                clearBusy();
                showError("The provider database is not installed, and there is no "
                          "WiFi or Ethernet connection to download it over. Connect "
                          "to WiFi first, or enter the APN by hand - your operator "
                          "publishes it.");
                return;
            }

            showBusy("Downloading the provider database over " + linkType +
                     " (" + link + ")...");

            int rc = 0;
            runPriv("apt-get", {"install", "-y", "mobile-broadband-provider-info"}, 180000, &rc);

            if (rc != 0) {
                // Stale package lists are the usual cause on a fresh install.
                showBusy("Refreshing package lists over " + linkType + "...");
                runPriv("apt-get", {"update"}, 180000, &rc);
                showBusy("Downloading the provider database over " + linkType + "...");
                runPriv("apt-get", {"install", "-y", "mobile-broadband-provider-info"},
                        180000, &rc);
            }

            err.clear();
            list = lookupApns(mcc, mnc, &provider, &err);

            if (list.isEmpty() && err == "missing-db") {
                clearBusy();
                showError("The provider database could not be downloaded over " +
                          linkType + ". Enter the APN by hand - your operator "
                          "publishes it.");
                return;
            }
        }

        clearBusy();

        if (list.isEmpty()) {
            if (!err.isEmpty() && err != "missing-db") {
                showError(err);
            } else if (announce) {
                showError("No APN on file for " +
                          (opName.isEmpty() ? ("network " + opId) : opName) +
                          " (MCC " + mcc + ", MNC " + mnc + "). Enter it by hand.");
            }
            return;
        }

        const ApnEntry &e = list.first();
        apnEdit->setText(e.apn);
        userEdit->setText(e.user);
        if (!e.pass.isEmpty())
            passEdit->setText(e.pass);

        QString who = provider.isEmpty() ? opName : provider;
        QString msg = "APN set automatically for " + (who.isEmpty() ? opId : who) +
                      ": " + e.apn;
        if (list.size() > 1)
            msg += QString("  (%1 profiles on file, first one used)").arg(list.size());
        showOk(msg);
    }

    void unlockSim()
    {
        QString pin = pinEdit->text().trimmed();
        if (pin.isEmpty()) {
            showError("Enter the SIM PIN first.");
            return;
        }

        int idx = modemIndex();
        if (idx < 0) {
            showError("No mobile adaptor found. Run Setup first.");
            return;
        }

        showBusy("Unlocking SIM...");
        int rc = 0;
        QString out = runPriv("mmcli",
                              {"-m", QString::number(idx), "--pin=" + pin},
                              20000, &rc);

        if (rc != 0 && !out.contains("successfully", Qt::CaseInsensitive))
            showError("SIM unlock failed: " + out.trimmed());
        else
            showOk("SIM unlocked.");
    }

    void apply(bool alsoConnect)
    {
        conf.apn    = apnEdit->text().trimmed();
        conf.user   = userEdit->text().trimmed();
        conf.auth   = authBox->currentData().toString();
        conf.iptype = ipBox->currentData().toString();

        if (conf.apn.isEmpty()) {
            showError("APN cannot be empty. Your network operator supplies this value.");
            return;
        }

        saveMobileConf(conf);

        showBusy("Saving mobile profile...");
        int rc = 0;
        QString log = applyMobileProfile(conf, passEdit->text(), &rc);
        if (rc != 0) {
            showError("Could not write the mobile profile:\n" + log.trimmed());
            return;
        }

        if (!alsoConnect) {
            showOk("Settings saved.");
            return;
        }

        showBusy("Connecting...");

        log = bringUpMobileProfile(&rc);
        if (rc != 0) {
            showError("Connect failed:\n" + log.trimmed());
            return;
        }

        showOk("Connected.");
        accept();
    }
};

// ---------------------------------------------------------
// MobilePage widget
// ---------------------------------------------------------

class MobilePage : public QWidget
{
public:
    explicit MobilePage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), stackedWidget(stack)
    {
        // -------------------------------------------------
        // GLOBAL FONT FIX — identical to WiFi/Bluetooth
        // -------------------------------------------------
        // GREY NUMBERS FIX - DO NOT REMOVE
        // The family here must be DejaVu Sans, as wifi.cpp uses. "Noto Sans"
        // resolved to a face with no digit glyphs on the target hardware, so
        // fontconfig substituted a different font for 0-9 only. The
        // substituted face renders its own colour and ignores the stylesheet,
        // which is why letters were white and every number came out grey.
        QFont f;
        f.setFamily("DejaVu Sans");
        f.setPointSize(26);
        setFont(f);   // page-local: do NOT change the app-wide font

        // -------------------------------------------------
        // GREY NUMBERS FIX - DO NOT REMOVE
        // Same method as wifi.cpp: explicit class selectors, no palette
        // manipulation of any kind. Setting a palette from inside a plugin
        // is what caused this bug originally. QLabel gets a transparent
        // background so labels sitting on the #3a3a3a card do not paint the
        // page colour behind themselves, matching how wifi.cpp styles the
        // labels inside its info frame.
        // -------------------------------------------------
        setStyleSheet(
            "QScrollArea { background:#282828; font-family:'DejaVu Sans'; border:none; }"
            "QWidget { background:#282828; font-family:'DejaVu Sans'; }"
            "QLabel { color:white; background:transparent; font-family:'DejaVu Sans'; }"
            "QMessageBox QLabel { color:white; font-family:'DejaVu Sans'; }"
        );

        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(40, 40, 40, 40);
        root->setSpacing(20);
        // No layout-level alignment: the scroll area below takes stretch 1,
        // which keeps the title pinned at the top and the buttons/back pinned
        // at the bottom, with the card content scrolling in between.

        // -------------------------------------------------
        // Title
        // -------------------------------------------------
        QLabel *title = new QLabel("Mobile", this);
        title->setStyleSheet("font-size:42px; color:white; font-weight:bold;");
        title->setAlignment(Qt::AlignCenter);
        root->addWidget(title);

        // -------------------------------------------------
        // Main info card
        // -------------------------------------------------
        QFrame *infoCard = new QFrame(this);
        infoCard->setStyleSheet(
            "QFrame { background:#3a3a3a; border-radius:40px; }"
        );
        infoCard->setMinimumHeight(520);

        QVBoxLayout *infoLayout = new QVBoxLayout(infoCard);
        infoLayout->setContentsMargins(35, 35, 35, 35);
        infoLayout->setSpacing(25);

        QLabel *title1 = new QLabel("Mobile Data information", this);
        title1->setStyleSheet("font-size:28px; color:white;");
        title1->setAlignment(Qt::AlignCenter);
        infoLayout->addWidget(title1);

        QLabel *vlabel = new QLabel("Visible towers:", this);
        vlabel->setStyleSheet("font-size:26px; color:white;");
        infoLayout->addWidget(vlabel);

        visibleTowerContainer = new QWidget(infoCard);
        visibleTowerContainer->setStyleSheet("background:transparent;");
        visibleTowerLayout = new QVBoxLayout(visibleTowerContainer);
        visibleTowerLayout->setContentsMargins(10, 0, 10, 0);
        visibleTowerLayout->setSpacing(10);

        infoLayout->addWidget(visibleTowerContainer);

        simLabel = new QLabel("SIM: ---", this);
        simLabel->setStyleSheet("font-size:26px; color:white;");
        infoLayout->addWidget(simLabel);

        carrierLabel = new QLabel("Carrier: ---", this);
        carrierLabel->setStyleSheet("font-size:26px; color:white;");
        infoLayout->addWidget(carrierLabel);

        timeLabel = new QLabel("Connection time: ---", this);
        timeLabel->setStyleSheet("font-size:26px; color:white;");
        infoLayout->addWidget(timeLabel);

        ipLabel = new QLabel("IP address: ---", this);
        ipLabel->setStyleSheet("font-size:26px; color:white;");
        infoLayout->addWidget(ipLabel);

        // Persistent error line: written on failure, never auto-cleared.
        errorLabel = new QLabel("", this);
        errorLabel->setWordWrap(true);
        errorLabel->setStyleSheet("font-size:24px; color:#FF5555; font-weight:bold;");
        errorLabel->setVisible(false);
        infoLayout->addWidget(errorLabel);

        // -------------------------------------------------
        // Scrollable middle: card content scrolls, title stays pinned
        // -------------------------------------------------
        QScrollArea *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

        QWidget *scrollContainer = new QWidget(scroll);
        scrollContainer->setStyleSheet("background:#282828;");
        QVBoxLayout *scrollLayout = new QVBoxLayout(scrollContainer);
        scrollLayout->setContentsMargins(0, 0, 0, 0);
        scrollLayout->setSpacing(20);

        scrollLayout->addWidget(infoCard);
        scrollLayout->addStretch();

        scroll->setWidget(scrollContainer);
        root->addWidget(scroll, 1);

        // -------------------------------------------------
        // Buttons (two rows so they still fit in portrait)
        // -------------------------------------------------
        QHBoxLayout *btnsTop = new QHBoxLayout();
        btnsTop->setSpacing(40);

        powerButton = smallBtnBT("Off");
        refreshButton = smallBtnBT("Refresh");

        btnsTop->addWidget(powerButton);
        btnsTop->addWidget(refreshButton);
        root->addLayout(btnsTop);

        QHBoxLayout *btnsBottom = new QHBoxLayout();
        btnsBottom->setSpacing(20);

        // 190px each + 2x20 spacing + 80 margins = 690, inside the 720 limit.
        setupButton    = twoLineBtnBT("Setup", 190);
        connectButton  = twoLineBtnBT("Connect", 190);
        settingsButton = twoLineBtnBT("APN\nSettings", 190);

        btnsBottom->addWidget(setupButton);
        btnsBottom->addWidget(connectButton);
        btnsBottom->addWidget(settingsButton);
        root->addLayout(btnsBottom);

        // -------------------------------------------------
        // BACK BUTTON PINNED TO BOTTOM (scroll area owns the stretch)
        // -------------------------------------------------
        QPushButton *backButton = new QPushButton("❮", this);
        backButton->setFixedSize(140, 60);
        backButton->setStyleSheet(
            "QPushButton { background:#444444; color:white; border:1px solid #222222; "
            "border-radius:16px; font-size:34px; font-family:'DejaVu Sans'; } "
            "QPushButton:hover { background:#555555; }"
            "QPushButton:pressed { background:#333333; }"
        );

        QHBoxLayout *backWrap = new QHBoxLayout();
        backWrap->addWidget(backButton, 0, Qt::AlignHCenter);
        root->addLayout(backWrap);

        // -------------------------------------------------
        // SIGNALS
        // -------------------------------------------------
        connect(powerButton, &QPushButton::clicked, this, &MobilePage::togglePower);
        connect(refreshButton, &QPushButton::clicked, this, &MobilePage::refreshInfo);
        connect(setupButton, &QPushButton::clicked, this, &MobilePage::openSetup);
        connect(connectButton, &QPushButton::clicked, this, &MobilePage::connectMobile);
        connect(settingsButton, &QPushButton::clicked, this, &MobilePage::openSettings);

        connect(backButton, &QPushButton::clicked, this, [this]() {
            if (refreshTimer) refreshTimer->stop();
            stackedWidget->setCurrentIndex(0);
        });

        // -------------------------------------------------
        // INITIAL STATE + AUTO REFRESH
        //
        // The periodic refresh only reads cheap modem status. The tower
        // scan is NOT run here: --3gpp-scan takes 30-120s and would block
        // the UI on every tick.
        // -------------------------------------------------
        refreshTimer = new QTimer(this);
        refreshTimer->setInterval(2000);
        connect(refreshTimer, &QTimer::timeout, this, &MobilePage::refreshInfo);
        refreshTimer->start();

        refreshInfo();
    }

private:
    QStackedWidget *stackedWidget;

    QWidget *visibleTowerContainer = nullptr;
    QVBoxLayout *visibleTowerLayout = nullptr;

    QLabel *simLabel = nullptr;
    QLabel *carrierLabel = nullptr;
    QLabel *timeLabel = nullptr;
    QLabel *ipLabel = nullptr;
    QLabel *errorLabel = nullptr;

    QPushButton *powerButton = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *setupButton = nullptr;
    QPushButton *connectButton = nullptr;
    QPushButton *settingsButton = nullptr;
    int connectSpinFrame = 0;

    QTimer *refreshTimer = nullptr;

    bool mobilePowered = false;
    bool busy = false;

    QStringList towerList;

    // ---------------------------------------------------
    // UI Updates
    // ---------------------------------------------------
    void setError(const QString &msg)
    {
        errorLabel->setText("ERROR: " + msg);
        errorLabel->setVisible(true);
    }

    void updatePowerButton()
    {
        if (mobilePowered) {
            powerButton->setText("On");
            powerButton->setStyleSheet(
                "QPushButton { background:#444444; color:#7CFC00; border:1px solid #222222; "
                "border-radius:16px; font-size:26px; font-weight:bold; padding:10px 24px; }"
                "QPushButton:hover { background:#555555; }"
                "QPushButton:pressed { background:#333333; }"
            );
        } else {
            powerButton->setText("Off");
            powerButton->setStyleSheet(
                "QPushButton { background:#444444; color:#CC6666; border:1px solid #222222; "
                "border-radius:16px; font-size:26px; font-weight:bold; padding:10px 24px; }"
                "QPushButton:hover { background:#555555; }"
                "QPushButton:pressed { background:#333333; }"
            );
        }
    }

    void togglePower()
    {
        if (busy) return;
        busy = true;

        bool want = !mobilePowered;

        powerButton->setText(want ? "Turning\non..." : "Turning\noff...");
        powerButton->setEnabled(false);
        QApplication::processEvents();

        QString err = setMobilePowered(want);

        powerButton->setEnabled(true);
        busy = false;

        if (!err.isEmpty())
            setError(err);

        // The button now follows the modem's real state, not the click.
        refreshInfo();
    }

    void connectMobile()
    {
        if (busy) return;

        if (!modemAvailable()) {
            setError("No mobile adaptor found. Run Setup first.");
            return;
        }

        if (!mobileProfileExists()) {
            setError("No mobile profile yet. Open APN Settings and save one first - "
                     "it can fill the APN in for you automatically.");
            return;
        }

        busy = true;
        refreshTimer->stop();

        const QString label = connectButton->text();
        connectButton->setEnabled(false);

        // Spinner on the button itself while the session comes up, the same
        // way wifi.cpp animates its Refresh button.
        connectSpinFrame = 0;
        connectButton->setText(QString::fromUtf8(SPIN_FRAMES[0]));
        g_spinTick = [this]() {
            connectSpinFrame = (connectSpinFrame + 1) % SPIN_COUNT;
            connectButton->setText(QString::fromUtf8(SPIN_FRAMES[connectSpinFrame]));
        };

        // The data session cannot come up on a disabled modem.
        QString err;
        if (!isMobilePowered(modemStatus()))
            err = setMobilePowered(true);

        int rc = 0;
        QString log;
        if (err.isEmpty())
            log = bringUpMobileProfile(&rc);

        g_spinTick = nullptr;
        connectButton->setText(label);
        connectButton->setEnabled(true);
        busy = false;
        refreshTimer->start();

        if (!err.isEmpty()) {
            setError(err);
        } else if (rc != 0) {
            setError(log.trimmed().isEmpty()
                         ? QString("Connect failed with no output from nmcli.")
                         : log.trimmed());
        } else {
            errorLabel->setVisible(false);   // a successful connect clears the last failure
        }

        refreshInfo();
    }

    void openSetup()
    {
        if (busy) return;
        busy = true;
        refreshTimer->stop();

        MobileSetupDialog dlg(window());
        dlg.exec();

        if (!dlg.towers().isEmpty())
            towerList = dlg.towers();

        busy = false;
        refreshTimer->start();
        refreshInfo();
    }

    void openSettings()
    {
        if (busy) return;
        busy = true;
        refreshTimer->stop();

        MobileSettingsDialog dlg(window());
        dlg.exec();

        busy = false;
        refreshTimer->start();
        refreshInfo();
    }

    void setTowerList(const QStringList &items)
    {
        QLayoutItem *child;
        while ((child = visibleTowerLayout->takeAt(0)) != nullptr) {
            if (child->widget())
                child->widget()->deleteLater();
            delete child;
        }

        for (const QString &t : items) {
            QLabel *lbl = new QLabel(" - " + t, visibleTowerContainer);
            lbl->setWordWrap(true);
            lbl->setStyleSheet("color:white; font-size:24px;");
            visibleTowerLayout->addWidget(lbl);
        }
    }

    void refreshInfo()
    {
        if (busy) return;
        busy = true;

        QString status = modemStatus();

        mobilePowered = isMobilePowered(status);
        updatePowerButton();

        if (!modemAvailable()) {
            setTowerList({"No modem detected"});
            simLabel->setText("SIM: no modem");
            carrierLabel->setText("No modem detected");
            timeLabel->setText("Connection time: N/A");
            ipLabel->setText("IP address: -");
            if (!mmcliInstalled())
                setError("mmcli is missing - the 'modemmanager' package is not installed.");
            busy = false;
            return;
        }

        if (towerList.isEmpty())
            setTowerList({"Press Setup to search for towers"});
        else
            setTowerList(towerList);

        simLabel->setText(getSimSummary(status));
        carrierLabel->setText(getCurrentCarrier(status));
        timeLabel->setText(getConnectionTime(status));

        // Only query the address when there is a data session: getMobileIP()
        // costs two nmcli calls and this runs every two seconds.
        if (mmcliField(status, "state").startsWith("connected")) {
            QString ip = getMobileIP();
            ipLabel->setText("IP address: " + (ip.isEmpty() ? QString("-") : ip));
        } else {
            ipLabel->setText("IP address: -");
        }

        busy = false;
    }
};

// ---------------------------------------------------------
// Plugin Factory
// ---------------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new MobilePage(stack);
}
