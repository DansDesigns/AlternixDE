#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
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
#include <QComboBox>
#include <QFileDialog>
#include <QCheckBox>
#include <QProgressBar>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QDateTime>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextDocument>
#include <QSet>

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
    ~SecurityPage() override;

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

    // ClamAV is invoked only when a scan is requested; it does not need clamd.
    QComboBox *m_scanMode = nullptr;
    QComboBox *m_scanDrives = nullptr;
    QCheckBox *m_scanProtected = nullptr;
    QPushButton *m_scanPick = nullptr;
    QPushButton *m_scanStart = nullptr;
    QPushButton *m_scanCancel = nullptr;
    QPushButton *m_scanSave = nullptr;
    QPushButton *m_scanRefresh = nullptr;
    QLabel *m_clamStatus = nullptr;
    QLabel *m_scanTargetLabel = nullptr;
    QLabel *m_scanStatus = nullptr;
    QLabel *m_scanHelp = nullptr;
    QTextEdit *m_scanOutput = nullptr;
    QProgressBar *m_scanProgress = nullptr;
    QTimer *m_scanTimer = nullptr;
    QProcess *m_scanProcess = nullptr;
    QElapsedTimer m_scanElapsed;
    std::unique_ptr<QTemporaryDir> m_scanControlDir;
    QString m_selectedFolder;
    QString m_scannedTarget;
    QString m_scanTranscript;
    bool m_scanActive = false;
    bool m_scanCancelRequested = false;

    QWidget *makeClamCard();
    void refreshClamState();
    void refreshScanDrives();
    void updateScanUi();
    void startClamScan();
    void cancelClamScan();
    void consumeClamOutput();
    void finishClamScan(bool started, int exitCode, QProcess::ExitStatus exitStatus);
    void saveClamReport();
    QString clamscanExecutable() const;

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

    outerLay->addWidget(makeClamCard());

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

    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(500);
    connect(m_scanTimer, &QTimer::timeout, this, [this]() {
        if (m_scanActive) {
            const qint64 secs = m_scanElapsed.elapsed() / 1000;
            m_scanStatus->setText(m_scanCancelRequested
                ? QString("Stopping scan...  %1:%2 elapsed").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'))
                : QString("Scanning...  %1:%2 elapsed").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0')));
        }
    });

    m_logTimer = new QTimer(this);
    m_logTimer->setInterval(3000);
    connect(m_logTimer, &QTimer::timeout, this, [this]() { refreshLogs(); });
}

// Return an absolute, system-installed executable, not an arbitrary entry
// found in the user's PATH. In particular this command may run via osm-sudo.
QString SecurityPage::clamscanExecutable() const
{
    const QStringList paths = {"/usr/bin/clamscan", "/usr/local/bin/clamscan"};
    for (const QString &path : paths) {
        const QFileInfo info(path);
        if (info.isFile() && info.isExecutable())
            return path;
    }
    return QString();
}

