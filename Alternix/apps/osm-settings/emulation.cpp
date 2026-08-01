#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QStackedWidget>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QTextStream>
#include <QMessageBox>
#include <QInputDialog>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QStringList>
#include <QVector>

// -----------------------------------------------------
// Alternix compact button style (same as Storage page)
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
        "QPushButton:disabled { background:#3a3a3a; color:#777777; }"
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

// Selected/unselected look for the two-way choice buttons (runner, image type)
static void setToggled(QPushButton *b, bool on)
{
    if (!b) return;
    b->setStyleSheet(on
        ? QString("QPushButton {"
                  " background:#2f5f2f;"
                  " color:white;"
                  " border:2px solid #7CFC00;"
                  " border-radius:16px;"
                  " font-size:22px;"
                  " font-weight:bold;"
                  " padding:6px 16px;"
                  "}")
        : altBtnStyle("white"));
}

// Simple helper to grab stdout (ignore errors/exit code)
static QString runCmd(const QString &cmd, int timeoutMs = 5000)
{
    QProcess p;
    p.start("/bin/sh", {"-c", cmd});
    if (!p.waitForFinished(timeoutMs))
        p.kill();
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

// The bracketed first letter stops pgrep matching the /bin/sh that is running
// the pgrep itself — without it these checks always report "running".
static const char *WAYDROID_CONTAINER_CHECK =
    "pgrep -f '[w]aydroid container' >/dev/null 2>&1 && echo running || echo stopped";
static const char *WAYDROID_SESSION_CHECK =
    "pgrep -f '[w]aydroid session' >/dev/null 2>&1 && echo running || echo stopped";

// -----------------------------------------------------
// Root privileges for the setup actions.
// NOPASSWD fast path first, osm-sudo askpass fallback — same pattern as
// wifi.cpp and startWaydroid(). Prepend this to any script that needs root and
// prefix the privileged commands with $SUDOX. Deliberately NOT nested inside a
// second `sh -c` so scripts can use quotes normally.
// -----------------------------------------------------
static const char *SUDO_PREAMBLE =
    "if sudo -n true 2>/dev/null; then SUDOX=\"sudo -n\"; "
    "else export SUDO_ASKPASS=$(command -v osm-sudo); SUDOX=\"sudo -A\"; fi\n";

// -----------------------------------------------------
// Config — ~/.config/Alternix/osm-settings.conf (QSettings INI)
//   Emulation/Runner        = wine | proton
//   Emulation/ProtonBuild   = directory name of the selected Proton build
//   Emulation/WaydroidImage = VANILLA | GAPPS
// -----------------------------------------------------
static QSettings* cfg()
{
    static QSettings s(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                       QSettings::IniFormat);
    return &s;
}

static QString protonRoot()   { return QDir::homePath() + "/.local/share/Alternix/proton"; }
static QString protonPrefix() { return QDir::homePath() + "/.local/share/Alternix/proton-prefix"; }
static QString protonTmpFile(){ return QDir::homePath() + "/.cache/Alternix/proton-dl.tar.gz"; }

struct ProtonBuild {
    QString name;   // GE-Proton9-20
    QString dir;    // full path to the directory containing the `proton` script
};

// Alternix-managed builds plus anything Steam already has installed.
static QVector<ProtonBuild> findProtonBuilds()
{
    QVector<ProtonBuild> out;
    QStringList roots;
    roots << protonRoot()
          << QDir::homePath() + "/.steam/root/compatibilitytools.d"
          << QDir::homePath() + "/.steam/steam/compatibilitytools.d"
          << QDir::homePath() + "/.local/share/Steam/compatibilitytools.d";

    QStringList seen;
    for (const QString &r : roots) {
        QDir d(r);
        if (!d.exists()) continue;
        const QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &e : entries) {
            const QString full = d.absoluteFilePath(e);
            if (!QFile::exists(full + "/proton")) continue;
            if (seen.contains(e)) continue;
            seen << e;
            ProtonBuild b;
            b.name = e;
            b.dir  = full;
            out.append(b);
        }
    }
    return out;
}

// -----------------------------------------------------
// EmulationPage
// -----------------------------------------------------
class EmulationPage : public QWidget
{
public:
    explicit EmulationPage(QStackedWidget *stack);

private:
    QStackedWidget *m_stack = nullptr;   // osm-settings page stack
    QStackedWidget *m_inner = nullptr;   // main / windows setup / waydroid setup
    QLabel *m_title = nullptr;

    QLabel *m_wayInfo  = nullptr;
    QLabel *m_wineInfo = nullptr;

    // Windows apps setup page
    QLabel      *m_winStatus       = nullptr;
    QLabel      *m_winDetail       = nullptr;
    QPushButton *m_runnerWine      = nullptr;
    QPushButton *m_runnerProton    = nullptr;
    QComboBox   *m_protonCombo     = nullptr;
    QPushButton *m_btnInstallWine  = nullptr;
    QPushButton *m_btnGetProton    = nullptr;
    QPushButton *m_btnDelProton    = nullptr;
    QPushButton *m_btnConfigure    = nullptr;

    // Waydroid setup page
    QLabel      *m_wayStatus       = nullptr;
    QLabel      *m_wayDetail       = nullptr;
    QPushButton *m_imgVanilla      = nullptr;
    QPushButton *m_imgGapps        = nullptr;
    QPushButton *m_btnInstallWay   = nullptr;
    QPushButton *m_btnInitWay      = nullptr;
    QPushButton *m_btnBinder       = nullptr;
    QPushButton *m_btnRemoveWay    = nullptr;

    // page construction
    QWidget *buildMainPage();
    QWidget *buildWindowsPage();
    QWidget *buildWaydroidPage();
    void gotoPage(int idx);

    // helpers
    bool runCmdOk(const QString &cmd, QString &output, int timeoutMs = 30000);
    bool runTask(QLabel *status, const QString &busyText, const QString &cmd,
                 QString &output, int timeoutMs, const QString &progressFile = QString());
    void reportTask(QLabel *status, const QString &what, bool ok, const QString &output);

    QString buildWaydroidInfoHtml();
    QString buildWineInfoHtml();

    void refreshWaydroid();
    void refreshWine();
    void refreshWindowsPage();
    void refreshWaydroidPage();

    void startWaydroid();
    void stopWaydroid();

    void stopWine();

    // setup actions
    void setRunner(const QString &runner);
    QString selectedProtonDir();
    void installWine();
    void downloadProton();
    void removeProton();
    void configureRunner();

    void setWaydroidImage(const QString &img);
    void installWaydroid();
    void initWaydroid();
    void fixBinder();
    void removeWaydroid();
};

