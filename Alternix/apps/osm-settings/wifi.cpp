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
#include <QScrollArea>
#include <QScroller>
#include <QInputDialog>
#include <QMessageBox>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QProcessEnvironment>
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

static QString runCmd(const QString &cmd, int timeoutMs = 5000,
                       QString *errOut = nullptr) {
    QProcess p;
    p.start("bash", {"-c", cmd});
    p.waitForFinished(timeoutMs);  // FIX: bumped from 1500ms — rescan needs more time
    if (errOut) *errOut = QString::fromUtf8(p.readAllStandardError()).trimmed();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// Spinner frames (Braille pattern rotation — present in DejaVu Sans).
static const char* const SPIN_FRAMES[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"
};
static const int SPIN_COUNT = 10;

// Runs a command while calling tick() roughly every 80ms, so the UI can
// animate during what would otherwise be a frozen blocking wait. Same
// synchronous flow as runCmd — just sliced. Returns stdout; stderr goes
// to errOut if provided.
static QString runCmdAnimated(const QString &cmd, int timeoutMs,
                              const std::function<void()> &tick,
                              QString *errOut = nullptr) {
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

    if (errOut) *errOut = QString::fromUtf8(p.readAllStandardError()).trimmed();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// ---------------------------------------------------------------------
// Privileged commands (nmcli connect / radio on-off / rescan need
// org.freedesktop.NetworkManager.network-control, which this minimal
// build has no polkit agent to satisfy interactively).
//
// Strategy: try `sudo -n` first — silent and instant if NOPASSWD is
// configured for the user (Alternix sets this up by default). If that
// specifically fails because a password is needed, retry with `sudo -A`
// so sudo calls the SUDO_ASKPASS helper — osm-sudo, the GUI pattern
// unlock — instead of trying (and failing) to prompt on a controlling
// terminal that doesn't exist for a GUI app.
// ---------------------------------------------------------------------

static QProcessEnvironment sudoEnv() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // osm-sudo is installed to /usr/local/bin and normally exported via
    // /etc/profile.d/osm-sudo.sh — this is just a safety net in case a
    // process ends up with a stripped environment.
    if (env.value("SUDO_ASKPASS").isEmpty())
        env.insert("SUDO_ASKPASS", "/usr/local/bin/osm-sudo");
    return env;
}

static QString runCmdAsRoot(const QString &cmd, int timeoutMs = 5000,
                             QString *errOut = nullptr) {
    QProcessEnvironment env = sudoEnv();

    QProcess p;
    p.setProcessEnvironment(env);
    p.start("sudo", {"-n", "bash", "-c", cmd});
    p.waitForFinished(timeoutMs);
    QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();

    if (err.contains("password is required") || err.contains("a password is required")) {
        QProcess p2;
        p2.setProcessEnvironment(env);
        p2.start("sudo", {"-A", "bash", "-c", cmd});
        p2.waitForFinished(qMax(timeoutMs, 60000));  // give osm-sudo time to unlock
        out = QString::fromUtf8(p2.readAllStandardOutput()).trimmed();
        err = QString::fromUtf8(p2.readAllStandardError()).trimmed();
    }

    if (errOut) *errOut = err;
    return out;
}

// Animated variant of runCmdAsRoot, mirroring runCmdAnimated so callers
// (scan / connect) can keep their spinner alive while osm-sudo is up and
// while the privileged command itself runs.
static QString runCmdAsRootAnimated(const QString &cmd, int timeoutMs,
                                    const std::function<void()> &tick,
                                    QString *errOut = nullptr) {
    QProcessEnvironment env = sudoEnv();

    // FIX: this used to be a hardcoded p.waitForFinished(3000) here. sudo -n
    // fails almost instantly when a password IS required, but when NOPASSWD
    // is configured (the normal case) it just runs the real command —
    // which for a wifi rescan can legitimately take up to timeoutMs (15s).
    // Capping this at 3s meant a working NOPASSWD scan got killed and
    // read back as empty output before it ever finished, which looked
    // identical to "no networks" / "nmcli returned nothing" regardless of
    // what was actually going on. Give it the full timeoutMs, same as the
    // -A path below.
    QProcess p;
    p.setProcessEnvironment(env);
    p.start("sudo", {"-n", "bash", "-c", cmd});

    {
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
    }
    QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();

    if (err.contains("password is required") || err.contains("a password is required")) {
        QProcess p2;
        p2.setProcessEnvironment(env);
        p2.start("sudo", {"-A", "bash", "-c", cmd});

        QElapsedTimer t;
        t.start();
        while (!p2.waitForFinished(80)) {
            if (t.elapsed() > timeoutMs) {
                p2.kill();
                p2.waitForFinished(500);
                break;
            }
            if (tick) tick();
            QCoreApplication::processEvents();
        }
        out = QString::fromUtf8(p2.readAllStandardOutput()).trimmed();
        err = QString::fromUtf8(p2.readAllStandardError()).trimmed();
    }

    if (errOut) *errOut = err;
    return out;
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

// DIAGNOSTIC — DO NOT REMOVE.
// nmcli prints its real error ("Error: Could not create NMClient
// object...") to STDERR when it can't reach the NetworkManager daemon —
// most commonly because dbus/NetworkManager were never enabled as boot
// services (see install_desktop.sh). runCmd() previously only captured
// stdout, so every nmcli query looked identical to "no networks found"
// whether NM was unreachable, misbehaving, or genuinely idle. This
// checks for that specific failure so the UI can say so directly.
static bool isNetworkManagerUnreachable(QString *detail = nullptr) {
    QString err;
    runCmd("nmcli -t -f RUNNING general status", 3000, &err);
    bool unreachable = err.contains("Could not create NMClient", Qt::CaseInsensitive) ||
                       err.contains("NetworkManager is not running", Qt::CaseInsensitive) ||
                       err.contains("Could not connect", Qt::CaseInsensitive);
    if (detail) *detail = err;
    return unreachable;
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

// FIX: detect the "unmanaged" state specifically. On Devuan/Debian this is
// almost always caused by /etc/NetworkManager/NetworkManager.conf having
// [ifupdown] managed=false, or the interface being listed in
// /etc/network/interfaces (NM's ifupdown plugin then hands it back to
// ifupdown regardless of the managed= setting). Surfacing this directly
// saves an SSH session next time an Alternix build hits it.
static QString getDeviceState(const QString &iface) {
    if (iface.isEmpty()) return "";
    QString raw = runCmd("nmcli -t -f DEVICE,STATE device status");
    for (const QString &line : raw.split("\n")) {
        int sep = line.indexOf(':');
        if (sep < 0) continue;
        if (line.left(sep).trimmed() == iface)
            return line.mid(sep + 1).trimmed();
    }
    return "";
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
    // No layout-level alignment: AlignTop|AlignHCenter made Qt ignore stretch
    // factors and shrink children to their size hints. The scroll area below
    // takes stretch 1 instead, pinning the title at the top and the button
    // rows at the bottom.

    // -----------------------------------------------------
    // TITLE
    // -----------------------------------------------------
    QLabel *title = new QLabel("WiFi");
    title->setStyleSheet("font-size:42px; color:white; font-weight:bold;");
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    // -----------------------------------------------------
    // SCROLLABLE MIDDLE (cards scroll, title stays pinned)
    // -----------------------------------------------------
    QScrollArea *midScroll = new QScrollArea(page);
    midScroll->setWidgetResizable(true);
    midScroll->setFrameShape(QFrame::NoFrame);
    midScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    midScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(midScroll->viewport(), QScroller::LeftMouseButtonGesture);

    QWidget *midContainer = new QWidget(midScroll);
    QVBoxLayout *midLayout = new QVBoxLayout(midContainer);
    midLayout->setContentsMargins(0, 0, 0, 0);
    midLayout->setSpacing(20);

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
    ssidList->setWordWrap(true);
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

    midLayout->addWidget(ssidFrame);

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

    midLayout->addWidget(infoFrame);
    midLayout->addStretch();

    midScroll->setWidget(midContainer);
    root->addWidget(midScroll, 1);

    // -----------------------------------------------------
    // ON/OFF + REFRESH ROW
    // -----------------------------------------------------
    QHBoxLayout *switchRow = new QHBoxLayout;
    switchRow->setSpacing(40);
    switchRow->setAlignment(Qt::AlignHCenter);

    QPushButton *toggleWifi = smallBtn("On");
    QPushButton *refresh    = smallBtn("Refresh");
    QPushButton *fixBtn     = smallBtn("Fix");

    switchRow->addWidget(toggleWifi);
    switchRow->addWidget(refresh);
    switchRow->addWidget(fixBtn);

    root->addLayout(switchRow);

    // =====================================================
    // LAMBDAS FOR STATE UPDATES
    // =====================================================

    // FIX: Trigger a real radio rescan before listing SSIDs.
    // Also fetch IN_USE and SSID together so we can mark the active network.
    // extraTick (optional) receives the spinner frame index so callers can
    // animate their own widget (e.g. the Refresh button) in sync.
    auto doScan = [ssidList](const std::function<void(int)> &extraTick = {}) {
        // Reentrancy guard: processEvents during the animated wait means the
        // user can tap Refresh mid-scan; a nested scan would clear the list
        // and leave the outer scan holding a dangling placeholder pointer.
        static bool scanning = false;
        if (scanning) return;
        scanning = true;

        ssidList->clear();
        QListWidgetItem *ph = new QListWidgetItem(
            QString::fromUtf8(SPIN_FRAMES[0]) + "  Scanning…");
        ph->setFlags(ph->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
        ssidList->addItem(ph);
        QCoreApplication::processEvents();

        // nmcli's "--rescan yes" triggers a scan via D-Bus and blocks until
        // fresh results are available — the wait is sliced by runCmdAnimated
        // so the spinner keeps moving the whole time.
        int frame = 0;
        QString err;
        QString out = runCmdAsRootAnimated(
            "nmcli -t -f IN-USE,SSID device wifi list --rescan yes", 15000,
            [&]() {
                frame = (frame + 1) % SPIN_COUNT;
                ph->setText(QString::fromUtf8(SPIN_FRAMES[frame]) + "  Scanning…");
                if (extraTick) extraTick(frame);
            },
            &err);

        ssidList->clear();

        if (out.isEmpty()) {
            // Surface the actual reason instead of a generic "no networks".
            QString diag;
            QString nmErr;
            if (!err.isEmpty()) {
                diag = err;
            } else if (isNetworkManagerUnreachable(&nmErr)) {
                diag = "NetworkManager isn't reachable"
                       + (nmErr.isEmpty() ? QString() : (" (" + nmErr + ")"))
                       + " — it may not be enabled/running. On this system: "
                         "sudo rc-update add dbus default; "
                         "sudo rc-update add NetworkManager default; "
                         "sudo rc-service dbus start; "
                         "sudo rc-service NetworkManager start";
            } else {
                QString iface = getWifiIface();
                QString state = getDeviceState(iface);
                if (state == "unmanaged") {
                    diag = "Wi-Fi device '" + iface + "' is UNMANAGED by "
                           "NetworkManager. Check [ifupdown] managed=true in "
                           "/etc/NetworkManager/NetworkManager.conf, and that "
                           "the interface isn't listed in /etc/network/interfaces.";
                } else if (!state.isEmpty()) {
                    diag = "Wi-Fi device '" + iface + "' state: " + state;
                } else {
                    diag = "nmcli returned nothing (check: wifi radio on? "
                           "wifi device present? nmcli installed?)";
                }
            }
            QListWidgetItem *item = new QListWidgetItem(diag);
            item->setForeground(QColor("#CC6666"));
            item->setFlags(item->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
            ssidList->addItem(item);
            scanning = false;
            return;
        }

        QStringList lines = out.split("\n");

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

        if (ssidList->count() == 0) {
            QListWidgetItem *none = new QListWidgetItem("No networks found");
            none->setFlags(none->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
            ssidList->addItem(none);
        }
        scanning = false;
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

    // Initial population — deferred so make_page() returns immediately and
    // the page becomes visible at once. Previously the full nmcli scan ran
    // synchronously here, freezing the settings hub for several seconds
    // before the page even appeared. The 50ms delay lets the first paint
    // happen, then the animated scan takes over. ssidList is the context
    // object so the callback is dropped if the page is destroyed first.
    QTimer::singleShot(50, ssidList, [doScan, updateInfo, updateWifiState]() mutable {
        updateWifiState();
        updateInfo();
        doScan();
    });

    // -----------------------------------------------------
    // BUTTON CONNECTIONS
    // -----------------------------------------------------

    // Refresh = rescan SSIDs + refresh info + wifi state.
    // The button itself spins in sync with the list spinner while running.
    QObject::connect(refresh, &QPushButton::clicked, [refresh, doScan, updateInfo, updateWifiState]() mutable {
        if (!refresh->isEnabled()) return;
        refresh->setEnabled(false);
        refresh->setText(QString::fromUtf8(SPIN_FRAMES[0]));

        doScan([refresh](int frame) {
            refresh->setText(QString::fromUtf8(SPIN_FRAMES[frame]));
        });

        refresh->setText("Refresh");
        refresh->setEnabled(true);
        updateInfo();
        updateWifiState();
    });

    // Toggle WiFi radio and update visual state
    QObject::connect(toggleWifi, &QPushButton::clicked, [updateWifiState]() mutable {
        QString state = runCmd("nmcli radio wifi");
        if (state == "enabled")
            runCmdAsRoot("nmcli radio wifi off");
        else
            runCmdAsRoot("nmcli radio wifi on");

        updateWifiState();
    });

    // -----------------------------------------------------
    // FIX BUTTON — scan for known issues and repair them
    // -----------------------------------------------------
    // Currently handles the "unmanaged by NetworkManager" case common on
    // Devuan/Debian: [ifupdown] managed=false in NetworkManager.conf, and/or
    // the wifi interface being claimed by /etc/network/interfaces. Any file
    // it edits is backed up first (.bak-<timestamp>).
    // The script runs via 'sudo -n' since editing /etc requires root —
    // Alternix installs set NOPASSWD sudo for the user, so no prompt appears.
    // -n makes sudo fail immediately (reported) instead of hanging on a
    // password prompt if NOPASSWD isn't configured.
    QObject::connect(fixBtn, &QPushButton::clicked, [ssidList, doScan, updateInfo, updateWifiState]() mutable {
        QString iface = getWifiIface();

        const QString scriptPath = "/tmp/alternix_wifi_fix.sh";
        QFile f(scriptPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream ts(&f);
            ts << R"SCRIPT(#!/bin/bash
set -u
IFACE="$1"
TS=$(date +%Y%m%d-%H%M%S)
NMCONF=/etc/NetworkManager/NetworkManager.conf
CHANGED=0
FAILED=0

# 1. NetworkManager.conf: [ifupdown] managed=false -> managed=true
# Only report FIXED if the edit actually took effect.
if [ -f "$NMCONF" ] && grep -Pzo '\[ifupdown\][^\[]*managed=false' "$NMCONF" >/dev/null 2>&1; then
    if cp "$NMCONF" "$NMCONF.bak-$TS" \
       && sed -i '/\[ifupdown\]/,/^\[/{s/^managed=false/managed=true/}' "$NMCONF" \
       && ! grep -Pzo '\[ifupdown\][^\[]*managed=false' "$NMCONF" >/dev/null 2>&1; then
        echo "FIXED: NetworkManager.conf managed=false -> managed=true"
        echo "       (backup: $NMCONF.bak-$TS)"
        CHANGED=1
    else
        echo "FAILED: could not edit $NMCONF (need root?)"
        FAILED=1
    fi
fi

# 2. /etc/network/interfaces and interfaces.d/* claiming this iface
if [ -n "$IFACE" ]; then
    for cf in /etc/network/interfaces /etc/network/interfaces.d/*; do
        [ -f "$cf" ] || continue
        if grep -qE "^[^#]*\b$IFACE\b" "$cf"; then
            if cp "$cf" "$cf.bak-$TS" \
               && sed -i -E "s/^([^#].*\b$IFACE\b.*)$/#\1/" "$cf" \
               && ! grep -qE "^[^#]*\b$IFACE\b" "$cf"; then
                echo "FIXED: commented $IFACE entries in $cf"
                echo "       (backup: $cf.bak-$TS)"
                CHANGED=1
            else
                echo "FAILED: could not edit $cf (need root?)"
                FAILED=1
            fi
        fi
    done
fi

# 2b. Stale interface-name pins in NetworkManager connection profiles.
# Cloning the same Devuan/Alternix image across different hardware (e.g.
# built/connected on the Lenovo Miix520, then imaged onto the BBEN C100 or
# RPi 3B+) carries forward saved connection profiles with the ORIGINAL
# machine's wifi interface name baked into [connection] interface-name=...
# NM then refuses to apply that profile to today's actual device (which has
# a different kernel-assigned name), which looks exactly like "wifi isn't
# detected" even though the card, driver and firmware are all fine.
NMCONN_DIR=/etc/NetworkManager/system-connections
REALIFACES=$(ls /sys/class/net 2>/dev/null | grep -vx 'lo')
if [ -d "$NMCONN_DIR" ]; then
    for cf in "$NMCONN_DIR"/*; do
        [ -f "$cf" ] || continue
        PINNED=$(grep -oP '^interface-name=\K.*' "$cf" 2>/dev/null)
        [ -n "$PINNED" ] || continue
        if ! echo "$REALIFACES" | grep -qx "$PINNED"; then
            if cp "$cf" "$cf.bak-$TS" && sed -i '/^interface-name=/d' "$cf"; then
                echo "FIXED: removed stale interface-name=$PINNED from $(basename "$cf")"
                echo "       (no such device on this machine — profile can now"
                echo "       match by type/SSID instead. backup: $cf.bak-$TS)"
                CHANGED=1
            else
                echo "FAILED: could not edit $cf (need root?)"
                FAILED=1
            fi
        fi
    done
fi

# 2c. Stale udev persistent-net rules pinning an old MAC to a fixed name —
# same cloned-image symptom, one layer lower. Flagged only (not auto-edited,
# since malformed rewrites here can break naming further); the actual fix
# is usually just deleting the file and rebooting to let udev regenerate it.
UDEV_RULES=/etc/udev/rules.d/70-persistent-net.rules
if [ -f "$UDEV_RULES" ]; then
    REALMACS=$(for i in $REALIFACES; do cat "/sys/class/net/$i/address" 2>/dev/null; done)
    STALE=0
    while IFS= read -r line; do
        case "$line" in \#*|"") continue ;; esac
        MAC=$(echo "$line" | grep -oiP 'address\}=="\K[0-9a-f:]+')
        [ -n "$MAC" ] || continue
        echo "$REALMACS" | grep -qix "$MAC" || STALE=1
    done < "$UDEV_RULES"
    if [ "$STALE" -eq 1 ]; then
        echo "NOTE: $UDEV_RULES pins a MAC address not present on this"
        echo "      machine — leftover from a cloned image. Consider:"
        echo "        sudo rm $UDEV_RULES && sudo reboot"
        echo "      to let udev regenerate it fresh for this hardware."
    fi
fi

# 3. Device 'unavailable' — the state after unmanaged is fixed but the
# radio/interface still can't be used. Handle the common causes:
#    - rfkill soft block (leftover from ifupdown days or Fn-key toggle)
#    - interface administratively DOWN
#    - NM wifi radio switched off
CURSTATE=""
if [ -n "$IFACE" ]; then
    CURSTATE=$(nmcli -t -f DEVICE,STATE device status 2>/dev/null | awk -F: -v d="$IFACE" '$1==d{print $2}')
fi

if [ "$CURSTATE" = "unavailable" ] || [ "$CHANGED" -eq 1 ]; then
    if command -v rfkill >/dev/null 2>&1; then
        if rfkill list 2>/dev/null | grep -A1 -i wireless | grep -qi "Soft blocked: yes"; then
            rfkill unblock wifi \
                && echo "FIXED: rfkill soft block removed from wifi radio" \
                && CHANGED=1
        fi
        if rfkill list 2>/dev/null | grep -A2 -i wireless | grep -qi "Hard blocked: yes"; then
            echo "BLOCKED: wifi radio is HARD blocked — this is a physical"
            echo "         switch or BIOS setting; software cannot unblock it."
        fi
    else
        echo "NOTE: rfkill not installed — cannot check or clear radio blocks."
        echo "      (sudo apt install rfkill)"
    fi

    # Leftover ifupdown-style wpa_supplicant: an instance bound directly to
    # the interface (-i IFACE) holds exclusive control of it, so NM's own
    # D-Bus supplicant can't attach and the device sits at 'unavailable'.
    # These get spawned at boot by wpa-conf lines in /etc/network/interfaces
    # and survive even after that config is commented out. Kill only the
    # interface-bound instance — the D-Bus one (-u) must stay running.
    if [ -n "$IFACE" ]; then
        for pid in $(pgrep -x wpa_supplicant 2>/dev/null); do
            CMDLINE=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
            # Only the interface-bound instance (-i IFACE); never the -u D-Bus one.
            case "$CMDLINE" in
                *"-i $IFACE"*|*"-i$IFACE"*)
                    if kill "$pid" 2>/dev/null; then
                        rm -f "/run/wpa_supplicant.$IFACE.pid"
                        echo "FIXED: killed leftover per-interface wpa_supplicant"
                        echo "       (PID $pid was holding $IFACE, blocking NM)"
                        CHANGED=1
                        sleep 1
                    else
                        echo "FAILED: could not kill per-interface wpa_supplicant (PID $pid)"
                        FAILED=1
                    fi
                    ;;
            esac
        done
    fi

    # wpa_supplicant: NetworkManager cannot use ANY wifi device without a
    # running supplicant — without one, every wifi device sits permanently
    # at 'unavailable'. On systemd it gets D-Bus-activated automatically;
    # on Devuan/sysvinit that path is often broken, so start it ourselves.
    if ! pgrep -x wpa_supplicant >/dev/null 2>&1; then
        if command -v wpa_supplicant >/dev/null 2>&1; then
            STARTED=0
            if [ -x /etc/init.d/wpasupplicant ]; then
                /etc/init.d/wpasupplicant start >/dev/null 2>&1
            fi
            pgrep -x wpa_supplicant >/dev/null 2>&1 && STARTED=1
            if [ "$STARTED" -eq 0 ]; then
                # -u = D-Bus interface mode (what NM talks to), -B = daemonize
                wpa_supplicant -u -B >/dev/null 2>&1
                sleep 1
                pgrep -x wpa_supplicant >/dev/null 2>&1 && STARTED=1
            fi
            if [ "$STARTED" -eq 1 ]; then
                echo "FIXED: wpa_supplicant was not running — started it"
                echo "       (NM cannot use wifi devices without it)"
                CHANGED=1
            else
                echo "FAILED: wpa_supplicant is installed but would not start"
                FAILED=1
            fi
        else
            echo "MISSING: wpasupplicant is not installed. NetworkManager"
            echo "         cannot use wifi without it. Connect ethernet and:"
            echo "         sudo apt install wpasupplicant"
            FAILED=1
        fi
    fi

    if [ -n "$IFACE" ] && command -v ip >/dev/null 2>&1; then
        if ip link show "$IFACE" 2>/dev/null | grep -q "state DOWN"; then
            ip link set "$IFACE" up \
                && echo "FIXED: brought interface $IFACE up" \
                && CHANGED=1
        fi
    fi

    if [ "$(nmcli radio wifi 2>/dev/null)" = "disabled" ]; then
        nmcli radio wifi on \
            && echo "FIXED: NetworkManager wifi radio switched on" \
            && CHANGED=1
    fi

    # Firmware problems also present as 'unavailable' — surface any kernel
    # complaints so it's obvious if this is a missing-firmware situation.
    # (dmesg works here: this script runs as root via sudo.)
    if command -v dmesg >/dev/null 2>&1; then
        FWERR=$(dmesg 2>/dev/null | grep -iE "(firmware|microcode).*(fail|error|missing|not found|timed out)|Direct firmware load.*failed" | grep -i -m1 -E "wifi|wlan|iwl|ath|rtl|brcm|mt7|80211")
        if [ -n "$FWERR" ]; then
            echo "FIRMWARE: kernel reports a wifi firmware problem:"
            echo "  $FWERR"
            echo "  Likely fix: enable non-free-firmware repo and install the"
            echo "  package for this card (Intel: firmware-iwlwifi)."
        fi
    fi

    # NM's own explanation for the device state — often names the cause
    # directly (e.g. supplicant, firmware) so show it in the report.
    if [ -n "$IFACE" ]; then
        NMREASON=$(nmcli -f GENERAL.STATE,GENERAL.REASON device show "$IFACE" 2>/dev/null)
        [ -n "$NMREASON" ] && echo "$NMREASON"
    fi
fi

# 4. restart NetworkManager if config files were changed
if [ "$CHANGED" -eq 1 ]; then
    if [ -x /etc/init.d/network-manager ]; then
        /etc/init.d/network-manager restart >/dev/null 2>&1 && echo "RESTARTED: network-manager"
    elif command -v systemctl >/dev/null 2>&1; then
        systemctl restart NetworkManager >/dev/null 2>&1 && echo "RESTARTED: NetworkManager"
    else
        echo "WARN: could not find a way to restart NetworkManager"
    fi

    # The device can take a few seconds to be claimed after restart —
    # poll for up to 10s rather than sampling once too early.
    if [ -n "$IFACE" ]; then
        for i in $(seq 1 10); do
            STATE=$(nmcli -t -f DEVICE,STATE device status 2>/dev/null | awk -F: -v d="$IFACE" '$1==d{print $2}')
            [ -n "$STATE" ] && [ "$STATE" != "unmanaged" ] && [ "$STATE" != "unavailable" ] && break
            sleep 1
        done
        echo "STATE: $IFACE is now '${STATE:-unknown}'"
        case "${STATE:-}" in
            disconnected) echo "READY: device is working — hit Refresh to scan for networks." ;;
            connected)    echo "READY: device is connected." ;;
            unavailable)  echo "STILL UNAVAILABLE: check rfkill hard block or firmware messages above." ;;
            unmanaged)    echo "STILL UNMANAGED: config change may not have taken effect." ;;
        esac
    fi
elif [ -n "$IFACE" ]; then
    STATE=$(nmcli -t -f DEVICE,STATE device status 2>/dev/null | awk -F: -v d="$IFACE" '$1==d{print $2}')
    echo "STATE: $IFACE is '${STATE:-unknown}'"
fi

if [ "$CHANGED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "No known auto-fixable issue found."
fi
)SCRIPT";
            f.close();
        }

        // Run as root. Alternix sets NOPASSWD for the user, so this succeeds
        // silently via `sudo -n`; if that isn't configured, runCmdAsRoot
        // falls back to `sudo -A`, which calls osm-sudo for a GUI
        // pattern-unlock instead of hanging on a terminal prompt that a
        // GUI app doesn't have.
        QString err;
        QString out = runCmdAsRoot(
            "bash " + scriptPath + " '" + iface + "'", 30000, &err);

        QString report = out.isEmpty() ? "No output from fix script." : out;
        if (!err.isEmpty())
            report += "\n\n[stderr]\n" + err;

        QMessageBox::information(nullptr, "Wi-Fi Fix", report);

        doScan();
        updateInfo();
        updateWifiState();
    });

    // -----------------------------------------------------
    // CONNECT ON SSID CLICK
    // -----------------------------------------------------
    QObject::connect(ssidList, &QListWidget::itemClicked,
                     [ssidList, refresh, toggleWifi, fixBtn, doScan, updateInfo, updateWifiState]() mutable {
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

        // FIX: escape single quotes — an SSID or password containing '
        // previously broke the shell command entirely.
        QString ssidEsc = ssid;  ssidEsc.replace("'", "'\\''");
        QString passEsc = pass;  passEsc.replace("'", "'\\''");
        QString cmd = QString("nmcli device wifi connect '%1' password '%2'")
                        .arg(ssidEsc).arg(passEsc);

        // Lock the controls while connecting: the animated wait pumps
        // events, and a mid-connect Refresh would clear the list and
        // dangle the item pointer being animated below.
        ssidList->setEnabled(false);
        refresh->setEnabled(false);
        toggleWifi->setEnabled(false);
        fixBtn->setEnabled(false);

        QString original = item->text();
        int frame = 0;
        item->setText(QString::fromUtf8(SPIN_FRAMES[0]) + "  Connecting to " + ssid + "…");
        QCoreApplication::processEvents();

        // FIX: 45s timeout — the old 5s runCmd cut off real associations,
        // which commonly take 10-20s (scan + auth + DHCP).
        QString err;
        QString out = runCmdAsRootAnimated(cmd, 45000, [&]() {
            frame = (frame + 1) % SPIN_COUNT;
            item->setText(QString::fromUtf8(SPIN_FRAMES[frame]) + "  Connecting to " + ssid + "…");
        }, &err);

        item->setText(original);
        ssidList->setEnabled(true);
        refresh->setEnabled(true);
        toggleWifi->setEnabled(true);
        fixBtn->setEnabled(true);

        QString msg = !out.isEmpty() ? out : err;
        QMessageBox::information(nullptr, "Wi-Fi", msg.isEmpty() ? "Done." : msg);

        // Refresh so the ✔ lands on the newly joined network and the
        // IP/DNS/gateway info reflects the new connection.
        doScan();
        updateInfo();
        updateWifiState();
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
        " font-family:'DejaVu Sans';"
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