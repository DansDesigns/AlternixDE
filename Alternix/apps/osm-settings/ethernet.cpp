#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QFrame>
#include <QProcess>
#include <QScrollArea>
#include <QScroller>
#include <QStringList>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QTimer>
#include <functional>

// ---------------------------------------------------------
// Helpers (Ethernet)
// ---------------------------------------------------------

static QString runCommandEth(const QString &cmd)
{
    QProcess p;
    p.start("bash", {"-c", cmd});
    p.waitForFinished();
    QString out = p.readAllStandardOutput();
    out += p.readAllStandardError();
    return out.trimmed();
}

// Spinner frames (Braille pattern rotation — present in DejaVu Sans).
static const char* const SPIN_FRAMES_ETH[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"
};
static const int SPIN_COUNT_ETH = 10;

// Runs a command while calling tick() roughly every 80ms so the UI can
// animate during the wait. Same synchronous flow, just sliced.
static QString runCommandEthAnimated(const QString &cmd, int timeoutMs,
                                     const std::function<void()> &tick)
{
    QProcess p;
    p.start("bash", {"-c", cmd});

    QElapsedTimer t;
    t.start();
    while (!p.waitForFinished(80)) {
        if (t.elapsed() > timeoutMs) {
            p.kill();
            p.waitForFinished(500);
            break;
        }
        if (tick) tick();
        QCoreApplication::processEvents();
    }

    QString out = p.readAllStandardOutput();
    out += p.readAllStandardError();
    return out.trimmed();
}

// FIX: the interface was hardcoded as eth0, but with predictable interface
// naming (the norm now — this same machine's wifi is wlp1s0) the real name
// is enp*/ens*/etc., so every field permanently showed "Unknown". Detect
// the first ethernet-class interface instead.
static QString getEthIface()
{
    QString res = runCommandEth(
        "for i in /sys/class/net/e*; do [ -e \"$i\" ] && basename \"$i\" && break; done");
    return res.trimmed();
}

// Bluetooth-style uniform button
static QPushButton* smallBtnEth(const QString &txt)
{
    QPushButton *b = new QPushButton(txt);
    b->setFixedSize(180, 60);
    b->setStyleSheet(
        "QPushButton {"
        " background:#444444;"
        " color:white;"
        " border:1px solid #222222;"
        " border-radius:16px;"
        " font-size:26px;"
        " font-weight:bold;"
        " padding:10px 24px;"
        "}"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
    );
    return b;
}

static bool isEthernetPowered()
{
    QString iface = getEthIface();
    if (iface.isEmpty()) return false;
    QString s = runCommandEth("cat /sys/class/net/" + iface + "/operstate 2>/dev/null");
    return s.contains("up", Qt::CaseInsensitive);
}

static void setEthernetPowered(bool on)
{
    QString iface = getEthIface();
    if (iface.isEmpty()) return;
    if (on) runCommandEth("sudo -n ip link set " + iface + " up");
    else    runCommandEth("sudo -n ip link set " + iface + " down");
}

// ---------------------------------------------------------
// EthernetPage
// ---------------------------------------------------------