// -----------------------------------------------------
// Constructor
// -----------------------------------------------------
EmulationPage::EmulationPage(QStackedWidget *stack)
    : QWidget(stack), m_stack(stack)
{
    setStyleSheet("background:#282828; color:white; font-family:Sans;");

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(40, 40, 40, 40);
    root->setSpacing(10);

    // Title
    m_title = new QLabel("Emulation");
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setStyleSheet("font-size:42px; font-weight:bold;");
    root->addWidget(m_title);

    // Main view / setup views
    m_inner = new QStackedWidget(this);
    m_inner->addWidget(buildMainPage());       // 0
    m_inner->addWidget(buildWindowsPage());    // 1
    m_inner->addWidget(buildWaydroidPage());   // 2
    root->addWidget(m_inner);

    // Back button pinned at bottom — leaves a setup view first, then the page
    QPushButton *back = makeBtn("❮");
    back->setFixedSize(140, 60);
    connect(back, &QPushButton::clicked, this, [this]() {
        if (m_inner && m_inner->currentIndex() != 0) {
            gotoPage(0);
            return;
        }
        if (m_stack)
            m_stack->setCurrentIndex(0);
    });
    root->addWidget(back, 0, Qt::AlignCenter);

    // Initial info
    refreshWaydroid();
    refreshWine();
}

// -----------------------------------------------------
// Main view
// -----------------------------------------------------
QWidget* EmulationPage::buildMainPage()
{
    // Scroll area (no visible scrollbars, kinetic scrolling)
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

    QWidget *wrap = new QWidget(scroll);
    QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
    wrapLay->setSpacing(10);
    wrapLay->setContentsMargins(0,0,0,0);

    // Outer big rounded card
    QFrame *outer = new QFrame(wrap);
    outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
    QVBoxLayout *outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(50, 30, 50, 30);
    outerLay->setSpacing(30);

    // =================================================
    // Waydroid card
    // =================================================
    QFrame *cardWaydroid = new QFrame(outer);
    cardWaydroid->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *wayLay = new QVBoxLayout(cardWaydroid);
    wayLay->setContentsMargins(30, 30, 30, 30);
    wayLay->setSpacing(16);

    QLabel *wayLabel = new QLabel("Android apps (Waydroid)");
    wayLabel->setAlignment(Qt::AlignCenter);
    wayLabel->setStyleSheet("font-size:28px; font-weight:bold;");
    wayLay->addWidget(wayLabel);

    m_wayInfo = new QLabel("Loading...");
    m_wayInfo->setWordWrap(true);
    m_wayInfo->setAlignment(Qt::AlignCenter);
    m_wayInfo->setStyleSheet(
        "QLabel { background:#3a3a3a; border-radius:20px; font-size:24px; padding:20px; }"
    );
    wayLay->addWidget(m_wayInfo);

    // Buttons row 1: Start / Stop
    QHBoxLayout *wayBtnRow = new QHBoxLayout();
    wayBtnRow->setSpacing(16);

    QPushButton *wayStart = makeBtn("Start");
    QPushButton *wayStop  = makeBtn("Stop", "#CC6666");

    wayBtnRow->addWidget(wayStart);
    wayBtnRow->addWidget(wayStop);
    wayLay->addLayout(wayBtnRow);

    // Buttons row 2: Install / setup / Refresh
    QHBoxLayout *wayBtnRow2 = new QHBoxLayout();
    wayBtnRow2->setSpacing(16);

    QPushButton *waySetup   = makeBtn("Install / setup");
    QPushButton *wayRefresh = makeBtn("Refresh");

    wayBtnRow2->addWidget(waySetup);
    wayBtnRow2->addWidget(wayRefresh);
    wayLay->addLayout(wayBtnRow2);

    outerLay->addWidget(cardWaydroid);

    // =================================================
    // Windows apps card (Wine / Proton)
    // =================================================
    QFrame *cardWine = new QFrame(outer);
    cardWine->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *wineLay = new QVBoxLayout(cardWine);
    wineLay->setContentsMargins(30, 30, 30, 30);
    wineLay->setSpacing(16);

    QLabel *wineLabel = new QLabel("Windows apps (Wine / Proton)");
    wineLabel->setAlignment(Qt::AlignCenter);
    wineLabel->setStyleSheet("font-size:28px; font-weight:bold;");
    wineLay->addWidget(wineLabel);

    m_wineInfo = new QLabel("Loading...");
    m_wineInfo->setWordWrap(true);
    m_wineInfo->setAlignment(Qt::AlignCenter);
    m_wineInfo->setStyleSheet(
        "QLabel { background:#3a3a3a; border-radius:20px; font-size:24px; padding:20px; }"
    );
    wineLay->addWidget(m_wineInfo);

    QHBoxLayout *wineBtnRow = new QHBoxLayout();
    wineBtnRow->setSpacing(16);

    QPushButton *wineSetup = makeBtn("Install / setup");
    QPushButton *wineCfg   = makeBtn("Configure");

    wineBtnRow->addWidget(wineSetup);
    wineBtnRow->addWidget(wineCfg);
    wineLay->addLayout(wineBtnRow);

    QHBoxLayout *wineBtnRow2 = new QHBoxLayout();
    wineBtnRow2->setSpacing(16);

    QPushButton *wineStop    = makeBtn("Force-close", "#CC6666");
    QPushButton *wineRefresh = makeBtn("Refresh");

    wineBtnRow2->addWidget(wineStop);
    wineBtnRow2->addWidget(wineRefresh);
    wineLay->addLayout(wineBtnRow2);

    outerLay->addWidget(cardWine);

    // =================================================
    wrapLay->addWidget(outer);
    wrapLay->addStretch();

    scroll->setWidget(wrap);

    // Connections
    connect(wayStart,   &QPushButton::clicked, this, [this]() { startWaydroid();  refreshWaydroid(); });
    connect(wayStop,    &QPushButton::clicked, this, [this]() { stopWaydroid();   refreshWaydroid(); });
    connect(wayRefresh, &QPushButton::clicked, this, [this]() { refreshWaydroid(); });
    connect(waySetup,   &QPushButton::clicked, this, [this]() { gotoPage(2); });

    connect(wineStop,    &QPushButton::clicked, this, [this]() { stopWine();   refreshWine(); });
    connect(wineRefresh, &QPushButton::clicked, this, [this]() { refreshWine(); });
    connect(wineSetup,   &QPushButton::clicked, this, [this]() { gotoPage(1); });
    connect(wineCfg,     &QPushButton::clicked, this, [this]() { configureRunner(); });

    return scroll;
}

