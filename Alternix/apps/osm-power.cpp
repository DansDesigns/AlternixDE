#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QElapsedTimer>
#include <QDateTime>
#include <QPainter>
#include <QPolygonF>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVector>
#include <QDebug>

// ────────────────────────────────
// /proc + /sys sampling helpers
// ────────────────────────────────

static QString readFirstLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QString line = QString::fromLatin1(f.readLine()).trimmed();
    f.close();
    return line;
}

// Aggregate CPU jiffies from /proc/stat.  idle includes iowait.
static bool readCpuTotals(quint64 &total, quint64 &idle)
{
    QString line = readFirstLine("/proc/stat");
    if (!line.startsWith("cpu"))
        return false;

    QStringList parts = line.simplified().split(QLatin1Char(' '));
    if (parts.size() < 5)
        return false;

    quint64 t = 0;
    for (int i = 1; i < parts.size(); ++i)
        t += parts.at(i).toULongLong();

    quint64 idleAll = parts.at(4).toULongLong();          // idle
    if (parts.size() > 5)
        idleAll += parts.at(5).toULongLong();             // iowait

    total = t;
    idle  = idleAll;
    return true;
}

// MemTotal / used (kB) from /proc/meminfo.
static bool readMem(quint64 &totalKb, quint64 &usedKb)
{
    QFile f("/proc/meminfo");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    quint64 memTotal = 0, memAvail = 0, memFree = 0, buffers = 0, cached = 0;
    bool haveAvail = false;

    while (!f.atEnd()) {
        QString line = QString::fromLatin1(f.readLine()).simplified();
        int c = line.indexOf(QLatin1Char(':'));
        if (c < 0)
            continue;
        QString key  = line.left(c);
        QString rest = line.mid(c + 1).trimmed();
        int sp = rest.indexOf(QLatin1Char(' '));
        if (sp > 0)
            rest = rest.left(sp);
        quint64 v = rest.toULongLong();

        if (key == "MemTotal")           memTotal = v;
        else if (key == "MemAvailable")  { memAvail = v; haveAvail = true; }
        else if (key == "MemFree")       memFree = v;
        else if (key == "Buffers")       buffers = v;
        else if (key == "Cached")        cached = v;
    }
    f.close();

    if (memTotal == 0)
        return false;

    quint64 avail = haveAvail ? memAvail : (memFree + buffers + cached);
    if (avail > memTotal)
        avail = memTotal;

    totalKb = memTotal;
    usedKb  = memTotal - avail;
    return true;
}

// Summed rx/tx byte counters from /proc/net/dev, excluding loopback.
static bool readNetBytes(quint64 &rx, quint64 &tx)
{
    QFile f("/proc/net/dev");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    rx = 0;
    tx = 0;
    int lineNo = 0;

    while (!f.atEnd()) {
        QString line = QString::fromLatin1(f.readLine());
        if (++lineNo <= 2)                 // two header lines
            continue;
        int c = line.indexOf(QLatin1Char(':'));
        if (c < 0)
            continue;
        QString iface = line.left(c).trimmed();
        if (iface == "lo")
            continue;
        QStringList fields = line.mid(c + 1).simplified().split(QLatin1Char(' '));
        if (fields.size() < 9)
            continue;
        rx += fields.at(0).toULongLong();  // rx bytes
        tx += fields.at(8).toULongLong();  // tx bytes
    }
    f.close();
    return true;
}

// GPU load source.  amdgpu exposes a real busy percentage; i915 does not
// (intel_gpu_top needs perf privileges), so we fall back to current GPU
// clock as a percentage of maximum, which tracks load closely enough.
struct GpuSource {
    enum Mode { None, BusyPercent, FreqRatio };
    Mode mode;
    QString busyPath;
    QString curFreqPath;
    QString maxFreqPath;
    GpuSource() : mode(None) {}
};