QWidget *SecurityPage::makeClamCard()
{
    QFrame *card = new QFrame;
    card->setObjectName("alternixClamavCard");
    card->setStyleSheet(
        "QFrame#alternixClamavCard { background:#444444; border-radius:30px; }"
        "QComboBox { color:white; background:#363636; padding:10px; border:1px solid #666;"
        " border-radius:12px; font-size:19px; min-height:38px; }"
        "QComboBox QAbstractItemView { color:white; background:#333; selection-background-color:#555; }"
        "QCheckBox { color:white; font-size:18px; spacing:12px; }"
        "QLabel { color:white; }"
    );
    QVBoxLayout *layout = new QVBoxLayout(card);
    layout->setContentsMargins(30, 23, 30, 24);
    layout->setSpacing(14);

    QLabel *heading = new QLabel("ClamAV Malware Scanner", card);
    heading->setStyleSheet("font-size:28px; font-weight:bold;");
    layout->addWidget(heading);

    m_clamStatus = new QLabel(card);
    m_clamStatus->setWordWrap(true);
    m_clamStatus->setStyleSheet("font-size:17px; color:#dddddd;");
    layout->addWidget(m_clamStatus);

    QLabel *typeLabel = new QLabel("Choose what to scan", card);
    typeLabel->setStyleSheet("font-size:20px; font-weight:bold;");
    layout->addWidget(typeLabel);

    m_scanMode = new QComboBox(card);
    m_scanMode->addItem("Specific folder");
    m_scanMode->addItem("Internal / external drive");
    m_scanMode->addItem("Entire system");
    layout->addWidget(m_scanMode);

    QHBoxLayout *targetLayout = new QHBoxLayout;
    targetLayout->setSpacing(12);
    m_scanTargetLabel = new QLabel(card);
    m_scanTargetLabel->setWordWrap(true);
    m_scanTargetLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_scanTargetLabel->setStyleSheet("font-size:18px;");
    targetLayout->addWidget(m_scanTargetLabel, 1);

    m_scanPick = makeBtn("Choose folder");
    m_scanPick->setMinimumWidth(155);
    targetLayout->addWidget(m_scanPick);
    layout->addLayout(targetLayout);

    m_scanDrives = new QComboBox(card);
    m_scanDrives->setToolTip("All currently mounted storage volumes. Mount a drive in Files first if needed.");
    layout->addWidget(m_scanDrives);

    m_scanProtected = new QCheckBox("Include protected files (osm-sudo authentication)", card);
    m_scanProtected->setToolTip("Run this scan with elevated file-read permissions via osm-sudo."
        " The entire-system scan always requires this.");
    layout->addWidget(m_scanProtected);

    m_scanHelp = new QLabel(card);
    m_scanHelp->setWordWrap(true);
    m_scanHelp->setStyleSheet("font-size:16px; color:#cccccc;");
    layout->addWidget(m_scanHelp);

    QGridLayout *actions = new QGridLayout;
    actions->setSpacing(10);
    m_scanStart = makeBtn("Scan now");
    m_scanCancel = makeBtn("Cancel scan");
    m_scanSave = makeBtn("Save report");
    m_scanRefresh = makeBtn("Refresh drives");
    for (QPushButton *button : {m_scanStart, m_scanCancel, m_scanSave, m_scanRefresh}) {
        button->setMinimumSize(110, 50);
        button->setStyleSheet(
            "QPushButton { background:#555; color:white; border:1px solid #666;"
            " border-radius:14px; font-size:18px; padding:8px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#333; }"
            "QPushButton:disabled { color:#999; background:#3b3b3b; }");
    }
    m_scanStart->setStyleSheet(
        "QPushButton { background:#23683c; color:white; border-radius:14px;"
        " font-size:19px; font-weight:bold; padding:8px; }"
        "QPushButton:hover { background:#29874b; }"
        "QPushButton:disabled { background:#395444; color:#aaa; }");
    actions->addWidget(m_scanStart, 0, 0);
    actions->addWidget(m_scanCancel, 0, 1);
    actions->addWidget(m_scanSave, 1, 0);
    actions->addWidget(m_scanRefresh, 1, 1);
    layout->addLayout(actions);

    m_scanProgress = new QProgressBar(card);
    m_scanProgress->setRange(0, 0); // Indeterminate; ClamAV does not supply a total-file count.
    m_scanProgress->setTextVisible(false);
    m_scanProgress->setFixedHeight(12);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);

    m_scanStatus = new QLabel("Ready to scan.", card);
    m_scanStatus->setWordWrap(true);
    m_scanStatus->setStyleSheet("font-size:18px; font-weight:bold;");
    layout->addWidget(m_scanStatus);

    m_scanOutput = new QTextEdit(card);
    m_scanOutput->setReadOnly(true);
    m_scanOutput->setPlaceholderText("Detected threats, scanning errors and the final ClamAV summary will appear here.");
    m_scanOutput->document()->setMaximumBlockCount(450);
    m_scanOutput->setStyleSheet(
        "QTextEdit { background:#303030; color:white; border:1px solid #555;"
        " border-radius:14px; padding:10px; font-family:monospace; font-size:15px; }");
    m_scanOutput->setFixedHeight(230);
    layout->addWidget(m_scanOutput);

    m_selectedFolder = QDir::homePath();
    connect(m_scanMode, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateScanUi(); });
    connect(m_scanPick, &QPushButton::clicked, this, [this]() {
        const QString picked = QFileDialog::getExistingDirectory(this, "Select folder to scan", m_selectedFolder,
                                                                  QFileDialog::ShowDirsOnly);
        if (!picked.isEmpty()) {
            m_selectedFolder = picked;
            updateScanUi();
        }
    });
    connect(m_scanStart, &QPushButton::clicked, this, [this]() { startClamScan(); });
    connect(m_scanCancel, &QPushButton::clicked, this, [this]() { cancelClamScan(); });
    connect(m_scanSave, &QPushButton::clicked, this, [this]() { saveClamReport(); });
    connect(m_scanRefresh, &QPushButton::clicked, this, [this]() {
        refreshScanDrives();
        refreshClamState();
    });
    refreshScanDrives();
    refreshClamState();
    updateScanUi();
    return card;
}