// -----------------------------------------------------
// Windows apps setup view
// -----------------------------------------------------
QWidget* EmulationPage::buildWindowsPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

    QWidget *wrap = new QWidget(scroll);
    QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
    wrapLay->setSpacing(10);
    wrapLay->setContentsMargins(0,0,0,0);

    QFrame *outer = new QFrame(wrap);
    outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
    QVBoxLayout *outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(50, 30, 50, 30);
    outerLay->setSpacing(30);

    // ---- Runner choice card ----
    QFrame *cardRunner = new QFrame(outer);
    cardRunner->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *rLay = new QVBoxLayout(cardRunner);
    rLay->setContentsMargins(30, 30, 30, 30);
    rLay->setSpacing(16);

    QLabel *rTitle = new QLabel("Run Windows apps with");
    rTitle->setAlignment(Qt::AlignCenter);
    rTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    rLay->addWidget(rTitle);

    QLabel *rHelp = new QLabel(
        "Wine is the plain compatibility layer and is enough for most desktop "
        "programs. Proton adds DirectX translation for games. Proton is optional "
        "— leave this on Wine if you do not need it.");
    rHelp->setWordWrap(true);
    rHelp->setAlignment(Qt::AlignCenter);
    rHelp->setStyleSheet("QLabel { background:#3a3a3a; border-radius:20px; font-size:22px; padding:16px; }");
    rLay->addWidget(rHelp);

    QHBoxLayout *rRow = new QHBoxLayout();
    rRow->setSpacing(16);
    m_runnerWine   = makeBtn("Wine");
    m_runnerProton = makeBtn("Proton");
    rRow->addWidget(m_runnerWine);
    rRow->addWidget(m_runnerProton);
    rLay->addLayout(rRow);

    outerLay->addWidget(cardRunner);

    // ---- Status card ----
    QFrame *cardStatus = new QFrame(outer);
    cardStatus->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *sLay = new QVBoxLayout(cardStatus);
    sLay->setContentsMargins(30, 30, 30, 30);
    sLay->setSpacing(16);

    QLabel *sTitle = new QLabel("Status");
    sTitle->setAlignment(Qt::AlignCenter);
    sTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    sLay->addWidget(sTitle);

    m_winDetail = new QLabel("Loading...");
    m_winDetail->setWordWrap(true);
    m_winDetail->setAlignment(Qt::AlignCenter);
    m_winDetail->setStyleSheet("QLabel { background:#3a3a3a; border-radius:20px; font-size:24px; padding:20px; }");
    sLay->addWidget(m_winDetail);

    // Last action result — never cleared automatically
    m_winStatus = new QLabel("");
    m_winStatus->setWordWrap(true);
    m_winStatus->setAlignment(Qt::AlignCenter);
    m_winStatus->setStyleSheet("QLabel { font-size:22px; }");
    m_winStatus->setVisible(false);
    sLay->addWidget(m_winStatus);

    outerLay->addWidget(cardStatus);

    // ---- Wine card ----
    QFrame *cardWine = new QFrame(outer);
    cardWine->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *wLay = new QVBoxLayout(cardWine);
    wLay->setContentsMargins(30, 30, 30, 30);
    wLay->setSpacing(16);

    QLabel *wTitle = new QLabel("Wine");
    wTitle->setAlignment(Qt::AlignCenter);
    wTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    wLay->addWidget(wTitle);

    QHBoxLayout *wRow = new QHBoxLayout();
    wRow->setSpacing(16);
    m_btnInstallWine = makeBtn("Install Wine");
    m_btnConfigure   = makeBtn("Configure");
    wRow->addWidget(m_btnInstallWine);
    wRow->addWidget(m_btnConfigure);
    wLay->addLayout(wRow);

    outerLay->addWidget(cardWine);

    // ---- Proton card ----
    QFrame *cardProton = new QFrame(outer);
    cardProton->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *pLay = new QVBoxLayout(cardProton);
    pLay->setContentsMargins(30, 30, 30, 30);
    pLay->setSpacing(16);

    QLabel *pTitle = new QLabel("Proton builds");
    pTitle->setAlignment(Qt::AlignCenter);
    pTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    pLay->addWidget(pTitle);

    m_protonCombo = new QComboBox(cardProton);
    m_protonCombo->setStyleSheet(
        "QComboBox {"
        " background:#3a3a3a; color:white; border:1px solid #222222;"
        " border-radius:16px; font-size:22px; padding:10px 16px; min-height:44px; }"
        "QComboBox::drop-down { width:44px; border:none; }"
        "QComboBox QAbstractItemView {"
        " background:#3a3a3a; color:white; font-size:22px;"
        " selection-background-color:#555555; }"
    );
    pLay->addWidget(m_protonCombo);

    QHBoxLayout *pRow = new QHBoxLayout();
    pRow->setSpacing(16);
    m_btnGetProton = makeBtn("Download Proton");
    m_btnDelProton = makeBtn("Remove", "#CC6666");
    pRow->addWidget(m_btnGetProton);
    pRow->addWidget(m_btnDelProton);
    pLay->addLayout(pRow);

    outerLay->addWidget(cardProton);

    wrapLay->addWidget(outer);
    wrapLay->addStretch();
    scroll->setWidget(wrap);

    connect(m_runnerWine,     &QPushButton::clicked, this, [this]() { setRunner("wine");   });
    connect(m_runnerProton,   &QPushButton::clicked, this, [this]() { setRunner("proton"); });
    connect(m_btnInstallWine, &QPushButton::clicked, this, [this]() { installWine();    });
    connect(m_btnConfigure,   &QPushButton::clicked, this, [this]() { configureRunner(); });
    connect(m_btnGetProton,   &QPushButton::clicked, this, [this]() { downloadProton(); });
    connect(m_btnDelProton,   &QPushButton::clicked, this, [this]() { removeProton();   });
    connect(m_protonCombo, QOverload<int>::of(&QComboBox::activated), this, [this](int) {
        if (!m_protonCombo) return;
        cfg()->setValue("Emulation/ProtonBuild", m_protonCombo->currentText());
        cfg()->sync();
        refreshWine();
    });

    return scroll;
}