static GpuSource detectGpu()
{
    GpuSource g;

    QDir drm("/sys/class/drm");
    if (!drm.exists())
        return g;

    QStringList entries = drm.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (int i = 0; i < entries.size(); ++i) {
        const QString name = entries.at(i);
        if (!name.startsWith("card"))
            continue;
        bool allDigits = name.length() > 4;
        for (int k = 4; k < name.length() && allDigits; ++k)
            if (!name.at(k).isDigit())
                allDigits = false;
        if (!allDigits)
            continue;

        const QString base = "/sys/class/drm/" + name;

        // amdgpu / some others
        if (QFileInfo::exists(base + "/device/gpu_busy_percent")) {
            g.mode     = GpuSource::BusyPercent;
            g.busyPath = base + "/device/gpu_busy_percent";
            return g;
        }

        // i915, older sysfs layout
        if (QFileInfo::exists(base + "/gt_cur_freq_mhz") &&
            QFileInfo::exists(base + "/gt_max_freq_mhz")) {
            g.mode        = GpuSource::FreqRatio;
            g.curFreqPath = base + "/gt_cur_freq_mhz";
            g.maxFreqPath = base + "/gt_max_freq_mhz";
            return g;
        }

        // i915, newer per-gt sysfs layout
        if (QFileInfo::exists(base + "/gt/gt0/rps_cur_freq_mhz") &&
            QFileInfo::exists(base + "/gt/gt0/rps_max_freq_mhz")) {
            g.mode        = GpuSource::FreqRatio;
            g.curFreqPath = base + "/gt/gt0/rps_cur_freq_mhz";
            g.maxFreqPath = base + "/gt/gt0/rps_max_freq_mhz";
            return g;
        }
    }

    return g;
}

static QString formatRate(double bytesPerSec)
{
    if (bytesPerSec >= 1048576.0)
        return QString("%1 MB/s").arg(bytesPerSec / 1048576.0, 0, 'f', 1);
    if (bytesPerSec >= 1024.0)
        return QString("%1 KB/s").arg(bytesPerSec / 1024.0, 0, 'f', 0);
    return QString("%1 B/s").arg(bytesPerSec, 0, 'f', 0);
}

// ────────────────────────────────
// Sparkline graph widget
// ────────────────────────────────
class GraphWidget : public QWidget {
public:
    // fixedMax > 0  : fixed vertical scale (e.g. 100 for a percentage)
    // fixedMax == 0 : auto-scale, never below autoFloor
    GraphWidget(const QString &title, const QColor &colour,
                double fixedMax, double autoFloor, QWidget *parent = nullptr)
        : QWidget(parent),
          m_title(title),
          m_readout("--"),
          m_colour(colour),
          m_fixedMax(fixedMax),
          m_autoFloor(autoFloor),
          m_maxSamples(90)
    {
        setMinimumHeight(70);
    }

    void addSample(double value, const QString &readout)
    {
        if (value < 0.0)
            value = 0.0;
        m_samples.append(value);
        while (m_samples.size() > m_maxSamples)
            m_samples.removeFirst();
        m_readout = readout;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

        // Card background
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 18));
        p.drawRoundedRect(r, 14, 14);

        QFont f = font();
        f.setPointSize(qBound(9, height() / 12, 18));
        f.setBold(true);
        p.setFont(f);
        QFontMetrics fm(f);
        int textH = fm.height();

        QRectF plot = r.adjusted(10, textH + 8, -10, -10);
        if (plot.width() < 4 || plot.height() < 4)
            return;

        // Grid lines
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, 25), 1));
        for (int i = 1; i < 4; ++i) {
            double y = plot.top() + plot.height() * (i / 4.0);
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }

        // Vertical scale
        double maxVal = m_fixedMax;
        if (maxVal <= 0.0) {
            maxVal = m_autoFloor;
            for (int i = 0; i < m_samples.size(); ++i)
                if (m_samples.at(i) > maxVal)
                    maxVal = m_samples.at(i);
            maxVal *= 1.15;
        }
        if (maxVal <= 0.0)
            maxVal = 1.0;

        if (m_samples.size() >= 2) {
            double stepX = plot.width() / double(m_maxSamples - 1);
            int n        = m_samples.size();
            int startIdx = m_maxSamples - n;   // right-align the trace

            QPolygonF fill;
            QPolygonF line;
            fill << QPointF(plot.left() + startIdx * stepX, plot.bottom());
            for (int i = 0; i < n; ++i) {
                double frac = m_samples.at(i) / maxVal;
                if (frac > 1.0) frac = 1.0;
                if (frac < 0.0) frac = 0.0;
                QPointF pt(plot.left() + (startIdx + i) * stepX,
                           plot.bottom() - frac * plot.height());
                fill << pt;
                line << pt;
            }
            fill << QPointF(plot.left() + (m_maxSamples - 1) * stepX, plot.bottom());

            QColor top = m_colour;    top.setAlpha(170);
            QColor bot = m_colour;    bot.setAlpha(25);
            QLinearGradient grad(0, plot.top(), 0, plot.bottom());
            grad.setColorAt(0.0, top);
            grad.setColorAt(1.0, bot);

            p.setPen(Qt::NoPen);
            p.setBrush(grad);
            p.drawPolygon(fill);

            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(m_colour, 2));
            p.drawPolyline(line);
        }

        // Title / current value
        QRectF textRect(r.left() + 12, r.top() + 5, r.width() - 24, textH);
        p.setPen(QColor(255, 255, 255, 205));
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, m_title);
        p.setPen(m_colour.lighter(135));
        p.drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, m_readout);
    }