void SecurityPage::refreshClamState()
{
    if (!m_clamStatus)
        return;
    if (clamscanExecutable().isEmpty()) {
        m_clamStatus->setText("ClamAV is not installed. Install the matching x86-64 or ARM64 "
                              "packages through the AlternixOS installer before scanning.");
        m_clamStatus->setStyleSheet("font-size:17px; color:#ffb4a9;");
    } else {
        QDateTime newest;
        const QStringList databases = {"daily.cld", "daily.cvd", "main.cvd", "main.cld", "bytecode.cvd", "bytecode.cld"};
        for (const QString &name : databases) {
            const QFileInfo info("/var/lib/clamav/" + name);
            if (info.exists() && (!newest.isValid() || info.lastModified() > newest))
                newest = info.lastModified();
        }
        QString description = "ClamAV installed.";
        if (newest.isValid())
            description += " Signature files last modified: " + newest.toLocalTime().toString("dd MMM yyyy, HH:mm") + ".";
        else
            description += " No signature database found at /var/lib/clamav; check freshclam or any custom database location.";
        m_clamStatus->setText(description);
        m_clamStatus->setStyleSheet("font-size:17px; color:#d6eadb;");
    }
    updateScanUi();
}

void SecurityPage::refreshScanDrives()
{
    if (!m_scanDrives)
        return;
    const QString previouslySelected = m_scanDrives->currentData().toString();
    const QSet<QByteArray> virtualTypes = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2", "securityfs",
        "debugfs", "tracefs", "pstore", "configfs", "hugetlbfs", "mqueue", "fusectl",
        "binfmt_misc", "efivarfs", "ramfs", "autofs", "rpc_pipefs", "nsfs"
    };
    m_scanDrives->clear();
    QSet<QString> seen;
    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &storage : volumes) {
        if (!storage.isValid() || !storage.isReady())
            continue;
        const QString path = QDir::cleanPath(storage.rootPath());
        if (!path.startsWith('/') || seen.contains(path) || virtualTypes.contains(storage.fileSystemType()))
            continue;
        if (path.startsWith("/proc/") || path == "/proc" || path.startsWith("/sys/") || path == "/sys" ||
            path.startsWith("/dev/") || path == "/dev" ||
            (path.startsWith("/run/") && !path.startsWith("/run/media/")))
            continue;
        seen.insert(path);
        QString name = storage.name().trimmed();
        if (name.isEmpty())
            name = path == "/" ? "System drive" : "Mounted volume";
        QString display = QString("%1  —  %2").arg(name, path);
        const QString device = QString::fromLocal8Bit(storage.device());
        if (!device.isEmpty())
            display += "  [" + device + "]";
        m_scanDrives->addItem(display, path);
    }
    if (!seen.contains("/") && QStorageInfo("/").isValid())
        m_scanDrives->insertItem(0, "System drive  —  /", "/");
    const int index = m_scanDrives->findData(previouslySelected);
    if (index >= 0)
        m_scanDrives->setCurrentIndex(index);
    updateScanUi();
}

