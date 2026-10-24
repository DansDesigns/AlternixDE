#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QBoxLayout>
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
#include <QFont>
#include <QSizePolicy>
#include <QTimer>
#include <QMap>
#include <QProgressBar>
#include <QResizeEvent>
#include <QtGlobal>

// AlternixOS System page. Only the appearance and refresh organisation of
// this page have been changed. No external assets or new packages are needed.

namespace {

static const char *BG       = "#282828";
static const char *TEXT     = "#F3F4F4";
static const char *MUTED    = "#B9BFC0";
static const char *ACCENT   = "#70DAC5";

// ---------------------------------------------------------------------------
// Existing system information sources
// ---------------------------------------------------------------------------
static QString runCmd(const QString &cmd)
{
    QProcess p;
    p.start("bash", {"-c", cmd});
    p.waitForFinished(300);
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

static QString readInstalledVersion(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return "Unknown";
    const QString v = QString::fromUtf8(f.readAll()).trimmed();
    return v.isEmpty() ? "Unknown" : v;
}

static QString getAlternixDEVersion()
{
    return readInstalledVersion("/usr/share/alternix/version.txt");
}

static QString getAlternixOSVersion()
{
    return readInstalledVersion(QDir::home().filePath("Alternix/version"));
}

static QString getOSName()
{
    QFile f("/etc/os-release");
    if (!f.open(QIODevice::ReadOnly)) return "Unknown";
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        QString line = ts.readLine();
        if (line.startsWith("PRETTY_NAME="))
            return line.section('=', 1).remove('"').trimmed();
    }
    return "Unknown";
}

static QString getKernel() { return runCmd("uname -r"); }

static QString getCPUModel()
{
    QFile f("/proc/cpuinfo");
    if (f.open(QIODevice::ReadOnly)) {
        QTextStream ts(&f);
        QString best;
        while (!ts.atEnd()) {
            const QString line = ts.readLine();
            const QString low = line.toLower();
            if (low.startsWith("model name"))
                return line.section(':', 1).trimmed();
            if (low.startsWith("model") || low.startsWith("cpu model") ||
                low.startsWith("hardware"))
                best = line.section(':', 1).trimmed();
        }
        if (!best.isEmpty()) return best;
    }

    QFile f2("/proc/device-tree/model");
    if (f2.open(QIODevice::ReadOnly)) {
        QString model = QString::fromUtf8(f2.readAll()).remove(QChar('\0')).trimmed();
        if (!model.isEmpty()) return model;
    }
    const QString m = runCmd("LC_ALL=C lscpu | awk -F: '/Model name/ {print $2; exit}'");
    return m.isEmpty() ? "Unknown" : m.trimmed();
}

static QString getCPUCores()
{
    const QString n = runCmd("nproc");
    return n.isEmpty() ? "1" : n;
}

static QString getCPUSpeedGHz()
{
    QString mhz = runCmd("LC_ALL=C lscpu | awk -F: '/max MHz/ {print $2; exit}'");
    if (mhz.isEmpty())
        mhz = runCmd("LC_ALL=C lscpu | awk -F: '/CPU MHz/ {print $2; exit}'");
    bool ok = false;
    const double v = mhz.trimmed().toDouble(&ok);
    return (ok && v > 0) ? QString::number(v / 1000.0, 'f', 2) + " GHz" : "Unknown";
}

static QString getUptime()
{
    QFile f("/proc/uptime");
    if (!f.open(QIODevice::ReadOnly)) return "Unknown";
    QTextStream ts(&f);
    const qint64 seconds = static_cast<qint64>(ts.readLine().section(' ', 0, 0).toDouble());
    return QString("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

static QString getFlatpakCount()
{
    const QString n = runCmd("flatpak list --app --columns=application 2>/dev/null | wc -l");
    return n.isEmpty() ? "0" : n;
}

static QString getSnapCount()
{
    const QString n = runCmd("snap list 2>/dev/null | tail -n +2 | wc -l");
    return n.isEmpty() ? "0" : n;
}

static QString getDPKGCount()
{
    const QString n = runCmd("dpkg -l | grep '^ii' | wc -l");
    return n.isEmpty() ? "0" : n;
}

struct MemoryInfo {
    qint64 totalKB = 0;
    qint64 availableKB = 0;
    bool valid = false;
};

// Read both values together so the RAM tiles and usage bar stay in sync.
static MemoryInfo getMemoryInfo()
{
    MemoryInfo m;
    QFile f("/proc/meminfo");
    if (!f.open(QIODevice::ReadOnly)) return m;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        const QString line = ts.readLine();
        if (line.startsWith("MemTotal:"))
            m.totalKB = line.mid(9).simplified().section(' ', 0, 0).toLongLong();
        else if (line.startsWith("MemAvailable:"))
            m.availableKB = line.mid(13).simplified().section(' ', 0, 0).toLongLong();
    }
    m.valid = m.totalKB > 0 && m.availableKB >= 0;
    if (m.valid)
        m.availableKB = qBound(qint64(0), m.availableKB, m.totalKB);
    return m;
}

static QString memoryMB(qint64 kb, bool valid)
{
    return valid ? QString::number(kb / 1024) + " MB" : "Unknown";
}

// ---------------------------------------------------------------------------
// Reusable visual components. Styles are scoped by objectName, so they don't
// affect the parent osm-settings application or other settings pages.
// ---------------------------------------------------------------------------
static QLabel *label(const QString &value, int pixels, bool bold = false,
                     const QString &colour = TEXT)
{
    QLabel *l = new QLabel(value);
    QFont font("DejaVu Sans");
    font.setPixelSize(pixels);
    font.setWeight(bold ? QFont::DemiBold : QFont::Normal);
    l->setFont(font);
    l->setWordWrap(true);
    l->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    l->setStyleSheet(QString("color:%1; background:transparent; border:none;").arg(colour));
    l->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    return l;
}

static QFrame *panel(const QString &objectName)
{
    QFrame *f = new QFrame;
    f->setObjectName(objectName);
    f->setStyleSheet(QString(
        "QFrame#%1 { background:#353535; border:1px solid #454545;"
        " border-radius:23px; }").arg(objectName));
    f->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    return f;
}

static QVBoxLayout *panelLayout(QFrame *p, int margin = 24, int space = 15)
{
    QVBoxLayout *v = new QVBoxLayout(p);
    v->setContentsMargins(margin, margin, margin, margin);
    v->setSpacing(space);
    return v;
}

static QFrame *statTile(const QString &title, const QString &value,
                        const QString &key, QMap<QString, QLabel*> &values,
                        int valuePixels = 25, bool highlight = false)
{
    QFrame *tile = new QFrame;
    tile->setObjectName("systemStatTile");
    tile->setStyleSheet(
        "QFrame#systemStatTile { background:#424242; border:1px solid #4D4D4D;"
        " border-radius:16px; }");
    tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    QVBoxLayout *v = new QVBoxLayout(tile);
    v->setContentsMargins(17, 14, 17, 14);
    v->setSpacing(5);

    QLabel *caption = label(title, 14, true, MUTED);
    QLabel *number  = label(value, valuePixels, true, highlight ? ACCENT : TEXT);
    number->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(caption);
    v->addWidget(number);
    if (!key.isEmpty()) values.insert(key, number);
    return tile;
}

static void addSectionHeading(QVBoxLayout *layout, const QString &heading,
                              const QString &description = QString())
{
    layout->addWidget(label(heading, 19, true));
    if (!description.isEmpty())
        layout->addWidget(label(description, 14, false, MUTED));
}

// A pair of large cards on desktop becomes a vertical stack on small tablets.
// The same mechanism is used for the three package cards and version tiles.
class AdaptiveRow : public QWidget
{
public:
    explicit AdaptiveRow(int threshold, QWidget *parent = nullptr)
        : QWidget(parent), switchWidth(threshold)
    {
        items = new QBoxLayout(QBoxLayout::LeftToRight, this);
        items->setContentsMargins(0, 0, 0, 0);
        items->setSpacing(14);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    }

    void add(QWidget *child) { items->addWidget(child, 1); }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        const QBoxLayout::Direction wanted =
            event->size().width() < switchWidth
                ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
        if (items->direction() != wanted) items->setDirection(wanted);
        QWidget::resizeEvent(event);
    }

private:
    QBoxLayout *items;
    int switchWidth;
};

static void setValue(const QMap<QString, QLabel*> &values,
                     const QString &key, const QString &content)
{
    QLabel *l = values.value(key, nullptr);
    if (l && l->text() != content) l->setText(content);
}

static QString buttonStyle()
{
    // Retain the original Alternix back-button shape and behaviour.
    return "QPushButton { background:#444444; color:white; border:1px solid #222222;"
           " border-radius:16px; font-size:22px; font-weight:bold;"
           " padding:6px 16px; }"
           "QPushButton:hover { background:#555555; border-color:#666666; }"
           "QPushButton:pressed { background:#333333; }";
}

} // namespace

