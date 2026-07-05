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
#include <QScreen>
#include <QGuiApplication>
#include <QSizePolicy>
#include <QTimer>

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

static bool rotateMonitorRunning()
{
    return QFile::exists(rotatePidPath());
}

// Runs alternix-rotate-setup.sh, which scans /sys/bus/iio for an
// accelerometer, tries to load the driver modules it needs, and starts
// iio-sensor-proxy if it isn't already running. Returns the KEY=VALUE
// lines it printed.
static QMap<QString, QString> runRotateScan()
{
    QMap<QString, QString> result;
    QString script = rotateScriptDir() + "/alternix-rotate-setup.sh";

    QProcess p;
    p.start("bash", { script });
    p.waitForFinished(45000);  // multiple driver passes + possible apt installs
    QString out = QString::fromUtf8(p.readAllStandardOutput());

    for (const QString &lineRaw : out.split('\n')) {
        QString line = lineRaw.trimmed();
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        result[line.left(eq)] = line.mid(eq + 1);
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
    QLabel *m_sensorOrientLabel = nullptr;
    QLabel *m_sensorHintLabel   = nullptr;
    QPushButton *m_scanBtn      = nullptr;

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

        // Config flag and the actual running monitor can disagree (e.g.
        // if the sensor setup failed) — reflect what's really running.
        bool on = readCfgBool("display_auto_rotate", false) && rotateMonitorRunning();
        m_autoRotatePill = new BoolPillToggle(on, card);
        lay->addWidget(m_autoRotatePill);

        connect(m_autoRotatePill, &QPushButton::clicked, this, [this]() {
            m_autoRotatePill->toggle();
            bool state = m_autoRotatePill->isOn();
            writeCfg("display_auto_rotate", state ? "true" : "false");

            QString script = rotateScriptDir() + "/alternix-rotate-toggle.sh";
            QProcess::startDetached("bash", { script, state ? "on" : "off" });

            // Give the monitor script a moment to (de)claim the sensor
            // and write/remove its pidfile, then refresh sensor status.
            QTimer::singleShot(600, this, [this]() { refreshSensorInfo(); });
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

        m_scanBtn = makeBtn("Scan / Setup", "white");
        m_scanBtn->setMinimumSize(220, 54);
        hdrRow->addWidget(m_scanBtn);
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
        });

        // Only shown when the scan didn't find/start something — carries
        // the REASON + HINT the detector script worked out (dmesg-based
        // diagnosis, log file paths, etc) instead of leaving a bare "no".
        m_sensorHintLabel = new QLabel("");
        m_sensorHintLabel->setWordWrap(true);
        m_sensorHintLabel->setStyleSheet("color:#ffb454; font-size:20px;");
        m_sensorHintLabel->setVisible(false);
        v->addWidget(m_sensorHintLabel);

        connect(m_scanBtn, &QPushButton::clicked, this, [this]() {
            m_scanBtn->setEnabled(false);
            m_scanBtn->setText("Scanning (up to a minute)...");
            // runRotateScan() blocks — it's trying several driver stacks,
            // possibly an apt install, and re-scanning between each — but
            // that's fine here since it's an explicit user-triggered action.
            QMap<QString, QString> r = runRotateScan();
            applyScanResult(r);
            m_scanBtn->setText("Scan / Setup");
            m_scanBtn->setEnabled(true);
        });

        // Do an initial silent status read (no driver poking) so the
        // page isn't blank on first open.
        QTimer::singleShot(0, this, [this]() { refreshSensorInfo(); });

        return card;
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
        if (m_sensorProxyLabel) {
            QProcess p;
            p.start("bash", {"-c",
                "pgrep -x iio-sensor-proxy >/dev/null 2>&1 && echo yes || echo no"});
            p.waitForFinished(300);
            bool running = (QString::fromUtf8(p.readAll()).trimmed() == "yes");
            m_sensorProxyLabel->setText(running ? "Running" : "Not running (use Scan / Setup)");
        }

        if (m_sensorOrientLabel) {
            QProcess p2;
            p2.start("bash", {"-c",
                "dbus-send --system --print-reply --dest=net.hadess.SensorProxy "
                "/net/hadess/SensorProxy org.freedesktop.DBus.Properties.Get "
                "string:net.hadess.SensorProxy string:AccelerometerOrientation "
                "2>/dev/null | awk -F'\"' '/string/{print $2}'"});
            p2.waitForFinished(500);
            QString orient = QString::fromUtf8(p2.readAll()).trimmed();
            m_sensorOrientLabel->setText(orient.isEmpty() ? "Unknown" : orient);
        }
    }

    // Applies the full output of alternix-rotate-setup.sh (driver load +
    // sysfs scan + iio-sensor-proxy start) to the info labels.
    void applyScanResult(const QMap<QString, QString> &r)
    {
        if (m_sensorFoundLabel)
            m_sensorFoundLabel->setText(r.value("SENSOR_FOUND", "no") == "yes" ? "Yes" : "No — check cabling / BIOS sensor settings");

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

        if (m_sensorOrientLabel) {
            QString o = r.value("ORIENTATION", "unknown");
            m_sensorOrientLabel->setText(o.isEmpty() ? "unknown" : o);
        }

        if (m_sensorHintLabel) {
            QString reason = r.value("REASON");
            QString hint   = r.value("HINT");

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