private:
    QString m_title;
    QString m_readout;
    QColor m_colour;
    double m_fixedMax;
    double m_autoFloor;
    int m_maxSamples;
    QVector<double> m_samples;
};

class PowerMenuWindow : public QWidget {
public:
    explicit PowerMenuWindow(QWidget *parent = nullptr)
        : QWidget(parent),
          panel(nullptr),
          helloLabel(nullptr),
          timeLabel(nullptr),
          statsPanel(nullptr),
          cpuGraph(nullptr),
          ramGraph(nullptr),
          netGraph(nullptr),
          gpuGraph(nullptr),
          lastCpuTotal(0),
          lastCpuIdle(0),
          lastRx(0),
          lastTx(0)
    {
        // Fullscreen, no decorations, overlay-style
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground, true);

        // Take up the full screen
        QScreen *scr = QGuiApplication::primaryScreen();
        if (scr) setGeometry(scr->geometry());

        // --- Central rounded "screen" panel ---
        panel = new QWidget(this);
        panel->setObjectName("panel");
        panel->setAutoFillBackground(false);

        QVBoxLayout *panelLayout = new QVBoxLayout(panel);
        panelLayout->setContentsMargins(30, 30, 30, 30);
        panelLayout->setSpacing(20);

        // Top row: Hello $USER    HH:MM
        QHBoxLayout *topRow = new QHBoxLayout();
        helloLabel = new QLabel(this);
        QString user = QString::fromLocal8Bit(qgetenv("USER"));
        if (user.isEmpty())
            user = QDir::homePath().split("/").last();
        helloLabel->setText("🐧 Hello " + user);
        helloLabel->setStyleSheet("color: white;");

        timeLabel = new QLabel("--:--", this);
        timeLabel->setStyleSheet("color: white;");
        timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        topRow->addWidget(helloLabel);
        topRow->addStretch(1);
        topRow->addWidget(timeLabel);

        panelLayout->addLayout(topRow);

        // Spacer
        panelLayout->addSpacing(10);

        // Icon row
        QHBoxLayout *iconRow = new QHBoxLayout();
        iconRow->setSpacing(30);
        iconRow->setAlignment(Qt::AlignHCenter);

        QWidget *lockWidget   = createIconButton("Lock",     QDir::homePath() + "/.config/qtile/images/lock.png");
        QWidget *sleepWidget  = createIconButton("Sleep",    QDir::homePath() + "/.config/qtile/images/sleep.png");
        QWidget *rebootWidget = createIconButton("Reboot",   QDir::homePath() + "/.config/qtile/images/restart.png");
        QWidget *powerWidget  = createIconButton("Power Off",QDir::homePath() + "/.config/qtile/images/shutdown.png");

        // Connect actions
        connect(lockWidget->findChild<QPushButton*>("btn"), &QPushButton::clicked,
                this, &PowerMenuWindow::doLock);
        connect(sleepWidget->findChild<QPushButton*>("btn"), &QPushButton::clicked,
                this, &PowerMenuWindow::doSleep);
        connect(rebootWidget->findChild<QPushButton*>("btn"), &QPushButton::clicked,
                this, &PowerMenuWindow::doReboot);
        connect(powerWidget->findChild<QPushButton*>("btn"), &QPushButton::clicked,
                this, &PowerMenuWindow::doPowerOff);

        iconRow->addStretch(1);
        iconRow->addWidget(lockWidget);
        iconRow->addWidget(sleepWidget);
        iconRow->addWidget(rebootWidget);
        iconRow->addWidget(powerWidget);
        iconRow->addStretch(1);

        panelLayout->addLayout(iconRow);

        // Spacer between icons and stats block
        panelLayout->addStretch(1);

        // Stats panel: live system graphs
        statsPanel = new QWidget(this);
        statsPanel->setObjectName("statsPanel");
        QGridLayout *statsLayout = new QGridLayout(statsPanel);
        statsLayout->setContentsMargins(22, 22, 22, 22);
        statsLayout->setHorizontalSpacing(16);
        statsLayout->setVerticalSpacing(16);

