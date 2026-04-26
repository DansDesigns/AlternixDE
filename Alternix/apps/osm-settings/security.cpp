#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QStackedWidget>
#include <QProcess>
#include <QMessageBox>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QMap>
#include <QTimer>
#include <QTextEdit>
#include <QShowEvent>
#include <QHideEvent>
#include <QTextCursor>

// -----------------------------------------------------
// Alternix compact button style (same as Emulation/Storage)
// -----------------------------------------------------
static QString altBtnStyle(const QString &txtColor)
{
    return QString(
        "QPushButton {"
        " background:#444444;"
        " color:%1;"
        " border:1px solid #222222;"
        " border-radius:16px;"
        " font-size:22px;"
        " font-weight:bold;"
        " padding:6px 16px;"
        "}"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
    ).arg(txtColor);
}

static QPushButton* makeBtn(const QString &txt, const QString &color = "white")
{
    QPushButton *b = new QPushButton(txt);
    b->setStyleSheet(altBtnStyle(color));
    b->setMinimumSize(140, 54);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

// -----------------------------------------------------
// Simple command helpers
// -----------------------------------------------------
static QString runCmd(const QString &cmd)
{
    QProcess p;
    p.start("/bin/sh", {"-c", cmd});
    p.waitForFinished();
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

static bool runCmdOk(const QString &cmd, QString &output)
{
    QProcess p;
    p.start("/bin/sh", {"-c", cmd});
    if (!p.waitForFinished())
        return false;

    output = QString::fromLocal8Bit(p.readAllStandardOutput()) +
             QString::fromLocal8Bit(p.readAllStandardError());

    return (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
}

// -----------------------------------------------------
// CONFIG LOAD / SAVE
// -----------------------------------------------------
static QString cfgFile()
{
    return QDir::homePath() + "/.config/Alternix/osm-settings.conf";
}

static QMap<QString,QString> loadCfg()
{
    QMap<QString,QString> map;
    QFile f(cfgFile());
    if (!f.exists()) return map;

    if (f.open(QFile::ReadOnly))
    {
        QTextStream s(&f);
        while (!s.atEnd())
        {
            QString line = s.readLine().trimmed();
            if (line.startsWith("#") || !line.contains("="))
                continue;
            QStringList parts = line.split("=");
            if (parts.size() == 2)
                map[parts[0].trimmed()] = parts[1].trimmed();
        }
    }
    return map;
}

static void saveCfg(const QMap<QString,QString> &map)
{
    QFile f(cfgFile());
    f.open(QFile::WriteOnly | QFile::Truncate);
    QTextStream s(&f);
    for (auto it = map.begin(); it != map.end(); ++it)
        s << it.key() << "=" << it.value() << "\n";
}

// -----------------------------------------------------
// Simple ON/OFF pill toggle for SSH / USB
// -----------------------------------------------------
class BoolPillToggle : public QPushButton
{
public:
    explicit BoolPillToggle(bool initial = false, QWidget *parent = nullptr)
        : QPushButton(parent), m_state(initial)
    {
        setFixedSize(140, 50);
        updateStyle();
    }

    void toggle()
    {
        m_state = !m_state;
        updateStyle();
    }

    void setState(bool on)
    {
        m_state = on;
        updateStyle();
    }

    bool isOn() const { return m_state; }

private:
    bool m_state;

    void updateStyle()
    {
        if (m_state) {
            setStyleSheet(
                "QPushButton {"
                " background:#2ecc71;"
                " border-radius:25px;"
                " color:white;"
                " font-size:22px;"
                " padding:4px 16px;"
                "}"
            );
            setText("");
        } else {
            setStyleSheet(
                "QPushButton {"
                " background:#666666;"
                " border-radius:25px;"
                " color:white;"
                " font-size:22px;"
                " padding:4px 16px;"
                "}"
            );
            setText("Disabled");
        }
    }
};

// -----------------------------------------------------
// Firewall multi-state pill (one pill, 4 states)
// -----------------------------------------------------
class FirewallPill : public QPushButton
{
public:
    enum State { Off, Home, Public, Strict };

    explicit FirewallPill(State initial = Off, QWidget *parent = nullptr)
        : QPushButton(parent), m_state(initial)
    {
        setFixedSize(180, 50);
        updateStyle();
    }

    void advance()
    {
        switch (m_state) {
        case Off:    m_state = Home;   break;
        case Home:   m_state = Public; break;
        case Public: m_state = Strict; break;
        case Strict: m_state = Off;    break;
        }
        updateStyle();
    }

    void setState(State st)
    {
        m_state = st;
        updateStyle();
    }

    State state() const { return m_state; }

    static QString stateToString(State s)
    {
        switch (s) {
        case Off:    return "off";
        case Home:   return "home";
        case Public: return "public";
        case Strict: return "strict";
        }
        return "off";
    }

    static State fromString(const QString &s)
    {
        QString t = s.toLower();
        if (t == "home")   return Home;
        if (t == "public") return Public;
        if (t == "strict") return Strict;
        return Off;
    }

private:
    State m_state;

    void updateStyle()
    {
        QString color;
        QString label;

        switch (m_state) {
        case Off:
            color = "#666666";
            label = "Disabled";
            break;
        case Home:
            color = "#2ecc71";
            label = "Home";
            break;
        case Public:
            color = "#f39c12";
            label = "Public";
            break;
        case Strict:
            color = "#CC0000";
            label = "Strict";
            break;
        }

        setStyleSheet(
            QString(
                "QPushButton {"
                " background:%1;"
                " border-radius:25px;"
                " color:white;"
                " font-size:22px;"
                " padding:4px 16px;"
                "}"
            ).arg(color)
        );
        setText(label);
    }
};

// -----------------------------------------------------
// SecurityPage
// -----------------------------------------------------
class SecurityPage : public QWidget
{
public:
    explicit SecurityPage(QStackedWidget *stack);

protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    QStackedWidget *m_stack = nullptr;

    FirewallPill   *m_fwPill   = nullptr;
    BoolPillToggle *m_sshPill  = nullptr;
    BoolPillToggle *m_usbPill  = nullptr;

    QTextEdit *m_liveConnEdit = nullptr;
    QTextEdit *m_ufwLogEdit   = nullptr;

    QTimer *m_logTimer = nullptr;

    QMap<QString,QString> cfg;

    // config helpers
    void writeCfgKey(const QString &k, const QString &v);
    QString readCfg(const QString &k, const QString &def = QString()) const;
    bool readCfgBool(const QString &k, bool def) const;

    // ui builders
    QWidget *makeFirewallRow();
    QWidget *makeSshRow();
    QWidget *makeUsbRow();
    QWidget *makePasswordRow();
    QWidget *makeLogsCardLive();
    QWidget *makeLogsCardUfw();

    // backend
    void applyFirewallProfile(FirewallPill::State st);
    bool readSSH();
    void setSSH(bool on);
    void setUSBLockdown(bool on);

    void refreshLogs();
};

// -----------------------------------------------------
// Constructor
// -----------------------------------------------------
SecurityPage::SecurityPage(QStackedWidget *stack)
    : QWidget(stack), m_stack(stack)
{
    setStyleSheet("background:#282828; color:white; font-family:Sans;");
    cfg = loadCfg();

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(40, 40, 40, 40);
    root->setSpacing(10);

    QLabel *title = new QLabel("Security");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:42px; font-weight:bold;");
    root->addWidget(title);

    // Scroll area
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

    QWidget *wrap = new QWidget(scroll);
    QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
    wrapLay->setSpacing(10);
    wrapLay->setContentsMargins(0, 0, 0, 0);

    QFrame *outer = new QFrame(wrap);
    outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
    QVBoxLayout *outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(50, 30, 50, 30);
    outerLay->setSpacing(30);

    // Build top rows
    outerLay->addWidget(makeFirewallRow());
    outerLay->addWidget(makeSshRow());
    outerLay->addWidget(makeUsbRow());
    outerLay->addWidget(makePasswordRow());

    // Logs
    outerLay->addWidget(makeLogsCardLive());
    outerLay->addWidget(makeLogsCardUfw());

    wrapLay->addWidget(outer);
    wrapLay->addStretch();

    scroll->setWidget(wrap);
    root->addWidget(scroll);

    // Back button
    QPushButton *back = makeBtn("❮");
    back->setFixedSize(140, 60);
    connect(back, &QPushButton::clicked, this, [this]() {
        if (m_stack)
            m_stack->setCurrentIndex(0);
    });
    root->addWidget(back, 0, Qt::AlignCenter);

    // --------------------------
    // Load REAL system SSH state
    // --------------------------
    bool sshOn = readSSH();
    m_sshPill->setState(sshOn);
    writeCfgKey("ssh_enabled", sshOn ? "true" : "false");

    // --------------------------
    // Load firewall state from config
    // --------------------------
    FirewallPill::State fwState =
        FirewallPill::fromString(readCfg("firewall_state", "off"));
    m_fwPill->setState(fwState);

    // --------------------------
    // Load USB state
    // --------------------------
    bool usbOn = readCfgBool("usb_lockdown", false);
    m_usbPill->setState(usbOn);

    // --------------------------
    // Connect pill actions
    // --------------------------
    connect(m_fwPill, &QPushButton::clicked, this, [this]() {
        m_fwPill->advance();
        FirewallPill::State st = m_fwPill->state();
        writeCfgKey("firewall_state", FirewallPill::stateToString(st));
        applyFirewallProfile(st);
    });

    connect(m_sshPill, &QPushButton::clicked, this, [this]() {
        m_sshPill->toggle();
        bool on = m_sshPill->isOn();
        writeCfgKey("ssh_enabled", on ? "true" : "false");
        setSSH(on);
    });

    connect(m_usbPill, &QPushButton::clicked, this, [this]() {
        m_usbPill->toggle();
        bool on = m_usbPill->isOn();
        writeCfgKey("usb_lockdown", on ? "true" : "false");
        setUSBLockdown(on);
    });

    // Auto-refresh logs
    m_logTimer = new QTimer(this);
    m_logTimer->setInterval(3000);
    connect(m_logTimer, &QTimer::timeout, this, [this]() { refreshLogs(); });
}

// -----------------------------------------------------
// show/hide events
// -----------------------------------------------------
void SecurityPage::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (m_logTimer && !m_logTimer->isActive()) {
        refreshLogs();
        m_logTimer->start();
    }
}

void SecurityPage::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    if (m_logTimer && m_logTimer->isActive())
        m_logTimer->stop();
}

// -----------------------------------------------------
// Config helpers
// -----------------------------------------------------
QString SecurityPage::readCfg(const QString &k, const QString &def) const
{
    return cfg.contains(k) ? cfg.value(k) : def;
}

bool SecurityPage::readCfgBool(const QString &k, bool def) const
{
    QString v = readCfg(k, def ? "true" : "false");
    return (v == "true");
}

void SecurityPage::writeCfgKey(const QString &k, const QString &v)
{
    cfg[k] = v;
    saveCfg(cfg);
}

// -----------------------------------------------------
// UI builders
// -----------------------------------------------------
QWidget *SecurityPage::makeFirewallRow()
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

    QHBoxLayout *lay = new QHBoxLayout(card);
    lay->setContentsMargins(30, 20, 30, 20);
    lay->setSpacing(20);

    QLabel *lbl = new QLabel("Firewall");
    lbl->setStyleSheet("font-size:30px; font-weight:bold;");
    lay->addWidget(lbl);

    lay->addStretch();

    m_fwPill = new FirewallPill(FirewallPill::Off, card);
    lay->addWidget(m_fwPill);

    return card;
}

