// kernel.cpp — Kernel settings page for osm-settings (Alternix)
// Shows kernel/build info, boot cmdline, loaded modules and the
// hardware → driver map. Read-only apart from Refresh.
//
// Scroll behaviour matches apps.cpp: the outer page uses a manual
// drag scroll that ignores drags starting on the inner list cards,
// so inner lists and the page never scroll together.
//
// Build (same pattern as the other .so modules):
//   g++ -std=c++17 -fPIC -shared kernel.cpp -o kernel.so \
//       $(pkg-config --cflags --libs Qt5Widgets Qt5Gui Qt5Core)

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScroller>
#include <QScrollBar>
#include <QFrame>
#include <QStackedWidget>
#include <QProcess>
#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <QFont>
#include <QSizePolicy>
#include <QTimer>
#include <QMouseEvent>
#include <QList>

// -----------------------------------------------------
// Global list of inner scroll areas (for hit testing)
// -----------------------------------------------------
static QList<QScrollArea*> g_innerScrollAreas;

// -----------------------------------------------------
// Shell helper
// -----------------------------------------------------
static QString runCmd(const QString &cmd)
{
    QProcess p;
    p.start("bash", {"-c", cmd});
    p.waitForFinished(2000);
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// -----------------------------------------------------
// Alternix compact button style (same as System page)
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
        " font-family:'DejaVu Sans';"
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
// Outer scroll area with manual drag scrolling
// (ignores drags that start on inner cards) — same as apps.cpp
// -----------------------------------------------------
class OuterScrollArea : public QScrollArea {
public:
    OuterScrollArea(QWidget *parent = nullptr)
        : QScrollArea(parent), dragging(false) {}

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            lastPos = event->pos();
            dragging = !isOnInnerScroll(event->pos());
        }
        QScrollArea::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging && (event->buttons() & Qt::LeftButton)) {
            int dy = event->pos().y() - lastPos.y();
            verticalScrollBar()->setValue(verticalScrollBar()->value() - dy);
            lastPos = event->pos();
            event->accept();
            return;
        }
        QScrollArea::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            dragging = false;
        QScrollArea::mouseReleaseEvent(event);
    }

private:
    bool dragging;
    QPoint lastPos;

    bool isOnInnerScroll(const QPoint &p)
    {
        QWidget *vp = viewport();
        for (QScrollArea *sa : g_innerScrollAreas) {
            if (!sa || !sa->viewport()) continue;

            QPoint topLeft = sa->viewport()->mapTo(vp, QPoint(0,0));
            QRect rect(topLeft, sa->viewport()->size());
            if (rect.contains(p))
                return true;
        }
        return false;
    }
};

// -----------------------------------------------------
// Info gatherers
// -----------------------------------------------------
static QString getKernelVersion() { return runCmd("uname -r"); }
static QString getKernelArch()    { return runCmd("uname -m"); }
static QString getKernelBuild()   { return runCmd("uname -v"); }

static QString getCmdline()
{
    QFile f("/proc/cmdline");
    if (!f.open(QIODevice::ReadOnly)) return "Unknown";
    return QString::fromUtf8(f.readAll()).trimmed();
}

static QString getInitSystem()
{
    // Alternix targets sysvinit (Devuan) but detect honestly
    QString p = runCmd("readlink -f /sbin/init 2>/dev/null");
    if (p.contains("systemd")) return "systemd";
    if (p.contains("runit"))   return "runit";
    if (p.contains("openrc"))  return "OpenRC";
    if (!p.isEmpty())          return "SysVinit (" + p + ")";
    return "Unknown";
}

