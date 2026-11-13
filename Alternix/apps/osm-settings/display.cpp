#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QStandardPaths>
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
#include <QScreen>
#include <QGuiApplication>
#include <QSizePolicy>
#include <QTimer>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>
#include <functional>

// -----------------------------------------------------
// Alternix compact button style (same as Security/Storage)
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

static QString altBtnBright()
{
    return
    "QPushButton {"
    " background:#33aaff;"
    " color:white;"
    " border:0px;"
    " border-radius:16px;"
    " font-size:22px;"
    " font-weight:bold;"
    " padding:6px 18px;"
    "}"
    "QPushButton:hover { background:#55bbff; }"
    "QPushButton:pressed { background:#2299dd; }";
}

// -----------------------------------------------------
// Simple CONFIG helpers (same file as Security)
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

// Tiny single-value files (e.g. the rotate-invert flag) that don't need
// the full key=value config format.
static QByteArray readSmallFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

static void writeSmallFile(const QString &path, const QString &content)
{
    QDir().mkpath(QDir::homePath() + "/.config/Alternix");
    QFile f(path);
    if (f.open(QFile::WriteOnly | QFile::Truncate))
        f.write(content.toUtf8());
}

// -----------------------------------------------------
// Simple ON/OFF pill toggle (same as Security BoolPillToggle)
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
// Timeout multi-state pill (6 states in greyscale)
// -----------------------------------------------------
class TimeoutPill : public QPushButton
{
public:
    explicit TimeoutPill(int initial = 0, QWidget *parent = nullptr)
        : QPushButton(parent), m_state(initial)
    {
        setFixedSize(180, 50);   // fixed width as requested
        updateStyle();
    }

    void advance()
    {
        m_state = (m_state + 1) % 6;
        updateStyle();
    }

    void setState(int s)
    {
        if (s < 0 || s > 5) s = 0;
        m_state = s;
        updateStyle();
    }

    int state() const { return m_state; }

    static QString labelForState(int s)
    {
        switch (s) {
        case 0: return "5s";
        case 1: return "10s";
        case 2: return "15s";
        case 3: return "30s";
        case 4: return "1m";
        case 5: return "Never";
        }
        return "5s";
    }

    static int secondsForState(int s)
    {
        switch (s) {
        case 0: return 5;
        case 1: return 10;
        case 2: return 15;
        case 3: return 30;
        case 4: return 60;
        case 5: return 0;   // 0 = never
        }
        return 5;
    }

private:
    int m_state;