        cpuGraph = new GraphWidget("CPU",     QColor(0x4f, 0xc3, 0xf7), 100.0,    0.0,     statsPanel);
        ramGraph = new GraphWidget("RAM",     QColor(0x81, 0xc7, 0x84), 100.0,    0.0,     statsPanel);
        netGraph = new GraphWidget("Network", QColor(0xff, 0xb7, 0x4d),   0.0, 65536.0,    statsPanel);

        gpuSrc = detectGpu();
        if (gpuSrc.mode != GpuSource::None) {
            const QString gpuTitle =
                (gpuSrc.mode == GpuSource::FreqRatio) ? "GPU (clock)" : "GPU";
            gpuGraph = new GraphWidget(gpuTitle, QColor(0xba, 0x68, 0xc8), 100.0, 0.0, statsPanel);
        }

        statsLayout->addWidget(cpuGraph, 0, 0);
        statsLayout->addWidget(ramGraph, 0, 1);
        if (gpuGraph) {
            statsLayout->addWidget(netGraph, 1, 0);
            statsLayout->addWidget(gpuGraph, 1, 1);
        } else {
            statsLayout->addWidget(netGraph, 1, 0, 1, 2);
        }

        panelLayout->addWidget(statsPanel);

        // Prime the counters so the first drawn sample is a real delta
        updateStats();

