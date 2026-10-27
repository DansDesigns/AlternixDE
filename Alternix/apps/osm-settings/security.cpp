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
#include <QFileInfo>
#include <QSaveFile>
#include <QDir>
#include <QTextStream>
#include <QTimer>
#include <QTextEdit>
#include <QShowEvent>
#include <QHideEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <QStringList>

#include <functional>
#include <memory>

// Security settings for AlternixOS (Devuan / SysVinit).
// Privileged operations go through the installed osm-sudo wrapper, which
// authenticates with osm-lock and then runs sudo non-interactively.

static QString altBtnStyle(const QString &txtColor)
{
    return QString(
        "QPushButton { background:#444444; color:%1; border:1px solid #222222;"
        " border-radius:16px; font-size:22px; font-weight:bold; padding:6px 16px; }"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
    ).arg(txtColor);
}

static QPushButton *makeBtn(const QString &txt, const QString &color = "white")
{
    QPushButton *b = new QPushButton(txt);
    b->setStyleSheet(altBtnStyle(color));
    b->setMinimumSize(140, 54);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

static QString cfgFile()
{
    return QDir::homePath() + "/.config/Alternix/osm-settings.conf";
}

// Only update our own key, leaving all other settings, comments and ordering
// intact; do not rewrite an out-of-date QMap over another page's changes.
static bool writeConfigKey(const QString &key, const QString &value, QString &error)
{
    const QString path = cfgFile();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = "Cannot create the Alternix settings directory.";
        return false;
    }

    QStringList lines;
    QFile previous(path);
    if (previous.exists()) {
        if (!previous.open(QIODevice::ReadOnly | QIODevice::Text)) {
            error = previous.errorString();
            return false;
        }
        QTextStream stream(&previous);
        while (!stream.atEnd())
            lines << stream.readLine();
    }

    bool found = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines[i].trimmed();
        if (trimmed.startsWith('#') || !trimmed.contains('='))
            continue;
        if (trimmed.section('=', 0, 0).trimmed() == key) {
            if (!found) {
                lines[i] = key + "=" + value;
                found = true;
            } else {
                // Remove duplicate versions of this *one* key only.
                lines.removeAt(i--);
            }
        }
    }
    if (!found)
        lines << key + "=" + value;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = file.errorString();
        return false;
    }
    QTextStream out(&file);
    for (const QString &line : lines)
        out << line << '\n';
    out.flush();
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

static QString readConfigKey(const QString &key, const QString &fallback = QString())
{
    QFile file(cfgFile());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return fallback;
    QString result = fallback;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.startsWith('#') && line.contains('=') &&
            line.section('=', 0, 0).trimmed() == key)
            result = line.mid(line.indexOf('=') + 1).trimmed();
    }
    return result;
}

struct CommandResult {
    bool ok = false;
    QString text;
};

// For short, unprivileged status reads only. Never run an interactive
// authentication command synchronously on the Qt UI thread.
static CommandResult runRead(const QString &program, const QStringList &args,
                             int timeoutMs = 1500)
{
    CommandResult result;
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, args);
    if (!process.waitForStarted(500)) {
        result.text = process.errorString();
        return result;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(200);
        result.text = "Command timed out.";
        return result;
    }
    result.ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    result.text = QString::fromLocal8Bit(process.readAll()).trimmed();
    return result;
}

static void setLogText(QTextEdit *edit, const QString &text)
{
    if (!edit || edit->toPlainText() == text || edit->textCursor().hasSelection())
        return;
    QScrollBar *bar = edit->verticalScrollBar();
    const int oldPosition = bar->value();
    const bool atBottom = oldPosition >= bar->maximum() - 5;
    edit->setPlainText(text);
    bar->setValue(atBottom ? bar->maximum() : oldPosition);
}

class BoolPillToggle : public QPushButton
{
public:
    explicit BoolPillToggle(bool initial = false, QWidget *parent = nullptr)
        : QPushButton(parent), m_state(initial)
    {
        setFixedSize(140, 50);
        updateStyle();
    }

    void setState(bool enabled) { m_state = enabled; updateStyle(); }
    bool isOn() const { return m_state; }

private:
    bool m_state;
    void updateStyle()
    {
        setStyleSheet(QString(
            "QPushButton { background:%1; color:white; border-radius:25px;"
            " font-size:20px; padding:4px 12px; }"
            "QPushButton:pressed { background:#333333; }"
            "QPushButton:disabled { color:#bbbbbb; background:#555555; }"
        ).arg(m_state ? "#2ecc71" : "#666666"));
        setText(m_state ? "Enabled" : "Disabled");
    }
};