// -----------------------------------------------------
// Waydroid setup view
// -----------------------------------------------------
QWidget* EmulationPage::buildWaydroidPage()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

    QWidget *wrap = new QWidget(scroll);
    QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
    wrapLay->setSpacing(10);
    wrapLay->setContentsMargins(0,0,0,0);

    QFrame *outer = new QFrame(wrap);
    outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
    QVBoxLayout *outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(50, 30, 50, 30);
    outerLay->setSpacing(30);

    // ---- Status card ----
    QFrame *cardStatus = new QFrame(outer);
    cardStatus->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *sLay = new QVBoxLayout(cardStatus);
    sLay->setContentsMargins(30, 30, 30, 30);
    sLay->setSpacing(16);

    QLabel *sTitle = new QLabel("Status");
    sTitle->setAlignment(Qt::AlignCenter);
    sTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    sLay->addWidget(sTitle);

    m_wayDetail = new QLabel("Loading...");
    m_wayDetail->setWordWrap(true);
    m_wayDetail->setAlignment(Qt::AlignCenter);
    m_wayDetail->setStyleSheet("QLabel { background:#3a3a3a; border-radius:20px; font-size:24px; padding:20px; }");
    sLay->addWidget(m_wayDetail);

    m_wayStatus = new QLabel("");
    m_wayStatus->setWordWrap(true);
    m_wayStatus->setAlignment(Qt::AlignCenter);
    m_wayStatus->setStyleSheet("QLabel { font-size:22px; }");
    m_wayStatus->setVisible(false);
    sLay->addWidget(m_wayStatus);

    outerLay->addWidget(cardStatus);

    // ---- Install card ----
    QFrame *cardInstall = new QFrame(outer);
    cardInstall->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *iLay = new QVBoxLayout(cardInstall);
    iLay->setContentsMargins(30, 30, 30, 30);
    iLay->setSpacing(16);

    QLabel *iTitle = new QLabel("Install");
    iTitle->setAlignment(Qt::AlignCenter);
    iTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    iLay->addWidget(iTitle);

    QLabel *iHelp = new QLabel(
        "Waydroid runs Android apps in a container. It is optional — install it "
        "only if you need Android apps. Setup order: Install, then Binder, then "
        "Android system.");
    iHelp->setWordWrap(true);
    iHelp->setAlignment(Qt::AlignCenter);
    iHelp->setStyleSheet("QLabel { background:#3a3a3a; border-radius:20px; font-size:22px; padding:16px; }");
    iLay->addWidget(iHelp);

    QHBoxLayout *iRow = new QHBoxLayout();
    iRow->setSpacing(16);
    m_btnInstallWay = makeBtn("Install Waydroid");
    m_btnBinder     = makeBtn("Set up binder");
    iRow->addWidget(m_btnInstallWay);
    iRow->addWidget(m_btnBinder);
    iLay->addLayout(iRow);

    outerLay->addWidget(cardInstall);

    // ---- Android image card ----
    QFrame *cardImg = new QFrame(outer);
    cardImg->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *gLay = new QVBoxLayout(cardImg);
    gLay->setContentsMargins(30, 30, 30, 30);
    gLay->setSpacing(16);

    QLabel *gTitle = new QLabel("Android system image");
    gTitle->setAlignment(Qt::AlignCenter);
    gTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    gLay->addWidget(gTitle);

    QLabel *gHelp = new QLabel(
        "Vanilla is plain Android. GAPPS includes Google Play services and needs "
        "the device to be registered with Google before Play will sign in. "
        "Downloading the image needs about 1 GB and can take a while.");
    gHelp->setWordWrap(true);
    gHelp->setAlignment(Qt::AlignCenter);
    gHelp->setStyleSheet("QLabel { background:#3a3a3a; border-radius:20px; font-size:22px; padding:16px; }");
    gLay->addWidget(gHelp);

    QHBoxLayout *gRow = new QHBoxLayout();
    gRow->setSpacing(16);
    m_imgVanilla = makeBtn("Vanilla");
    m_imgGapps   = makeBtn("GAPPS");
    gRow->addWidget(m_imgVanilla);
    gRow->addWidget(m_imgGapps);
    gLay->addLayout(gRow);

    QHBoxLayout *gRow2 = new QHBoxLayout();
    gRow2->setSpacing(16);
    m_btnInitWay   = makeBtn("Download Android system");
    m_btnRemoveWay = makeBtn("Remove Waydroid", "#CC6666");
    gRow2->addWidget(m_btnInitWay);
    gRow2->addWidget(m_btnRemoveWay);
    gLay->addLayout(gRow2);

    outerLay->addWidget(cardImg);

    wrapLay->addWidget(outer);
    wrapLay->addStretch();
    scroll->setWidget(wrap);

    connect(m_imgVanilla,    &QPushButton::clicked, this, [this]() { setWaydroidImage("VANILLA"); });
    connect(m_imgGapps,      &QPushButton::clicked, this, [this]() { setWaydroidImage("GAPPS");   });
    connect(m_btnInstallWay, &QPushButton::clicked, this, [this]() { installWaydroid(); });
    connect(m_btnBinder,     &QPushButton::clicked, this, [this]() { fixBinder();       });
    connect(m_btnInitWay,    &QPushButton::clicked, this, [this]() { initWaydroid();    });
    connect(m_btnRemoveWay,  &QPushButton::clicked, this, [this]() { removeWaydroid();  });

    return scroll;
}

void EmulationPage::gotoPage(int idx)
{
    if (!m_inner) return;
    m_inner->setCurrentIndex(idx);

    if (m_title) {
        if (idx == 1)      m_title->setText("Windows apps");
        else if (idx == 2) m_title->setText("Waydroid");
        else               m_title->setText("Emulation");
    }

    if (idx == 1) {
        refreshWindowsPage();
    } else if (idx == 2) {
        refreshWaydroidPage();
    } else {
        refreshWaydroid();
        refreshWine();
    }
}

// -----------------------------------------------------
// Helpers
// -----------------------------------------------------
bool EmulationPage::runCmdOk(const QString &cmd, QString &output, int timeoutMs)
{
    QProcess p;
    p.start("/bin/sh", {"-c", cmd});
    if (!p.waitForStarted(5000)) {
        output = "Could not start /bin/sh.";
        return false;
    }

    // Poll rather than block: waitForFinished() with no argument gives up after
    // 30s, and ~QProcess then kills a child that was still working normally.
    QElapsedTimer timer;
    timer.start();
    while (p.state() != QProcess::NotRunning) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        p.waitForFinished(50);
        if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
            p.kill();
            p.waitForFinished(2000);
            output = QString::fromLocal8Bit(p.readAllStandardOutput())
                   + QString::fromLocal8Bit(p.readAllStandardError())
                   + QString("\n\nTimed out after %1 seconds.").arg(timeoutMs / 1000);
            return false;
        }
    }

    output = QString::fromLocal8Bit(p.readAllStandardOutput())
           + QString::fromLocal8Bit(p.readAllStandardError());

    return (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
}

