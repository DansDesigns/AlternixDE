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
#include <QInputDialog>
#include <QMessageBox>
#include <QFont>
#include <QList>
#include <QStringList>
#include <QSpacerItem>
#include <QLineEdit>
#include <QTimer>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <functional>

// ---------------------------------------------------------
// Helpers
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

static QString runCommandBT(const QString &program, const QStringList &args)
{
    QProcess proc;
    proc.start(program, args);
    proc.waitForFinished();
    QString out = proc.readAllStandardOutput();
    out += proc.readAllStandardError();
    return out;
}

// Spinner frames (Braille pattern rotation — present in DejaVu Sans).
static const char* const SPIN_FRAMES_BT[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"
};
static const int SPIN_COUNT_BT = 10;

// Runs a command while calling tick() roughly every 80ms so the UI can
// animate during the wait (pair/connect can take 10s+).
static QString runCommandBTAnimated(const QString &program, const QStringList &args,
                                    int timeoutMs, const std::function<void()> &tick)
{
    QProcess proc;
    proc.start(program, args);

    QElapsedTimer t;
    t.start();
    while (!proc.waitForFinished(80)) {
        if (t.elapsed() > timeoutMs) {
            proc.kill();
            proc.waitForFinished(500);
            break;
        }
        if (tick) tick();
        QCoreApplication::processEvents();
    }

    QString out = proc.readAllStandardOutput();
    out += proc.readAllStandardError();
    return out;
}

// Non-blocking 60-second bluetoothctl scan
static void startBluetoothScanLong()
{
    // Detach so we do not block the UI / plugin
    QProcess::startDetached(
        "bash",
        QStringList() << "-c"
                      << "bluetoothctl --timeout 60 scan on >/dev/null 2>&1"
    );
}

static bool isBluetoothPowered()
{
    QString out = runCommandBT("bluetoothctl", {"show"});
    // Look for "Powered: yes" or "Powered: no"
    for (const QString &line : out.split('\n')) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("Powered:", Qt::CaseInsensitive)) {
            if (trimmed.contains("yes", Qt::CaseInsensitive))
                return true;
            if (trimmed.contains("no", Qt::CaseInsensitive))
                return false;
        }
    }
    // Fallback: assume off
    return false;
}

static void setBluetoothPowered(bool on)
{
    if (on)
        runCommandBT("bluetoothctl", {"power", "on"});
    else
        runCommandBT("bluetoothctl", {"power", "off"});
}

static bool isBluetoothDiscoverable()
{
    QString out = runCommandBT("bluetoothctl", {"show"});
    for (const QString &line : out.split('\n')) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("Discoverable:", Qt::CaseInsensitive)) {
            if (trimmed.contains("yes", Qt::CaseInsensitive))
                return true;
            if (trimmed.contains("no", Qt::CaseInsensitive))
                return false;
        }
    }
    return false;
}

static void setBluetoothDiscoverable(bool on, int timeoutSec = 0)
{
    if (on) {
        if (timeoutSec > 0) {
            QString script = QString(
                "echo -e 'discoverable-timeout %1\\ndiscoverable on\\nquit' | bluetoothctl"
            ).arg(timeoutSec);
            runCommandBT("bash", {"-c", script});
        } else {
            runCommandBT("bash",
                         {"-c", "echo -e 'discoverable on\nquit' | bluetoothctl"});
        }
    } else {
        runCommandBT("bash",
                     {"-c", "echo -e 'discoverable off\nquit' | bluetoothctl"});
    }
}

struct BluetoothDevice {
    QString mac;
    QString name;
    bool connected = false;
    bool paired = false;
};

static QList<BluetoothDevice> getBluetoothDevices()
{
    QList<BluetoothDevice> devices;

    // List all known devices
    QString out = runCommandBT("bluetoothctl", {"devices"});
    const QStringList lines = out.split('\n', Qt::SkipEmptyParts);

    for (const QString &line : lines) {
        // Example: "Device AA:BB:CC:DD:EE:FF DeviceName"
        if (!line.startsWith("Device "))
            continue;

        QString rest = line.mid(QString("Device ").length()).trimmed();
        int spaceIdx = rest.indexOf(' ');
        if (spaceIdx <= 0)
            continue;

        BluetoothDevice dev;
        dev.mac = rest.left(spaceIdx).trimmed();
        dev.name = rest.mid(spaceIdx + 1).trimmed();

        // Query info for paired/connected status
        QString info = runCommandBT("bluetoothctl", {"info", dev.mac});
        for (const QString &infLine : info.split('\n')) {
            QString t = infLine.trimmed();
            if (t.startsWith("Connected:", Qt::CaseInsensitive)) {
                dev.connected = t.contains("yes", Qt::CaseInsensitive);
            } else if (t.startsWith("Paired:", Qt::CaseInsensitive)) {
                dev.paired = t.contains("yes", Qt::CaseInsensitive);
            }
        }

        devices.append(dev);
    }

    return devices;
}

