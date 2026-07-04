#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QFrame>
#include <QScrollArea>
#include <QScroller>
#include <QProcess>
#include <QFont>
#include <QStringList>
#include <QTimer>
#include <QApplication>

// ---------------------------------------------------------
// Helpers (same as Bluetooth)
// ---------------------------------------------------------

static QPushButton* smallBtnBT(const QString &txt) {
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

static QString runCmd(const QString &cmd)
{
    QProcess p;
    p.start("bash", {"-c", cmd});
    p.waitForFinished();
    return p.readAllStandardOutput() + p.readAllStandardError();
}

// ---------------------------------------------------------
// REAL MOBILE DATA (mmcli)
// ---------------------------------------------------------

static bool modemAvailable()
{
    QString out = runCmd("mmcli -L 2>/dev/null");
    return out.contains("Modem", Qt::CaseInsensitive);
}

static bool isMobilePowered()
{
    if (!modemAvailable()) return false;

    QString out = runCmd("mmcli -m 0 2>/dev/null");
    return out.contains("state: connected") ||
           out.contains("state: registered") ||
           out.contains("state: enabled");
}

static void setMobilePowered(bool on)
{
    if (!modemAvailable()) return;

    if (on)
        runCmd("mmcli -m 0 --enable 2>/dev/null");
    else
        runCmd("mmcli -m 0 --disable 2>/dev/null");
}

static QStringList getVisibleTowers()
{
    QStringList list;

    if (!modemAvailable()) {
        list << "No modem detected";
        return list;
    }

    QString out = runCmd("mmcli -m 0 --3gpp-scan 2>/dev/null");

    if (out.contains("no scan results")) {
        list << "No towers found";
        return list;
    }

    for (const QString &line : out.split("\n")) {
        QString t = line.trimmed();
        if (t.startsWith("operator id")) continue;
        if (t.startsWith("operator name")) continue;
        if (t.contains("mcc")) continue;

        if (t.contains("operator")) list << t;
    }

    if (list.isEmpty())
        list << "No towers found";

    return list;
}

static QString getCurrentCarrier()
{
    if (!modemAvailable())
        return "No modem detected";

    QString out = runCmd("mmcli -m 0 2>/dev/null");

    QString result = "No carrier detected";

    for (const QString &line : out.split("\n")) {
        QString t = line.trimmed();
        if (t.startsWith("operator name:", Qt::CaseInsensitive)) {
            QString name = t.section(":", 1).trimmed();
            if (!name.isEmpty())
                result = "Carrier: " + name;
        }
    }

    return result;
}

static QString getConnectionTime()
{
    if (!modemAvailable())
        return "Connection time: N/A";

    QString out = runCmd("mmcli -m 0 2>/dev/null");

    for (const QString &line : out.split("\n")) {
        QString t = line.trimmed();
        if (t.startsWith("duration:", Qt::CaseInsensitive)) {
            QString s = t.section(":", 1).trimmed();
            return "Connection time: " + s;
        }
    }

    return "Connection time: Unknown";
}

// ---------------------------------------------------------
// MobilePage widget
// ---------------------------------------------------------

class MobilePage : public QWidget
{
public:
    explicit MobilePage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), stackedWidget(stack)
    {
        // -------------------------------------------------
        // GLOBAL FONT FIX — identical to WiFi/Bluetooth
        // -------------------------------------------------
        QFont f;
        f.setFamily("Noto Sans");
        f.setPointSize(26);
        setFont(f);   // page-local: do NOT change the app-wide font

        setStyleSheet("background:#282828;");

        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(40, 40, 40, 40);
        root->setSpacing(20);
        // No layout-level alignment: the scroll area below takes stretch 1,
        // which keeps the title pinned at the top and the buttons/back pinned
        // at the bottom, with the card content scrolling in between.

        // -------------------------------------------------
        // Title
        // -------------------------------------------------
        QLabel *title = new QLabel("Mobile", this);
        title->setStyleSheet("font-size:42px; color:white; font-weight:bold;");
        title->setAlignment(Qt::AlignCenter);
        root->addWidget(title);

        // -------------------------------------------------
        // Main info card
        // -------------------------------------------------
        QFrame *infoCard = new QFrame(this);
        infoCard->setStyleSheet(
            "QFrame { background:#3a3a3a; border-radius:40px; }"
        );
        infoCard->setFixedHeight(520);

        QVBoxLayout *infoLayout = new QVBoxLayout(infoCard);
        infoLayout->setContentsMargins(35, 35, 35, 35);
        infoLayout->setSpacing(25);

        QLabel *title1 = new QLabel("Mobile Data information", this);
        title1->setStyleSheet("font-size:28px; color:white;");
        title1->setAlignment(Qt::AlignCenter);
        infoLayout->addWidget(title1);

        QLabel *vlabel = new QLabel("Visible towers:", this);
        vlabel->setStyleSheet("font-size:26px; color:white;");
        infoLayout->addWidget(vlabel);

        visibleTowerContainer = new QWidget(infoCard);
        visibleTowerLayout = new QVBoxLayout(visibleTowerContainer);
        visibleTowerLayout->setContentsMargins(10, 0, 10, 0);
        visibleTowerLayout->setSpacing(10);

        infoLayout->addWidget(visibleTowerContainer);

        carrierLabel = new QLabel("Carrier: ---", this);
        carrierLabel->setStyleSheet("font-size:26px; color:white;");
        infoLayout->addWidget(carrierLabel);

        timeLabel = new QLabel("Connection time: ---", this);
        timeLabel->setStyleSheet("font-size:26px; color:white;");
        infoLayout->addWidget(timeLabel);

        // -------------------------------------------------
        // Scrollable middle: card content scrolls, title stays pinned
        // -------------------------------------------------
        QScrollArea *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

        QWidget *scrollContainer = new QWidget(scroll);
        scrollContainer->setStyleSheet("background:#282828;");
        QVBoxLayout *scrollLayout = new QVBoxLayout(scrollContainer);
        scrollLayout->setContentsMargins(0, 0, 0, 0);
        scrollLayout->setSpacing(20);

        scrollLayout->addWidget(infoCard);
        scrollLayout->addStretch();

        scroll->setWidget(scrollContainer);
        root->addWidget(scroll, 1);

        // -------------------------------------------------
        // Buttons
        // -------------------------------------------------
        QHBoxLayout *btns = new QHBoxLayout();
        btns->setSpacing(40);

        powerButton = smallBtnBT("Off");
        refreshButton = smallBtnBT("Refresh");

        btns->addWidget(powerButton);
        btns->addWidget(refreshButton);

        root->addLayout(btns);

        // -------------------------------------------------
        // BACK BUTTON PINNED TO BOTTOM (scroll area owns the stretch)
        // -------------------------------------------------
        QPushButton *backButton = new QPushButton("❮", this);
        backButton->setFixedSize(140, 60);
        backButton->setStyleSheet(
            "QPushButton { background:#444444; color:white; border:1px solid #222222; "
            "border-radius:16px; font-size:34px; font-family:'DejaVu Sans'; } "
            "QPushButton:hover { background:#555555; }"
            "QPushButton:pressed { background:#333333; }"
        );

        QHBoxLayout *backWrap = new QHBoxLayout();
        backWrap->addWidget(backButton, 0, Qt::AlignHCenter);
        root->addLayout(backWrap);

        // -------------------------------------------------
        // SIGNALS
        // -------------------------------------------------
        connect(powerButton, &QPushButton::clicked, this, &MobilePage::togglePower);
        connect(refreshButton, &QPushButton::clicked, this, &MobilePage::refreshInfo);

        connect(backButton, &QPushButton::clicked, this, [this]() {
            if (refreshTimer) refreshTimer->stop();
            stackedWidget->setCurrentIndex(0);
        });

        // -------------------------------------------------
        // INITIAL STATE + AUTO REFRESH
        // -------------------------------------------------
        refreshTimer = new QTimer(this);
        refreshTimer->setInterval(2000);
        connect(refreshTimer, &QTimer::timeout, this, &MobilePage::refreshInfo);
        refreshTimer->start();

        refreshInfo();
    }

private:
    QStackedWidget *stackedWidget;

    QWidget *visibleTowerContainer = nullptr;
    QVBoxLayout *visibleTowerLayout = nullptr;

    QLabel *carrierLabel = nullptr;
    QLabel *timeLabel = nullptr;

    QPushButton *powerButton = nullptr;
    QPushButton *refreshButton = nullptr;

    QTimer *refreshTimer = nullptr;

    bool mobilePowered = false;

    // ---------------------------------------------------
    // UI Updates
    // ---------------------------------------------------
    void updatePowerButton()
    {
        if (mobilePowered) {
            powerButton->setText("On");
            powerButton->setStyleSheet(
                "QPushButton { background:#444444; color:#7CFC00; border:1px solid #222222; "
                "border-radius:16px; font-size:26px; font-weight:bold; padding:10px 24px; }"
                "QPushButton:hover { background:#555555; }"
                "QPushButton:pressed { background:#333333; }"
            );
        } else {
            powerButton->setText("Off");
            powerButton->setStyleSheet(
                "QPushButton { background:#444444; color:#CC6666; border:1px solid #222222; "
                "border-radius:16px; font-size:26px; font-weight:bold; padding:10px 24px; }"
                "QPushButton:hover { background:#555555; }"
                "QPushButton:pressed { background:#333333; }"
            );
        }
    }

    void togglePower()
    {
        mobilePowered = !mobilePowered;
        setMobilePowered(mobilePowered);
        refreshInfo();
    }

    void refreshInfo()
    {
        mobilePowered = isMobilePowered();
        updatePowerButton();

        // Clear tower list
        QLayoutItem *child;
        while ((child = visibleTowerLayout->takeAt(0)) != nullptr) {
            if (child->widget())
                child->widget()->deleteLater();
            delete child;
        }

        QStringList towers = getVisibleTowers();
        for (const QString &t : towers) {
            QLabel *lbl = new QLabel(" - " + t, visibleTowerContainer);
            lbl->setStyleSheet("color:white; font-size:24px;");
            visibleTowerLayout->addWidget(lbl);
        }

        carrierLabel->setText(getCurrentCarrier());
        timeLabel->setText(getConnectionTime());
    }
};

// ---------------------------------------------------------
// Plugin Factory
// ---------------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new MobilePage(stack);
}