// Long-running setup task: same polling approach as runCmdOk, but the page is
// locked while it runs and the status label reports elapsed time (and bytes
// fetched, when progressFile is given) so a 20-minute download is not a
// frozen screen.
bool EmulationPage::runTask(QLabel *status, const QString &busyText, const QString &cmd,
                            QString &output, int timeoutMs, const QString &progressFile)
{
    QProcess p;
    p.start("/bin/sh", {"-c", cmd});
    if (!p.waitForStarted(5000)) {
        output = "Could not start /bin/sh.";
        return false;
    }

    setEnabled(false);
    if (status) {
        status->setVisible(true);
        status->setText(QString("<span style='color:#FFAA00;'>%1</span>").arg(busyText.toHtmlEscaped()));
    }

    QElapsedTimer timer;
    timer.start();
    qint64 lastTick = -1;
    bool timedOut = false;

    while (p.state() != QProcess::NotRunning) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        p.waitForFinished(50);

        const qint64 tick = timer.elapsed() / 500;
        if (status && tick != lastTick) {
            lastTick = tick;
            QString extra;
            if (!progressFile.isEmpty()) {
                QFileInfo fi(progressFile);
                if (fi.exists())
                    extra = QString(" — %1 MB").arg(fi.size() / (1024 * 1024));
            }
            status->setText(QString("<span style='color:#FFAA00;'>%1%2 (%3s)</span>")
                            .arg(busyText.toHtmlEscaped(), extra)
                            .arg(timer.elapsed() / 1000));
        }

        if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
            p.kill();
            p.waitForFinished(2000);
            timedOut = true;
            break;
        }
    }

    output = QString::fromLocal8Bit(p.readAllStandardOutput())
           + QString::fromLocal8Bit(p.readAllStandardError());

    setEnabled(true);

    if (timedOut) {
        output += QString("\n\nTimed out after %1 seconds.").arg(timeoutMs / 1000);
        return false;
    }

    return (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
}

// Result stays on screen until the next action replaces it.
void EmulationPage::reportTask(QLabel *status, const QString &what, bool ok, const QString &output)
{
    if (status) {
        status->setVisible(true);
        status->setText(ok
            ? QString("<span style='color:#7CFC00;'>%1: done.</span>").arg(what.toHtmlEscaped())
            : QString("<span style='color:#FF5555;'>%1: FAILED. See details below.</span>")
                  .arg(what.toHtmlEscaped()));
    }

    if (!ok) {
        QString tail = output.trimmed();
        if (tail.length() > 2000)
            tail = tail.right(2000);
        if (tail.isEmpty())
            tail = "(no output)";
        QMessageBox::warning(this, what + " failed", what + " failed.\n\n" + tail);
    }
}

// Waydroid info — systemd services unavailable on Devuan, use process/CLI check instead
QString EmulationPage::buildWaydroidInfoHtml()
{
    QString hasWaydroid = runCmd("command -v waydroid >/dev/null 2>&1 && echo yes || echo no");
    if (hasWaydroid.trimmed() != "yes") {
        return "Waydroid is not installed.<br>"
               "<span style='color:#FFAA00;'>Use Install / setup to add Android app support.</span>";
    }

    QString version = runCmd("waydroid -V 2>/dev/null");

    // Container and session are separate processes and fail independently.
    QString container = runCmd(WAYDROID_CONTAINER_CHECK);
    QString session   = runCmd(WAYDROID_SESSION_CHECK);

    // /dev/binderfs existing only means binderfs is mounted; Waydroid needs the
    // control node, and some packages name the nodes anbox-* instead.
    QString binder = runCmd(
        "if [ -e /dev/binderfs/binder-control ]; then echo ok; "
        "elif [ -e /dev/binderfs/anbox-binder ]; then echo 'nodes named anbox-*'; "
        "elif [ -d /dev/binderfs ]; then echo 'mounted, no control node'; "
        "else echo missing; fi");

    QString wayland = qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")
                    ? QString("none (X11 session)")
                    : qEnvironmentVariable("WAYLAND_DISPLAY");

    const bool inited = QFile::exists("/var/lib/waydroid/waydroid.cfg");

    QString contColor = (container.trimmed() == "running") ? "#7CFC00" : "#FF5555";
    QString sessColor = (session.trimmed()   == "running") ? "#7CFC00" : "#FF5555";
    QString bindColor = (binder.trimmed()    == "ok")      ? "#7CFC00" : "#FF5555";
    QString wayColor  = qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ? "#FF5555" : "#7CFC00";
    QString initColor = inited ? "#7CFC00" : "#FF5555";

    QString html;
    html  = "Installed: <b>yes</b><br>";
    html += "Version: " + version.toHtmlEscaped() + "<br>";
    html += QString("Android system: <span style='color:%1;'>%2</span><br>")
            .arg(initColor, inited ? "installed" : "not downloaded");
    html += QString("Container: <span style='color:%1;'>%2</span><br>")
            .arg(contColor, container.toHtmlEscaped());
    html += QString("Session: <span style='color:%1;'>%2</span><br>")
            .arg(sessColor, session.toHtmlEscaped());
    html += QString("Binder: <span style='color:%1;'>%2</span><br>")
            .arg(bindColor, binder.toHtmlEscaped());
    html += QString("Wayland: <span style='color:%1;'>%2</span>")
            .arg(wayColor, wayland.toHtmlEscaped());

    if (!inited)
        html += "<br><span style='color:#FFAA00;'>The Android system image has not "
                "been downloaded yet. Use Install / setup.</span>";

    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        html += "<br><span style='color:#FFAA00;'>Waydroid renders through Wayland "
                "and cannot start on an X11 session.</span>";

    return html;
}

// Deeper Wine/Proton info (wineserver based)
QString EmulationPage::buildWineInfoHtml()
{
    const QString runner = cfg()->value("Emulation/Runner", "wine").toString();
    const QVector<ProtonBuild> builds = findProtonBuilds();
    const QString protonDir = selectedProtonDir();

    QString hasWine = runCmd("command -v wine >/dev/null 2>&1 && echo yes || echo no");
    const bool wineOk = (hasWine.trimmed() == "yes");

    if (!wineOk && builds.isEmpty()) {
        return "No Windows compatibility layer installed.<br>"
               "<span style='color:#FFAA00;'>Use Install / setup to add Wine, and "
               "optionally Proton.</span>";
    }

    QString running = runCmd("pidof wineserver >/dev/null 2>&1 && echo running || echo stopped");
    QString backend = runCmd("if [ -n \"$WAYLAND_DISPLAY\" ]; then echo Wayland; "
                             "elif [ -n \"$DISPLAY\" ]; then echo X11; "
                             "else echo Unknown; fi");
    QString runColor = (running.trimmed() == "running") ? "#7CFC00" : "#CCCCCC";

    QString html;

    if (runner == "proton") {
        if (protonDir.isEmpty()) {
            html += "<span style='color:#FF5555;'>Runner: Proton — no Proton build "
                    "installed.</span><br>";
        } else {
            html += QString("Runner: <span style='color:#7CFC00;'>Proton (%1)</span><br>")
                    .arg(QFileInfo(protonDir).fileName().toHtmlEscaped());
        }
    } else {
        html += "Runner: <b>Wine</b><br>";
    }

    if (wineOk) {
        QString version = runCmd("wine --version 2>/dev/null");
        html += "Wine: " + version.toHtmlEscaped() + "<br>";
    } else {
        html += "<span style='color:#FF5555;'>Wine: not installed</span><br>";
    }

    html += QString("Proton builds installed: %1<br>").arg(builds.size());
    html += QString("Wineserver: <span style='color:%1;'>%2</span><br>")
            .arg(runColor, running.toHtmlEscaped());

    if (runner == "proton" && !protonDir.isEmpty())
        html += "Prefix: " + (protonPrefix() + "/pfx").toHtmlEscaped() + "<br>";
    else
        html += "Prefix: " + runCmd("printf \"%s\" \"${WINEPREFIX:-$HOME/.wine}\" 2>/dev/null")
                             .toHtmlEscaped() + "<br>";

    html += "Graphics backend: " + backend.toHtmlEscaped();

    if (runner == "proton" && protonDir.isEmpty())
        html += "<br><span style='color:#FFAA00;'>Windows apps will fall back to Wine "
                "until a Proton build is downloaded.</span>";

    return html;
}

