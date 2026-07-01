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
        //
        // FIX: runCmd() only ever captured stdout and threw stderr away, so
        // if nmcli was failing (missing polkit auth, radio off, no wifi
        // device, nmcli not installed, etc.) the list just silently went
        // empty with no clue why. Use a raw QProcess here and read stdout
        // and stderr SEPARATELY so a real failure is shown, not hidden.
        QProcess proc;
        proc.start("bash", {"-c", "nmcli -t -f IN-USE,SSID device wifi list --rescan yes"});
        proc.waitForFinished(15000);  // a real scan can take 5-10s
        QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();

        ssidList->clear();

        if (out.isEmpty()) {
            // Surface the actual reason instead of a generic "no networks".
            QString diag;
            if (!err.isEmpty()) {
                diag = err;
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

        // Run as root via sudo -n (non-interactive). Alternix sets NOPASSWD
        // for the user, so this succeeds silently; if it can't, sudo exits
        // immediately with an error we can show instead of hanging on a
        // password prompt inside a GUI app with no terminal.
        QProcess proc;
        proc.start("sudo", {"-n", "bash", scriptPath, iface});
        proc.waitForFinished(30000);
        QString out = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();

        QString report = out.isEmpty() ? "No output from fix script." : out;
        if (!err.isEmpty())
            report += "\n\n[stderr]\n" + err;
        if (err.contains("password is required") || err.contains("a password is required"))
            report += "\n\nTip: passwordless sudo isn't configured for this "
                      "user, so the fix can't edit /etc. Run the app as root "
                      "or add a NOPASSWD sudoers rule.";

        QMessageBox::information(nullptr, "Wi-Fi Fix", report);

        doScan();
        updateInfo();
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