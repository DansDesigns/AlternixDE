#include <QWidget>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QFrame>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QStringList>
#include <QFont>
#include <QListWidget>
#include <QInputDialog>
#include <QMessageBox>
#include <QCoreApplication>
#include <functional>

// =========================================================
// HELPERS
// =========================================================

static QPushButton* smallBtn(const QString &txt) {
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

static QString runCmd(const QString &cmd, int timeoutMs = 5000) {
    QProcess p;
    p.start("bash", {"-c", cmd});
    p.waitForFinished(timeoutMs);  // FIX: bumped from 1500ms — rescan needs more time
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

static QString cidrToMask(const QString &cidrStr) {
    bool ok;
    int bits = cidrStr.toInt(&ok);
    if (!ok || bits < 0 || bits > 32) return "-";
    quint32 mask = bits == 0 ? 0 : 0xFFFFFFFF << (32 - bits);
    QStringList octets;
    for (int i = 3; i >= 0; --i)
        octets << QString::number((mask >> (i * 8)) & 0xFF);
    return octets.join(".");
}

static QString getWifiIface() {
    QString res = runCmd("nmcli -t -f DEVICE,TYPE device | grep ':wifi' | cut -d: -f1");
    return res.split("\n").value(0).trimmed();
}

static QString getIP(const QString &iface) {
    if (iface.isEmpty()) return "-";
    QString ip = runCmd("ip -4 addr show " + iface + " | grep -oP '(?<=inet\\s)\\d+(\\.\\d+){3}' | head -n1");
    return ip.isEmpty() ? "-" : ip;
}

static QString getMask(const QString &iface) {
    if (iface.isEmpty()) return "-";
    QString cidr = runCmd("ip -4 addr show " + iface +
                          " | grep -oP '(?<=inet\\s)\\d+(\\.\\d+){3}/\\d+' | head -n1 | cut -d/ -f2");
    if (cidr.isEmpty()) return "-";
    return cidrToMask(cidr);
}

static QString getDNS() {
    QString dns = runCmd("grep 'nameserver' /etc/resolv.conf | head -n1 | awk '{print $2}'");
    return dns.isEmpty() ? "-" : dns;
}

static QString getGateway() {
    QString gw = runCmd("ip route | grep default | awk '{print $3}'");
    return gw.isEmpty() ? "-" : gw;
}

// =========================================================
// MAIN PAGE
// =========================================================

extern "C" QWidget* make_page(QStackedWidget *stack) {

    QWidget *page = new QWidget;
    page->setStyleSheet(
            "QScrollArea { background:#282828;  font-family:Sans; border:none; }"
            "QWidget { background:#282828; font-family:Sans; }"
            "QLabel { color:white; font-family:Sans;}"
            "QMessageBox QLabel { color:white; font-family:Sans; }"
        );

    QVBoxLayout *root = new QVBoxLayout(page);
    root->setContentsMargins(40, 40, 40, 40);
    root->setSpacing(20);
    root->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

    // -----------------------------------------------------
    // TITLE
    // -----------------------------------------------------
    QLabel *title = new QLabel("WiFi");
    title->setStyleSheet("font-size:42px; color:white; font-weight:bold;");
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    // -----------------------------------------------------
    // SSID LIST FRAME (Card 1)
    // -----------------------------------------------------
    QFrame *ssidFrame = new QFrame;
    ssidFrame->setStyleSheet(
        "QFrame {"
        " background:#3a3a3a;"
        " border-radius:40px;"
        "}"
    );
    ssidFrame->setFixedHeight(520);

    QVBoxLayout *ssidLayout = new QVBoxLayout(ssidFrame);
    ssidLayout->setContentsMargins(25, 25, 25, 25);
    ssidLayout->setSpacing(0);

    QListWidget *ssidList = new QListWidget;
    ssidList->setStyleSheet(
        "QListWidget {"
        " background:#444444;"
        " color:white;"
        " border-radius:22px;"
        " font-size:26px;"
        " padding-left:18px;"
        " padding-right:18px;"
        "}"
        "QListWidget::item {"
        " padding:18px;"
        " border-radius:20px;"
        "}"
        "QListWidget::item:selected {"
        " background:#555555;"
        " border-radius:20px;"
        "}"
    );
    ssidLayout->addWidget(ssidList);

    root->addWidget(ssidFrame);

    // -----------------------------------------------------
    // IP / DNS / MASK / GATEWAY FRAME (Card 2)
    // -----------------------------------------------------
    QFrame *infoFrame = new QFrame;
    infoFrame->setStyleSheet(
        "QFrame {"
        " background:#3a3a3a;"
        " border-radius:30px;"
        "}"
    );
    infoFrame->setFixedHeight(240);

    QVBoxLayout *infoLayout = new QVBoxLayout(infoFrame);
    infoLayout->setContentsMargins(20, 20, 20, 20);
    infoLayout->setSpacing(8);

    QFont infoFont("DejaVu Sans");
    infoFont.setPointSize(24);

    QLabel *ipLbl   = new QLabel;
    QLabel *dnsLbl  = new QLabel;
    QLabel *maskLbl = new QLabel;
    QLabel *gwLbl   = new QLabel;

    for (QLabel *lbl : {ipLbl, dnsLbl, maskLbl, gwLbl}) {
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setFont(infoFont);
        lbl->setStyleSheet("color:white; background:transparent; border:none;");
        infoLayout->addWidget(lbl);
    }

    root->addWidget(infoFrame);

    // -----------------------------------------------------
    // ON/OFF + REFRESH ROW
    // -----------------------------------------------------
    QHBoxLayout *switchRow = new QHBoxLayout;
    switchRow->setSpacing(40);
    switchRow->setAlignment(Qt::AlignHCenter);

    QPushButton *toggleWifi = smallBtn("On");
    QPushButton *refresh    = smallBtn("Refresh");

    switchRow->addWidget(toggleWifi);
    switchRow->addWidget(refresh);

    root->addLayout(switchRow);

    // =====================================================
    // LAMBDAS FOR STATE UPDATES
    // =====================================================

    // FIX: Trigger a real radio rescan before listing SSIDs.
    // Also fetch IN_USE and SSID together so we can mark the active network.
    auto doScan = [ssidList]() {
        ssidList->clear();
        ssidList->addItem("Scanning…");
        // FIX: repaint immediately so the placeholder is visible while the
        // blocking nmcli call below runs (can take a few seconds).
        QCoreApplication::processEvents();

        // FIX (root cause of the empty list): the previous version fired
        // "nmcli device wifi rescan" asynchronously and then *immediately*
        // read the list with --rescan no. The rescan command returns as
        // soon as the request is sent to NetworkManager, NOT once the scan
        // has actually completed, so the following "list" call almost
        // always read the old (often empty, e.g. on first launch) cache.
        //
        // nmcli's own "--rescan yes" (the default) handles this correctly:
        // it triggers a scan via D-Bus and BLOCKS until fresh results are
        // available before printing them, so we get real data every time.
        // Give it a generous timeout since a Wi-Fi scan can take 5-10s.
        QString raw = runCmd("nmcli -t -f IN_USE,SSID device wifi list --rescan yes", 15000);

        ssidList->clear();
        QStringList lines = raw.split("\n");

        for (QString line : lines) {
            line = line.trimmed();
            if (line.isEmpty()) continue;

            // FIX: Split on the FIRST colon only — SSIDs can contain colons.
            int sep = line.indexOf(':');
            QString inUseFlag = (sep >= 0) ? line.left(sep).trimmed()  : "";
            QString ssid      = (sep >= 0) ? line.mid(sep + 1).trimmed() : line;

            // Skip rows where SSID is empty (hidden networks, separator lines).
            if (ssid.isEmpty()) continue;

            // FIX: Mark the currently connected network with a ✔ prefix
            //      and a distinct colour so it's obvious at a glance.
            bool active = (inUseFlag == "*");
            QListWidgetItem *item = new QListWidgetItem(
                active ? ("✔  " + ssid) : ssid
            );
            if (active) {
                item->setForeground(QColor("#7CFC00"));  // bright green for active
            }
            ssidList->addItem(item);
        }

        if (ssidList->count() == 0)
            ssidList->addItem("No networks found");
    };

    // Update IP/DNS/Mask/Gateway info
    auto updateInfo = [ipLbl, dnsLbl, maskLbl, gwLbl]() {
        QString iface = getWifiIface();
        ipLbl->setText("IP address: "   + getIP(iface));
        dnsLbl->setText("DNS server: "  + getDNS());
        maskLbl->setText("Subnet mask: " + getMask(iface));
        gwLbl->setText("Gateway: "     + getGateway());
    };

    // WiFi state updater (text + colour)
    std::function<void()> updateWifiState;
    updateWifiState = [toggleWifi]() {
        QString state = runCmd("nmcli radio wifi");
        bool on = (state == "enabled");

        if (on) {
            toggleWifi->setText("On");
            toggleWifi->setStyleSheet(
                "QPushButton {"
                " background:#444444;"
                " color:#7CFC00;"
                " border:1px solid #222222;"
                " border-radius:16px;"
                " font-size:26px;"
                " font-weight:bold;"
                " padding:10px 24px;"
                "}"
                "QPushButton:hover { background:#555555; }"
                "QPushButton:pressed { background:#333333; }"
            );
        } else {
            toggleWifi->setText("Off");
            toggleWifi->setStyleSheet(
                "QPushButton {"
                " background:#444444;"
                " color:#CC6666;"
                " border:1px solid #222222;"
                " border-radius:16px;"
                " font-size:26px;"
                " font-weight:bold;"
                " padding:10px 24px;"
                "}"
                "QPushButton:hover { background:#555555; }"
                "QPushButton:pressed { background:#333333; }"
            );
        }
    };

    // Initial population
    doScan();
    updateInfo();
    updateWifiState();

    // -----------------------------------------------------
    // BUTTON CONNECTIONS
    // -----------------------------------------------------

    // Refresh = rescan SSIDs + refresh info + wifi state
    QObject::connect(refresh, &QPushButton::clicked, [doScan, updateInfo, updateWifiState]() mutable {
        doScan();
        updateInfo();
        updateWifiState();
    });

    // Toggle WiFi radio and update visual state
    QObject::connect(toggleWifi, &QPushButton::clicked, [updateWifiState]() mutable {
        QString state = runCmd("nmcli radio wifi");
        if (state == "enabled")
            runCmd("nmcli radio wifi off");
        else
            runCmd("nmcli radio wifi on");

        updateWifiState();
    });

    // -----------------------------------------------------
    // CONNECT ON SSID CLICK
    // -----------------------------------------------------
    QObject::connect(ssidList, &QListWidget::itemClicked, [ssidList]() {
        QListWidgetItem *item = ssidList->currentItem();
        if (!item) return;

        QString ssid = item->text();
        if (ssid.contains("No networks") || ssid.contains("Scanning")) return;

        // FIX: Strip the ✔ prefix added to the active network before connecting.
        if (ssid.startsWith("✔  "))
            ssid = ssid.mid(3);

        bool ok;
        QString pass = QInputDialog::getText(
            nullptr, "Wi-Fi Password",
            "Enter password for:\n" + ssid,
            QLineEdit::Password,
            "", &ok
        );
        if (!ok || pass.isEmpty()) return;

        QString cmd = QString("nmcli device wifi connect '%1' password '%2'")
                        .arg(ssid).arg(pass);

        QString out = runCmd(cmd);
        QMessageBox::information(nullptr, "Wi-Fi", out.isEmpty() ? "Done." : out);
    });

    // -----------------------------------------------------
    // BACK BUTTON ( ❮ )
    // -----------------------------------------------------
    QPushButton *back = new QPushButton("❮");
    back->setFixedSize(140, 60);
    back->setStyleSheet(
        "QPushButton {"
        " background:#444444;"
        " color:white;"
        " border:1px solid #222222;"
        " border-radius:16px;"
        " font-size:34px;"
        "}"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
    );

    QObject::connect(back, &QPushButton::clicked, [stack]() {
        stack->setCurrentIndex(0);
    });

    root->addWidget(back, 0, Qt::AlignHCenter);

    return page;
}