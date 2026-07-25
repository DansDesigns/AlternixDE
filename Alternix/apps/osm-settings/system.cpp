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
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QStringList>
#include <QFont>
#include <QScreen>
#include <QGuiApplication>
#include <QSizePolicy>
#include <QTimer>
#include <QMap>

// -----------------------------------------------------
// Shell helper
// -----------------------------------------------------
static QString runCmd(const QString &cmd)
{
    QProcess p;
    p.start("bash", {"-c", cmd});
    p.waitForFinished(300);
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// -----------------------------------------------------
// Alternix compact button style (same as Display/Security)
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
// SYSTEM INFO HELPERS
// -----------------------------------------------------
static QString getKernel() { return runCmd("uname -r"); }

static QString getOSName()
{
    QFile f("/etc/os-release");
    if (!f.open(QIODevice::ReadOnly)) return "Unknown";

    QTextStream ts(&f);
    while (!ts.atEnd()) {
        QString line = ts.readLine();
        if (line.startsWith("PRETTY_NAME="))
            return line.section("=",1).replace("\"","").trimmed();
    }
    return "Unknown";
}

static QString getCPUModel()
{
    QFile f("/proc/cpuinfo");
    if (f.open(QIODevice::ReadOnly)) {
        QTextStream ts(&f);
        QString best;
        while (!ts.atEnd()) {
            QString line = ts.readLine();
            QString low = line.toLower();

            if (low.startsWith("model name"))
                return line.section(":",1).trimmed();
            if (low.startsWith("model"))
                best = line.section(":",1).trimmed();
            if (low.startsWith("cpu model"))
                best = line.section(":",1).trimmed();
            if (low.startsWith("hardware"))
                best = line.section(":",1).trimmed();
        }
        if (!best.isEmpty())
            return best;
    }

    QFile f2("/proc/device-tree/model");
    if (f2.open(QIODevice::ReadOnly)) {
        QString model = QString::fromUtf8(f2.readAll()).trimmed();
        if (!model.isEmpty())
            return model;
    }

    QString m = runCmd("LC_ALL=C lscpu | awk -F: '/Model name/ {print $2; exit}'");
    if (!m.isEmpty())
        return m.trimmed();

    return "Unknown";
}

static QString getCPUCores()
{
    QString n = runCmd("nproc");
    return n.isEmpty() ? "1" : n.trimmed();
}

static QString getCPUSpeedGHz()
{
    QString mhz = runCmd("LC_ALL=C lscpu | awk -F: '/max MHz/ {print $2; exit}'");

    if (mhz.trimmed().isEmpty())
        mhz = runCmd("LC_ALL=C lscpu | awk -F: '/CPU MHz/ {print $2; exit}'");

    bool ok = false;
    double v = mhz.toDouble(&ok);
    if (!ok || v <= 0.0)
        return "Unknown";

    return QString::number(v / 1000.0, 'f', 2) + " GHz";
}

// RAM
static QString getRAMTotal()
{
    QString kb = runCmd("grep MemTotal /proc/meminfo | awk '{print $2}'");
    bool ok = false;
    int v = kb.toInt(&ok);
    return ok ? QString::number(v / 1024) + " MB" : "Unknown";
}

static QString getRAMFree()
{
    QString kb = runCmd("grep MemAvailable /proc/meminfo | awk '{print $2}'");
    bool ok = false;
    int v = kb.toInt(&ok);
    return ok ? QString::number(v / 1024) + " MB" : "Unknown";
}

static QString getRAMUsed()
{
    QString t = runCmd("grep MemTotal /proc/meminfo | awk '{print $2}'");
    QString a = runCmd("grep MemAvailable /proc/meminfo | awk '{print $2}'");

    bool ok1=false, ok2=false;
    int total = t.toInt(&ok1);
    int avail = a.toInt(&ok2);

    if (!ok1 || !ok2) return "Unknown";
    return QString::number((total - avail) / 1024) + " MB";
}

// Uptime
static QString getUptime()
{
    QFile f("/proc/uptime");
    if (!f.open(QIODevice::ReadOnly)) return "Unknown";

    QTextStream ts(&f);
    double secs = ts.readLine().split(" ").first().toDouble();
    int hrs = int(secs) / 3600;
    int mins = (int(secs) % 3600) / 60;

    return QString("%1h %2m").arg(hrs).arg(mins);
}

// Packages
static QString getFlatpakCount()
{
    QString out = runCmd("flatpak list --app --columns=application 2>/dev/null | wc -l");
    return out.trimmed().isEmpty() ? "0" : out.trimmed();
}

static QString getSnapCount()
{
    QString out = runCmd("snap list 2>/dev/null | tail -n +2 | wc -l");
    return out.trimmed().isEmpty() ? "0" : out.trimmed();
}

static QString getDPKGCount()
{
    QString out = runCmd("dpkg -l | grep '^ii' | wc -l");
    return out.trimmed().isEmpty() ? "0" : out.trimmed();
}

// Alternix version — read from the same file install-update.sh writes to
// (sudo cp "$ALT_ROOT/update/version.txt" /usr/share/alternix/version.txt),
// so this always reflects whatever the last update actually installed.
static QString getAlternixVersion()
{
    QFile f("/usr/share/alternix/version.txt");
    if (!f.open(QIODevice::ReadOnly))
        return "Unknown";

    QString v = QString::fromUtf8(f.readAll()).trimmed();
    return v.isEmpty() ? "Unknown" : v;
}

// -----------------------------------------------------
// LABEL + LIVE REGISTRY
// -----------------------------------------------------
static QMap<QString, QLabel*> liveValues;

static QLabel* makeInfoLabel(const QString &txt, int px, bool bold = false,
                             Qt::Alignment align = Qt::AlignLeft)
{
    QLabel *lbl = new QLabel(txt);
    QFont f("DejaVu Sans");
    f.setPointSize(px);
    f.setBold(bold);
    lbl->setFont(f);
    lbl->setAlignment(align);
    lbl->setStyleSheet("color:#ffffff;");
    lbl->setWordWrap(true);
    return lbl;
}

// -----------------------------------------------------
// MINI CARD (#555, radius 18) – two-line (label + value)
// -----------------------------------------------------
static QFrame* makeMiniCard(const QString &key,
                            const QString &line1,
                            const QString &line2)
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#555555; border-radius:18px; }");
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    QVBoxLayout *v = new QVBoxLayout(card);
    v->setContentsMargins(20, 12, 20, 12);
    v->setSpacing(4);

    QLabel *L1 = makeInfoLabel(line1, 18, true, Qt::AlignCenter);
    QLabel *L2 = makeInfoLabel(line2, 18, false, Qt::AlignCenter);

    v->addWidget(L1);
    v->addWidget(L2);

    liveValues[key] = L2;

    return card;
}