extern "C" QWidget* make_page(QStackedWidget *stack)
{
    QWidget *root = new QWidget(stack);
    root->setObjectName("alternixSystemPage");
    root->setStyleSheet(QString("QWidget#alternixSystemPage { background:%1; }").arg(BG));

    QMap<QString, QLabel*> values; // per-page, not a shared static registry
    const MemoryInfo initialRAM = getMemoryInfo();

    QVBoxLayout *page = new QVBoxLayout(root);
    page->setContentsMargins(26, 20, 26, 18);
    page->setSpacing(14);

    // Header and manual refresh
    QHBoxLayout *header = new QHBoxLayout;
    header->setSpacing(12);
    QVBoxLayout *headText = new QVBoxLayout;
    headText->setSpacing(3);
    headText->addWidget(label("System Info", 35, true));
    headText->addWidget(label("System, hardware and installed software", 15, false, MUTED));
    header->addLayout(headText, 1);

    QPushButton *reload = new QPushButton("Refresh");
    reload->setToolTip("Update system information now");
    reload->setCursor(Qt::PointingHandCursor);
    reload->setFixedSize(118, 43);
    reload->setStyleSheet(
        "QPushButton { background:#3F4846; color:#C5F4E9;"
        " border:1px solid #577169; border-radius:14px;"
        " font-size:15px; font-weight:bold; }"
        "QPushButton:hover { background:#4D5B56; }"
        "QPushButton:pressed { background:#313F3A; }");
    header->addWidget(reload, 0, Qt::AlignVCenter);
    page->addLayout(header);

    QScrollArea *scroll = new QScrollArea(root);
    scroll->setObjectName("systemInfoScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setStyleSheet(
        "QScrollArea#systemInfoScroll { border:none; background:transparent; }"
        "QScrollBar:vertical { background:#282828; width:8px; margin:0px; }"
        "QScrollBar::handle:vertical { background:#606060; border-radius:4px; min-height:28px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0px; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }");
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

    QWidget *content = new QWidget;
    content->setObjectName("alternixSystemContent");
    content->setStyleSheet(
        "QWidget#alternixSystemContent { background:#282828; }");
    scroll->viewport()->setStyleSheet("background:#282828;");
    QVBoxLayout *body = new QVBoxLayout(content);
    body->setContentsMargins(2, 2, 12, 8);
    body->setSpacing(16);

    // -----------------------------------------------------------------------
    // Alternix versions: prominently displayed side by side
    // -----------------------------------------------------------------------
    QFrame *alternix = panel("alternixVersionsPanel");
    QVBoxLayout *av = panelLayout(alternix);
    av->addWidget(label("ALTERNIX", 13, true, ACCENT));
    av->addWidget(label("Installed versions", 24, true));
    AdaptiveRow *versionRow = new AdaptiveRow(440);
    versionRow->add(statTile("AlternixDE", getAlternixDEVersion(),
                             "alt_de_version", values, 29, true));
    versionRow->add(statTile("AlternixOS", getAlternixOSVersion(),
                             "alt_os_version", values, 29, true));
    av->addWidget(versionRow);
    body->addWidget(alternix);

    // -----------------------------------------------------------------------
    // OS information
    // -----------------------------------------------------------------------
    QFrame *operating = panel("alternixOperatingPanel");
    QVBoxLayout *op = panelLayout(operating);
    addSectionHeading(op, "Operating system");
    op->addWidget(label(getOSName(), 19, false, TEXT));
    AdaptiveRow *osRow = new AdaptiveRow(440);
    osRow->add(statTile("Kernel version", getKernel(), "kernel", values, 19));
    osRow->add(statTile("System uptime", getUptime(), "uptime", values, 23));
    op->addWidget(osRow);
    body->addWidget(operating);

    // -----------------------------------------------------------------------
    // Hardware cards
    // -----------------------------------------------------------------------
    AdaptiveRow *hardware = new AdaptiveRow(750);

    QFrame *cpu = panel("alternixCpuPanel");
    QVBoxLayout *cp = panelLayout(cpu);
    addSectionHeading(cp, "Processor");
    cp->addWidget(label(getCPUModel(), 18, false, TEXT));
    cp->addStretch();
    AdaptiveRow *cpuRow = new AdaptiveRow(400);
    cpuRow->add(statTile("CPU cores", getCPUCores(), "cores", values, 24));
    cpuRow->add(statTile("CPU speed", getCPUSpeedGHz(), "cpuspeed", values, 24));
    cp->addWidget(cpuRow);
    hardware->add(cpu);

    QFrame *ram = panel("alternixRamPanel");
    QVBoxLayout *rp = panelLayout(ram);
    addSectionHeading(rp, "Memory");
    QHBoxLayout *usage = new QHBoxLayout;
    QLabel *usageTitle = label("RAM usage", 15, false, MUTED);
    QLabel *usagePercent = label("0% used", 15, true, ACCENT);
    usagePercent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    usage->addWidget(usageTitle, 1);
    usage->addWidget(usagePercent);
    rp->addLayout(usage);

    QProgressBar *ramBar = new QProgressBar;
    ramBar->setRange(0, 100);
    ramBar->setTextVisible(false);
    ramBar->setFixedHeight(11);
    ramBar->setStyleSheet(
        "QProgressBar { background:#242424; border:none; border-radius:5px; }"
        "QProgressBar::chunk { background:#70DAC5; border-radius:5px; }");
    rp->addWidget(ramBar);
    rp->addStretch();

    AdaptiveRow *ramRow = new AdaptiveRow(400);
    ramRow->add(statTile("Total", memoryMB(initialRAM.totalKB, initialRAM.valid),
                         "ram_total", values, 19));
    ramRow->add(statTile("Used", memoryMB(initialRAM.totalKB - initialRAM.availableKB,
                                          initialRAM.valid), "ram_used", values, 19));
    rp->addWidget(ramRow);
    rp->addWidget(statTile("Available", memoryMB(initialRAM.availableKB, initialRAM.valid),
                           "ram_free", values, 20));
    hardware->add(ram);
    body->addWidget(hardware);

    // -----------------------------------------------------------------------
    // Installed packages
    // -----------------------------------------------------------------------
    QFrame *packages = panel("alternixPackagesPanel");
    QVBoxLayout *pk = panelLayout(packages);
    addSectionHeading(pk, "Installed packages", "Packages available on this system");
    AdaptiveRow *packageRow = new AdaptiveRow(550);
    packageRow->add(statTile("APT / DPKG", getDPKGCount(), "dpkg", values, 27));
    packageRow->add(statTile("Flatpak", getFlatpakCount(), "flatpaks", values, 27));
    packageRow->add(statTile("Snap", getSnapCount(), "snaps", values, 27));
    pk->addWidget(packageRow);
    body->addWidget(packages);
    body->addStretch();

    scroll->setWidget(content);
    page->addWidget(scroll, 1);

    // Keep the familiar bottom-centred Back control.
    QPushButton *back = new QPushButton(QString::fromUtf8("❮"));
    back->setStyleSheet(buttonStyle());
    back->setFixedSize(140, 60);
    back->setCursor(Qt::PointingHandCursor);
    QObject::connect(back, &QPushButton::clicked, root, [stack]() {
        if (stack) stack->setCurrentIndex(0);
    });
    page->addWidget(back, 0, Qt::AlignHCenter);

    // -----------------------------------------------------------------------
    // Live updates. Memory and uptime are inexpensive /proc reads. Commands
    // for package counts and static system details run less often to avoid
    // needless CPU use on low-power AlternixOS tablets.
    // -----------------------------------------------------------------------
    const auto refreshMemory = [values, ramBar, usagePercent]() {
        const MemoryInfo m = getMemoryInfo();
        setValue(values, "ram_total", memoryMB(m.totalKB, m.valid));
        setValue(values, "ram_used", memoryMB(m.totalKB - m.availableKB, m.valid));
        setValue(values, "ram_free", memoryMB(m.availableKB, m.valid));
        if (m.valid) {
            const int percent = static_cast<int>(
                (100 * (m.totalKB - m.availableKB)) / m.totalKB);
            ramBar->setValue(percent);
            usagePercent->setText(QString::number(percent) + "% used");
        } else {
            ramBar->setValue(0);
            usagePercent->setText("Unknown");
        }
    };

    const auto refreshFast = [values, refreshMemory]() {
        setValue(values, "uptime", getUptime());
        refreshMemory();
    };

    const auto refreshSlow = [values]() {
        setValue(values, "alt_de_version", getAlternixDEVersion());
        setValue(values, "alt_os_version", getAlternixOSVersion());
        setValue(values, "kernel", getKernel());
        setValue(values, "cores", getCPUCores());
        setValue(values, "cpuspeed", getCPUSpeedGHz());
        setValue(values, "dpkg", getDPKGCount());
        setValue(values, "flatpaks", getFlatpakCount());
        setValue(values, "snaps", getSnapCount());
    };

    refreshMemory();
    QObject::connect(reload, &QPushButton::clicked, root,
                     [refreshFast, refreshSlow]() {
        refreshFast();
        refreshSlow();
    });

    QTimer *fastTimer = new QTimer(root);
    fastTimer->setInterval(2000);
    QObject::connect(fastTimer, &QTimer::timeout, root, refreshFast);

    QTimer *slowTimer = new QTimer(root);
    slowTimer->setInterval(30000);
    QObject::connect(slowTimer, &QTimer::timeout, root, refreshSlow);

    if (stack) {
        QObject::connect(stack, &QStackedWidget::currentChanged, root,
                         [=](int idx) {
            if (stack->widget(idx) == root) {
                refreshFast();
                refreshSlow();
                fastTimer->start();
                slowTimer->start();
            } else {
                fastTimer->stop();
                slowTimer->stop();
            }
        });
        if (stack->currentWidget() == root) {
            fastTimer->start();
            slowTimer->start();
        }
    }

    return root;
}