// ---------------------------------------------------------
// BluetoothPage widget
// ---------------------------------------------------------

class BluetoothPage : public QWidget
{
public:
    explicit BluetoothPage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), stackedWidget(stack)
    {
        // Background + font, and force white for LABELS & popup labels only
        setStyleSheet(
            "QScrollArea { background:#282828;  font-family:Sans; border:none; }"
            "QWidget { background:#282828; font-family:Sans; }"
            "QLabel { color:white; font-family:Sans;}"
            "QMessageBox QLabel { color:white; font-family:Sans; }"
        );

        QVBoxLayout *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(40, 40, 40, 40);
        rootLayout->setSpacing(20);
        rootLayout->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

        // Title
        QLabel *titleLabel = new QLabel("Bluetooth", this);
        titleLabel->setStyleSheet("font-size:42px; font-weight:bold;");
        titleLabel->setAlignment(Qt::AlignCenter);
        rootLayout->addWidget(titleLabel);

        // -------------------------------------------------
        // Card 1: Device list
        // -------------------------------------------------
        QFrame *listFrame = new QFrame(this);
        listFrame->setStyleSheet(
            "QFrame {"
            " background:#3a3a3a;"
            " border-radius:40px;"
            "}"
        );
        listFrame->setFixedHeight(520);

        QVBoxLayout *listLayout = new QVBoxLayout(listFrame);
        listLayout->setContentsMargins(25, 25, 25, 25);
        listLayout->setSpacing(0);

        scrollArea = new QScrollArea(listFrame);
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameShape(QFrame::NoFrame);
        QScroller::grabGesture(scrollArea->viewport(), QScroller::LeftMouseButtonGesture);

        deviceContainer = new QWidget(scrollArea);
        deviceLayout = new QVBoxLayout(deviceContainer);
        deviceLayout->setContentsMargins(18, 10, 18, 10); // left/right padding similar to WiFi list
        deviceLayout->setSpacing(10);
        deviceLayout->addStretch();

        scrollArea->setWidget(deviceContainer);
        scrollArea->setStyleSheet(
            "QScrollArea {"
            " border:none;"
            "}"
            "QWidget {"
            " background-color:#444444;"
            " border-radius:22px;"
            "}"
        );

        listLayout->addWidget(scrollArea);
        rootLayout->addWidget(listFrame);

        // -------------------------------------------------
        // Card 2: Empty adapter/info card (same height as WiFi)
        // -------------------------------------------------
        QFrame *infoFrame = new QFrame(this);
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
        // Intentionally empty for now, just for symmetry with WiFi

        rootLayout->addWidget(infoFrame);

        // -------------------------------------------------
        // Bottom buttons: On/Off + Scan + Visible
        // -------------------------------------------------
        QHBoxLayout *bottomButtonsLayout = new QHBoxLayout();
        bottomButtonsLayout->setSpacing(40);
        bottomButtonsLayout->setAlignment(Qt::AlignHCenter);

        powerButton    = smallBtnBT("On");
        scanButton     = smallBtnBT("Scan");
        visibleButton  = smallBtnBT("Visible");

        bottomButtonsLayout->addWidget(powerButton);
        bottomButtonsLayout->addWidget(scanButton);
        bottomButtonsLayout->addWidget(visibleButton);

        rootLayout->addLayout(bottomButtonsLayout);

        // -------------------------------------------------
        // Back button
        // -------------------------------------------------
        QPushButton *backButton = new QPushButton(QStringLiteral("❮"), this);
        backButton->setFixedSize(140, 60);
        backButton->setStyleSheet(
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

        QHBoxLayout *backLayout = new QHBoxLayout();
        backLayout->addWidget(backButton, 0, Qt::AlignHCenter);
        rootLayout->addLayout(backLayout);

        // -------------------------------------------------
        // Scan refresh timer (for live updates while scanning)
        // -------------------------------------------------
        scanRefreshTimer = new QTimer(this);
        scanRefreshTimer->setInterval(2000); // every 2 seconds
        connect(scanRefreshTimer, &QTimer::timeout, this, [this]() {
            if (scanInProgress) {
                refreshDevices();
            } else {
                scanRefreshTimer->stop();
            }
        });

        // Spinner animation timer for the Scan button (the scan itself is a
        // detached process, so a plain timer drives the frames).
        scanSpinTimer = new QTimer(this);
        scanSpinTimer->setInterval(90);
        connect(scanSpinTimer, &QTimer::timeout, this, [this]() {
            spinFrame = (spinFrame + 1) % SPIN_COUNT_BT;
            scanButton->setText(QString::fromUtf8(SPIN_FRAMES_BT[spinFrame]));
        });

        // Connections
        connect(powerButton, &QPushButton::clicked, this, &BluetoothPage::togglePower);

        connect(scanButton, &QPushButton::clicked, this, [this]() {
            if (!bluetoothPowered) {
                QMessageBox::warning(this, "Bluetooth Off",
                                     "Bluetooth is currently turned off.\n"
                                     "Please turn it on before scanning.");
                return;
            }

            if (scanInProgress)
                return;

            scanInProgress = true;
            scanButton->setEnabled(false);

            // Start non-blocking 60s scan
            startBluetoothScanLong();

            // Spinner on the Scan button for the whole scan window —
            // replaces the modal "scanning for 60 seconds" popup.
            spinFrame = 0;
            scanButton->setText(QString::fromUtf8(SPIN_FRAMES_BT[0]));
            scanSpinTimer->start();

            // Start periodic refresh while scanning
            scanRefreshTimer->start();
            refreshDevices(); // immediate refresh at start

            // After 60s, re-enable and final refresh
            QTimer::singleShot(60000, this, [this]() {
                scanInProgress = false;
                scanSpinTimer->stop();
                scanButton->setText("Scan");
                scanButton->setEnabled(true);
                refreshDevices();
            });
        });

        connect(visibleButton, &QPushButton::clicked, this, &BluetoothPage::toggleVisible);

        connect(backButton, &QPushButton::clicked, this, [this]() {
            if (stackedWidget) {
                stackedWidget->setCurrentIndex(0);
            }
        });

        // Initial state — deferred so the page paints immediately; the
        // bluetoothctl queries (show, devices, per-device info) previously
        // all ran before the page could appear.
        QTimer::singleShot(50, this, [this]() {
            bluetoothPowered = isBluetoothPowered();
            discoverable     = isBluetoothDiscoverable();
            updatePowerButton();
            updateVisibleButton();
            refreshDevices();
        });
    }

private:
    QStackedWidget *stackedWidget = nullptr;
    QScrollArea *scrollArea = nullptr;
    QWidget *deviceContainer = nullptr;
    QVBoxLayout *deviceLayout = nullptr;
    QPushButton *powerButton = nullptr;
    QPushButton *scanButton = nullptr;
    QPushButton *visibleButton = nullptr;

    bool bluetoothPowered = false;
    bool discoverable = false;
    bool scanInProgress = false;
    bool connectInProgress = false;
    QTimer *scanRefreshTimer = nullptr;
    QTimer *scanSpinTimer = nullptr;
    int spinFrame = 0;

    void clearDeviceList()
    {
        int count = deviceLayout->count();
        for (int i = count - 1; i >= 0; --i) {
            QLayoutItem *item = deviceLayout->itemAt(i);
            if (item->spacerItem())
                continue; // keep the final stretch
            QWidget *w = item->widget();
            if (w) {
                deviceLayout->removeWidget(w);
                w->deleteLater();
            } else {
                deviceLayout->removeItem(item);
                delete item;
            }
        }
    }

    void refreshDevices()
    {
        // Never rebuild the list while a connect is animating a row button —
        // the scan-refresh timer still fires during pumped waits, and
        // clearing the list would delete the button mid-animation.
        if (connectInProgress) return;

        clearDeviceList();

        QList<BluetoothDevice> devices = getBluetoothDevices();

        QFont itemFont;
        itemFont.setPointSize(itemFont.pointSize() + 4); // roughly 26px equivalent

        for (const BluetoothDevice &dev : devices) {
            QFrame *rowFrame = new QFrame(deviceContainer);
            rowFrame->setStyleSheet(
                "QFrame { "
                "  background-color:#444444; "
                "  border-radius:20px; "
                "  border:1px solid #222222; "
                "}"
            );
            QHBoxLayout *rowLayout = new QHBoxLayout(rowFrame);
            rowLayout->setContentsMargins(14, 10, 14, 10);
            rowLayout->setSpacing(10);

            QPushButton *deviceButton = new QPushButton(dev.name, rowFrame);
            deviceButton->setFlat(true);
            deviceButton->setStyleSheet(
                "QPushButton { "
                "  background-color:transparent; "
                "  border:none; "
                "  text-align:left; "
                "  color:white; "
                "  font-size:26px;"
                "}"
                "QPushButton:pressed { "
                "  background-color:rgba(255,255,255,30); "
                "  border-radius:20px; "
                "}"
            );
            deviceButton->setFont(itemFont);

            rowLayout->addWidget(deviceButton, 1);

            // 🕱 Remove device button (always shown) – no color set so emoji keeps native colouring
            QPushButton *removeBtn = new QPushButton(QStringLiteral("🕱"), rowFrame);
            removeBtn->setFixedWidth(48);
            removeBtn->setStyleSheet(
                "QPushButton { "
                "  background-color:transparent; "
                "  border:none; "
                "  color:#ff4a6a; "
                "  font-size:32px; "
                "}"
        		"QPushButton:hover { color:#ff1616; background:#ad1236; border-radius:18px; }"
        		"QPushButton:pressed { color:#ffffff; background:#550000; border-radius:18px; }"
            );
            rowLayout->addWidget(removeBtn, 0, Qt::AlignRight);

            connect(removeBtn, &QPushButton::clicked, this, [this, dev]() {
                onRemoveDevice(dev);
            });

            // Red X disconnect button (only if connected)
            if (dev.connected) {
                QPushButton *disconnectBtn = new QPushButton(QStringLiteral("❌"), rowFrame);
                disconnectBtn->setFixedWidth(40);
                disconnectBtn->setStyleSheet(
                    "QPushButton { "
                    "  background-color:transparent; "
                    "  border:none; "
                    "  color:#ff4a6a; "
                    "  font-size:32px; "
                    "}"
        		    "QPushButton:hover { color:#ff1616; background:#ad1236; border-radius:18px; }"
        		    "QPushButton:pressed { color:#ffffff; background:#550000; border-radius:18px; }"

                );
                rowLayout->addWidget(disconnectBtn, 0, Qt::AlignRight);

                connect(disconnectBtn, &QPushButton::clicked, this, [this, dev]() {
                    onDisconnectDevice(dev);
                });
            }

            connect(deviceButton, &QPushButton::clicked, this, [this, dev, deviceButton]() {
                onDeviceClicked(dev, deviceButton);
            });

            deviceLayout->insertWidget(deviceLayout->count() - 1, rowFrame);
        }

        if (devices.isEmpty()) {
            QLabel *emptyLbl = new QLabel(
                bluetoothPowered
                    ? "No Bluetooth devices found"
                    : "Bluetooth is off",
                deviceContainer
            );
            emptyLbl->setStyleSheet("font-size:26px;"); // color from QLabel rule
            emptyLbl->setAlignment(Qt::AlignCenter);
            deviceLayout->insertWidget(deviceLayout->count() - 1, emptyLbl);
        }
    }

    void updatePowerButton()
    {
        if (bluetoothPowered) {
            powerButton->setText("On");
            powerButton->setStyleSheet(
                "QPushButton {"
                " background:#444444;"
                " color:#7CFC00;"      // bright green
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
            powerButton->setText("Off");
            powerButton->setStyleSheet(
                "QPushButton {"
                " background:#444444;"
                " color:#CC6666;"      // dim red
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
    }

    void updateVisibleButton()
    {
        if (discoverable) {
            visibleButton->setText("Visible");
            visibleButton->setStyleSheet(
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
            visibleButton->setText("Visible");
            visibleButton->setStyleSheet(
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
    }

    void togglePower()
    {
        bluetoothPowered = !bluetoothPowered;
        setBluetoothPowered(bluetoothPowered);
        updatePowerButton();
        refreshDevices();
    }

    void toggleVisible()
    {
        if (!bluetoothPowered) {
            QMessageBox::warning(this, "Bluetooth Off",
                                 "Bluetooth is currently turned off.\n"
                                 "Please turn it on before enabling visibility.");
            return;
        }

        if (!discoverable) {
            // Enable discoverable with 30-second timeout
            setBluetoothDiscoverable(true, 30);
            discoverable = true;
            updateVisibleButton();

            // Try to auto-refresh discoverable state after ~30s
            QTimer::singleShot(32000, this, [this]() {
                discoverable = isBluetoothDiscoverable();
                updateVisibleButton();
            });
        } else {
            // Manually turn off discoverable
            setBluetoothDiscoverable(false, 0);
            discoverable = false;
            updateVisibleButton();
        }
    }

    void onDeviceClicked(const BluetoothDevice &dev, QPushButton *deviceButton)
    {
        if (!bluetoothPowered) {
            QMessageBox::warning(this, "Bluetooth Off",
                                 "Bluetooth is currently turned off.\n"
                                 "Please turn it on before connecting.");
            return;
        }

        QString script;
        QString title = "Bluetooth";

        if (dev.paired) {
            // Simple connect for already paired device
            script = QString(
                "echo -e '"
                "connect %1\n"
                "quit\n' | bluetoothctl"
            ).arg(dev.mac);
        } else {
            // Not paired: (optional) PIN dialog, then simple pair + connect
            bool ok = false;
            QString pin = QInputDialog::getText(
                this,
                "Bluetooth Pairing",
                QString("Enter PIN / passkey for %1\n(leave blank if not required):").arg(dev.name),
                QLineEdit::Normal,
                "",
                &ok
            );

            if (!ok)
                return;

            Q_UNUSED(pin); // script is intentionally simple: just pair/connect by MAC

            script = QString(
                "echo -e '"
                "pair %1\n"
                "connect %1\n"
                "quit\n' | bluetoothctl"
            ).arg(dev.mac);
        }

        // Lock controls while connecting — the animated wait pumps events,
        // so without this another row / scan / power tap could re-enter.
        setControlsEnabled(false);
        connectInProgress = true;

        int frame = 0;
        deviceButton->setText(
            QString::fromUtf8(SPIN_FRAMES_BT[0]) + "  Connecting to " + dev.name + "…");
        QCoreApplication::processEvents();

        QString out = runCommandBTAnimated("bash", {"-c", script}, 30000, [&]() {
            frame = (frame + 1) % SPIN_COUNT_BT;
            deviceButton->setText(
                QString::fromUtf8(SPIN_FRAMES_BT[frame]) + "  Connecting to " + dev.name + "…");
        });

        deviceButton->setText(dev.name);
        connectInProgress = false;
        setControlsEnabled(true);

        // Simple error detection – if bluetoothctl reports "Failed" or "Error", show it
        if (out.contains("Failed", Qt::CaseInsensitive) ||
            out.contains("Error", Qt::CaseInsensitive)) {
            QMessageBox::warning(
                this,
                title,
                QString("Failed to connect to %1.\n\nDetails:\n%2")
                    .arg(dev.name, out.trimmed())
            );
        } else {
            QMessageBox::information(
                this,
                title,
                QString("Pairing / connecting to %1...\n\nbluetoothctl output:\n%2")
                    .arg(dev.name, out.trimmed())
            );
        }

        // Give BlueZ a moment to settle state before re-reading "info"
        QTimer::singleShot(1500, this, [this]() {
            refreshDevices();
        });
    }

    // Enable/disable the page's interactive controls as a group (used while
    // a connect is in progress and events are being pumped).
    void setControlsEnabled(bool en)
    {
        powerButton->setEnabled(en);
        scanButton->setEnabled(en && !scanInProgress);
        visibleButton->setEnabled(en);
        if (deviceContainer) deviceContainer->setEnabled(en);
    }

    void onDisconnectDevice(const BluetoothDevice &dev)
    {
        runCommandBT("bash", {"-c",
                              QString("echo -e 'disconnect %1\nquit' | bluetoothctl").arg(dev.mac)});
        QMessageBox::information(this, "Bluetooth",
                                 QString("Disconnecting from %1...").arg(dev.name));
        refreshDevices();
    }

    void onRemoveDevice(const BluetoothDevice &dev)
    {
        runCommandBT("bash", {"-c",
                              QString("echo -e 'remove %1\nquit' | bluetoothctl").arg(dev.mac)});
        QMessageBox::information(this, "Bluetooth",
                                 QString("Removing %1 from known devices...").arg(dev.name));
        refreshDevices();
    }
};

// ---------------------------------------------------------
// Factory function for osm-settings plugin
// ---------------------------------------------------------

extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new BluetoothPage(stack);
}