class FirewallPill : public QPushButton
{
public:
    enum State { Off, Home, Public, Strict, Custom };
    explicit FirewallPill(QWidget *parent = nullptr) : QPushButton(parent)
    {
        setFixedSize(180, 50);
        setState(Off);
    }
    State state() const { return m_state; }
    State nextState() const
    {
        switch (m_state) {
        case Off: return Home;
        case Home: return Public;
        case Public: return Strict;
        case Strict: return Off;
        case Custom: return Home;
        }
        return Home;
    }
    static QString stateToString(State s)
    {
        switch (s) {
        case Off: return "off";
        case Home: return "home";
        case Public: return "public";
        case Strict: return "strict";
        case Custom: return "custom";
        }
        return "custom";
    }
    static State fromString(const QString &s)
    {
        if (s == "off") return Off;
        if (s == "home") return Home;
        if (s == "public") return Public;
        if (s == "strict") return Strict;
        return Custom;
    }
    void setState(State state)
    {
        m_state = state;
        QString color, label;
        switch (state) {
        case Off: color = "#666666"; label = "Disabled"; break;
        case Home: color = "#2ecc71"; label = "Home"; break;
        case Public: color = "#f39c12"; label = "Public"; break;
        case Strict: color = "#cc0000"; label = "Strict"; break;
        case Custom: color = "#555555"; label = "Custom"; break;
        }
        setStyleSheet(QString(
            "QPushButton { background:%1; color:white; border-radius:25px;"
            " font-size:20px; padding:4px 16px; }"
            "QPushButton:pressed { background:#333333; }"
            "QPushButton:disabled { background:#555555; color:#bbbbbb; }"
        ).arg(color));
        setText(label);
    }
private:
    State m_state = Off;
};

static QWidget *settingRow(const QString &name, QWidget *control)
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QHBoxLayout *lay = new QHBoxLayout(card);
    lay->setContentsMargins(30, 20, 30, 20);
    lay->setSpacing(20);
    QLabel *label = new QLabel(name, card);
    label->setStyleSheet("font-size:30px; font-weight:bold;");
    lay->addWidget(label);
    lay->addStretch();
    control->setParent(card);
    lay->addWidget(control);
    return card;
}

class SecurityPage : public QWidget
{
public:
    explicit SecurityPage(QStackedWidget *stack);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QStackedWidget *m_stack = nullptr;
    FirewallPill *m_fwPill = nullptr;
    BoolPillToggle *m_sshPill = nullptr;
    BoolPillToggle *m_usbPill = nullptr;
    QPushButton *m_resetButton = nullptr;
    QTextEdit *m_liveConnEdit = nullptr;
    QTextEdit *m_ufwLogEdit = nullptr;
    QTimer *m_logTimer = nullptr;
    bool m_busy = false;
    bool m_networkRefreshing = false;
    bool m_firewallLogRefreshing = false;

    QWidget *makeLogsCard(const QString &title, QTextEdit *&edit);
    void refreshSystemState();
    void refreshLogs();
    void setBusy(bool busy);
    void runAsync(const QString &program, const QStringList &args,
                  const std::function<void(bool, const QString &)> &completion);
    void runAsRoot(const QString &script,
                   const std::function<void(bool, const QString &)> &completion);
    void applyFirewall(FirewallPill::State profile);
    void applySSH(bool enable);
    void resetPassword();
};