// lsmod → "name  (size, used by N)"
static QStringList getModules()
{
    QStringList out;
    QString raw = runCmd("lsmod | tail -n +2");
    for (const QString &line : raw.split('\n', Qt::SkipEmptyParts)) {
        QStringList c = line.simplified().split(' ');
        if (c.isEmpty()) continue;
        QString name = c.value(0);
        QString size = c.value(1);
        QString used = c.value(2, "0");
        out << QString("%1   (%2 bytes, used %3)").arg(name, size, used);
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

// lspci -nnk → "Device — driver"
static QStringList getDriverMap()
{
    QStringList out;
    QString raw = runCmd("lspci -nnk 2>/dev/null");
    QString device;
    for (const QString &line : raw.split('\n')) {
        if (!line.startsWith('\t') && !line.trimmed().isEmpty()) {
            // "00:02.0 VGA compatible controller [0300]: Intel ..."
            device = line.section(": ", 1).trimmed();
            if (device.isEmpty()) device = line.trimmed();
        } else if (line.trimmed().startsWith("Kernel driver in use:")) {
            QString drv = line.section(':', 1).trimmed();
            out << device + "\n    → " + drv;
        }
    }
    if (out.isEmpty())
        out << "No PCI driver information available";
    return out;
}

// -----------------------------------------------------
// Mini card (matches system.cpp)
// -----------------------------------------------------
static QFrame* makeMiniCard(const QString &line1, const QString &line2,
                            QLabel **valueOut = nullptr)
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#555555; border-radius:18px; }");
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    QVBoxLayout *v = new QVBoxLayout(card);
    v->setContentsMargins(20, 12, 20, 12);
    v->setSpacing(4);

    QLabel *L1 = makeInfoLabel(line1, 18, true,  Qt::AlignCenter);
    QLabel *L2 = makeInfoLabel(line2, 18, false, Qt::AlignCenter);

    v->addWidget(L1);
    v->addWidget(L2);
    if (valueOut) *valueOut = L2;
    return card;
}

// -----------------------------------------------------
// Scrollable list card (same inner scroll pattern as apps.cpp,
// registered in g_innerScrollAreas for outer hit testing)
// -----------------------------------------------------
static QFrame* makeListCard(const QString &title,
                            QVBoxLayout **outList,
                            QLabel **outCount)
{
    QFrame *card = new QFrame;
    card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

    QVBoxLayout *v = new QVBoxLayout(card);
    v->setContentsMargins(30, 30, 30, 30);
    v->setSpacing(16);

    QHBoxLayout *hdr = new QHBoxLayout();
    QLabel *t = makeInfoLabel(title, 26, true, Qt::AlignLeft);
    hdr->addWidget(t);
    hdr->addStretch();
    QLabel *count = makeInfoLabel("", 20, false, Qt::AlignRight);
    count->setStyleSheet("color:#aaaaaa;");
    hdr->addWidget(count);
    v->addLayout(hdr);

    QScrollArea *sa = new QScrollArea;
    sa->setWidgetResizable(true);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setFixedHeight(340);

    if (title.startsWith("Loaded"))        sa->setObjectName("inner_scroll_modules");
    else if (title.startsWith("Hardware")) sa->setObjectName("inner_scroll_drivers");
    else                                   sa->setObjectName("inner_scroll_generic");

    QScroller::grabGesture(sa->viewport(), QScroller::LeftMouseButtonGesture);
    sa->viewport()->setAttribute(Qt::WA_AcceptTouchEvents, true);

    QWidget *wrap = new QWidget;
    wrap->setStyleSheet("background:transparent;");
    QVBoxLayout *list = new QVBoxLayout(wrap);
    list->setSpacing(10);
    list->setContentsMargins(0, 0, 0, 0);
    list->addWidget(makeInfoLabel("Loading…", 20, false, Qt::AlignCenter));
    list->addStretch();

    sa->setWidget(wrap);
    v->addWidget(sa);

    g_innerScrollAreas.append(sa);

    *outList = list;
    *outCount = count;
    return card;
}

static void fillList(QVBoxLayout *list, QLabel *count,
                     const QStringList &entries)
{
    QLayoutItem *c;
    while ((c = list->takeAt(0))) {
        if (c->widget()) c->widget()->deleteLater();
        delete c;
    }

    for (const QString &e : entries) {
        QFrame *row = new QFrame;
        row->setStyleSheet("background:#3A3A3A; border-radius:16px;");
        QVBoxLayout *h = new QVBoxLayout(row);
        h->setContentsMargins(16, 10, 16, 10);
        QLabel *lbl = makeInfoLabel(e, 17, false, Qt::AlignLeft);
        h->addWidget(lbl);
        list->addWidget(row);
    }
    list->addStretch();

    count->setText(QString::number(entries.size()));
}

// -----------------------------------------------------
// ENTRY POINT
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    g_innerScrollAreas.clear();

    QWidget *root = new QWidget(stack);
    root->setStyleSheet("background:#282828; color:white; font-family:Sans;");

    QVBoxLayout *rootLay = new QVBoxLayout(root);
    rootLay->setContentsMargins(40, 40, 40, 40);
    rootLay->setSpacing(10);

    QLabel *title = new QLabel("Kernel");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:42px; font-weight:bold;");
    rootLay->addWidget(title);

    // Outer scroll area (manual drag, ignores inner cards — as in apps.cpp)
    OuterScrollArea *scroll = new OuterScrollArea(root);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget *wrap = new QWidget(scroll);
    QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
    wrapLay->setSpacing(10);
    wrapLay->setContentsMargins(0, 0, 0, 0);

    QFrame *outer = new QFrame(wrap);
    outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
    QVBoxLayout *outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(50, 30, 50, 30);
    outerLay->setSpacing(30);

    // ── Kernel info section ─────────────────────────────
    {
        QWidget *sec = new QWidget;
        QVBoxLayout *v = new QVBoxLayout(sec);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(8);

        v->addWidget(makeInfoLabel("Kernel", 26, true));
        v->addWidget(makeInfoLabel(getKernelBuild(), 20, false));

        QWidget *gridWrap = new QWidget;
        QGridLayout *grid = new QGridLayout(gridWrap);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(16);
        grid->setVerticalSpacing(16);
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);

        grid->addWidget(makeMiniCard("Version",      getKernelVersion()), 0, 0);
        grid->addWidget(makeMiniCard("Architecture", getKernelArch()),    0, 1);
        grid->addWidget(makeMiniCard("Init system",  getInitSystem()),    1, 0, 1, 2);

        v->addWidget(gridWrap);
        outerLay->addWidget(sec);
    }

    // ── Boot cmdline ────────────────────────────────────
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30, 24, 30, 24);
        v->setSpacing(10);

        v->addWidget(makeInfoLabel("Boot parameters", 26, true));
        QLabel *cmd = makeInfoLabel(getCmdline(), 17, false);
        cmd->setStyleSheet(
            "color:#dddddd; background:#3a3a3a; border-radius:16px;"
            " padding:14px; font-family:'DejaVu Sans Mono';");
        v->addWidget(cmd);
        outerLay->addWidget(card);
    }

    // ── Loaded modules list ─────────────────────────────
    QVBoxLayout *modList;  QLabel *modCount;
    outerLay->addWidget(makeListCard("Loaded modules", &modList, &modCount));

    // ── Hardware drivers list ───────────────────────────
    QVBoxLayout *drvList;  QLabel *drvCount;
    outerLay->addWidget(makeListCard("Hardware drivers", &drvList, &drvCount));

    wrapLay->addWidget(outer);
    wrapLay->addStretch();

    scroll->setWidget(wrap);
    rootLay->addWidget(scroll);

    // ── Refresh + Back row ──────────────────────────────
    auto refreshAll = [=]() {
        fillList(modList, modCount, getModules());
        fillList(drvList, drvCount, getDriverMap());
    };

    QHBoxLayout *btnRow = new QHBoxLayout();
    btnRow->setSpacing(40);
    btnRow->setAlignment(Qt::AlignHCenter);

    QPushButton *refresh = makeBtn("Refresh");
    refresh->setFixedSize(180, 60);
    QObject::connect(refresh, &QPushButton::clicked, [=]() { refreshAll(); });
    btnRow->addWidget(refresh);

    QPushButton *back = makeBtn("❮");
    back->setFixedSize(140, 60);
    QObject::connect(back, &QPushButton::clicked, [stack]() {
        if (stack) stack->setCurrentIndex(0);
    });
    btnRow->addWidget(back);

    rootLay->addLayout(btnRow);

    // initial load, deferred so the page paints immediately
    QTimer::singleShot(50, root, [=]() { refreshAll(); });

    return root;
}