// -----------------------------------------------------
// SECTION: header (26px) + optional subtitle + grid of mini-cards (2 per row)
// -----------------------------------------------------
static QWidget* makeSection(const QString &title,
                            const QString &subtitle,
                            const QList<QPair<QString,QString>> &items,
                            const QList<QString> &keys)
{
    QWidget *wrap = new QWidget;
    QVBoxLayout *v = new QVBoxLayout(wrap);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    // Header
    QLabel *hdr = makeInfoLabel(title, 26, true, Qt::AlignLeft);
    v->addWidget(hdr);

    // Subtitle (OS name / CPU model, etc.)
    if (!subtitle.isEmpty()) {
        QLabel *sub = makeInfoLabel(subtitle, 22, false, Qt::AlignLeft);
        v->addWidget(sub);
    }

    // Grid of mini-cards, 2 per row
    QWidget *gridWrap = new QWidget;
    QGridLayout *grid = new QGridLayout(gridWrap);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(16);
    gridWrap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    for (int i = 0; i < items.size(); ++i) {
        int row = i / 2;
        int col = i % 2;
        grid->addWidget(
            makeMiniCard(keys[i], items[i].first, items[i].second),
            row, col
        );
    }

    v->addWidget(gridWrap);
    return wrap;
}

// -----------------------------------------------------
// ENTRY POINT
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    liveValues.clear();

    QWidget *root = new QWidget(stack);
    root->setStyleSheet("background:#282828; color:white; font-family:Sans;");

    QVBoxLayout *rootLay = new QVBoxLayout(root);
    rootLay->setContentsMargins(40, 40, 40, 40);
    rootLay->setSpacing(10);

    // Title
    QLabel *title = new QLabel("System Info");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:42px; font-weight:bold;");
    rootLay->addWidget(title);

    // Scroll area (same behaviour as Display)
    QScrollArea *scroll = new QScrollArea(root);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

    QWidget *wrap = new QWidget(scroll);
    QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
    wrapLay->setSpacing(10);
    wrapLay->setContentsMargins(0, 0, 0, 0);

    // Outer big card (same pattern as Display)
    QFrame *outer = new QFrame(wrap);
    outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");

    QVBoxLayout *outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(50, 30, 50, 30);
    outerLay->setSpacing(30);

    // Sections inside outer card
    outerLay->addWidget(
        makeSection(
            "Alternix",
            "",
            {
                { "Version", getAlternixVersion() }
            },
            { "alt_version" }
        )
    );

    outerLay->addWidget(
        makeSection(
            "Linux distro & version",
            getOSName(),
            {
                { "Kernel version", getKernel() },
                { "Uptime",         getUptime() }
            },
            { "kernel", "uptime" }
        )
    );

    outerLay->addWidget(
        makeSection(
            "CPU info",
            getCPUModel(),
            {
                { "No. cores",    getCPUCores()    },
                { "Speed in GHz", getCPUSpeedGHz() }
            },
            { "cores", "cpuspeed" }
        )
    );

    outerLay->addWidget(
        makeSection(
            "RAM amount",
            "",
            {
                { "Total",     getRAMTotal() },
                { "Used",      getRAMUsed()  },
                { "Available", getRAMFree()  }
            },
            { "ram_total", "ram_used", "ram_free" }
        )
    );

    outerLay->addWidget(
        makeSection(
            "Packages installed",
            "",
            {
                { "Flatpaks", getFlatpakCount() },
                { "Snaps",    getSnapCount()    },
                { "APT/DPKG", getDPKGCount()    }
            },
            { "flatpaks", "snaps", "dpkg" }
        )
    );

    wrapLay->addWidget(outer);
    wrapLay->addStretch();

    scroll->setWidget(wrap);
    rootLay->addWidget(scroll);

    // Back button (same style as Display)
    QPushButton *back = makeBtn("❮");
    back->setFixedSize(140, 60);
    QObject::connect(back, &QPushButton::clicked, [stack]() {
        if (stack)
            stack->setCurrentIndex(0);
    });
    rootLay->addWidget(back, 0, Qt::AlignCenter);

    // -------------------------------------------------
    // Refresh timer (2s) – only when page visible
    // -------------------------------------------------
    QTimer *refresh = new QTimer(root);
    refresh->setInterval(2000);

    QObject::connect(refresh, &QTimer::timeout, [=]() {
        if (liveValues.contains("alt_version"))
            liveValues["alt_version"]->setText(getAlternixVersion());

        if (liveValues.contains("kernel"))
            liveValues["kernel"]->setText(getKernel());

        if (liveValues.contains("uptime"))
            liveValues["uptime"]->setText(getUptime());

        if (liveValues.contains("cores"))
            liveValues["cores"]->setText(getCPUCores());

        if (liveValues.contains("cpuspeed"))
            liveValues["cpuspeed"]->setText(getCPUSpeedGHz());

        if (liveValues.contains("ram_total"))
            liveValues["ram_total"]->setText(getRAMTotal());

        if (liveValues.contains("ram_used"))
            liveValues["ram_used"]->setText(getRAMUsed());

        if (liveValues.contains("ram_free"))
            liveValues["ram_free"]->setText(getRAMFree());

        if (liveValues.contains("flatpaks"))
            liveValues["flatpaks"]->setText(getFlatpakCount());

        if (liveValues.contains("snaps"))
            liveValues["snaps"]->setText(getSnapCount());

        if (liveValues.contains("dpkg"))
            liveValues["dpkg"]->setText(getDPKGCount());
    });

    QObject::connect(stack, &QStackedWidget::currentChanged, root,
                     [=](int idx) {
        bool visible = (stack->widget(idx) == root);
        if (visible)
            refresh->start();
        else
            refresh->stop();
    });

    if (stack->currentWidget() == root)
        refresh->start();

    return root;
}