SecurityPage::SecurityPage(QStackedWidget *stack)
    : QWidget(stack), m_stack(stack)
{
    setStyleSheet("background:#282828; color:white; font-family:Sans;");

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(40, 40, 40, 40);
    root->setSpacing(10);

    QLabel *title = new QLabel("Security");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:42px; font-weight:bold;");
    root->addWidget(title);

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

    m_fwPill = new FirewallPill;
    m_fwPill->setToolTip("Home: allow local private-network connections.\n"
                         "Public: block unsolicited incoming connections.\n"
                         "Strict: block incoming connections and increase firewall logging.\n"
                         "Changing profiles replaces existing UFW rules.");
    outerLay->addWidget(settingRow("Firewall", m_fwPill));

    m_sshPill = new BoolPillToggle;
    m_sshPill->setToolTip("Start/stop the SSH service and enable/disable its SysVinit boot startup.");
    outerLay->addWidget(settingRow("SSH", m_sshPill));

    // This was a nonfunctional stub in the original page. Do not imply a
    // system-wide USB restriction has been applied when it has not.
    m_usbPill = new BoolPillToggle;
    m_usbPill->setState(false);
    m_usbPill->setEnabled(false);
    m_usbPill->setText("Unavailable");
    m_usbPill->setToolTip("USB Lockdown has no implemented backend. No USB devices are blocked.");
    outerLay->addWidget(settingRow("USB Lockdown", m_usbPill));

    m_resetButton = new QPushButton("Reset");
    m_resetButton->setStyleSheet(
        "QPushButton { background:#cc0000; color:white; border-radius:20px;"
        " font-size:24px; padding:8px 20px; }"
        "QPushButton:hover { background:#dd2222; }"
        "QPushButton:pressed { background:#990000; }"
    );
    m_resetButton->setFixedHeight(54);
    m_resetButton->setToolTip("Verify the existing osm-lock pattern before resetting it.");
    outerLay->addWidget(settingRow("Password", m_resetButton));

    outerLay->addWidget(makeLogsCard("Network Connections", m_liveConnEdit));
    outerLay->addWidget(makeLogsCard("Firewall Log (ufw)", m_ufwLogEdit));

    wrapLay->addWidget(outer);
    wrapLay->addStretch();
    scroll->setWidget(wrap);
    root->addWidget(scroll);

    QPushButton *back = makeBtn("❮");
    back->setFixedSize(140, 60);
    connect(back, &QPushButton::clicked, this, [this]() {
        if (m_stack)
            m_stack->setCurrentIndex(0);
    });
    root->addWidget(back, 0, Qt::AlignCenter);

    connect(m_fwPill, &QPushButton::clicked, this, [this]() {
        applyFirewall(m_fwPill->nextState());
    });
    connect(m_sshPill, &QPushButton::clicked, this, [this]() {
        applySSH(!m_sshPill->isOn());
    });
    connect(m_resetButton, &QPushButton::clicked, this, [this]() {
        resetPassword();
    });

    m_logTimer = new QTimer(this);
    m_logTimer->setInterval(3000);
    connect(m_logTimer, &QTimer::timeout, this, [this]() { refreshLogs(); });
}

QWidget *SecurityPage::makeLogsCard(const QString &title, QTextEdit *&edit)
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(30, 20, 30, 20);
    lay->setSpacing(10);

    QLabel *label = new QLabel(title, card);
    label->setStyleSheet("font-size:26px; font-weight:bold;");
    lay->addWidget(label);

    edit = new QTextEdit(card);
    edit->setReadOnly(true);
    edit->setStyleSheet(
        "QTextEdit { background:#3a3a3a; border-radius:20px; color:white;"
        " font-family:monospace; font-size:18px; }"
    );
    edit->setFixedHeight(220);
    lay->addWidget(edit);
    return card;
}

void SecurityPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_busy)
        refreshSystemState();
    if (!m_logTimer->isActive()) {
        refreshLogs();
        m_logTimer->start();
    }
}

void SecurityPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_logTimer->stop();
}

void SecurityPage::refreshSystemState()
{
    const bool hasUfw = !QStandardPaths::findExecutable("ufw").isEmpty() ||
                        QFileInfo::exists("/usr/sbin/ufw");
    m_fwPill->setEnabled(hasUfw && !m_busy);
    if (!hasUfw) {
        m_fwPill->setState(FirewallPill::Custom);
        m_fwPill->setText("Unavailable");
        m_fwPill->setToolTip("UFW is not installed.");
    } else {
        // /etc/ufw/ufw.conf is normally readable by regular users. UFW's
        // `status` command itself may require root; do not prompt to read it.
        QFile ufwConfig("/etc/ufw/ufw.conf");
        QString enabled;
        if (ufwConfig.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&ufwConfig);
            while (!stream.atEnd()) {
                const QString line = stream.readLine().trimmed();
                if (line.startsWith("ENABLED="))
                    enabled = line.mid(8).trimmed().toLower();
            }
        }
        if (enabled == "no") {
            m_fwPill->setState(FirewallPill::Off);
        } else if (enabled == "yes") {
            const QString configured = readConfigKey("firewall_state", "custom").toLower();
            FirewallPill::State state = FirewallPill::fromString(configured);
            m_fwPill->setState(state == FirewallPill::Off ? FirewallPill::Custom : state);
        } else {
            // Do not show "Disabled" just because status cannot be read.
            m_fwPill->setState(FirewallPill::Custom);
            m_fwPill->setText("Unknown");
        }
    }

    const bool hasSshService = QFileInfo::exists("/etc/init.d/ssh") ||
                               QFileInfo::exists("/etc/init.d/sshd");
    m_sshPill->setEnabled(hasSshService && !m_busy);
    if (!hasSshService) {
        m_sshPill->setState(false);
        m_sshPill->setText("Unavailable");
        m_sshPill->setToolTip("SSH service is not installed.");
    } else {
        const CommandResult running = runRead("pgrep", {"-x", "sshd"});
        m_sshPill->setState(running.ok);
    }
}