QWidget *SecurityPage::makeSshRow()
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

    QHBoxLayout *lay = new QHBoxLayout(card);
    lay->setContentsMargins(30, 20, 30, 20);
    lay->setSpacing(20);

    QLabel *lbl = new QLabel("SSH");
    lbl->setStyleSheet("font-size:30px; font-weight:bold;");
    lay->addWidget(lbl);

    lay->addStretch();

    m_sshPill = new BoolPillToggle(false, card);
    lay->addWidget(m_sshPill);

    return card;
}

QWidget *SecurityPage::makeUsbRow()
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

    QHBoxLayout *lay = new QHBoxLayout(card);
    lay->setContentsMargins(30, 20, 30, 20);
    lay->setSpacing(20);

    QLabel *lbl = new QLabel("USB Lockdown");
    lbl->setStyleSheet("font-size:30px; font-weight:bold;");
    lay->addWidget(lbl);

    lay->addStretch();

    m_usbPill = new BoolPillToggle(false, card);
    lay->addWidget(m_usbPill);

    return card;
}

QWidget *SecurityPage::makePasswordRow()
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

    QHBoxLayout *lay = new QHBoxLayout(card);
    lay->setContentsMargins(30, 20, 30, 20);
    lay->setSpacing(20);

    QLabel *lbl = new QLabel("Password");
    lbl->setStyleSheet("font-size:30px; font-weight:bold;");
    lay->addWidget(lbl);

    lay->addStretch();

    QPushButton *reset = new QPushButton("Reset");
    reset->setStyleSheet(
        "QPushButton {"
        " background:#CC0000;"
        " color:white;"
        " border-radius:20px;"
        " font-size:24px;"
        " padding:8px 20px;"
        "}"
        "QPushButton:pressed { background:#990000; }"
    );
    reset->setFixedHeight(54);
    lay->addWidget(reset);

    connect(reset,&QPushButton::clicked,this,[this](){
        QProcess::execute("osm-lock", QStringList());

        QFile pwd(QDir::homePath()+"/.config/Alternix/.osm_lockdata");
        if (pwd.exists()) pwd.remove();

        QProcess::startDetached("osm-lock", QStringList());
    });

    return card;
}