class EthernetPage : public QWidget
{
public:
    explicit EthernetPage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), stackedWidget(stack)
    {
        //
        // ★ GLOBAL FONT FIX — NEVER REMOVE
        //
        setStyleSheet("background:#282828; color:white; font-family:Sans;");

        QVBoxLayout *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(40,40,40,40);
        rootLayout->setSpacing(20);
        rootLayout->setAlignment(Qt::AlignTop);

        // TITLE (Bluetooth size)
        QLabel *title = new QLabel("Ethernet", this);
        title->setStyleSheet("font-size:42px; font-weight:bold;");
        title->setAlignment(Qt::AlignCenter);
        rootLayout->addWidget(title);

        // -------------------------------------------------
        // CARD 1 — NETWORK MAP (Bluetooth card style)
        // -------------------------------------------------
        QFrame *mapCard = new QFrame(this);
        mapCard->setStyleSheet(
            "QFrame { background:#3a3a3a; border-radius:40px; }"
        );
        mapCard->setFixedHeight(300);

        QVBoxLayout *mapLay = new QVBoxLayout(mapCard);
        mapLay->setContentsMargins(25,25,25,25);

        QFrame *mapInner = new QFrame(this);
        mapInner->setStyleSheet(
            "QFrame { background:#444444; border-radius:22px; }"
        );

        QVBoxLayout *mapInnerLay = new QVBoxLayout(mapInner);
        mapInnerLay->setContentsMargins(20,20,20,20);

        QLabel *mapLbl = new QLabel(
            "Network map\n"
            "- Show network locations (click location to open\n"
            "  osm-files at that address)"
        );
        mapLbl->setStyleSheet("font-size:26px;");
        mapLbl->setAlignment(Qt::AlignCenter);
        mapLbl->setWordWrap(true);

        mapInnerLay->addWidget(mapLbl);
        mapLay->addWidget(mapInner);

        rootLayout->addWidget(mapCard);

        // -------------------------------------------------
        // CARD 2 — IP INFO
        // -------------------------------------------------
        QFrame *ipCard = new QFrame(this);
        ipCard->setStyleSheet(
            "QFrame { background:#3a3a3a; border-radius:30px; }"
        );
        ipCard->setFixedHeight(240);

        QVBoxLayout *ipLay = new QVBoxLayout(ipCard);
        ipLay->setContentsMargins(20,20,20,20);

        ipInfoLabel = new QLabel("Loading…");
        ipInfoLabel->setStyleSheet("font-size:26px;");
        ipInfoLabel->setAlignment(Qt::AlignCenter);
        ipInfoLabel->setWordWrap(true);

        ipLay->addWidget(ipInfoLabel);

        rootLayout->addWidget(ipCard);

        // (initial refresh happens in the deferred init below, after the
        // buttons exist — the label shows "Loading…" until then)

        // -------------------------------------------------
        // BUTTON ROW (Bluetooth identical)
        // -------------------------------------------------
        QHBoxLayout *btnRow = new QHBoxLayout();
        btnRow->setSpacing(40);
        btnRow->setAlignment(Qt::AlignHCenter);

        powerButton = smallBtnEth("On");
        refreshButton = smallBtnEth("Refresh");

        btnRow->addWidget(powerButton);
        btnRow->addWidget(refreshButton);

        rootLayout->addLayout(btnRow);

        // -------------------------------------------------
        // ★ PIN BACK BUTTON TO BOTTOM
        // -------------------------------------------------
        rootLayout->addStretch();  // pushes below to bottom

        QPushButton *backButton = new QPushButton(QStringLiteral("❮"));
        backButton->setFixedSize(140,60);
        backButton->setStyleSheet(
            "QPushButton{ background:#444444; color:white; border-radius:16px; "
            "border:1px solid #222; font-size:34px; }"
            "QPushButton:hover{ background:#555; }"
            "QPushButton:pressed{ background:#333; }"
        );

        QHBoxLayout *backLay = new QHBoxLayout();
        backLay->addWidget(backButton, 0, Qt::AlignHCenter);
        rootLayout->addLayout(backLay);

        // -------------------------------------------------
        // CONNECTIONS
        // -------------------------------------------------
        connect(powerButton, &QPushButton::clicked, this, &EthernetPage::togglePower);
        connect(refreshButton, &QPushButton::clicked, this, &EthernetPage::refreshIpInfo);
        connect(backButton, &QPushButton::clicked, this, [this]{
            if (stackedWidget) stackedWidget->setCurrentIndex(0);
        });

        // INITIAL STATE — deferred so the page paints immediately; the
        // constructor previously spawned several processes before the page
        // could appear, making the settings hub feel frozen.
        QTimer::singleShot(50, this, [this]() {
            ethernetPowered = isEthernetPowered();
            updatePowerButton();
            refreshIpInfo();
        });
    }