void SecurityPage::setBusy(bool busy)
{
    m_busy = busy;
    m_fwPill->setEnabled(!busy);
    m_sshPill->setEnabled(!busy);
    m_resetButton->setEnabled(!busy);
    if (!busy)
        refreshSystemState();
}

void SecurityPage::runAsync(const QString &program, const QStringList &args,
                            const std::function<void(bool, const QString &)> &completion)
{
    QProcess *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    const auto completed = std::make_shared<bool>(false);
    const auto conclude = [process, completed, completion](bool ok, const QString &message) {
        if (*completed)
            return;
        *completed = true;
        completion(ok, message.trimmed());
        process->deleteLater();
    };
    connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [process, conclude](int code, QProcess::ExitStatus status) {
                const QString output = QString::fromLocal8Bit(process->readAll());
                conclude(status == QProcess::NormalExit && code == 0, output);
            });
    connect(process, &QProcess::errorOccurred, this,
            [process, conclude](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    conclude(false, process->errorString());
            });
    process->start(program, args);
}

void SecurityPage::runAsRoot(const QString &script,
                             const std::function<void(bool, const QString &)> &completion)
{
    const QString wrapper = "/usr/local/bin/osm-sudo";
    if (!QFileInfo::exists(wrapper)) {
        completion(false, "Alternix privilege helper /usr/local/bin/osm-sudo was not found.");
        return;
    }
    // One osm-lock pattern authentication for the entire requested change.
    // No terminal sudo calls, no embedded password and no writable shell file.
    runAsync(wrapper, {"/bin/sh", "-ec", script}, completion);
}