// -----------------------------------------------------
// Refresh functions
// -----------------------------------------------------
void EmulationPage::refreshWaydroid()
{
    if (m_wayInfo)
        m_wayInfo->setText(buildWaydroidInfoHtml());
}

void EmulationPage::refreshWine()
{
    if (m_wineInfo)
        m_wineInfo->setText(buildWineInfoHtml());
}

void EmulationPage::refreshWindowsPage()
{
    const QString runner = cfg()->value("Emulation/Runner", "wine").toString();
    setToggled(m_runnerWine,   runner != "proton");
    setToggled(m_runnerProton, runner == "proton");

    const QVector<ProtonBuild> builds = findProtonBuilds();
    const QString saved = cfg()->value("Emulation/ProtonBuild").toString();

    if (m_protonCombo) {
        m_protonCombo->blockSignals(true);
        m_protonCombo->clear();
        for (const ProtonBuild &b : builds)
            m_protonCombo->addItem(b.name);
        if (builds.isEmpty()) {
            m_protonCombo->addItem("none installed");
            m_protonCombo->setEnabled(false);
        } else {
            m_protonCombo->setEnabled(true);
            int idx = m_protonCombo->findText(saved);
            m_protonCombo->setCurrentIndex(idx >= 0 ? idx : 0);
        }
        m_protonCombo->blockSignals(false);
    }

    const bool wineOk =
        (runCmd("command -v wine >/dev/null 2>&1 && echo yes || echo no").trimmed() == "yes");

    if (m_btnInstallWine)
        m_btnInstallWine->setText(wineOk ? "Reinstall Wine" : "Install Wine");
    if (m_btnDelProton)
        m_btnDelProton->setEnabled(!builds.isEmpty());
    if (m_btnConfigure)
        m_btnConfigure->setEnabled(wineOk || !builds.isEmpty());

    if (m_winDetail)
        m_winDetail->setText(buildWineInfoHtml());

    refreshWine();
}

void EmulationPage::refreshWaydroidPage()
{
    const QString img = cfg()->value("Emulation/WaydroidImage", "VANILLA").toString();
    setToggled(m_imgVanilla, img != "GAPPS");
    setToggled(m_imgGapps,   img == "GAPPS");

    const bool installed =
        (runCmd("command -v waydroid >/dev/null 2>&1 && echo yes || echo no").trimmed() == "yes");
    const bool inited = QFile::exists("/var/lib/waydroid/waydroid.cfg");

    if (m_btnInstallWay)
        m_btnInstallWay->setText(installed ? "Reinstall Waydroid" : "Install Waydroid");
    if (m_btnInitWay) {
        m_btnInitWay->setEnabled(installed);
        m_btnInitWay->setText(inited ? "Re-download Android system" : "Download Android system");
    }
    if (m_btnRemoveWay)
        m_btnRemoveWay->setEnabled(installed);

    if (m_wayDetail)
        m_wayDetail->setText(buildWaydroidInfoHtml());

    refreshWaydroid();
}

// -----------------------------------------------------
// Start/Stop actions
// -----------------------------------------------------
// Devuan has no systemd unit for the container half. Waydroid is two long-lived
// foreground processes: "waydroid container start" (root, owns the LXC
// container — this is what the systemd unit used to run) and
// "waydroid session start" (user). Neither returns, so both are launched
// detached rather than waited on.
void EmulationPage::startWaydroid()
{
    if (runCmd("command -v waydroid >/dev/null 2>&1 && echo yes || echo no").trimmed() != "yes") {
        QMessageBox::warning(
            this,
            "Waydroid not installed",
            "Waydroid is not installed.\n\n"
            "Use Install / setup on this page to install it first."
        );
        return;
    }

    if (!QFile::exists("/var/lib/waydroid/waydroid.cfg")) {
        QMessageBox::warning(
            this,
            "Waydroid cannot start",
            "The Android system image has not been downloaded.\n\n"
            "Use Install / setup, then 'Download Android system'."
        );
        return;
    }

    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        QMessageBox::warning(
            this,
            "Waydroid cannot start",
            "No Wayland compositor is running (WAYLAND_DISPLAY is unset).\n\n"
            "Waydroid renders through Wayland and cannot run on this X11 session."
        );
        return;
    }

    if (!QFile::exists("/dev/binderfs/binder-control")) {
        QMessageBox::warning(
            this,
            "Waydroid cannot start",
            "/dev/binderfs/binder-control is missing.\n\n"
            "The binder kernel module is not loaded, or binderfs is not mounted.\n\n"
            "Use Install / setup, then 'Set up binder'."
        );
        return;
    }

    if (runCmd(WAYDROID_CONTAINER_CHECK).trimmed() != "running") {
        // NOPASSWD fast path first, osm-sudo askpass fallback — same pattern as wifi.cpp.
        QProcess::startDetached("/bin/sh", QStringList()
            << "-c"
            << "sudo -n waydroid container start >/dev/null 2>&1 || "
               "{ SUDO_ASKPASS=$(command -v osm-sudo) sudo -A waydroid container start >/dev/null 2>&1; }");

        // Wait for it without freezing the page.
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 250);
            if (runCmd(WAYDROID_CONTAINER_CHECK).trimmed() == "running")
                break;
        }

        if (runCmd(WAYDROID_CONTAINER_CHECK).trimmed() != "running") {
            QMessageBox::warning(
                this,
                "Waydroid start failed",
                "The Waydroid container did not start.\n\n"
                "Check that the Android system has been downloaded and that the "
                "binder module is loaded."
            );
            return;
        }
    }

    QProcess::startDetached("/bin/sh", QStringList()
        << "-c" << "waydroid session start >/dev/null 2>&1");
}