void SecurityPage::updateScanUi()
{
    if (!m_scanMode || !m_scanStart)
        return;
    const int mode = m_scanMode->currentIndex();
    m_scanPick->setVisible(mode == 0);
    m_scanDrives->setVisible(mode == 1);
    if (mode == 2) {
        m_scanProtected->setChecked(true);
        m_scanProtected->setEnabled(false);
        m_scanTargetLabel->setText("Target: Entire installed system (/)");
        m_scanHelp->setText("Scans system files and separate /home or /boot filesystems. "
            "Excludes virtual /proc, /sys, /dev, /run, mounted drives under /media and /mnt, "
            "and ClamAV's signature directory. Select a drive separately to scan removable storage. "
            "Large files and archives remain subject to ClamAV engine limits.");
    } else {
        m_scanProtected->setEnabled(!m_scanActive);
        if (mode == 0) {
            m_scanTargetLabel->setText("Folder: " + m_selectedFolder);
            m_scanHelp->setText("Recursively scans this folder on its own filesystem. "
                "Symlinks are not followed. Tick protected files if some contents require elevated access.");
        } else {
            m_scanTargetLabel->setText("Choose a currently mounted volume:");
            m_scanHelp->setText("Scans the selected internal, USB or other mounted filesystem, "
                "without crossing into nested mounts. Connect and mount a drive before refreshing this list.");
        }
    }
    m_scanStart->setEnabled(!m_scanActive && !clamscanExecutable().isEmpty() &&
                            (mode != 1 || m_scanDrives->count() != 0));
    m_scanMode->setEnabled(!m_scanActive);
    m_scanDrives->setEnabled(!m_scanActive);
    m_scanPick->setEnabled(!m_scanActive);
    m_scanRefresh->setEnabled(!m_scanActive);
    m_scanCancel->setEnabled(m_scanActive && !m_scanCancelRequested);
    m_scanSave->setEnabled(!m_scanTranscript.isEmpty());
}