private:
    QStackedWidget *stackedWidget = nullptr;
    QLabel *ipInfoLabel = nullptr;
    QPushButton *powerButton = nullptr;
    QPushButton *refreshButton = nullptr;
    bool ethernetPowered = false;

    // -------------------------------------------------
    // POWER BUTTON (Bluetooth exact behaviour)
    // -------------------------------------------------
    void updatePowerButton()
    {
        if (ethernetPowered) {
            powerButton->setText("On");
            powerButton->setStyleSheet(
                "QPushButton { background:#444444; color:#7CFC00; border-radius:16px;"
                " border:1px solid #222; font-size:26px; font-weight:bold; padding:10px 24px; }"
                "QPushButton:hover{ background:#555; }"
                "QPushButton:pressed{ background:#333; }"
            );
        } else {
            powerButton->setText("Off");
            powerButton->setStyleSheet(
                "QPushButton { background:#444444; color:#CC6666; border-radius:16px;"
                " border:1px solid #222; font-size:26px; font-weight:bold; padding:10px 24px; }"
                "QPushButton:hover{ background:#555; }"
                "QPushButton:pressed{ background:#333; }"
            );
        }
    }

    void togglePower()
    {
        powerButton->setEnabled(false);
        bool target = !ethernetPowered;

        int frame = 0;
        powerButton->setText(QString::fromUtf8(SPIN_FRAMES_ETH[0]));

        QString iface = getEthIface();
        if (!iface.isEmpty()) {
            runCommandEthAnimated(
                QString("sudo -n ip link set %1 %2").arg(iface, target ? "up" : "down"),
                8000,
                [&]() {
                    frame = (frame + 1) % SPIN_COUNT_ETH;
                    powerButton->setText(QString::fromUtf8(SPIN_FRAMES_ETH[frame]));
                });
        }

        // Re-read the real state rather than assuming the command worked.
        ethernetPowered = isEthernetPowered();
        powerButton->setEnabled(true);
        updatePowerButton();
        refreshIpInfo();
    }

    void refreshIpInfo()
    {
        static bool busy = false;
        if (busy) return;
        busy = true;

        refreshButton->setEnabled(false);
        int frame = 0;
        refreshButton->setText(QString::fromUtf8(SPIN_FRAMES_ETH[0]));

        QString iface = getEthIface();

        // One sliced call instead of four separate spawns — faster, and the
        // Refresh button spinner ticks the whole way through.
        QString combined = runCommandEthAnimated(
            QString(
                "echo \"IP:$(hostname -I | awk '{print $1}')\";"
                "echo \"MASK:$(ip -o -f inet addr show %1 2>/dev/null | awk '{print $4}' | cut -d/ -f2)\";"
                "echo \"DNS:$(grep nameserver /etc/resolv.conf 2>/dev/null | awk '{print $2}' | head -n1)\";"
                "echo \"GW:$(ip route 2>/dev/null | grep default | awk '{print $3}')\""
            ).arg(iface.isEmpty() ? "eth0" : iface),
            8000,
            [&]() {
                frame = (frame + 1) % SPIN_COUNT_ETH;
                refreshButton->setText(QString::fromUtf8(SPIN_FRAMES_ETH[frame]));
            });

        QString ip, mask, dns, gateway;
        for (const QString &line : combined.split('\n')) {
            if      (line.startsWith("IP:"))   ip      = line.mid(3).trimmed();
            else if (line.startsWith("MASK:")) mask    = line.mid(5).trimmed();
            else if (line.startsWith("DNS:"))  dns     = line.mid(4).trimmed();
            else if (line.startsWith("GW:"))   gateway = line.mid(3).trimmed();
        }

        if (ip.isEmpty()) ip="Unknown";
        if (mask.isEmpty()) mask="Unknown";
        if (dns.isEmpty()) dns="Unknown";
        if (gateway.isEmpty()) gateway="Unknown";

        ipInfoLabel->setText(
            "IP address:   " + ip + "\n"
            "Subnet mask:  " + mask + "\n"
            "DNS server:   " + dns + "\n"
            "Gateway:      " + gateway
        );

        refreshButton->setText("Refresh");
        refreshButton->setEnabled(true);
        busy = false;
    }
};

// ---------------------------------------------------------
// Factory
// ---------------------------------------------------------

extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new EthernetPage(stack);
}