// Devuan: no systemd container service — use waydroid CLI directly
void EmulationPage::stopWaydroid()
{
    QString out;
    bool ok = runCmdOk("waydroid session stop", out, 30000);
    if (!ok) {
        QMessageBox::warning(
            this,
            "Waydroid stop failed",
            "Could not stop Waydroid session.\n\n" + out
        );
    }
}

// Wine has no lifecycle to start — wineserver spawns on demand when a Windows
// program runs and exits on its own. Only the force-close escape hatch remains.
void EmulationPage::stopWine()
{
    if (runCmd("pidof wineserver >/dev/null 2>&1 && echo running || echo stopped")
            .trimmed() != "running")
        return;   // nothing running is not an error

    QString out;
    bool ok = runCmdOk("wineserver -k", out, 15000);
    if (!ok) {
        QMessageBox::warning(
            this,
            "Force-close failed",
            "Could not close the running Windows programs.\n\n" + out
        );
    }
}

// -----------------------------------------------------
// Windows apps setup actions
// -----------------------------------------------------
void EmulationPage::setRunner(const QString &runner)
{
    if (runner == "proton" && selectedProtonDir().isEmpty()) {
        QMessageBox::warning(
            this,
            "No Proton installed",
            "No Proton build is installed.\n\n"
            "Use 'Download Proton' first, then select Proton here."
        );
        return;
    }

    cfg()->setValue("Emulation/Runner", runner);
    cfg()->sync();
    refreshWindowsPage();
}

QString EmulationPage::selectedProtonDir()
{
    const QVector<ProtonBuild> builds = findProtonBuilds();
    if (builds.isEmpty())
        return QString();

    const QString saved = cfg()->value("Emulation/ProtonBuild").toString();
    for (const ProtonBuild &b : builds)
        if (b.name == saved)
            return b.dir;

    return builds.first().dir;
}

void EmulationPage::installWine()
{
    // 32-bit support is a separate step: if the i386 architecture cannot be
    // added the 64-bit install must still succeed, with a loud warning.
    const QString cmd = QString(SUDO_PREAMBLE) +
        "set -e\n"
        "$SUDOX dpkg --add-architecture i386 || echo \"WARNING: could not add the i386 architecture; 32-bit Windows programs will not run.\"\n"
        "$SUDOX env DEBIAN_FRONTEND=noninteractive apt-get update\n"
        "$SUDOX env DEBIAN_FRONTEND=noninteractive apt-get -y install wine wine64 winbind\n"
        "$SUDOX env DEBIAN_FRONTEND=noninteractive apt-get -y install wine32:i386 "
        "|| echo \"WARNING: wine32:i386 could not be installed; 32-bit Windows programs will not run.\"\n";

    QString out;
    const bool ok = runTask(m_winStatus, "Installing Wine", cmd, out, 30 * 60 * 1000);
    reportTask(m_winStatus, "Install Wine", ok, out);
    refreshWindowsPage();
}

void EmulationPage::downloadProton()
{
    // Release list straight from the GitHub API — no extra tooling, just the
    // download URLs pulled out with grep.
    const QString listCmd =
        "if command -v curl >/dev/null 2>&1; then "
        "  curl -sL \"https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases?per_page=10\"; "
        "elif command -v wget >/dev/null 2>&1; then "
        "  wget -qO- \"https://api.github.com/repos/GloriousEggroll/proton-ge-custom/releases?per_page=10\"; "
        "else echo NO_DOWNLOADER; fi "
        "| grep -o 'https://[^\"]*GE-Proton[^\"]*\\.tar\\.gz' | head -n 8";

    QString listOut;
    const bool listOk = runTask(m_winStatus, "Checking for Proton builds", listCmd, listOut, 60 * 1000);

    QStringList urls = listOut.split('\n', Qt::SkipEmptyParts);
    for (QString &u : urls) u = u.trimmed();

    if (!listOk || urls.isEmpty()) {
        if (m_winStatus) {
            m_winStatus->setVisible(true);
            m_winStatus->setText("<span style='color:#FF5555;'>Could not get the Proton "
                                 "build list.</span>");
        }
        QMessageBox::warning(
            this,
            "Proton download failed",
            "Could not get the list of Proton builds.\n\n"
            "Check the network connection, and that curl or wget is installed.\n\n"
            + listOut.right(1000)
        );
        return;
    }

    QStringList names;
    for (const QString &u : urls) {
        QString n = u.section('/', -1);
        n.remove(".tar.gz");
        names << n;
    }

    bool chosen = false;
    const QString pick = QInputDialog::getItem(
        this, "Download Proton", "Proton build to install:", names, 0, false, &chosen);
    if (!chosen || pick.isEmpty())
        return;

    const int idx = names.indexOf(pick);
    if (idx < 0)
        return;

    const QString url = urls.at(idx);

    const QString dlCmd = QString(
        "set -e\n"
        "DEST=\"%1\"\n"
        "TMP=\"%2\"\n"
        "mkdir -p \"$DEST\"\n"
        "mkdir -p \"$(dirname \"$TMP\")\"\n"
        "rm -f \"$TMP\"\n"
        "if command -v curl >/dev/null 2>&1; then curl -L --fail -o \"$TMP\" \"%3\"; "
        "else wget -O \"$TMP\" \"%3\"; fi\n"
        "tar -xf \"$TMP\" -C \"$DEST\"\n"
        "rm -f \"$TMP\"\n"
    ).arg(protonRoot(), protonTmpFile(), url);

    QString out;
    const bool ok = runTask(m_winStatus, "Downloading " + pick, dlCmd, out,
                            60 * 60 * 1000, protonTmpFile());
    reportTask(m_winStatus, "Download " + pick, ok, out);

    if (ok) {
        cfg()->setValue("Emulation/ProtonBuild", pick);
        cfg()->sync();
    }
    refreshWindowsPage();
}

