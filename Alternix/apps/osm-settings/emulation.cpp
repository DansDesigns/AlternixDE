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
#include <QTextStream>
#include <QMessageBox>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>

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
// EmulationPage
// -----------------------------------------------------
class EmulationPage : public QWidget
{
public:
    explicit EmulationPage(QStackedWidget *stack);

private:
    QStackedWidget *m_stack = nullptr;

    QLabel *m_wayInfo  = nullptr;
    QLabel *m_wineInfo = nullptr;

    // helpers
    bool runCmdOk(const QString &cmd, QString &output, int timeoutMs = 30000);

    QString buildWaydroidInfoHtml();
    QString buildWineInfoHtml();

    void refreshWaydroid();
    void refreshWine();

    void startWaydroid();
    void stopWaydroid();

    void stopWine();
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
    QLabel *title = new QLabel("Emulation");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:42px; font-weight:bold;");
    root->addWidget(title);

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

    QLabel *wayLabel = new QLabel("Waydroid info");
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

    // Buttons row: Start / Stop / Refresh
    QHBoxLayout *wayBtnRow = new QHBoxLayout();
    wayBtnRow->setSpacing(16);

    QPushButton *wayStart = makeBtn("Start");
    QPushButton *wayStop  = makeBtn("Stop", "#CC6666");
    QPushButton *wayRefresh = makeBtn("Refresh");

    wayBtnRow->addWidget(wayStart);
    wayBtnRow->addWidget(wayStop);
    wayBtnRow->addWidget(wayRefresh);

    wayLay->addLayout(wayBtnRow);
    outerLay->addWidget(cardWaydroid);

    // =================================================
    // Wine card
    // =================================================
    QFrame *cardWine = new QFrame(outer);
    cardWine->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *wineLay = new QVBoxLayout(cardWine);
    wineLay->setContentsMargins(30, 30, 30, 30);
    wineLay->setSpacing(16);

    QLabel *wineLabel = new QLabel("Wine info");
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

    QPushButton *wineStop  = makeBtn("Force-close", "#CC6666");
    QPushButton *wineRefresh = makeBtn("Refresh");

    wineBtnRow->addWidget(wineStop);
    wineBtnRow->addWidget(wineRefresh);

    wineLay->addLayout(wineBtnRow);
    outerLay->addWidget(cardWine);

    // =================================================
    wrapLay->addWidget(outer);
    wrapLay->addStretch();

    scroll->setWidget(wrap);
    root->addWidget(scroll);

    // Back button pinned at bottom
    QPushButton *back = makeBtn("❮");
    back->setFixedSize(140, 60);
    connect(back, &QPushButton::clicked, this, [this]() {
        if (m_stack)
            m_stack->setCurrentIndex(0);
    });
    root->addWidget(back, 0, Qt::AlignCenter);

    // Connections
    connect(wayStart,   &QPushButton::clicked, this, [this]() { startWaydroid();  refreshWaydroid(); });
    connect(wayStop,    &QPushButton::clicked, this, [this]() { stopWaydroid();   refreshWaydroid(); });
    connect(wayRefresh, &QPushButton::clicked, this, [this]() { refreshWaydroid(); });

    connect(wineStop,    &QPushButton::clicked, this, [this]() { stopWine();   refreshWine(); });
    connect(wineRefresh, &QPushButton::clicked, this, [this]() { refreshWine(); });

    // Initial info
    refreshWaydroid();
    refreshWine();
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

// Waydroid info — systemd services unavailable on Devuan, use process/CLI check instead
QString EmulationPage::buildWaydroidInfoHtml()
{
    QString hasWaydroid = runCmd("command -v waydroid >/dev/null 2>&1 && echo yes || echo no");
    if (hasWaydroid.trimmed() != "yes") {
        return "Waydroid not installed or not in PATH.";
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

    QString contColor = (container.trimmed() == "running") ? "#7CFC00" : "#FF5555";
    QString sessColor = (session.trimmed()   == "running") ? "#7CFC00" : "#FF5555";
    QString bindColor = (binder.trimmed()    == "ok")      ? "#7CFC00" : "#FF5555";
    QString wayColor  = qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ? "#FF5555" : "#7CFC00";

    QString html;
    html  = "Installed: <b>yes</b><br>";
    html += "Version: " + version.toHtmlEscaped() + "<br>";
    html += QString("Container: <span style='color:%1;'>%2</span><br>")
            .arg(contColor, container.toHtmlEscaped());
    html += QString("Session: <span style='color:%1;'>%2</span><br>")
            .arg(sessColor, session.toHtmlEscaped());
    html += QString("Binder: <span style='color:%1;'>%2</span><br>")
            .arg(bindColor, binder.toHtmlEscaped());
    html += QString("Wayland: <span style='color:%1;'>%2</span>")
            .arg(wayColor, wayland.toHtmlEscaped());

    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        html += "<br><span style='color:#FFAA00;'>Waydroid renders through Wayland "
                "and cannot start on an X11 session.</span>";

    return html;
}

// Deeper Wine info (wineserver based)
QString EmulationPage::buildWineInfoHtml()
{
    QString hasWine = runCmd("command -v wine >/dev/null 2>&1 && echo yes || echo no");
    if (hasWine.trimmed() != "yes") {
        return "Wine not installed or not in PATH.";
    }

    QString version  = runCmd("wine --version 2>/dev/null");
    QString prefix   = runCmd("printf \"%s\" \"${WINEPREFIX:-$HOME/.wine}\" 2>/dev/null");
    QString backend  = runCmd("if [ -n \"$WAYLAND_DISPLAY\" ]; then echo Wayland; "
                              "elif [ -n \"$DISPLAY\" ]; then echo X11; "
                              "else echo Unknown; fi");
    QString running  = runCmd("pidof wineserver >/dev/null 2>&1 && echo running || echo stopped");

    QString runColor = (running.trimmed() == "running") ? "#7CFC00" : "#CCCCCC";

    QString html;
    html  = "Installed: <b>yes</b><br>";
    html += "Version: " + version.toHtmlEscaped() + "<br>";
    html += QString("Wineserver: <span style='color:%1;'>%2</span><br>")
            .arg(runColor, running.toHtmlEscaped());
    html += "Default prefix: " + prefix.toHtmlEscaped() + "<br>";
    html += "Graphics backend: " + backend.toHtmlEscaped();

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
            "The binder kernel module is not loaded, or binderfs is not mounted."
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
                "Check that 'waydroid init' has been run and that the binder "
                "module is loaded."
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
// Factory for osm-settings
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new EmulationPage(stack);
}
