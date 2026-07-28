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

// ---------------------------------------------------------
// Constants
// ---------------------------------------------------------

static const char *NM_CON_NAME = "Alternix Mobile";

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
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, [&]() {
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
    return QString::fromUtf8(p.readAll());
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
static QString mmcliSimPath(const QString &out)
{
    QString p = mmcliField(out, "primary sim path");
    if (p.isEmpty()) p = mmcliField(out, "sim path");
    if (p.isEmpty()) p = mmcliField(out, "path");
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

static void setMobilePowered(bool on)
{
    if (!modemAvailable()) return;

    runPriv("mmcli",
            {QString("-m"), QString::number(g_modemIndex),
             on ? QString("--enable") : QString("--disable")},
            20000);
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
    d->setStyleSheet(
        "QDialog { background:#282828; }"
        "QLabel { color:white; font-size:26px; }"
        "QLineEdit { background:#3a3a3a; color:white; border:1px solid #222222; "
        "  border-radius:12px; font-size:26px; padding:10px 14px; min-height:44px; }"
        "QComboBox { background:#3a3a3a; color:white; border:1px solid #222222; "
        "  border-radius:12px; font-size:26px; padding:10px 14px; min-height:44px; }"
        "QComboBox QAbstractItemView { background:#3a3a3a; color:white; "
        "  selection-background-color:#555555; font-size:26px; }"
        "QTextEdit { background:#1e1e1e; color:white; border:1px solid #222222; "
        "  border-radius:16px; font-size:22px; }"
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

    QStringList towers() const { return foundTowers; }
    bool succeeded() const { return ok; }

private:
    QTextEdit *logView = nullptr;
    QPushButton *runButton = nullptr;
    QPushButton *closeButton = nullptr;
    QStringList foundTowers;
    bool ok = false;
    bool running = false;

    void say(const QString &s)      { logView->append("<span style='color:#dddddd;'>" + s.toHtmlEscaped() + "</span>"); pump(); }
    void good(const QString &s)     { logView->append("<span style='color:#7CFC00;'>OK: " + s.toHtmlEscaped() + "</span>"); pump(); }
    void bad(const QString &s)      { logView->append("<span style='color:#FF5555; font-weight:bold;'>ERROR: " + s.toHtmlEscaped() + "</span>"); pump(); }
    void warnLine(const QString &s) { logView->append("<span style='color:#FFC066;'>WARNING: " + s.toHtmlEscaped() + "</span>"); pump(); }

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
            bad("SIM is locked (" + lock + "). Enter the PIN in Data Settings, then run setup again.");
            finish();
            return;
        }

        QString imsiOut = runPriv("mmcli", {"-i", simPath}, 15000, &rc);
        QString simOp = mmcliField(imsiOut, "operator name");
        if (!simOp.isEmpty() && simOp != "--")
            say("SIM operator: " + simOp);

        // 5. enable the modem
        say("Enabling modem...");
        QString out = runPriv("mmcli", {"-m", QString::number(idx), "--enable"}, 30000, &rc);
        if (rc != 0 && !out.contains("successfully", Qt::CaseInsensitive))
            warnLine("Enable returned an error: " + out.trimmed());
        else
            good("Modem enabled.");

        // 6. automatic network selection (home network / automatic registration)
        say("Selecting network automatically...");
        out = runPriv("mmcli", {"-m", QString::number(idx), "--3gpp-register-home"}, 60000, &rc);
        if (rc != 0 && !out.contains("successfully", Qt::CaseInsensitive))
            warnLine("Automatic registration returned an error: " + out.trimmed());
        else
            good("Automatic network selection requested.");

        status = modemStatus();
        QString carrier = mmcliField(status, "operator name");
        if (!carrier.isEmpty() && carrier != "--")
            good("Registered on: " + carrier);
        else
            warnLine("Not registered on any network yet - the scan below may take a minute.");

        // 7. tower scan (slow - this is why it is behind a button)
        say("Scanning for towers - this can take up to 2 minutes...");
        out = runPriv("mmcli",
                      {"--timeout=180", "-m", QString::number(idx), "--3gpp-scan"},
                      190000, &rc);

        foundTowers = parseScan(out);
        if (foundTowers.isEmpty()) {
            warnLine("No towers reported. Some modems refuse a manual scan while connected.");
        } else {
            good(QString("%1 network(s) found:").arg(foundTowers.size()));
            for (const QString &t : foundTowers)
                say("   " + t);
        }

        ok = true;
        say("Setup finished. Set your APN in Data Settings if mobile data does not connect.");
        finish();
    }

    void finish()
    {
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

        QLabel *t = new QLabel("Mobile data settings", this);
        t->setStyleSheet("font-size:34px; color:white; font-weight:bold;");
        t->setAlignment(Qt::AlignCenter);
        outer->addWidget(t);

        QScrollArea *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

        QWidget *body = new QWidget(scroll);
        body->setStyleSheet("background:#282828;");
        QVBoxLayout *v = new QVBoxLayout(body);
        v->setContentsMargins(10, 10, 10, 10);
        v->setSpacing(16);

        v->addWidget(mkLabel("APN"));
        apnEdit = new QLineEdit(conf.apn, body);
        apnEdit->setPlaceholderText("e.g. everywhere");
        v->addWidget(apnEdit);

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

        connect(unlockButton, &QPushButton::clicked, this, &MobileSettingsDialog::unlockSim);
        connect(saveButton, &QPushButton::clicked, this, [this]() { apply(false); });
        connect(connectButton, &QPushButton::clicked, this, [this]() { apply(true); });
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

        if (parent) resize(parent->width() - 120, parent->height() - 120);
        else resize(900, 800);
    }

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
    QLabel *resultLabel = nullptr;

    QLabel* mkLabel(const QString &txt)
    {
        QLabel *l = new QLabel(txt, this);
        l->setStyleSheet("font-size:24px; color:#cccccc;");
        return l;
    }

    void showError(const QString &msg)
    {
        resultLabel->setStyleSheet("font-size:24px; color:#FF5555; font-weight:bold;");
        resultLabel->setText("ERROR: " + msg);
    }

    void showOk(const QString &msg)
    {
        resultLabel->setStyleSheet("font-size:24px; color:#7CFC00;");
        resultLabel->setText(msg);
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
            showError("No mobile adaptor found. Run Setup & Scan first.");
            return;
        }

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

        showOk("Connecting...");
        QApplication::processEvents();

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
        QFont f;
        f.setFamily("Noto Sans");
        f.setPointSize(26);
        setFont(f);   // page-local: do NOT change the app-wide font

        setStyleSheet("background:#282828;");

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
        btnsBottom->setSpacing(40);

        setupButton = smallBtnBT("Setup && Scan");
        settingsButton = smallBtnBT("Data Settings");

        btnsBottom->addWidget(setupButton);
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
    QLabel *errorLabel = nullptr;

    QPushButton *powerButton = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *setupButton = nullptr;
    QPushButton *settingsButton = nullptr;

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

        mobilePowered = !mobilePowered;
        setMobilePowered(mobilePowered);

        busy = false;
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
            if (!mmcliInstalled())
                setError("mmcli is missing - the 'modemmanager' package is not installed.");
            busy = false;
            return;
        }

        if (towerList.isEmpty())
            setTowerList({"Press Setup & Scan to search for towers"});
        else
            setTowerList(towerList);

        simLabel->setText(getSimSummary(status));
        carrierLabel->setText(getCurrentCarrier(status));
        timeLabel->setText(getConnectionTime(status));

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