// --------------------------
// Network Logs (connections)
// --------------------------
QWidget *SecurityPage::makeLogsCardLive()
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(30, 20, 30, 20);
    lay->setSpacing(10);

    QLabel *lbl = new QLabel("Network Connections");
    lbl->setStyleSheet("font-size:26px; font-weight:bold;");
    lay->addWidget(lbl);

    m_liveConnEdit = new QTextEdit(card);
    m_liveConnEdit->setReadOnly(true);
    m_liveConnEdit->setStyleSheet(
        "QTextEdit {"
        " background:#3a3a3a;"
        " border-radius:20px;"
        " color:white;"
        " font-family:monospace;"
        " font-size:18px;"
        "}"
    );
    m_liveConnEdit->setFixedHeight(220);
    lay->addWidget(m_liveConnEdit);

    return card;
}

// --------------------------
// UFW Log
// --------------------------
QWidget *SecurityPage::makeLogsCardUfw()
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(30, 20, 30, 20);
    lay->setSpacing(10);

    QLabel *lbl = new QLabel("Firewall Log (ufw)");
    lbl->setStyleSheet("font-size:26px; font-weight:bold;");
    lay->addWidget(lbl);

    m_ufwLogEdit = new QTextEdit(card);
    m_ufwLogEdit->setReadOnly(true);
    m_ufwLogEdit->setStyleSheet(
        "QTextEdit {"
        " background:#3a3a3a;"
        " border-radius:20px;"
        " color:white;"
        " font-family:monospace;"
        " font-size:18px;"
        "}"
    );
    m_ufwLogEdit->setFixedHeight(220);
    lay->addWidget(m_ufwLogEdit);

    return card;
}