        // Timer to update clock and graphs every second
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &PowerMenuWindow::updateClock);
        connect(timer, &QTimer::timeout, this, &PowerMenuWindow::updateStats);
        timer->start(1000);
        updateClock();

        updateStyles();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // Dim background overlay
        p.fillRect(rect(), QColor(0, 0, 0, 160));
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        if (!panel)
            return;

        // Center the panel and size it based on screen
        int w = width();
        int h = height();

        // Panel size: 80% height, 70% width (clamped a bit)
        int panelW = int(w * 0.75);
        int panelH = int(h * 0.80);
        if (panelW > 900) panelW = 900;
        if (panelH > 1000) panelH = 1000;

        int x = (w - panelW) / 2;
        int y = (h - panelH) / 2;

        panel->setGeometry(x, y, panelW, panelH);

        // Stats panel height: about 40% of panel
        if (statsPanel) {
            int statsH = int(panelH * 0.45);
            statsPanel->setMinimumHeight(statsH);
        }

        // Font scaling
        int base = panelH;
        int helloSize = qMax(14, base / 30);
        int timeSize  = qMax(14, base / 20);
        int statsTextSize = qMax(14, base / 30);

        QFont f = helloLabel->font();
        f.setPointSize(helloSize);
        helloLabel->setFont(f);

        QFont f2 = timeLabel->font();
        f2.setPointSize(timeSize);
        timeLabel->setFont(f2);

        // Set stats label font (child of statsPanel)
        QList<QLabel*> labels = statsPanel->findChildren<QLabel*>();
        for (QLabel *lab : labels) {
            QFont lf = lab->font();
            lf.setPointSize(statsTextSize);
            lab->setFont(lf);
        }

        // Adjust icon sizes
        int iconSize = qMax(48, base / 10);
        QList<QPushButton*> btns = panel->findChildren<QPushButton*>("btn");
        for (QPushButton *b : btns) {
            b->setIconSize(QSize(iconSize, iconSize));
            QFont bf = b->font();
            bf.setPointSize(qMax(14, base / 40));
            b->setFont(bf);
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        // If click is outside the panel, close the menu
        if (panel && !panel->geometry().contains(event->pos())) {
            close();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override {
        // Escape closes the menu
        if (event->key() == Qt::Key_Escape) {
            close();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    QWidget *panel;
    QLabel *helloLabel;
    QLabel *timeLabel;
    QWidget *statsPanel;

    GraphWidget *cpuGraph;
    GraphWidget *ramGraph;
    GraphWidget *netGraph;
    GraphWidget *gpuGraph;

    GpuSource gpuSrc;
    quint64 lastCpuTotal;
    quint64 lastCpuIdle;
    quint64 lastRx;
    quint64 lastTx;
    QElapsedTimer netTimer;

    QWidget* createIconButton(const QString &labelText, const QString &iconPath) {
        QWidget *wrapper = new QWidget(this);
        QVBoxLayout *v = new QVBoxLayout(wrapper);
        v->setContentsMargins(0,0,0,0);
        v->setSpacing(5);
        v->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

        QPushButton *btn = new QPushButton(wrapper);
        btn->setObjectName("btn");
        btn->setFlat(true);
        btn->setCursor(Qt::PointingHandCursor);

        QFileInfo fi(iconPath);
        if (fi.exists()) {
            QIcon icon(iconPath);
            btn->setIcon(icon);
        } else {
            btn->setText(labelText);
        }

        QLabel *lab = new QLabel(labelText, wrapper);
        lab->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        lab->setStyleSheet("color: white;");

        v->addWidget(btn, 0, Qt::AlignHCenter);
        v->addWidget(lab, 0, Qt::AlignHCenter);

        return wrapper;
    }

    void updateClock() {
        timeLabel->setText(QTime::currentTime().toString("HH:mm"));
    }

    void updateStats() {
        // ---- CPU ----
        quint64 total = 0, idle = 0;
        if (readCpuTotals(total, idle)) {
            if (lastCpuTotal > 0 && total > lastCpuTotal) {
                quint64 dTotal = total - lastCpuTotal;
                quint64 dIdle  = (idle > lastCpuIdle) ? (idle - lastCpuIdle) : 0;
                if (dIdle > dTotal) dIdle = dTotal;
                double busy = 100.0 * double(dTotal - dIdle) / double(dTotal);
                if (busy < 0.0)   busy = 0.0;
                if (busy > 100.0) busy = 100.0;
                if (cpuGraph)
                    cpuGraph->addSample(busy, QString::number(busy, 'f', 0) + "%");
            }
            lastCpuTotal = total;
            lastCpuIdle  = idle;
        }

        // ---- RAM ----
        quint64 totalKb = 0, usedKb = 0;
        if (readMem(totalKb, usedKb) && totalKb > 0 && ramGraph) {
            double pct    = 100.0 * double(usedKb) / double(totalKb);
            double usedGb = double(usedKb) / 1048576.0;
            QString readout;
            if (totalKb >= 1048576)
                readout = QString("%1%  %2 GB").arg(pct, 0, 'f', 0).arg(usedGb, 0, 'f', 1);
            else
                readout = QString("%1%  %2 MB").arg(pct, 0, 'f', 0)
                                               .arg(double(usedKb) / 1024.0, 0, 'f', 0);
            ramGraph->addSample(pct, readout);
        }

        // ---- Network ----
        quint64 rx = 0, tx = 0;
        if (readNetBytes(rx, tx)) {
            if (netTimer.isValid()) {
                double secs = double(netTimer.restart()) / 1000.0;
                if (secs > 0.05 && rx >= lastRx && tx >= lastTx && netGraph) {
                    double bps = double((rx - lastRx) + (tx - lastTx)) / secs;
                    netGraph->addSample(bps, formatRate(bps));
                }
            } else {
                netTimer.start();
            }
            lastRx = rx;
            lastTx = tx;
        }

        // ---- GPU ----
        if (gpuGraph) {
            double val = -1.0;
            if (gpuSrc.mode == GpuSource::BusyPercent) {
                QString s = readFirstLine(gpuSrc.busyPath);
                if (!s.isEmpty())
                    val = s.toDouble();
            } else if (gpuSrc.mode == GpuSource::FreqRatio) {
                double cur = readFirstLine(gpuSrc.curFreqPath).toDouble();
                double max = readFirstLine(gpuSrc.maxFreqPath).toDouble();
                if (max > 0.0)
                    val = 100.0 * cur / max;
            }
            if (val >= 0.0) {
                if (val > 100.0) val = 100.0;
                gpuGraph->addSample(val, QString::number(val, 'f', 0) + "%");
            }
        }
    }

    void updateStyles() {
        // Rounded grey panel (screen area)
        QString panelStyle =
            "QWidget#panel {"
            "  background-color: #80708099;"
            "  border-radius: 40px;"
            "}";

        // Black rounded stats area
        QString statsStyle =
            "QWidget#statsPanel {"
            "  background-color: #000000;"
            "  border-radius: 35px;"
            "}";

        panel->setStyleSheet(panelStyle);
        statsPanel->setStyleSheet(statsStyle);
    }

private slots:
    void doLock() {
        QProcess::startDetached("osm-lockd", QStringList());
        close();
    }

    void doSleep() {
        QProcess::startDetached("osm-lockd", QStringList());
        QProcess::startDetached("osm-sudo", QStringList() << "pm-suspend");
        close();
    }

    void doReboot() {
        QProcess::startDetached("osm-sudo", QStringList() << "reboot");
        close();
    }

    void doPowerOff() {
        QProcess::startDetached("osm-sudo", QStringList() << "poweroff");
        close();
    }

};

// ────────────────────────────────
// main
// ────────────────────────────────
int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);

    PowerMenuWindow w;
    w.showFullScreen();

    return app.exec();
}