void EmulationPage::removeProton()
{
    if (!m_protonCombo || !m_protonCombo->isEnabled())
        return;

    const QString name = m_protonCombo->currentText();
    QString dir;
    const QVector<ProtonBuild> builds = findProtonBuilds();
    for (const ProtonBuild &b : builds)
        if (b.name == name) { dir = b.dir; break; }

    if (dir.isEmpty())
        return;

    // Only remove builds Alternix installed — leave Steam's own copies alone.
    if (!dir.startsWith(protonRoot() + "/")) {
        QMessageBox::warning(
            this,
            "Cannot remove",
            name + " was not installed by Alternix.\n\n"
            "It lives in a Steam folder and has to be removed from Steam."
        );
        return;
    }

    if (QMessageBox::question(this, "Remove Proton",
                              "Remove " + name + "?\n\nWindows programs using it will stop working.",
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;

    QString out;
    const bool ok = runTask(m_winStatus, "Removing " + name,
                            QString("rm -rf \"%1\"").arg(dir), out, 5 * 60 * 1000);
    reportTask(m_winStatus, "Remove " + name, ok, out);

    if (ok && cfg()->value("Emulation/ProtonBuild").toString() == name) {
        cfg()->remove("Emulation/ProtonBuild");
        if (findProtonBuilds().isEmpty())
            cfg()->setValue("Emulation/Runner", "wine");
        cfg()->sync();
    }
    refreshWindowsPage();
}

// winecfg, run through whichever runner is selected.
void EmulationPage::configureRunner()
{
    const QString runner    = cfg()->value("Emulation/Runner", "wine").toString();
    const QString protonDir = selectedProtonDir();

    if (runner == "proton" && !protonDir.isEmpty()) {
        if (runCmd("command -v python3 >/dev/null 2>&1 && echo yes || echo no").trimmed() != "yes") {
            QMessageBox::warning(
                this,
                "Proton cannot run",
                "python3 is not installed.\n\n"
                "Proton needs python3 to start."
            );
            return;
        }

        const QString cmd = QString(
            "export STEAM_COMPAT_DATA_PATH=\"%1\"\n"
            "export STEAM_COMPAT_CLIENT_INSTALL_PATH=\"$HOME/.steam/steam\"\n"
            "mkdir -p \"$STEAM_COMPAT_DATA_PATH\" \"$STEAM_COMPAT_CLIENT_INSTALL_PATH\"\n"
            "\"%2/proton\" run winecfg >/dev/null 2>&1\n"
        ).arg(protonPrefix(), protonDir);

        QProcess::startDetached("/bin/sh", QStringList() << "-c" << cmd);
        return;
    }

    if (runCmd("command -v winecfg >/dev/null 2>&1 && echo yes || echo no").trimmed() != "yes") {
        QMessageBox::warning(
            this,
            "Wine not installed",
            "winecfg was not found.\n\n"
            "Install Wine on the Windows apps setup page first."
        );
        return;
    }

    QProcess::startDetached("/bin/sh", QStringList() << "-c" << "winecfg >/dev/null 2>&1");
}

// -----------------------------------------------------
// Waydroid setup actions
// -----------------------------------------------------
void EmulationPage::setWaydroidImage(const QString &img)
{
    cfg()->setValue("Emulation/WaydroidImage", img);
    cfg()->sync();
    refreshWaydroidPage();
}

void EmulationPage::installWaydroid()
{
    const QString cmd = QString(SUDO_PREAMBLE) +
        "set -e\n"
        "$SUDOX env DEBIAN_FRONTEND=noninteractive apt-get update\n"
        "if ! apt-cache show waydroid >/dev/null 2>&1; then\n"
        "  echo \"ERROR: no waydroid package is available in the configured repositories.\"\n"
        "  exit 1\n"
        "fi\n"
        "$SUDOX env DEBIAN_FRONTEND=noninteractive apt-get -y install waydroid\n";

    QString out;
    const bool ok = runTask(m_wayStatus, "Installing Waydroid", cmd, out, 30 * 60 * 1000);
    reportTask(m_wayStatus, "Install Waydroid", ok, out);
    refreshWaydroidPage();
}

// binder module + binderfs, made permanent for the next boot.
void EmulationPage::fixBinder()
{
    const QString cmd = QString(SUDO_PREAMBLE) +
        "set -e\n"
        "$SUDOX modprobe binder_linux devices=binder,hwbinder,vndbinder 2>/dev/null "
        "|| $SUDOX modprobe binder_linux 2>/dev/null "
        "|| echo \"WARNING: binder_linux module could not be loaded; it may be built into the kernel.\"\n"
        "if ! grep -q \"^binder_linux\" /etc/modules 2>/dev/null; then\n"
        "  echo binder_linux | $SUDOX tee -a /etc/modules >/dev/null\n"
        "fi\n"
        "$SUDOX mkdir -p /dev/binderfs\n"
        "mountpoint -q /dev/binderfs || $SUDOX mount -t binder none /dev/binderfs\n"
        "if ! grep -q \"/dev/binderfs\" /etc/fstab 2>/dev/null; then\n"
        "  printf \"none\\t/dev/binderfs\\tbinder\\tnofail\\t0\\t0\\n\" | $SUDOX tee -a /etc/fstab >/dev/null\n"
        "fi\n"
        "if [ ! -e /dev/binderfs/binder-control ]; then\n"
        "  echo \"ERROR: /dev/binderfs is mounted but binder-control is missing. This kernel has no binder support.\"\n"
        "  exit 1\n"
        "fi\n";

    QString out;
    const bool ok = runTask(m_wayStatus, "Setting up binder", cmd, out, 3 * 60 * 1000);
    reportTask(m_wayStatus, "Set up binder", ok, out);
    refreshWaydroidPage();
}

void EmulationPage::initWaydroid()
{
    const bool inited = QFile::exists("/var/lib/waydroid/waydroid.cfg");
    if (inited) {
        if (QMessageBox::question(this, "Re-download Android system",
                                  "The Android system is already installed.\n\n"
                                  "Downloading it again replaces it and loses installed "
                                  "Android apps. Continue?",
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) != QMessageBox::Yes)
            return;
    }

    const QString img = cfg()->value("Emulation/WaydroidImage", "VANILLA").toString();
    QString args;
    if (img == "GAPPS")
        args += " -s GAPPS";
    if (inited)
        args += " -f";

    const QString cmd = QString(SUDO_PREAMBLE) +
        "set -e\n"
        "$SUDOX waydroid init" + args + "\n";

    QString out;
    const bool ok = runTask(m_wayStatus, "Downloading Android system (" + img + ")",
                            cmd, out, 90 * 60 * 1000);
    reportTask(m_wayStatus, "Download Android system", ok, out);
    refreshWaydroidPage();
}

void EmulationPage::removeWaydroid()
{
    if (QMessageBox::question(this, "Remove Waydroid",
                              "Remove Waydroid and the downloaded Android system?\n\n"
                              "Installed Android apps and their data will be deleted.",
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;

    const QString cmd = QString(SUDO_PREAMBLE) +
        "waydroid session stop >/dev/null 2>&1 || true\n"
        "$SUDOX waydroid container stop >/dev/null 2>&1 || true\n"
        "$SUDOX env DEBIAN_FRONTEND=noninteractive apt-get -y purge waydroid\n"
        "$SUDOX rm -rf /var/lib/waydroid\n"
        "rm -rf \"$HOME/.local/share/waydroid\"\n";

    QString out;
    const bool ok = runTask(m_wayStatus, "Removing Waydroid", cmd, out, 20 * 60 * 1000);
    reportTask(m_wayStatus, "Remove Waydroid", ok, out);
    refreshWaydroidPage();
}

// -----------------------------------------------------
// Factory for osm-settings
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new EmulationPage(stack);
}