void SecurityPage::startClamScan()
{
    if (m_scanActive)
        return;
    const QString binary = clamscanExecutable();
    if (binary.isEmpty()) {
        QMessageBox::warning(this, "ClamAV", "ClamAV is not installed. Install ClamAV and its signatures first.");
        refreshClamState();
        return;
    }
    const int mode = m_scanMode->currentIndex();
    QString target = mode == 2 ? "/" : (mode == 1 ? m_scanDrives->currentData().toString() : m_selectedFolder);
    if (target.isEmpty()) {
        QMessageBox::warning(this, "ClamAV", "Choose a folder or mounted drive first.");
        return;
    }
    // Check again because a user can remove a USB drive after the list was built.
    const QFileInfo targetInfo(target);
    if (!targetInfo.exists() || !targetInfo.isDir()) {
        QMessageBox::warning(this, "ClamAV", "The chosen folder or drive is no longer available. Refresh the list.");
        return;
    }
    if (mode == 1) {
        bool mounted = false;
        for (const QStorageInfo &s : QStorageInfo::mountedVolumes()) {
            if (s.isValid() && s.isReady() && QDir::cleanPath(s.rootPath()) == QDir::cleanPath(target)) {
                mounted = true;
                break;
            }
        }
        if (!mounted) {
            QMessageBox::warning(this, "ClamAV", "That drive is no longer mounted. Refresh the list.");
            return;
        }
    }
    const bool privileged = mode == 2 || m_scanProtected->isChecked();
    if (privileged && !QFileInfo::exists("/usr/local/bin/osm-sudo")) {
        QMessageBox::warning(this, "ClamAV", "The AlternixOS osm-sudo authentication helper was not found.");
        return;
    }
    if (mode == 2 && QMessageBox::question(this, "Scan entire system",
            "Scan the system with osm-sudo authentication? This can take a long time and "
            "use significant memory and CPU. Virtual filesystems and removable drive "
            "mountpoints are excluded. No files will be deleted or quarantined automatically.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;

    m_scanControlDir.reset(new QTemporaryDir(QDir::tempPath() + "/alternix-clamav-XXXXXX"));
    if (!m_scanControlDir->isValid()) {
        m_scanControlDir.reset();
        QMessageBox::warning(this, "ClamAV", "Unable to create a private scan cancellation channel.");
        return;
    }
    m_scannedTarget = target;
    m_scanTranscript = QString("AlternixOS ClamAV scan\nStarted: %1\nTarget: %2\nAccess: %3\n\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODate), target,
             privileged ? "Protected files included (osm-sudo)" : "Current user");
    m_scanOutput->clear();
    m_scanOutput->setPlainText(m_scanTranscript);
    m_scanActive = true;
    m_scanCancelRequested = false;
    m_scanElapsed.start();
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0);
    m_scanStatus->setText(privileged ? "Authenticating via osm-sudo..." : "Starting scan...");
    updateScanUi();

    QStringList scanArgs = {"--recursive", "--infected", "--stdout", "--follow-dir-symlinks=0",
                            "--follow-file-symlinks=0"};
    if (mode == 2) {
        scanArgs << "--cross-fs=yes";
        scanArgs << "--exclude-dir=^/(proc|sys|dev|run|media|mnt)(/|$)";
        scanArgs << "--exclude-dir=^/var/lib/clamav(/|$)";
        // Network mounts outside conventional removable-mount folders are not
        // local system storage. Scan them explicitly as a selected drive.
        const QSet<QByteArray> networkTypes = {"nfs", "nfs4", "cifs", "smb3", "sshfs", "fuse.sshfs"};
        for (const QStorageInfo &vol : QStorageInfo::mountedVolumes()) {
            if (vol.isValid() && vol.isReady() && networkTypes.contains(vol.fileSystemType())) {
                const QString mountPath = QDir::cleanPath(vol.rootPath());
                if (mountPath != "/")
                    scanArgs << ("--exclude-dir=^" + QRegularExpression::escape(mountPath) + "(/|$)");
            }
        }
    } else {
        scanArgs << "--cross-fs=no";
    }
    // -- stops paths beginning with '-' being parsed as ClamAV flags.
    scanArgs << "--" << target;

    // Cancellation works even for privileged scans. A root-owned /bin/sh
    // supervises only its *own* clamscan child and polls the marker in a
    // private directory. No pidfiles, broad pkill, shell-injected paths, or
    // second osm-sudo authentication are needed to request cancellation.
    static const QString supervisor = QString::fromLatin1(R"SH(
cancel_file=$1
shift
"$@" &
scan_pid=$!
(
    while kill -0 "$scan_pid" 2>/dev/null; do
        if [ -e "$cancel_file" ]; then
            kill -TERM "$scan_pid" 2>/dev/null || :
            exit 0
        fi
        sleep 0.25
    done
) &
watch_pid=$!
wait "$scan_pid"
result=$?
kill "$watch_pid" 2>/dev/null || :
wait "$watch_pid" 2>/dev/null || :
if [ -e "$cancel_file" ]; then exit 130; fi
exit "$result"
)SH");

    QStringList supervisorArgs = {"-c", supervisor, "alternix-clamav",
                                  m_scanControlDir->filePath("cancel"), binary};
    supervisorArgs.append(scanArgs);
    const QString launchProgram = privileged ? "/usr/local/bin/osm-sudo" : "/bin/sh";
    if (privileged)
        supervisorArgs.prepend("/bin/sh");

    QProcess *process = new QProcess(this);
    m_scanProcess = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        if (m_scanProcess == process)
            consumeClamOutput();
    });
    connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, process](int code, QProcess::ExitStatus status) {
        if (m_scanProcess == process) {
            consumeClamOutput();
            finishClamScan(true, code, status);
        }
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        if (m_scanProcess == process && error == QProcess::FailedToStart) {
            m_scanTranscript += "\nUnable to start scanner: " + process->errorString() + "\n";
            m_scanOutput->append("Unable to start scanner: " + process->errorString());
            finishClamScan(false, -1, QProcess::CrashExit);
            process->deleteLater();
        }
    });
    process->start(launchProgram, supervisorArgs);
    m_scanTimer->start();
}