// -----------------------------------------------------
// Backend
// -----------------------------------------------------
bool SecurityPage::readSSH()
{
    // Devuan uses SysVinit — check via service status output
    QString st1 = runCmd("service ssh status 2>/dev/null");
    QString st2 = runCmd("service sshd status 2>/dev/null");

    return (st1.contains("running") || st2.contains("running"));
}

void SecurityPage::setSSH(bool on)
{
    QString out;
    if (on) {
        // Enable at boot and start now (try both ssh and sshd service names)
        runCmdOk("sudo update-rc.d ssh enable && sudo service ssh start 2>/dev/null || "
                 "sudo update-rc.d sshd enable && sudo service sshd start 2>/dev/null", out);
    } else {
        // Stop now and disable at boot
        runCmdOk("sudo service ssh stop 2>/dev/null && sudo update-rc.d ssh disable 2>/dev/null || "
                 "sudo service sshd stop 2>/dev/null && sudo update-rc.d sshd disable 2>/dev/null", out);
    }
}

void SecurityPage::setUSBLockdown(bool on)
{
    Q_UNUSED(on);
    // placeholder for future real USB lockdown
}

void SecurityPage::applyFirewallProfile(FirewallPill::State st)
{
    QString out;

    switch (st) {
    case FirewallPill::Off:
        runCmdOk("sudo ufw disable", out);
        break;

    case FirewallPill::Home:
        runCmdOk("sudo ufw --force reset", out);
        runCmdOk("sudo ufw default allow incoming", out);
        runCmdOk("sudo ufw allow from 192.168.0.0/16", out);
        runCmdOk("sudo ufw allow ssh", out);
        runCmdOk("sudo ufw enable", out);
        break;

    case FirewallPill::Public:
        runCmdOk("sudo ufw --force reset", out);
        runCmdOk("sudo ufw default deny incoming", out);
        runCmdOk("sudo ufw allow ssh", out);
        runCmdOk("sudo ufw enable", out);
        break;

    case FirewallPill::Strict:
        runCmdOk("sudo ufw --force reset", out);
        runCmdOk("sudo ufw default deny incoming", out);
        runCmdOk("sudo ufw deny ssh", out);
        runCmdOk("sudo ufw deny from 192.168.0.0/16", out);
        runCmdOk("sudo ufw enable", out);
        break;
    }
}

// -----------------------------------------------------
// Logs
// -----------------------------------------------------
void SecurityPage::refreshLogs()
{
    if (m_liveConnEdit) {
        QString txt = runCmd("ss -tupn | grep -v LISTEN 2>/dev/null");
        if (txt.isEmpty())
            txt = "No active non-listening TCP connections.";
        m_liveConnEdit->setPlainText(txt);
        m_liveConnEdit->moveCursor(QTextCursor::Start);
    }

    if (m_ufwLogEdit) {
        QString txt = runCmd("tail -n 200 /var/log/ufw.log 2>/dev/null");
        if (txt.isEmpty())
            txt = "No ufw log entries found or /var/log/ufw.log is missing.";
        m_ufwLogEdit->setPlainText(txt);
        m_ufwLogEdit->moveCursor(QTextCursor::End);
    }
}

// -----------------------------------------------------
// Factory
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new SecurityPage(stack);
}