void SecurityPage::applySSH(bool enable)
{
    if (m_busy)
        return;
    if (!enable && QMessageBox::question(this, "Disable SSH",
        "Stop SSH now and prevent it starting at boot?\n\n"
        "This can disconnect remote sessions.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    const QString service = QFileInfo::exists("/etc/init.d/ssh") ? "ssh" : "sshd";
    const QString script = enable
        ? QString("/usr/sbin/update-rc.d %1 enable\n/usr/sbin/service %1 start\n").arg(service)
        : QString("/usr/sbin/service %1 stop\n/usr/sbin/update-rc.d %1 disable\n").arg(service);

    setBusy(true);
    runAsRoot(script, [this, enable](bool ok, const QString &out) {
        if (ok) {
            const bool running = runRead("pgrep", {"-x", "sshd"}).ok;
            if (running != enable)
                ok = false;
        }
        setBusy(false);
        if (!ok) {
            QMessageBox::warning(this, "SSH", "Unable to change SSH state.\n\n" +
                (out.isEmpty() ? "Check SysVinit service and osm-sudo permissions." : out.left(1500)));
        }
        // Runtime state wins; never write a desired state as if it succeeded.
    });
}

void SecurityPage::applyFirewall(FirewallPill::State profile)
{
    if (m_busy || profile == FirewallPill::Custom)
        return;

    const QString name = FirewallPill::stateToString(profile).toUpper();
    const QString message = profile == FirewallPill::Off
        ? "Disable UFW firewall protection?"
        : QString("Apply the %1 firewall profile?\n\n"
                  "This will RESET and replace existing UFW rules. "
                  "Remote connections may be disconnected.\n\n"
                  "Home permits incoming traffic from private IPv4 ranges; "
                  "Public and Strict deny unsolicited incoming traffic.").arg(name);
    if (QMessageBox::question(this, "Firewall", message,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    QString script;
    if (profile == FirewallPill::Off) {
        script = "ufw --force disable\n";
    } else {
        script =
            "ufw --force reset\n"
            "ufw default deny incoming\n"
            "ufw default allow outgoing\n"
            "ufw default deny routed\n";
        if (profile == FirewallPill::Home) {
            script +=
                "ufw allow from 10.0.0.0/8\n"
                "ufw allow from 172.16.0.0/12\n"
                "ufw allow from 192.168.0.0/16\n";
        }
        script += (profile == FirewallPill::Strict) ? "ufw logging high\n" : "ufw logging low\n";
        script += "ufw --force enable\n";
    }

    setBusy(true);
    runAsRoot(script, [this, profile](bool ok, const QString &out) {
        if (ok) {
            // Verify the persistent activation state as well as the exit code.
            QFile file("/etc/ufw/ufw.conf");
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString content = QString::fromLocal8Bit(file.readAll());
                const QString expected = profile == FirewallPill::Off ? "ENABLED=no" : "ENABLED=yes";
                if (!content.contains(expected))
                    ok = false;
            }
        }
        if (ok) {
            QString error;
            if (!writeConfigKey("firewall_state", FirewallPill::stateToString(profile), error))
                QMessageBox::warning(this, "Firewall settings",
                                     "Firewall was applied, but its profile label could not be saved:\n" + error);
        }
        setBusy(false);
        if (!ok) {
            QMessageBox::warning(this, "Firewall",
                "Firewall change failed or authentication was cancelled.\n\n" +
                (out.isEmpty() ? "Check UFW and osm-sudo permissions." : out.left(1500)));
        }
    });
}

void SecurityPage::resetPassword()
{
    if (m_busy)
        return;
    if (QMessageBox::question(this, "Reset osm-lock password",
        "Authenticate with your CURRENT osm-lock pattern, then create a new one.\n\n"
        "If new setup fails, the existing credentials will be restored. Continue?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    const QString lock = "/usr/local/bin/osm-lock";
    const QString path = QDir::homePath() + "/.config/Alternix/.osm_lockdata";
    const QString backup = path + ".security-reset-backup";
    if (!QFileInfo::exists(lock)) {
        QMessageBox::warning(this, "Password", "osm-lock is not installed.");
        return;
    }
    if (QFileInfo::exists(backup)) {
        QMessageBox::warning(this, "Password",
            "A previous password-reset backup exists. Existing credentials have not been changed.\n" + backup);
        return;
    }

    setBusy(true);
    runAsync(lock, {"--auth"}, [this, lock, path, backup](bool authenticated, const QString &) {
        if (!authenticated) {
            setBusy(false);
            QMessageBox::information(this, "Password", "Authentication failed or was cancelled. No credentials were changed.");
            return;
        }
        if (!QFileInfo::exists(path) || !QFile::rename(path, backup)) {
            setBusy(false);
            QMessageBox::warning(this, "Password", "Could not safely preserve the existing osm-lock credentials.");
            return;
        }
        // Without the old data osm-lock enters its existing first-run setup.
        runAsync(lock, {}, [this, path, backup](bool setupOk, const QString &) {
            if (setupOk && QFileInfo::exists(path)) {
                QFile::remove(backup);
                setBusy(false);
                QMessageBox::information(this, "Password", "New osm-lock credentials were saved.");
                return;
            }
            if (QFileInfo::exists(path))
                QFile::remove(path);
            const bool restored = QFile::rename(backup, path);
            setBusy(false);
            QMessageBox::warning(this, "Password", restored
                ? "New password setup did not finish. Previous credentials were restored."
                : "Unable to restore the previous credentials automatically. Backup location:\n" + backup);
        });
    });
}

void SecurityPage::refreshLogs()
{
    if (m_liveConnEdit && !m_networkRefreshing) {
        const QString ss = QStandardPaths::findExecutable("ss");
        if (ss.isEmpty()) {
            setLogText(m_liveConnEdit, "The 'ss' network utility is not installed.");
        } else {
            m_networkRefreshing = true;
            runAsync(ss, {"-tunp", "state", "established"},
                     [this](bool ok, const QString &output) {
                m_networkRefreshing = false;
                const QStringList lines = output.split('\n', Qt::SkipEmptyParts);
                setLogText(m_liveConnEdit, !ok
                    ? "Could not read network connections: " + output
                    : lines.size() <= 1 ? "No established TCP/UDP network connections."
                                        : output);
            });
        }
    }

    if (m_ufwLogEdit && !m_firewallLogRefreshing) {
        QFileInfo info("/var/log/ufw.log");
        if (!info.exists()) {
            setLogText(m_ufwLogEdit, "No UFW log file exists at /var/log/ufw.log.");
        } else if (!info.isReadable()) {
            setLogText(m_ufwLogEdit, "UFW log exists but is not readable by this user.");
        } else {
            m_firewallLogRefreshing = true;
            runAsync("tail", {"-n", "200", info.filePath()},
                     [this](bool ok, const QString &output) {
                m_firewallLogRefreshing = false;
                setLogText(m_ufwLogEdit, ok
                    ? (output.isEmpty() ? "No UFW log entries found." : output)
                    : "Could not read UFW log: " + output);
            });
        }
    }
}

extern "C" QWidget *make_page(QStackedWidget *stack)
{
    return new SecurityPage(stack);
}