void SecurityPage::cancelClamScan()
{
    if (!m_scanActive || m_scanCancelRequested || !m_scanControlDir)
        return;
    QFile marker(m_scanControlDir->filePath("cancel"));
    if (!marker.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, "ClamAV", "Unable to send cancellation request: " + marker.errorString());
        return;
    }
    marker.close();
    m_scanCancelRequested = true;
    m_scanStatus->setText("Stopping scan and cleaning up...");
    updateScanUi();
}

void SecurityPage::consumeClamOutput()
{
    if (!m_scanProcess)
        return;
    const QString chunk = QString::fromLocal8Bit(m_scanProcess->readAllStandardOutput());
    if (chunk.isEmpty())
        return;
    // Keep a bounded, savable report; the view keeps only recent blocks too.
    if (m_scanTranscript.size() + chunk.size() > 8 * 1024 * 1024) {
        const int discard = m_scanTranscript.size() + chunk.size() - 7 * 1024 * 1024;
        m_scanTranscript.remove(0, qMin(discard, m_scanTranscript.size()));
        m_scanTranscript.prepend("[Earlier output truncated to limit memory usage]\n");
    }
    m_scanTranscript += chunk;
    QTextCursor cursor = m_scanOutput->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(chunk);
    m_scanOutput->setTextCursor(cursor);
    m_scanOutput->ensureCursorVisible();
    m_scanSave->setEnabled(true);
}

void SecurityPage::finishClamScan(bool started, int exitCode, QProcess::ExitStatus exitStatus)
{
    if (!m_scanActive)
        return;
    m_scanTimer->stop();
    const bool cancelled = m_scanCancelRequested;
    const bool hasSummary = m_scanTranscript.contains("SCAN SUMMARY", Qt::CaseInsensitive);
    const QRegularExpression infectedRegex("Infected files:\\s*(\\d+)", QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = infectedRegex.match(m_scanTranscript);
    const int detections = match.hasMatch() ? match.captured(1).toInt() : -1;
    QString status;
    if (cancelled || exitCode == 130) {
        status = "Scan cancelled. Results are incomplete.";
    } else if (!started || exitStatus != QProcess::NormalExit || !hasSummary || exitCode > 1 || exitCode < 0) {
        status = "Scan failed or was interrupted. Results may be incomplete. Check the report.";
    } else if (exitCode == 1 && detections > 0) {
        status = QString("Threats detected: %1. Review the findings below; no files were modified.").arg(detections);
    } else if (exitCode == 0 && detections == 0) {
        status = "Scan complete. No threats detected in the files ClamAV was able to scan.";
    } else {
        status = "Scan returned an unexpected result. Review the report before treating it as clean.";
    }
    m_scanStatus->setText(status);
    m_scanTranscript += "\nAlternixOS scan status: " + status + "\n";
    m_scanOutput->append("\n" + status);
    m_scanProgress->setVisible(false);
    m_scanActive = false;
    m_scanCancelRequested = false;
    m_scanProcess = nullptr;
    m_scanControlDir.reset();
    updateScanUi();
}

void SecurityPage::saveClamReport()
{
    if (m_scanTranscript.isEmpty())
        return;
    const QString suggested = QDir::homePath() + "/ClamAV-" +
        QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".txt";
    const QString path = QFileDialog::getSaveFileName(this, "Save ClamAV scan report", suggested,
                                                     "Text reports (*.txt);;All files (*)");
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "ClamAV report", "Could not save report: " + file.errorString());
        return;
    }
    file.write(m_scanTranscript.toUtf8());
    if (!file.commit())
        QMessageBox::warning(this, "ClamAV report", "Could not complete report save: " + file.errorString());
}

SecurityPage::~SecurityPage()
{
    // On a Settings/application close, request a clean stop for an active
    // root-owned scanner before the private cancellation marker disappears.
    if (m_scanActive)
        cancelClamScan();
    if (m_scanProcess && m_scanProcess->state() != QProcess::NotRunning) {
        if (!m_scanProcess->waitForFinished(1500)) {
            if (m_scanControlDir)
                m_scanControlDir->setAutoRemove(false);
            m_scanProcess->terminate();
            m_scanProcess->waitForFinished(300);
        }
    }
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
    if (!m_scanActive) {
        refreshScanDrives();
        refreshClamState();
    }
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