    void updateStyle()
    {
        QString color;
        switch (m_state) {
        case 0: color = "#555555"; break; // 5s
        case 1: color = "#666666"; break; // 10s
        case 2: color = "#777777"; break; // 15s
        case 3: color = "#888888"; break; // 30s
        case 4: color = "#AAAAAA"; break; // 1m
        case 5: color = "#CCCCCC"; break; // Never
        default: color = "#555555"; break;
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
        setText(labelForState(m_state));
    }
};

// -----------------------------------------------------
// Auto-Rotate helpers (shared script dir/pidfile with osm-notify)
// -----------------------------------------------------
static QString rotateScriptDir()
{
    return QDir::homePath() + "/.config/Alternix/scripts";
}

static QString rotatePidPath()
{
    return QDir::homePath() + "/.config/Alternix/rotate-monitor.pid";
}

static QString rotateInvertPath()
{
    return QDir::homePath() + "/.config/Alternix/rotate-invert";
}

// A pidfile alone is not evidence of a live monitor: failed starts and
// unclean shutdowns can leave one behind.
static bool rotateMonitorRunning()
{
    QFile f(rotatePidPath());
    if (!f.open(QIODevice::ReadOnly)) return false;
    bool ok = false;
    const qlonglong pid = QString::fromUtf8(f.readAll()).trimmed().toLongLong(&ok);
    if (!ok || pid <= 1) return false;
    const QString cmdlinePath = QString("/proc/%1/cmdline").arg(pid);
    const QByteArray command = readSmallFile(cmdlinePath);
    if (!command.contains("alternix-rotate-monitor.sh")) return false;
    return QString::fromUtf8(readSmallFile(
        QDir::homePath() + "/.config/Alternix/rotate-monitor.ready")).trimmed()
        == QString::number(pid);
}

// A process' /proc/comm name is limited to 15 characters on Linux;
// 'iio-sensor-proxy' has 16, so `pgrep -x iio-sensor-proxy` is unreliable.
static bool rotationProxyRunning()
{
    QProcess p;
    p.start("pgrep", {"-f", "(^|/)iio-sensor-proxy([[:space:]]|$)"});
    if (!p.waitForFinished(1000)) {
        p.kill();
        p.waitForFinished(200);
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// Read IIO information only; do not touch drivers or permissions on page load.
struct RotateSensorInfo {
    QString name;
    QString driver;
    bool found = false;
};

static RotateSensorInfo readRotateSensorInfo()
{
    RotateSensorInfo info;
    const QDir devices("/sys/bus/iio/devices");
    const QStringList names = devices.entryList({"iio:device*"},
                                                QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : names) {
        const QString base = devices.filePath(name);
        // A generic IIO device (for instance an ALS sensor) is NOT
        // necessarily an accelerometer.
        if (!QFile::exists(base + "/in_accel_x_raw") &&
            !QFile::exists(base + "/in_accel_x_input") &&
            !QFile::exists(base + "/scan_elements/in_accel_x_en"))
            continue;

        info.found = true;
        info.name = QString::fromUtf8(readSmallFile(base + "/name")).trimmed();
        if (info.name.isEmpty()) info.name = name;
        const QFileInfo driverLink(base + "/device/driver");
        if (driverLink.isSymLink())
            info.driver = QFileInfo(driverLink.symLinkTarget()).fileName();
        return info;
    }
    return info;
}

// Spinner frames — same Braille rotation used by the WiFi/Bluetooth
// refresh buttons.
static const char* const ROTATE_SPIN_FRAMES[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"
};
static const int ROTATE_SPIN_COUNT = 10;

struct RotateCommandResult {
    QString standardOutput;
    QString standardError;
    int exitCode = -1;
    bool started = false;
    bool timedOut = false;
};

// Pass the script path as a bash ARGUMENT, not through `bash -c` (which
// interprets paths and shell metacharacters). Retain stderr and exit status.
static RotateCommandResult runRotateCmdAnimated(const QString &script,
                                                 int timeoutMs,
                                                 const std::function<void()> &tick,
                                                 bool elevated = false)
{
    RotateCommandResult result;
    if (!QFileInfo::exists(script)) {
        result.standardError = "Rotation setup script is missing: " + script;
        return result;
    }

    QProcess p;
    if (elevated) {
        const QString osmSudo = QStandardPaths::findExecutable("osm-sudo");
        if (osmSudo.isEmpty()) {
            result.standardError = "osm-sudo was not found. Administrator setup requires Alternix pattern/PIN authorisation.";
            return result;
        }
        p.start(osmSudo, {"bash", script});
    } else {
        p.start("bash", {script});
    }
    if (!p.waitForStarted(2500)) {
        result.standardError = "Could not start rotation setup: " + p.errorString();
        return result;
    }
    result.started = true;

    QElapsedTimer elapsed;
    elapsed.start();
    while (p.state() != QProcess::NotRunning) {
        if (p.waitForFinished(80)) break;
        if (elapsed.elapsed() >= timeoutMs) {
            result.timedOut = true;
            p.kill();
            p.waitForFinished(500);
            break;
        }
        if (tick) tick();
        QCoreApplication::processEvents();
    }

    result.standardOutput = QString::fromUtf8(p.readAllStandardOutput());
    result.standardError = QString::fromUtf8(p.readAllStandardError()).trimmed();
    if (!result.timedOut && p.exitStatus() == QProcess::NormalExit)
        result.exitCode = p.exitCode();
    return result;
}

// The setup script supplies KEY=VALUE results. If it fails or is absent,
// report the actual error instead of presenting 'no sensor' as a diagnosis.
static QMap<QString, QString> runRotateScan(bool elevated,
                                             const std::function<void(int)> &tick = {})
{
    QMap<QString, QString> result;
    const QString script = rotateScriptDir() + "/alternix-rotate-setup.sh";

    int frame = 0;
    const RotateCommandResult cmd = runRotateCmdAnimated(script, elevated ? 90000 : 25000, [&]() {
        frame = (frame + 1) % ROTATE_SPIN_COUNT;
        if (tick) tick(frame);
    }, elevated);

    for (const QString &raw : cmd.standardOutput.split('\n')) {
        const QString line = raw.trimmed();
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed();
        if (key.isEmpty() || key.contains(' ')) continue;
        result[key] = line.mid(eq + 1).trimmed();
    }

    if (!cmd.started || cmd.timedOut || cmd.exitCode != 0 || result.isEmpty()) {
        QString problem;
        if (!cmd.started) problem = "Rotation setup could not start";
        else if (cmd.timedOut) problem = elevated ? "Administrator setup timed out after 90 seconds" : "Sensor scan timed out after 25 seconds";
        else if (cmd.exitCode != 0)
            problem = QString("Rotation setup failed (exit code %1)").arg(cmd.exitCode);
        else problem = "Rotation setup returned no sensor information";

        const QString existing = result.value("REASON");
        result["REASON"] = existing.isEmpty() ? problem : existing + "; " + problem;
        const QString stderrText = cmd.standardError.left(1200);
        if (!stderrText.isEmpty()) {
            const QString existingHint = result.value("HINT");
            result["HINT"] = existingHint.isEmpty()
                ? stderrText : existingHint + "\n" + stderrText;
        }
    }
    return result;
}

// -----------------------------------------------------
// DisplayPage
// -----------------------------------------------------
class DisplayPage : public QWidget
{
public:
    explicit DisplayPage(QStackedWidget *stack)
        : QWidget(stack), m_stack(stack)
    {
        setStyleSheet("background:#282828; color:white; font-family:Sans;");

        cfg = loadCfg();

        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(40, 40, 40, 40);
        root->setSpacing(10);

        QLabel *title = new QLabel("Display");
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:42px; font-weight:bold;");
        root->addWidget(title);

        // Scroll area
        QScrollArea *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        // Keep pen and mouse clicks available for controls; touch gestures
        // scroll the settings page without stealing stylus button events.
        QScroller::grabGesture(scroll->viewport(), QScroller::TouchGesture);

        QWidget *wrap = new QWidget(scroll);
        QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
        wrapLay->setSpacing(10);
        wrapLay->setContentsMargins(0, 0, 0, 0);

        QFrame *outer = new QFrame(wrap);
        outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
        QVBoxLayout *outerLay = new QVBoxLayout(outer);
        outerLay->setContentsMargins(50, 30, 50, 30);
        outerLay->setSpacing(30);

        // Nightlight (BoolPill)
        outerLay->addWidget(makeNightlightRow());

        // Sleep Timeout (TimeoutPill)
        outerLay->addWidget(makeSleepTimeoutRow());

        // Adaptive Brightness (BoolPill)
        outerLay->addWidget(makeAdaptiveRow());

        // Boot animation toggle (BoolPill)
        outerLay->addWidget(makeBootRow());

        // Auto-Rotate (BoolPill) + rotation sensor detection/setup
        outerLay->addWidget(makeAutoRotateRow());
        outerLay->addWidget(makeRotationSensorCard());

        // Wacom AES, libinput stylus and integrated touchscreen settings.
        outerLay->addWidget(makeTouchPenCard());

        // Screen Info (mini-cards)
        outerLay->addWidget(makeScreenInfoCard());

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
    }

private:
    QStackedWidget *m_stack = nullptr;
    QMap<QString,QString> cfg;

    BoolPillToggle *m_nightlightPill = nullptr;
    BoolPillToggle *m_adaptivePill   = nullptr;
    BoolPillToggle *m_bootPill       = nullptr;
    TimeoutPill    *m_timeoutPill    = nullptr;

    BoolPillToggle *m_autoRotatePill = nullptr;
    BoolPillToggle *m_invertPill     = nullptr;
    QLabel *m_sensorFoundLabel  = nullptr;
    QLabel *m_sensorNameLabel   = nullptr;
    QLabel *m_sensorDriverLabel = nullptr;
    QLabel *m_sensorProxyLabel  = nullptr;
    QLabel *m_sensorCapabilityLabel = nullptr;
    QLabel *m_sensorOrientLabel = nullptr;
    QLabel *m_sensorHintLabel   = nullptr;
    QPushButton *m_scanBtn      = nullptr;
    QPushButton *m_adminSetupBtn = nullptr;

    QComboBox *m_penCombo = nullptr;
    QComboBox *m_penPressureCombo = nullptr;
    QComboBox *m_penButton1Combo = nullptr;
    QComboBox *m_penButton2Combo = nullptr;
    QComboBox *m_penOutputCombo = nullptr;
    BoolPillToggle *m_touchEnabledPill = nullptr;
    QLabel *m_penStatusLabel = nullptr;
    QLabel *m_penHardwareLabel = nullptr;
    QPushButton *m_penScanBtn = nullptr;
    QPushButton *m_penApplyBtn = nullptr;

    // ------------- config helpers (local) -------------
    QString readCfg(const QString &k, const QString &def = QString()) const
    {
        return cfg.contains(k) ? cfg.value(k) : def;
    }

    bool readCfgBool(const QString &k, bool def = false) const
    {
        QString v = readCfg(k, def ? "true" : "false");
        return (v == "true");
    }

    void writeCfg(const QString &k, const QString &v)
    {
        cfg[k] = v;
        saveCfg(cfg);
    }

    // ------------- rows -------------------------------
    QWidget *makeNightlightRow()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Nightlight");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);

        lay->addStretch();

        bool on = readCfgBool("display_nightlight", false);
        m_nightlightPill = new BoolPillToggle(on, card);
        lay->addWidget(m_nightlightPill);

        connect(m_nightlightPill, &QPushButton::clicked, this, [this]() {
            m_nightlightPill->toggle();
            bool state = m_nightlightPill->isOn();
            writeCfg("display_nightlight", state ? "true" : "false");
            // Placeholder: actual nightlight overlay can be applied later
        });

        return card;
    }

    QWidget *makeSleepTimeoutRow()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Sleep Timeout");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);

        lay->addStretch();

        int st = readCfg("display_sleep_timeout", "0").toInt();
        if (st < 0 || st > 5) st = 0;

        m_timeoutPill = new TimeoutPill(st, card);
        lay->addWidget(m_timeoutPill);

        applyTimeoutState(st);

        connect(m_timeoutPill, &QPushButton::clicked, this, [this]() {
            m_timeoutPill->advance();
            int s = m_timeoutPill->state();
            writeCfg("display_sleep_timeout", QString::number(s));
            applyTimeoutState(s);
        });

        return card;
    }

    QWidget *makeAdaptiveRow()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Adaptive Brightness");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);

        lay->addStretch();

        bool on = readCfgBool("display_adaptive_brightness", false);
        m_adaptivePill = new BoolPillToggle(on, card);
        lay->addWidget(m_adaptivePill);

        connect(m_adaptivePill, &QPushButton::clicked, this, [this]() {
            m_adaptivePill->toggle();
            bool state = m_adaptivePill->isOn();
            writeCfg("display_adaptive_brightness", state ? "true" : "false");
            // Placeholder for real adaptive logic
        });

        return card;
    }

    QWidget *makeBootRow()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Boot animation toggle");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);

        lay->addStretch();

        bool on = readCfgBool("display_boot_animation", true);
        m_bootPill = new BoolPillToggle(on, card);
        lay->addWidget(m_bootPill);

        // Apply initial state
        applyBootState(on);

        connect(m_bootPill, &QPushButton::clicked, this, [this]() {
            m_bootPill->toggle();
            bool state = m_bootPill->isOn();
            writeCfg("display_boot_animation", state ? "true" : "false");
            applyBootState(state);
        });

        return card;
    }

    void setRotationError(const QString &message)
    {
        if (!m_sensorHintLabel) return;
        m_sensorHintLabel->setText(message);
        m_sensorHintLabel->setVisible(!message.isEmpty());
    }

    QWidget *makeAutoRotateRow()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Auto-Rotate");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);
        lay->addStretch();

        // Show the actual running state even if the configuration is stale.
        m_autoRotatePill = new BoolPillToggle(rotateMonitorRunning(), card);
        lay->addWidget(m_autoRotatePill);

        connect(m_autoRotatePill, &QPushButton::clicked, this, [this]() {
            const bool desired = !m_autoRotatePill->isOn();
            const QString script = rotateScriptDir() + "/alternix-rotate-toggle.sh";
            if (!QFileInfo::exists(script)) {
                setRotationError("Auto-Rotate script missing: " + script);
                return;
            }

            // The controller starts the monitor detached itself. Wait for its
            // short command to exit so missing commands and permissions errors
            // are actually reported to the user.
            QProcess controller;
            controller.start("bash", {script, desired ? "on" : "off"});
            if (!controller.waitForFinished(4500) || controller.exitCode() != 0) {
                if (controller.state() != QProcess::NotRunning) controller.kill();
                const QString detail = QString::fromUtf8(controller.readAllStandardError()).trimmed();
                setRotationError(detail.isEmpty() ? "Auto-Rotate controller did not start successfully" : detail);
                return;
            }

            if (!desired) {
                m_autoRotatePill->setState(false);
                writeCfg("display_auto_rotate", "false");
                setRotationError(QString());
                refreshSensorInfo();
                return;
            }

            m_autoRotatePill->setEnabled(false);
            QTimer *check = new QTimer(this);
            check->setInterval(1000);
            int *attempts = new int(0);
            connect(check, &QTimer::timeout, this, [this, check, attempts]() {
                ++(*attempts);
                const bool ready = rotateMonitorRunning();
                if (ready || *attempts >= 18) {
                    check->stop();
                    check->deleteLater();
                    delete attempts;
                    m_autoRotatePill->setEnabled(true);
                    m_autoRotatePill->setState(ready);
                    writeCfg("display_auto_rotate", ready ? "true" : "false");
                    if (!ready) {
                        setRotationError("Sensor monitor has not reported a usable orientation. "
                                         "Check ~/.config/Alternix/logs/rotate-monitor.log "
                                         "and use Scan / Admin Setup below.");
                    } else {
                        setRotationError(QString());
                    }
                    refreshSensorInfo();
                }
            });
            check->start();
        });

        return card;
    }

    QWidget *makeRotationSensorCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30, 20, 30, 20);
        v->setSpacing(10);

        QHBoxLayout *hdrRow = new QHBoxLayout();
        QLabel *header = new QLabel("Rotation Sensor");
        header->setStyleSheet("font-size:26px; font-weight:bold;");
        hdrRow->addWidget(header);
        hdrRow->addStretch();

        m_scanBtn = makeBtn("Scan", "white");
        m_scanBtn->setMinimumSize(160, 54);
        hdrRow->addWidget(m_scanBtn);
        m_adminSetupBtn = makeBtn("Admin Setup", "white");
        m_adminSetupBtn->setMinimumSize(210, 54);
        hdrRow->addWidget(m_adminSetupBtn);
        v->addLayout(hdrRow);

        auto addRow = [&](const QString &name, QLabel *&valueLabel) {
            QHBoxLayout *h = new QHBoxLayout();
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(6);
            QLabel *l = new QLabel(name);
            valueLabel = new QLabel("Not scanned yet");
            valueLabel->setStyleSheet("color:#e0e0e0;");
            valueLabel->setWordWrap(true);
            h->addWidget(l);
            h->addStretch();
            h->addWidget(valueLabel, 1);
            v->addLayout(h);
        };

        addRow("Sensor detected", m_sensorFoundLabel);
        addRow("Sensor name",     m_sensorNameLabel);
        addRow("Driver / module", m_sensorDriverLabel);
        addRow("iio-sensor-proxy", m_sensorProxyLabel);
        addRow("Accelerometer available", m_sensorCapabilityLabel);
        addRow("Orientation",     m_sensorOrientLabel);

        // Invert direction — some accelerometers are mounted the other
        // way round on this exact panel, so left/right can come out
        // swapped. Flip this instead of touching the script.
        QHBoxLayout *invRow = new QHBoxLayout();
        QLabel *invLbl = new QLabel("Invert rotation direction");
        invRow->addWidget(invLbl);
        invRow->addStretch();

        bool inverted = QFile::exists(rotateInvertPath())
            && QString::fromUtf8(readSmallFile(rotateInvertPath())).trimmed() == "yes";
        m_invertPill = new BoolPillToggle(inverted, card);
        invRow->addWidget(m_invertPill);
        v->addLayout(invRow);

        connect(m_invertPill, &QPushButton::clicked, this, [this]() {
            m_invertPill->toggle();
            writeSmallFile(rotateInvertPath(), m_invertPill->isOn() ? "yes" : "no");
            QProcess::startDetached("bash", {rotateScriptDir() + "/alternix-rotate-toggle.sh", "refresh"});
        });

        // Only shown when the scan didn't find/start something — carries
        // the REASON + HINT the detector script worked out (dmesg-based
        // diagnosis, log file paths, etc) instead of leaving a bare "no".
        m_sensorHintLabel = new QLabel("");
        m_sensorHintLabel->setWordWrap(true);
        m_sensorHintLabel->setStyleSheet("color:#ffb454; font-size:20px;");
        m_sensorHintLabel->setVisible(false);
        v->addWidget(m_sensorHintLabel);

        const auto scan = [this](bool admin) {
            static bool scanning = false;
            if (scanning) return;
            scanning = true;
            m_scanBtn->setEnabled(false);
            m_adminSetupBtn->setEnabled(false);
            QPushButton *active = admin ? m_adminSetupBtn : m_scanBtn;
            const QString caption = admin ? "Admin Setup" : "Scan";
            QMap<QString, QString> r = runRotateScan(admin, [active](int frame) {
                active->setText(QString::fromUtf8(ROTATE_SPIN_FRAMES[frame]) + "  Working…");
            });
            applyScanResult(r);
            active->setText(caption);
            m_scanBtn->setEnabled(true);
            m_adminSetupBtn->setEnabled(true);
            scanning = false;
        };
        connect(m_scanBtn, &QPushButton::clicked, this, [scan]() { scan(false); });
        connect(m_adminSetupBtn, &QPushButton::clicked, this, [scan]() { scan(true); });

        // Do an initial silent status read (no driver poking) so the
        // page isn't blank on first open.
        QTimer::singleShot(0, this, [this]() { refreshSensorInfo(); });

        return card;
    }

    // ------------- touch / stylus settings -----------------
    static QString readPenSetting(const QString &key, const QString &def = QString())
    {
        QFile file(QDir::homePath() + "/.config/Alternix/stylus.conf");
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return def;
        const QByteArray prefix = (key + "=").toUtf8();
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.startsWith(prefix))
                return QString::fromUtf8(line.mid(prefix.size()));
        }
        return def;
    }

    static QComboBox *makePenCombo(QWidget *parent)
    {
        QComboBox *combo = new QComboBox(parent);
        combo->setMinimumHeight(54);
        combo->setMinimumWidth(220);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        combo->setStyleSheet(
            "QComboBox { background:#555555; color:white; border:1px solid #777777; "
            "border-radius:16px; padding:5px 16px; font-size:21px; }"
            "QComboBox::drop-down { border:0px; width:40px; }"
            "QComboBox QAbstractItemView { background:#444444; color:white; "
            "selection-background-color:#33aaff; font-size:21px; }");
        return combo;
    }

    QWidget *makeTouchPenCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30, 24, 30, 24);
        v->setSpacing(14);

        QHBoxLayout *header = new QHBoxLayout();
        QLabel *title = new QLabel("Touchscreen & Stylus");
        title->setStyleSheet("font-size:28px; font-weight:bold;");
        header->addWidget(title);
        header->addStretch();
        m_penScanBtn = makeBtn("Detect", "white");
        m_penScanBtn->setMinimumWidth(175);
        header->addWidget(m_penScanBtn);
        v->addLayout(header);

        QLabel *hint = new QLabel(
            "Barrel button 1 = lower button (nearest the pen tip); "
            "barrel button 2 = upper button (nearest the end cap). "
            "Wacom AES pens may expose the lower button as a separate eraser tool. "
            "The third button on the end cap is a separate Bluetooth shortcut, "
            "not an X11 barrel button. Settings apply to the current user.");
        hint->setWordWrap(true);
        hint->setStyleSheet("color:#dddddd; font-size:19px;");
        v->addWidget(hint);

        m_penHardwareLabel = new QLabel("Detecting touchscreen and pen devices…");
        m_penHardwareLabel->setWordWrap(true);
        m_penHardwareLabel->setStyleSheet("font-size:20px; color:#e0e0e0;");
        v->addWidget(m_penHardwareLabel);

        const auto addCombo = [card, v](const QString &caption, QComboBox *&combo) {
            QWidget *row = new QWidget(card);
            row->setStyleSheet("background:transparent;");
            QHBoxLayout *h = new QHBoxLayout(row);
            h->setContentsMargins(0, 6, 0, 6);
            QLabel *label = new QLabel(caption, row);
            label->setMinimumWidth(215);
            label->setStyleSheet("font-size:21px;");
            h->addWidget(label);
            combo = makePenCombo(row);
            h->addWidget(combo, 1);
            v->addWidget(row);
        };
        addCombo("Stylus", m_penCombo);
        m_penCombo->addItem("All detected pens", QString());

        addCombo("Pressure response", m_penPressureCombo);
        m_penPressureCombo->addItem("Soft (lighter touch)", "soft");
        m_penPressureCombo->addItem("Normal", "normal");
        m_penPressureCombo->addItem("Firm (more pressure)", "firm");

        addCombo("Barrel button 1 (lower)", m_penButton1Combo);
        addCombo("Barrel button 2 (upper)", m_penButton2Combo);
        for (QComboBox *combo : {m_penButton1Combo, m_penButton2Combo}) {
            combo->addItem("Right click", "right");
            combo->addItem("Middle click", "middle");
            combo->addItem("Left click", "left");
            combo->addItem("Back", "back");
            combo->addItem("Forward", "forward");
            combo->addItem("Disabled", "disabled");
        }
        addCombo("Map to display", m_penOutputCombo);
        m_penOutputCombo->addItem("Built-in / automatic", "auto");

        QWidget *touchRow = new QWidget(card);
        touchRow->setStyleSheet("background:transparent;");
        QHBoxLayout *touchLay = new QHBoxLayout(touchRow);
        touchLay->setContentsMargins(0, 4, 0, 4);
        QLabel *touchLabel = new QLabel("Touchscreen enabled");
        touchLabel->setStyleSheet("font-size:21px;");
        touchLay->addWidget(touchLabel);
        touchLay->addStretch();
        m_touchEnabledPill = new BoolPillToggle(readPenSetting("TOUCH_ENABLED", "yes") != "no", card);
        touchLay->addWidget(m_touchEnabledPill);
        v->addWidget(touchRow);
        connect(m_touchEnabledPill, &QPushButton::clicked, this, [this]() {
            m_touchEnabledPill->toggle();
        });

        const auto selectValue = [](QComboBox *combo, const QString &value) {
            const int index = combo->findData(value);
            if (index >= 0) combo->setCurrentIndex(index);
        };
        selectValue(m_penPressureCombo, readPenSetting("PRESSURE", "normal"));
        selectValue(m_penButton1Combo, readPenSetting("BUTTON_1", "right"));
        selectValue(m_penButton2Combo, readPenSetting("BUTTON_2", "middle"));

        m_penApplyBtn = makeBtn("Apply & Save", "white");
        m_penApplyBtn->setStyleSheet(altBtnBright());
        v->addWidget(m_penApplyBtn, 0, Qt::AlignRight);

        m_penStatusLabel = new QLabel;
        m_penStatusLabel->setWordWrap(true);
        m_penStatusLabel->setStyleSheet("font-size:19px; color:#ffcf80;");
        v->addWidget(m_penStatusLabel);

        connect(m_penScanBtn, &QPushButton::clicked, this, [this]() { scanPenHardware(); });
        connect(m_penApplyBtn, &QPushButton::clicked, this, [this]() { applyPenSettings(); });
        QTimer::singleShot(0, this, [this]() { scanPenHardware(); });
        return card;
    }

    void scanPenHardware()
    {
        const QString backend = rotateScriptDir() + "/alternix-stylus-apply.sh";
        if (!QFileInfo::exists(backend)) {
            m_penHardwareLabel->setText("Pen backend missing: " + backend);
            return;
        }
        m_penScanBtn->setEnabled(false);
        QProcess scan;
        scan.start("bash", {backend, "scan"});
        if (!scan.waitForFinished(6500) || scan.exitStatus() != QProcess::NormalExit || scan.exitCode() != 0) {
            if (scan.state() != QProcess::NotRunning) scan.kill();
            const QString out = QString::fromUtf8(scan.readAllStandardOutput()).trimmed();
            const QString err = QString::fromUtf8(scan.readAllStandardError()).trimmed();
            m_penHardwareLabel->setText("Detection unavailable: " + (out.isEmpty() ? err : out));
            m_penScanBtn->setEnabled(true);
            return;
        }

        QString savedPen = m_penCombo->currentData().toString();
        QString savedOutput = m_penOutputCombo->currentData().toString();
        if (m_penCombo->count() <= 1) savedPen = readPenSetting("PEN_NAME");
        if (m_penOutputCombo->count() <= 1) savedOutput = readPenSetting("OUTPUT", "auto");
        m_penCombo->clear();
        m_penCombo->addItem("All detected pens", QString());
        m_penOutputCombo->clear();
        m_penOutputCombo->addItem("Built-in / automatic", "auto");
        QStringList penNames, touchNames;
        int wacomCount = 0;
        int aesCount = 0;
        for (const QString &line : QString::fromUtf8(scan.readAllStandardOutput()).split('\n')) {
            const QStringList fields = line.trimmed().split('\t');
            if (fields.size() < 2) continue;
            if (fields.at(0) == "PEN" && fields.size() >= 4) {
                const QString name = fields.at(2);
                // Eraser tool entries have their own XInput endpoints; avoid
                // applying the tip button layout to a separate eraser.
                if (name.contains("eraser", Qt::CaseInsensitive)) continue;
                if (!penNames.contains(name)) {
                    penNames.append(name);
                    m_penCombo->addItem(name + (fields.at(3) == "wacom" ? " (Wacom)" : " (libinput)"), name);
                }
                if (fields.at(3) == "wacom") ++wacomCount;
            } else if (fields.at(0) == "TOUCH" && fields.size() >= 4) {
                touchNames.append(fields.at(2));
            } else if (fields.at(0) == "LAYOUT" && fields.size() >= 3) {
                if (fields.at(2) == "aes-eraser") ++aesCount;
            } else if (fields.at(0) == "OUTPUT") {
                m_penOutputCombo->addItem(fields.at(1), fields.at(1));
            }
        }
        const int penIndex = m_penCombo->findData(savedPen);
        if (penIndex >= 0) m_penCombo->setCurrentIndex(penIndex);
        const int outIndex = m_penOutputCombo->findData(savedOutput);
        if (outIndex >= 0) m_penOutputCombo->setCurrentIndex(outIndex);
        m_penHardwareLabel->setText(QString("Detected %1 pen device(s), %2 touchscreen(s). %3 %4")
            .arg(penNames.size()).arg(touchNames.size())
            .arg(wacomCount ? "Wacom X11 controls available." :
                 "libinput controls will be used where supported.")
            .arg(aesCount ? QString("%1 pen(s) have a separate lower-barrel eraser switch.").arg(aesCount)
                          : QString()));
        m_penStatusLabel->setText(penNames.isEmpty()
            ? "No compatible stylus currently detected. Touch may still work; check the kernel HID/Wacom device if a pen is expected."
            : QString());
        m_penScanBtn->setEnabled(true);
    }

    void applyPenSettings()
    {
        const QString backend = rotateScriptDir() + "/alternix-stylus-apply.sh";
        if (!QFileInfo::exists(backend)) {
            m_penStatusLabel->setText("Pen backend missing: " + backend);
            return;
        }
        m_penApplyBtn->setEnabled(false);
        QProcess apply;
        apply.start("bash", {
            backend, "save", m_penCombo->currentData().toString(),
            m_penPressureCombo->currentData().toString(),
            m_penButton1Combo->currentData().toString(),
            m_penButton2Combo->currentData().toString(),
            m_touchEnabledPill->isOn() ? "yes" : "no",
            m_penOutputCombo->currentData().toString()
        });
        if (!apply.waitForFinished(8000) || apply.exitStatus() != QProcess::NormalExit || apply.exitCode() != 0) {
            if (apply.state() != QProcess::NotRunning) apply.kill();
            const QString error = QString::fromUtf8(apply.readAllStandardError()).trimmed();
            const QString out = QString::fromUtf8(apply.readAllStandardOutput()).trimmed();
            m_penStatusLabel->setText("Could not apply settings: " + (error.isEmpty() ? out : error));
        } else {
            QStringList warnings, details;
            for (const QString &line : QString::fromUtf8(apply.readAllStandardOutput()).split('\n')) {
                if (line.startsWith("WARNING=")) warnings << line.mid(8);
                else if (line.startsWith("INFO=")) details << line.mid(5);
            }
            QString status = warnings.isEmpty() ? "Pen and touch settings saved and applied."
                                                : "Settings saved with device limitations:\n" + warnings.join("\n");
            if (!details.isEmpty()) status += "\n" + details.join("\n");
            m_penStatusLabel->setText(status);
        }
        m_penApplyBtn->setEnabled(true);
    }

    QWidget *makeScreenInfoCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30, 20, 30, 20);
        v->setSpacing(10);

        QLabel *header = new QLabel("Screen Info");
        header->setStyleSheet("font-size:26px; font-weight:bold;");
        v->addWidget(header);

        QList<QScreen*> screens = QGuiApplication::screens();
        if (screens.isEmpty()) {
            QLabel *none = new QLabel("No screens detected");
            none->setStyleSheet("font-size:22px;");
            v->addWidget(none);
        } else {
            for (int i = 0; i < screens.size(); ++i) {
                v->addWidget(makeScreenMiniCard(screens[i], i));
            }
        }

        return card;
    }

    QWidget *makeScreenMiniCard(QScreen *s, int idx)
    {
        QFrame *mini = new QFrame;
        mini->setStyleSheet("QFrame { background:#555555; border-radius:18px; }");

        QVBoxLayout *v = new QVBoxLayout(mini);
        v->setContentsMargins(20, 12, 20, 12);
        v->setSpacing(4);

        QLabel *hdr = new QLabel(QString("Screen %1: %2").arg(idx).arg(s->name()));
        hdr->setStyleSheet("font-size:24px; font-weight:bold;");
        v->addWidget(hdr);

        auto addRow = [&](const QString &name, const QString &val) {
            QHBoxLayout *h = new QHBoxLayout();
            h->setContentsMargins(0,0,0,0);
            h->setSpacing(6);
            QLabel *l = new QLabel(name);
            QLabel *r = new QLabel(val);
            r->setStyleSheet("color:#e0e0e0;");
            h->addWidget(l);
            h->addStretch();
            h->addWidget(r);
            v->addLayout(h);
        };

        addRow("Resolution",
               QString("%1 x %2").arg(s->size().width()).arg(s->size().height()));
        addRow("Refresh",
               QString("%1 Hz").arg((int)s->refreshRate()));
        addRow("DPI",
               QString::number((int)s->logicalDotsPerInch()));
        addRow("Physical",
               QString("%1mm x %2mm")
               .arg(s->physicalSize().width())
               .arg(s->physicalSize().height()));

        return mini;
    }

    // ------------- backends ---------------------------
    void applyTimeoutState(int st)
    {
        int secs = TimeoutPill::secondsForState(st);
        QString cmd = QString("alternix-set-sleep-timeout %1").arg(secs);
        system(cmd.toUtf8().constData());
    }

    void applyBootState(bool on)
    {
        if (on)
            system("alternix-toggle-bootanimation on");
        else
            system("alternix-toggle-bootanimation off");
    }

    // Lightweight, read-only status check — does NOT load kernel modules
    // or start iio-sensor-proxy. That's what the "Scan / Setup" button
    // (runRotateScan, which runs alternix-rotate-setup.sh) is for. This
    // just refreshes what's currently running so the page isn't blank.
    void refreshSensorInfo()
    {
        const RotateSensorInfo sensor = readRotateSensorInfo();
        if (m_sensorFoundLabel) {
            if (sensor.found) m_sensorFoundLabel->setText("Yes");
            else if (m_sensorFoundLabel->text() == "Not scanned yet")
                m_sensorFoundLabel->setText("Not detected yet (use Scan / Setup)");
        }
        if (sensor.found) {
            if (m_sensorNameLabel) m_sensorNameLabel->setText(sensor.name);
            if (m_sensorDriverLabel)
                m_sensorDriverLabel->setText(sensor.driver.isEmpty() ? "—" : sensor.driver);
        }

        bool sensorProxyOnBus = false;
        if (m_sensorOrientLabel || m_sensorProxyLabel) {
            QProcess p;
            p.start("dbus-send", {
                "--system", "--print-reply", "--dest=net.hadess.SensorProxy",
                "/net/hadess/SensorProxy", "org.freedesktop.DBus.Properties.Get",
                "string:net.hadess.SensorProxy", "string:AccelerometerOrientation"
            });
            if (!p.waitForFinished(1200)) {
                p.kill();
                p.waitForFinished(200);
            }
            const QString out = QString::fromUtf8(p.readAllStandardOutput());
            const QRegularExpression re("variant\\s+string\\s+\"([^\"]+)\"");
            const QRegularExpressionMatch match = re.match(out);
            sensorProxyOnBus = p.exitStatus() == QProcess::NormalExit
                               && p.exitCode() == 0;
            if (m_sensorOrientLabel)
                m_sensorOrientLabel->setText(match.hasMatch() ? match.captured(1) : "Unknown");
        }
        if (m_sensorProxyLabel) {
            m_sensorProxyLabel->setText(sensorProxyOnBus || rotationProxyRunning()
                ? "Running" : "Not running (use Admin Setup)");
        }
        if (m_sensorCapabilityLabel) {
            QProcess cap;
            cap.start("dbus-send", {
                "--system", "--print-reply", "--reply-timeout=1000",
                "--dest=net.hadess.SensorProxy", "/net/hadess/SensorProxy",
                "org.freedesktop.DBus.Properties.Get",
                "string:net.hadess.SensorProxy", "string:HasAccelerometer"
            });
            if (!cap.waitForFinished(1400)) { cap.kill(); cap.waitForFinished(150); }
            const QString resp = QString::fromUtf8(cap.readAllStandardOutput());
            m_sensorCapabilityLabel->setText(resp.contains("boolean true") ? "Yes" :
                (resp.contains("boolean false") ? "No" : "Unknown"));
        }
        if (m_autoRotatePill && m_autoRotatePill->isEnabled())
            m_autoRotatePill->setState(rotateMonitorRunning());
    }

    // Applies the full output of alternix-rotate-setup.sh (driver load +
    // sysfs scan + iio-sensor-proxy start) to the info labels.
    void applyScanResult(const QMap<QString, QString> &r)
    {
        if (m_sensorFoundLabel) {
            if (!r.contains("SENSOR_FOUND"))
                m_sensorFoundLabel->setText("Setup failed — detection not completed");
            else
                m_sensorFoundLabel->setText(r.value("SENSOR_FOUND") == "yes"
                    ? "Yes" : "No sensor found by setup");
        }

        if (m_sensorNameLabel) {
            QString name = r.value("SENSOR_NAME");
            m_sensorNameLabel->setText(name.isEmpty() ? "—" : name);
        }

        if (m_sensorDriverLabel) {
            QString drv = r.value("DRIVER_LOADED", "none");
            m_sensorDriverLabel->setText(drv.isEmpty() ? "none" : drv);
        }

        if (m_sensorProxyLabel)
            m_sensorProxyLabel->setText(r.value("PROXY_RUNNING", "no") == "yes" ? "Running" : "Not running");

        if (m_sensorCapabilityLabel)
            m_sensorCapabilityLabel->setText(r.value("HAS_ACCELEROMETER", "unknown"));

        if (m_sensorOrientLabel) {
            QString o = r.value("ORIENTATION", "unknown");
            m_sensorOrientLabel->setText(o.isEmpty() ? "unknown" : o);
        }

        if (m_sensorHintLabel) {
            QString reason = r.value("REASON");
            QString hint   = r.value("HINT");
            QString ranAsRoot = r.value("RAN_AS_ROOT");

            // A permission-flavoured reason is worth surfacing even when
            // the sensor itself was found fine — it's usually *why* the
            // proxy won't stay running.
            if (ranAsRoot == "no" && r.value("PROXY_RUNNING") != "yes" && r.value("PROXY_INSTALLED") == "yes") {
                QString privNote = "The rotation setup was run without administrative "
                    "privileges. Kernel-module or system-service changes may require "
                    "authorisation via Admin Setup; do not run the entire "
                    "settings application as root.";
                reason = reason.isEmpty() ? "Insufficient privileges" : reason;
                hint = hint.isEmpty() ? privNote : hint + "\n" + privNote;
            }

            if (reason.isEmpty() && hint.isEmpty()) {
                m_sensorHintLabel->setVisible(false);
            } else {
                QString text = reason;
                if (!hint.isEmpty())
                    text += (text.isEmpty() ? QString() : QString("\n")) + hint;
                m_sensorHintLabel->setText(text);
                m_sensorHintLabel->setVisible(true);
            }
        }
    }
};

// -----------------------------------------------------
// Factory for osm-settings
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new DisplayPage(stack);
}
