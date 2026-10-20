// AlternixOS / osm-settings Bluetooth page
// Qt5/Qt6 Widgets, Linux BlueZ (bluetoothctl). No QtBluetooth/QtDBus requirement.
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
#include <QLineEdit>
#include <QTimer>
#include <QThread>
#include <QPointer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTextStream>
#include <functional>
#include <algorithm>
#include <memory>
#include <QSet>

static QPushButton *smallBtnBT(const QString &text, int width = 180, int height = 60)
{
    auto *b = new QPushButton(text);
    b->setFixedSize(width, height);
    b->setStyleSheet(
        "QPushButton { background:#444444; color:white; border:1px solid #222222; "
        "border-radius:16px; font-size:24px; font-weight:bold; padding:7px; }"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
        "QPushButton:disabled { color:#888888; }");
    return b;
}

// Only used in the background query thread. Bounded waits prevent an absent
// bluetoothd from freezing osm-settings or hanging a worker forever.
static QString commandBT(const QString &program, const QStringList &arguments,
                         int timeoutMs = 2500)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, arguments);
    if (!p.waitForStarted(1500))
        return QStringLiteral("ERROR: Cannot run %1: %2").arg(program, p.errorString());
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return QString::fromUtf8(p.readAll()) + QStringLiteral("\nERROR: Command timed out.");
    }
    return QString::fromUtf8(p.readAll());
}

static QString fileTextBT(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll()).trimmed();
}

static QString cleanOutputBT(QString s)
{
    // bluetoothctl can print terminal colouring, control codes and redraws.
    s.remove(QRegularExpression(QStringLiteral("\\x1b\\[[0-9;?]*[ -/]*[@-~]")));
    s.remove(QLatin1Char('\r'));
    return s;
}

static bool yesPropertyBT(const QString &info, const QString &key)
{
    const QString prefix = key + QLatin1Char(':');
    for (const QString &line : info.split(QLatin1Char('\n'))) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith(prefix, Qt::CaseInsensitive))
            return trimmed.mid(prefix.size()).trimmed().compare("yes", Qt::CaseInsensitive) == 0;
    }
    return false;
}

static QString propertyBT(const QString &info, const QString &key)
{
    const QString prefix = key + QLatin1Char(':');
    for (const QString &line : info.split(QLatin1Char('\n'))) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith(prefix, Qt::CaseInsensitive))
            return trimmed.mid(prefix.size()).trimmed();
    }
    return {};
}

static bool validAddressBT(const QString &mac)
{
    static const QRegularExpression macRe(
        QStringLiteral("^[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}$"));
    return macRe.match(mac).hasMatch();
}

struct BluetoothDevice {
    QString mac;
    QString name;
    bool connected = false;
    bool paired = false;
    bool trusted = false;
};

struct BluetoothSnapshot {
    bool toolInstalled = false;
    bool daemonRunning = false;
    bool controllerAvailable = false;
    bool powered = false;
    bool discoverable = false;
    bool softBlocked = false;
    bool hardBlocked = false;
    int kernelControllerCount = 0;
    int usbBluetoothInterfaces = 0;
    QString adapterName;
    QString adapterAddress;
    QStringList adapterNames;
    QString status;
    QString advice;
    QList<BluetoothDevice> devices;
};

// All per-device bluetoothctl info calls run in a QThread, not in the UI thread.
// No Q_OBJECT is needed; QThread::finished is delivered back to the page.
class BluetoothQueryThread : public QThread {
public:
    BluetoothSnapshot snapshot;

    void run() override
    {
        auto &s = snapshot;
        s.toolInstalled = !QStandardPaths::findExecutable("bluetoothctl").isEmpty();

        const QDir bluetoothDir(QStringLiteral("/sys/class/bluetooth"));
        for (const QFileInfo &e : bluetoothDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (e.fileName().startsWith("hci")) ++s.kernelControllerCount;
        }

        // Probe generic USB Bluetooth interfaces even if no hciN controller was
        // created. This distinguishes an uninitialised USB radio from a
        // completely invisible radio on systems lacking lsusb.
        const QDir usbDir(QStringLiteral("/sys/bus/usb/devices"));
        for (const QFileInfo &e : usbDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString path = e.absoluteFilePath();
            if (fileTextBT(path + "/bInterfaceClass").compare("e0", Qt::CaseInsensitive) == 0 &&
                fileTextBT(path + "/bInterfaceSubClass").compare("01", Qt::CaseInsensitive) == 0 &&
                fileTextBT(path + "/bInterfaceProtocol").compare("01", Qt::CaseInsensitive) == 0)
                ++s.usbBluetoothInterfaces;
        }

        // Read rfkill from sysfs; this also works if the rfkill utility is absent.
        const QDir rfkillDir(QStringLiteral("/sys/class/rfkill"));
        for (const QFileInfo &e : rfkillDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QString path = e.absoluteFilePath();
            if (fileTextBT(path + "/type").compare("bluetooth", Qt::CaseInsensitive) != 0)
                continue;
            s.softBlocked |= (fileTextBT(path + "/soft") == "1");
            s.hardBlocked |= (fileTextBT(path + "/hard") == "1");
        }

        if (!s.toolInstalled) {
            s.status = "BlueZ tools are missing";
            s.advice = "Install the bluez package (which provides bluetoothctl).";
            return;
        }

        const QString daemon = commandBT("pgrep", {"-x", "bluetoothd"}, 1200);
        s.daemonRunning = QRegularExpression(QStringLiteral("^\\d+"))
                              .match(daemon.trimmed()).hasMatch();

        QString adapters = cleanOutputBT(commandBT("bluetoothctl", {"list"}, 2800));
        for (const QString &line : adapters.split('\n')) {
            QString l = line.trimmed();
            if (l.startsWith("Controller ")) s.adapterNames.append(l.mid(11));
        }
        s.controllerAvailable = !s.adapterNames.isEmpty();
        if (s.controllerAvailable) s.daemonRunning = true;

        QString show;
        if (s.controllerAvailable) {
            show = cleanOutputBT(commandBT("bluetoothctl", {"show"}, 2800));
            s.adapterName = propertyBT(show, "Alias");
            if (s.adapterName.isEmpty()) s.adapterName = propertyBT(show, "Name");
            s.adapterAddress = propertyBT(show, "Controller");
            // "Controller" is usually the first line, not a field in `show`.
            if (s.adapterAddress.isEmpty()) {
                QRegularExpression addr(QStringLiteral("([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})"));
                auto match = addr.match(show);
                if (match.hasMatch()) s.adapterAddress = match.captured(1);
            }
            s.powered = yesPropertyBT(show, "Powered");
            s.discoverable = yesPropertyBT(show, "Discoverable");
        }

        if (s.hardBlocked) {
            s.status = "Bluetooth hardware is blocked";
            s.advice = "Check the wireless switch, Fn key, BIOS/UEFI or airplane mode. Software cannot override a hardware block.";
        } else if (s.softBlocked) {
            s.status = "Bluetooth is blocked by rfkill";
            s.advice = "Choose Repair to clear the software radio block.";
        } else if (!s.daemonRunning) {
            s.status = "Bluetooth service is not running";
            s.advice = "Choose Repair to start bluetoothd through the system's service manager.";
        } else if (!s.controllerAvailable && s.kernelControllerCount) {
            s.status = "Kernel adapter found, but BlueZ cannot use it";
            s.advice = "Check bluetoothd and kernel/firmware logs. Choose Repair to retry adapter initialisation.";
        } else if (!s.controllerAvailable && s.usbBluetoothInterfaces > 0) {
            s.status = "USB Bluetooth hardware found, but no HCI adapter";
            s.advice = "USB interface is present but its Bluetooth controller did not initialise. Try Repair, then check dmesg for firmware-load or btusb driver errors.";
        } else if (!s.controllerAvailable) {
            s.status = "No Bluetooth adapter detected";
            s.advice = "No HCI controller is registered. Check BIOS/UEFI, USB/PCI detection, btusb and chipset firmware. Repair can try basic recovery but cannot install chipset-specific firmware.";
        } else if (!s.powered) {
            s.status = "Adapter detected, switched off";
            s.advice = "Press On to power up the adapter.";
        } else {
            s.status = "Bluetooth adapter ready";
            s.advice = "Put the remote device in pairing mode and use Scan to find it. Both Classic and BLE devices are scanned.";
        }

        if (!s.controllerAvailable || !s.daemonRunning || !s.powered)
            return;

        QString devicesText = cleanOutputBT(commandBT("bluetoothctl", {"devices"}, 3500));
        static const QRegularExpression deviceRe(QStringLiteral(
            "^Device\\s+([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})(?:\\s+(.*))?$"));
        for (const QString &line : devicesText.split('\n')) {
            if (isInterruptionRequested()) return;
            auto match = deviceRe.match(line.trimmed());
            if (!match.hasMatch()) continue;
            BluetoothDevice d;
            d.mac = match.captured(1);
            d.name = match.captured(2).trimmed();
            if (d.name.isEmpty() || d.name == d.mac) d.name = d.mac;
            s.devices.append(d);
            // Avoid a rogue device cache exhausting resources on old tablets.
            if (s.devices.size() >= 80) break;
        }

        // Most BlueZ versions support filtered device lists. This replaces
        // N slow `info` subprocesses with three queries even with dozens of
        // remembered devices. Old BlueZ releases fall back to `info`.
        const QString pairedText = cleanOutputBT(commandBT("bluetoothctl", {"devices", "Paired"}, 3000));
        const QString connectedText = cleanOutputBT(commandBT("bluetoothctl", {"devices", "Connected"}, 3000));
        const QString trustedText = cleanOutputBT(commandBT("bluetoothctl", {"devices", "Trusted"}, 3000));
        const auto filterWorks = [](const QString &text) {
            return !text.contains("Invalid", Qt::CaseInsensitive) &&
                   !text.contains("unknown", Qt::CaseInsensitive) &&
                   !text.contains("Error:", Qt::CaseInsensitive) &&
                   !text.contains("timed out", Qt::CaseInsensitive) &&
                   !text.contains("Too many arguments", Qt::CaseInsensitive);
        };
        const bool filtersSupported = filterWorks(pairedText) && filterWorks(connectedText) &&
                                      filterWorks(trustedText);
        if (filtersSupported) {
            const auto addresses = [](const QString &text) {
                QSet<QString> found;
                for (const QString &line : text.split('\n')) {
                    auto m = deviceRe.match(line.trimmed());
                    if (m.hasMatch()) found.insert(m.captured(1).toUpper());
                }
                return found;
            };
            const auto pairedSet = addresses(pairedText);
            const auto connectedSet = addresses(connectedText);
            const auto trustedSet = addresses(trustedText);
            for (BluetoothDevice &d : s.devices) {
                d.paired = pairedSet.contains(d.mac.toUpper());
                d.connected = connectedSet.contains(d.mac.toUpper());
                d.trusted = trustedSet.contains(d.mac.toUpper());
            }
        } else {
            for (BluetoothDevice &d : s.devices) {
                if (isInterruptionRequested()) return;
                const QString info = cleanOutputBT(commandBT("bluetoothctl", {"info", d.mac}, 1500));
                d.connected = yesPropertyBT(info, "Connected");
                d.paired = yesPropertyBT(info, "Paired");
                d.trusted = yesPropertyBT(info, "Trusted");
                QString alias = propertyBT(info, "Alias");
                if (!alias.isEmpty()) d.name = alias;
            }
        }
        // Connected devices first, then paired, then newly discovered devices.
        std::stable_sort(s.devices.begin(), s.devices.end(),
                         [](const BluetoothDevice &a, const BluetoothDevice &b) {
            return (a.connected ? 2 : (a.paired ? 1 : 0)) >
                   (b.connected ? 2 : (b.paired ? 1 : 0));
        });
    }
};

static const char *const BT_FRAMES[] = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"
};

static QString bluetoothErrorBT(const QString &raw)
{
    if (raw.contains("AuthenticationFailed", Qt::CaseInsensitive) ||
        raw.contains("AuthenticationRejected", Qt::CaseInsensitive))
        return "The device rejected the pairing or the code was incorrect.";
    if (raw.contains("AuthenticationCanceled", Qt::CaseInsensitive))
        return "The pairing was cancelled.";
    if (raw.contains("AuthenticationTimeout", Qt::CaseInsensitive))
        return "Pairing timed out. Keep the device in pairing mode.";
    if (raw.contains("not available", Qt::CaseInsensitive) ||
        raw.contains("No default controller", Qt::CaseInsensitive))
        return "The adapter or remote device is unavailable.";
    if (raw.contains("not ready", Qt::CaseInsensitive) ||
        raw.contains("NotReady", Qt::CaseInsensitive))
        return "The adapter is not ready. Check power and rfkill.";
    if (raw.contains("le-connection-abort-by-local", Qt::CaseInsensitive) ||
        raw.contains("Connection timeout", Qt::CaseInsensitive) ||
        raw.contains("br-connection-page-timeout", Qt::CaseInsensitive))
        return "The device stopped advertising or did not respond. Scan again.";
    if (raw.contains("org.bluez.Error.NotSupported", Qt::CaseInsensitive) ||
        raw.contains("br-connection-profile-unavailable", Qt::CaseInsensitive))
        return "This device needs a Bluetooth profile/service that may not be available on the system.";
    if (raw.contains("Connection refused", Qt::CaseInsensitive))
        return "The device refused the connection, possibly because it is connected elsewhere.";
    if (raw.contains("Timed out", Qt::CaseInsensitive))
        return "The operation timed out.";
    return "Check that the device is nearby, in pairing mode and not connected to another computer.";
}

class BluetoothPage : public QWidget
{
public:
    explicit BluetoothPage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), stackedWidget(stack)
    {
        setStyleSheet(
            "QScrollArea { background:#282828; font-family:Sans; border:none; }"
            "QWidget { background:#282828; font-family:Sans; }"
            "QLabel { color:white; font-family:Sans; }"
            "QMessageBox QLabel { color:white; font-family:Sans; }");

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(40, 40, 40, 40);
        root->setSpacing(20);
        auto *title = new QLabel("Bluetooth", this);
        title->setStyleSheet("font-size:42px; font-weight:bold;");
        title->setAlignment(Qt::AlignCenter);
        root->addWidget(title);

        auto *middle = new QScrollArea(this);
        middle->setWidgetResizable(true);
        middle->setFrameShape(QFrame::NoFrame);
        middle->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        middle->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(middle->viewport(), QScroller::LeftMouseButtonGesture);
        auto *middleContent = new QWidget(middle);
        auto *middleLayout = new QVBoxLayout(middleContent);
        middleLayout->setContentsMargins(0, 0, 0, 0);
        middleLayout->setSpacing(20);

        auto *devicesCard = new QFrame(middleContent);
        devicesCard->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
        devicesCard->setFixedHeight(520);
        auto *devicesCardLayout = new QVBoxLayout(devicesCard);
        devicesCardLayout->setContentsMargins(25, 25, 25, 25);
        scrollArea = new QScrollArea(devicesCard);
        scrollArea->setWidgetResizable(true);
        scrollArea->setFrameShape(QFrame::NoFrame);
        QScroller::grabGesture(scrollArea->viewport(), QScroller::LeftMouseButtonGesture);
        scrollArea->setStyleSheet(
            "QScrollArea { border:none; }"
            "QWidget { background:#444444; border-radius:22px; }");
        deviceContainer = new QWidget(scrollArea);
        deviceLayout = new QVBoxLayout(deviceContainer);
        deviceLayout->setContentsMargins(18, 10, 18, 10);
        deviceLayout->setSpacing(10);
        deviceLayout->addStretch();
        scrollArea->setWidget(deviceContainer);
        devicesCardLayout->addWidget(scrollArea);
        middleLayout->addWidget(devicesCard);

        // Formerly empty card: real adapter/driver/service diagnostics now.
        auto *infoCard = new QFrame(middleContent);
        infoCard->setStyleSheet("QFrame { background:#3a3a3a; border-radius:30px; }");
        infoCard->setMinimumHeight(240);
        auto *infoLayout = new QVBoxLayout(infoCard);
        infoLayout->setContentsMargins(24, 18, 24, 18);
        infoLayout->setSpacing(6);
        adapterLabel = new QLabel("Checking Bluetooth hardware…", infoCard);
        adapterLabel->setStyleSheet("font-size:23px; font-weight:bold; background:transparent;");
        adapterLabel->setWordWrap(true);
        statusLabel = new QLabel("", infoCard);
        statusLabel->setStyleSheet("font-size:20px; background:transparent;");
        statusLabel->setWordWrap(true);
        hintLabel = new QLabel("", infoCard);
        hintLabel->setStyleSheet("font-size:18px; color:#cccccc; background:transparent;");
        hintLabel->setWordWrap(true);
        infoLayout->addWidget(adapterLabel);
        infoLayout->addWidget(statusLabel);
        infoLayout->addWidget(hintLabel);
        infoLayout->addStretch();
        auto *infoActions = new QHBoxLayout;
        infoActions->addStretch();
        checkButton = smallBtnBT("Check", 145, 46);
        repairButton = smallBtnBT("Repair", 145, 46);
        infoActions->addWidget(checkButton);
        infoActions->addWidget(repairButton);
        infoLayout->addLayout(infoActions);
        middleLayout->addWidget(infoCard);
        middleLayout->addStretch();
        middle->setWidget(middleContent);
        root->addWidget(middle, 1);

        auto *bottom = new QHBoxLayout;
        bottom->setAlignment(Qt::AlignHCenter);
        bottom->setSpacing(40);
        powerButton = smallBtnBT("Off");
        scanButton = smallBtnBT("Scan");
        visibleButton = smallBtnBT("Visible");
        bottom->addWidget(powerButton);
        bottom->addWidget(scanButton);
        bottom->addWidget(visibleButton);
        root->addLayout(bottom);

        auto *backButton = new QPushButton(QStringLiteral("❮"), this);
        backButton->setFixedSize(140, 60);
        backButton->setStyleSheet(
            "QPushButton { background:#444444; color:white; border:1px solid #222222; "
            "border-radius:16px; font-size:34px; font-family:'DejaVu Sans'; }"
            "QPushButton:hover { background:#555555; }"
            "QPushButton:pressed { background:#333333; }");
        auto *back = new QHBoxLayout;
        back->addWidget(backButton, 0, Qt::AlignHCenter);
        root->addLayout(back);

        scanRefreshTimer = new QTimer(this);
        scanRefreshTimer->setInterval(2500);
        connect(scanRefreshTimer, &QTimer::timeout, this, [this] { refreshDevices(); });
        scanSpinTimer = new QTimer(this);
        scanSpinTimer->setInterval(100);
        connect(scanSpinTimer, &QTimer::timeout, this, [this] {
            spinFrame = (spinFrame + 1) % 10;
            scanButton->setText(QString::fromUtf8(BT_FRAMES[spinFrame]));
            if (scanStatusLabel)
                scanStatusLabel->setText(QString::fromUtf8(BT_FRAMES[spinFrame]) +
                                         "  Scanning for nearby devices…");
        });
        pairSpinTimer = new QTimer(this);
        pairSpinTimer->setInterval(140);
        connect(pairSpinTimer, &QTimer::timeout, this, [this] {
            if (activeDeviceButton)
                activeDeviceButton->setText(QString::fromUtf8(BT_FRAMES[spinFrame++ % 10]) +
                                            "  " + pairMessage + "…");
        });

        connect(powerButton, &QPushButton::clicked, this, [this] { togglePower(); });
        connect(scanButton, &QPushButton::clicked, this, [this] { startScan(); });
        connect(visibleButton, &QPushButton::clicked, this, [this] { toggleVisible(); });
        connect(checkButton, &QPushButton::clicked, this, [this] { refreshDevices(); });
        connect(repairButton, &QPushButton::clicked, this, [this] { repairAdapter(); });
        connect(backButton, &QPushButton::clicked, this, [this] {
            stopScan();
            if (stackedWidget) stackedWidget->setCurrentIndex(0);
        });

        QTimer::singleShot(50, this, [this] { refreshDevices(); });
    }

    ~BluetoothPage() override
    {
        if (queryThread) {
            queryThread->requestInterruption();
            // Worker owns itself until finished (see finished->deleteLater).
            queryThread = nullptr;
        }
        if (scanProcess) scanProcess->kill();
        if (pairProcess) pairProcess->kill();
    }

private:
    enum class PairStage { Idle, RegisterAgent, DefaultAgent, Pairable, Pairing,
                           Trusting, Connecting, Verifying };

    QStackedWidget *stackedWidget = nullptr;
    QScrollArea *scrollArea = nullptr;
    QWidget *deviceContainer = nullptr;
    QVBoxLayout *deviceLayout = nullptr;
    QPushButton *powerButton = nullptr;
    QPushButton *scanButton = nullptr;
    QPushButton *visibleButton = nullptr;
    QPushButton *checkButton = nullptr;
    QPushButton *repairButton = nullptr;
    QLabel *adapterLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QLabel *hintLabel = nullptr;
    QPointer<QLabel> scanStatusLabel;
    QTimer *scanRefreshTimer = nullptr;
    QTimer *scanSpinTimer = nullptr;
    QTimer *pairSpinTimer = nullptr;
    QPointer<BluetoothQueryThread> queryThread;
    QPointer<QProcess> scanProcess;
    QPointer<QProcess> pairProcess;
    QPointer<QPushButton> activeDeviceButton;
    BluetoothSnapshot state;
    PairStage pairStage = PairStage::Idle;
    bool scanInProgress = false;
    bool connectInProgress = false;
    bool pendingRefresh = false;
    int spinFrame = 0;
    QString pairMac;
    QString pairName;
    QString pairMessage;
    QString pairTranscript;
    QString recentPairText;
    QString lastPrompt;

    void refreshDevices()
    {
        if (connectInProgress) { pendingRefresh = true; return; }
        if (queryThread) { pendingRefresh = true; return; }
        auto *worker = new BluetoothQueryThread;
        queryThread = worker;
        checkButton->setEnabled(false);
        connect(worker, &QThread::finished, this, [this, worker] {
            queryThread = nullptr;
            state = worker->snapshot;
            checkButton->setEnabled(true);
            updateAdapterCard();
            renderDevices();
            updateButtons();
            const bool repeat = pendingRefresh && !connectInProgress && !scanInProgress;
            pendingRefresh = false;
            if (repeat)
                QTimer::singleShot(200, this, [this] { refreshDevices(); });
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    }

    void clearDeviceList()
    {
        for (int i = deviceLayout->count() - 1; i >= 0; --i) {
            QLayoutItem *item = deviceLayout->itemAt(i);
            if (item->spacerItem()) continue;
            if (QWidget *w = item->widget()) {
                deviceLayout->removeWidget(w);
                delete w; // No UI pointer is retained while connecting.
            } else {
                deviceLayout->removeItem(item);
                delete item;
            }
        }
        scanStatusLabel = nullptr;
    }

    void renderDevices()
    {
        if (connectInProgress) return;
        clearDeviceList();
        for (const BluetoothDevice &dev : state.devices) {
            auto *row = new QFrame(deviceContainer);
            row->setStyleSheet(
                "QFrame { background-color:#444444; border-radius:20px; border:1px solid #222222; }");
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(14, 10, 14, 10);
            rowLayout->setSpacing(8);
            const QString shown = dev.name + (dev.connected ? "  • Connected" :
                                             (dev.paired ? "  • Paired" : ""));
            auto *deviceButton = new QPushButton(shown, row);
            deviceButton->setFlat(true);
            deviceButton->setToolTip(dev.mac);
            deviceButton->setStyleSheet(
                "QPushButton { background:transparent; border:none; text-align:left; "
                "color:white; font-size:24px; }"
                "QPushButton:pressed { background:rgba(255,255,255,30); border-radius:20px; }");
            rowLayout->addWidget(deviceButton, 1);
            connect(deviceButton, &QPushButton::clicked, this, [this, dev, deviceButton] {
                if (!dev.connected) beginConnection(dev, deviceButton);
            });

            if (dev.connected) {
                auto *disconnectButton = new QPushButton(QStringLiteral("✕"), row);
                disconnectButton->setFixedWidth(45);
                disconnectButton->setToolTip("Disconnect");
                disconnectButton->setStyleSheet(
                    "QPushButton { color:#ff5a68; background:transparent; border:none; font-size:30px; }"
                    "QPushButton:hover { background:#ad1236; border-radius:14px; }");
                rowLayout->addWidget(disconnectButton);
                connect(disconnectButton, &QPushButton::clicked, this, [this, dev] {
                    deviceCommand({"disconnect", dev.mac}, "Disconnect", dev.name);
                });
            }
            auto *removeButton = new QPushButton(QStringLiteral("🗑"), row);
            removeButton->setFixedWidth(48);
            removeButton->setToolTip("Forget this device");
            removeButton->setStyleSheet(
                "QPushButton { color:#ff4a6a; background:transparent; border:none; font-size:27px; }"
                "QPushButton:hover { background:#ad1236; border-radius:14px; }");
            rowLayout->addWidget(removeButton);
            connect(removeButton, &QPushButton::clicked, this, [this, dev] {
                if (QMessageBox::question(this, "Forget Bluetooth device",
                       QString("Forget %1 and remove its saved pairing?").arg(dev.name))
                    == QMessageBox::Yes)
                    deviceCommand({"remove", dev.mac}, "Remove", dev.name);
            });
            deviceLayout->insertWidget(deviceLayout->count() - 1, row);
        }
        if (scanInProgress) {
            auto *status = new QLabel("Scanning for nearby devices…", deviceContainer);
            status->setAlignment(Qt::AlignCenter);
            status->setStyleSheet("font-size:22px; color:#aaaaaa; background:transparent;");
            deviceLayout->insertWidget(deviceLayout->count() - 1, status);
            scanStatusLabel = status;
        }
        if (state.devices.isEmpty()) {
            auto *empty = new QLabel(state.controllerAvailable
                ? (state.powered ? "No Bluetooth devices found yet" : "Bluetooth is switched off")
                : "No usable Bluetooth adapter detected", deviceContainer);
            empty->setAlignment(Qt::AlignCenter);
            empty->setWordWrap(true);
            empty->setStyleSheet("font-size:24px; background:transparent;");
            deviceLayout->insertWidget(deviceLayout->count() - 1, empty);
        }
    }

    void updateAdapterCard()
    {
        QString adapter = state.adapterName.isEmpty() ? QString("Adapter not available") : state.adapterName;
        if (state.controllerAvailable && !state.adapterAddress.isEmpty())
            adapter += "  (" + state.adapterAddress + ")";
        adapterLabel->setText(adapter);
        statusLabel->setText(state.status + QString("  •  HCI: %1  •  USB BT: %2  •  BlueZ: %3")
                             .arg(state.kernelControllerCount)
                             .arg(state.usbBluetoothInterfaces)
                             .arg(state.adapterNames.size()));
        hintLabel->setText(state.advice);
    }

    void updateButtons()
    {
        powerButton->setText(state.powered ? "On" : "Off");
        visibleButton->setText("Visible");
        auto accent = [](QPushButton *b, bool active) {
            b->setStyleSheet(QString(
                "QPushButton { background:#444444; color:%1; border:1px solid #222222; "
                "border-radius:16px; font-size:26px; font-weight:bold; padding:8px; }"
                "QPushButton:hover { background:#555555; }"
                "QPushButton:disabled { color:#888888; }")
                .arg(active ? "#7CFC00" : "#CC6666"));
        };
        accent(powerButton, state.powered);
        accent(visibleButton, state.discoverable);
        const bool enabled = !connectInProgress;
        powerButton->setEnabled(enabled && state.controllerAvailable && !state.hardBlocked);
        visibleButton->setEnabled(enabled && state.powered);
        scanButton->setEnabled(enabled && state.powered && !scanInProgress);
        checkButton->setEnabled(enabled && !queryThread);
        repairButton->setEnabled(enabled);
        deviceContainer->setEnabled(enabled);
    }

    // An async command with a hard deadline. No shell, and all externally
    // supplied addresses are validated before being passed as arguments.
    void runAsync(const QString &program, const QStringList &args,
                  const std::function<void(const QString &, bool)> &done,
                  int deadlineMs = 8000)
    {
        auto *proc = new QProcess(this);
        proc->setProcessChannelMode(QProcess::MergedChannels);
        auto *timeout = new QTimer(proc);
        timeout->setSingleShot(true);
        timeout->setInterval(deadlineMs);
        struct Outcome { bool timedOut = false; bool completed = false; };
        auto outcome = std::make_shared<Outcome>();
        auto complete = [proc, timeout, outcome, done](const QString &output, bool ok) {
            if (outcome->completed) return;
            outcome->completed = true;
            timeout->stop();
            done(cleanOutputBT(output), ok && !outcome->timedOut);
            proc->deleteLater();
        };
        connect(timeout, &QTimer::timeout, proc, [proc, outcome] {
            outcome->timedOut = true;
            if (proc->state() != QProcess::NotRunning) proc->kill();
        });
        connect(proc, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, [proc, outcome, complete](int code, QProcess::ExitStatus exitStatus) {
            QString output = QString::fromUtf8(proc->readAll());
            if (outcome->timedOut) output += "\nERROR: Command timed out.";
            bool ok = code == 0 && exitStatus == QProcess::NormalExit &&
                      !output.contains("Failed", Qt::CaseInsensitive) &&
                      !output.contains("Error:", Qt::CaseInsensitive);
            complete(output, ok);
        });
        connect(proc, &QProcess::errorOccurred, this,
                [proc, complete](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                complete("ERROR: " + proc->errorString(), false);
        });
        proc->start(program, args);
        timeout->start();
    }

    void togglePower()
    {
        if (!state.controllerAvailable) return;
        const bool newPower = !state.powered;
        powerButton->setEnabled(false);
        runAsync("bluetoothctl", {"power", newPower ? "on" : "off"},
                 [this](const QString &output, bool ok) {
            if (!ok) QMessageBox::warning(this, "Bluetooth power", output);
            refreshDevices();
        });
    }

    void toggleVisible()
    {
        if (!state.powered) return;
        const bool enable = !state.discoverable;
        visibleButton->setEnabled(false);
        auto setVisibility = [this, enable] {
            runAsync("bluetoothctl", {"discoverable", enable ? "on" : "off"},
                     [this](const QString &output, bool ok) {
                if (!ok) QMessageBox::warning(this, "Bluetooth visibility", output);
                refreshDevices();
            });
        };
        if (enable) {
            runAsync("bluetoothctl", {"discoverable-timeout", "60"},
                     [setVisibility](const QString &, bool) { setVisibility(); });
        } else {
            setVisibility();
        }
    }

    void startScan()
    {
        if (scanInProgress || connectInProgress) return;
        if (!state.controllerAvailable || !state.powered) {
            QMessageBox::warning(this, "Bluetooth", state.advice);
            return;
        }
        scanProcess = new QProcess(this);
        QProcess *p = scanProcess;
        p->setProcessChannelMode(QProcess::MergedChannels);
        scanInProgress = true;
        scanButton->setEnabled(false);
        spinFrame = 0;
        scanSpinTimer->start();
        scanRefreshTimer->start();
        // The default scan transport is automatic (Classic + BLE). Keep the
        // scanning D-Bus client alive rather than detaching it and losing errors.
        connect(p, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, [this, p](int, QProcess::ExitStatus) {
            const QString output = cleanOutputBT(QString::fromUtf8(p->readAll()));
            if (scanProcess == p) scanProcess = nullptr;
            p->deleteLater();
            bool wasScanning = scanInProgress;
            scanInProgress = false;
            scanRefreshTimer->stop();
            scanSpinTimer->stop();
            scanButton->setText("Scan");
            updateButtons();
            if (wasScanning && (output.contains("Failed to start discovery", Qt::CaseInsensitive) ||
                                output.contains("No default controller", Qt::CaseInsensitive))) {
                QMessageBox::warning(this, "Bluetooth scan", output);
            }
            refreshDevices();
        });
        connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError error) {
            if (scanProcess != p || error != QProcess::FailedToStart) return;
            scanProcess = nullptr;
            scanInProgress = false;
            scanRefreshTimer->stop();
            scanSpinTimer->stop();
            scanButton->setText("Scan");
            updateButtons();
            QMessageBox::warning(this, "Bluetooth scan",
                                 "Could not start bluetoothctl: " + p->errorString());
            p->deleteLater();
        });
        p->start("bluetoothctl", {"--timeout", "45", "scan", "on"});
        QTimer::singleShot(47000, this, [this, p] {
            if (scanProcess == p && p->state() != QProcess::NotRunning) p->kill();
        });
        renderDevices();
    }

    void stopScan()
    {
        if (scanProcess && scanProcess->state() != QProcess::NotRunning)
            scanProcess->kill();
        // finished signal resets scan buttons and timers.
    }

    void deviceCommand(const QStringList &args, const QString &action, const QString &name)
    {
        if (connectInProgress || args.size() != 2 || !validAddressBT(args.at(1))) return;
        deviceContainer->setEnabled(false);
        runAsync("bluetoothctl", args, [this, action, name](const QString &out, bool ok) {
            if (!ok)
                QMessageBox::warning(this, "Bluetooth", QString("%1 failed for %2.\n%3")
                                     .arg(action, name, out));
            refreshDevices();
        }, 12000);
    }

    void repairAdapter()
    {
        if (connectInProgress) return;
        if (state.hardBlocked) {
            QMessageBox::information(this, "Bluetooth hardware block",
                "The radio is hardware-blocked. Check the device's wireless switch, "
                "Fn key, airplane mode or BIOS/UEFI. Software cannot override this.");
            return;
        }
        if (QMessageBox::question(this, "Repair Bluetooth",
            "Try to unblock Bluetooth, load the standard USB Bluetooth driver "
            "and start the BlueZ service?\n\n"
            "This may require administrator authorisation. It will not install "
            "missing firmware or change BIOS settings.") != QMessageBox::Yes)
            return;

        // All commands are constant: no user/device strings are interpolated.
        // Compatible with systemd and Devuan's SysV/OpenRC service command.
        const QString script =
            "if command -v rfkill >/dev/null 2>&1; then rfkill unblock bluetooth; fi\n"
            "if command -v modprobe >/dev/null 2>&1; then modprobe btusb >/dev/null 2>&1 || :; fi\n"
            "if [ -d /run/systemd/system ] && command -v systemctl >/dev/null 2>&1; then\n"
            "  systemctl start bluetooth.service\n"
            "elif command -v service >/dev/null 2>&1; then\n"
            "  service bluetooth start\n"
            "elif [ -x /etc/init.d/bluetooth ]; then\n"
            "  /etc/init.d/bluetooth start\n"
            "else echo 'No supported Bluetooth service manager found'; exit 1; fi\n";
        QString program = QStandardPaths::findExecutable("pkexec");
        if (program.isEmpty()) {
            QMessageBox::warning(this, "Administrator access unavailable",
                "Automatic recovery requires pkexec and an active polkit authentication agent.\n\n"
                "Alternatively, run as administrator:\n"
                "rfkill unblock bluetooth\nmodprobe btusb\nservice bluetooth start\n\n"
                "If the adapter is still missing, inspect dmesg for firmware errors.");
            return;
        }
        repairButton->setEnabled(false);
        runAsync(program, {"/bin/sh", "-c", script}, [this](const QString &output, bool ok) {
            if (!ok)
                QMessageBox::warning(this, "Bluetooth recovery",
                    "Recovery did not complete.\n\n" + output +
                    "\n\nCheck that a polkit agent is running, then inspect kernel/firmware logs.");
            refreshDevices();
        }, 90000);
    }

    void beginConnection(const BluetoothDevice &dev, QPushButton *button)
    {
        if (connectInProgress || !state.powered || !validAddressBT(dev.mac)) return;
        connectInProgress = true;
        pendingRefresh = false;
        pairMac = dev.mac;
        pairName = dev.name;
        activeDeviceButton = button;
        pairMessage = dev.paired ? "Connecting" : "Pairing";
        pairTranscript.clear();
        recentPairText.clear();
        lastPrompt.clear();
        // Keep active LE advertising reports fresh while preparing the agent.
        // Discovery ends only immediately before initiating the connection.
        updateButtons();
        pairSpinTimer->start();
        pairProcess = new QProcess(this);
        auto *p = pairProcess.data();
        p->setProcessChannelMode(QProcess::MergedChannels);
        pairStage = PairStage::RegisterAgent;

        connect(p, &QProcess::readyRead, this, [this, p] {
            if (pairProcess != p) return;
            handlePairOutput(QString::fromUtf8(p->readAll()));
        });
        connect(p, &QProcess::started, this, [this, p] {
            if (pairProcess != p) return;
            sendPairLine("agent KeyboardDisplay");
            fallbackPairStage(PairStage::RegisterAgent, 4000);
        });
        connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError error) {
            if (pairProcess == p && error == QProcess::FailedToStart) {
                pairTranscript += "\nCould not start bluetoothctl.";
                finishPair();
            }
        });
        connect(p, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                this, [this, p](int, QProcess::ExitStatus) {
            if (pairProcess == p && pairStage != PairStage::Verifying) {
                pairTranscript += "\nBluetooth control process exited unexpectedly.";
                finishPair();
            }
        });
        pairedBefore = dev.paired;
        trustedBefore = dev.trusted;
        p->start("bluetoothctl");
        QTimer::singleShot(60000, this, [this, p] {
            if (pairProcess == p && pairStage != PairStage::Idle &&
                pairStage != PairStage::Verifying) {
                pairTranscript += "\nConnection timed out.";
                finishPair();
            }
        });
    }

    bool pairedBefore = false;
    bool trustedBefore = false;

    void sendPairLine(const QString &line)
    {
        if (pairProcess && pairProcess->state() == QProcess::Running)
            pairProcess->write((line + "\n").toUtf8());
    }

    void fallbackPairStage(PairStage expected, int afterMs)
    {
        QTimer::singleShot(afterMs, this, [this, expected] {
            if (pairStage == expected && pairProcess) advancePairSetup();
        });
    }

    void advancePairSetup()
    {
        switch (pairStage) {
        case PairStage::RegisterAgent:
            pairStage = PairStage::DefaultAgent;
            recentPairText.clear();
            sendPairLine("default-agent");
            fallbackPairStage(PairStage::DefaultAgent, 3500);
            break;
        case PairStage::DefaultAgent:
            pairStage = PairStage::Pairable;
            recentPairText.clear();
            sendPairLine("pairable on");
            fallbackPairStage(PairStage::Pairable, 3500);
            break;
        case PairStage::Pairable:
            stopScan(); // Fresh BLE scan reports are now available for pair/connect.
            if (pairedBefore) {
                if (trustedBefore) {
                    startConnectCommand();
                } else {
                    pairStage = PairStage::Trusting;
                    pairMessage = "Trusting";
                    recentPairText.clear();
                    sendPairLine("trust " + pairMac);
                    fallbackPairStage(PairStage::Trusting, 3500);
                }
            } else {
                pairStage = PairStage::Pairing;
                pairMessage = "Pairing";
                recentPairText.clear();
                sendPairLine("pair " + pairMac);
            }
            break;
        default: break;
        }
    }

    void handlePairOutput(const QString &bytes)
    {
        const QString text = cleanOutputBT(bytes);
        pairTranscript += text;
        if (pairTranscript.size() > 24000) pairTranscript = pairTranscript.right(24000);
        recentPairText += text;
        if (recentPairText.size() > 1600) recentPairText = recentPairText.right(1600);

        // Prompt answers are sent only AFTER the agent actually asks, never
        // blindly as stdin lines. These checks allow prompts without newlines.
        handleAgentPrompt();
        if (pairStage == PairStage::RegisterAgent &&
            (recentPairText.contains("Agent registered", Qt::CaseInsensitive) ||
             recentPairText.contains("Agent is already registered", Qt::CaseInsensitive) ||
             recentPairText.contains("Agent registration failed", Qt::CaseInsensitive))) {
            advancePairSetup();
        } else if (pairStage == PairStage::DefaultAgent &&
                   (recentPairText.contains("Default agent", Qt::CaseInsensitive) ||
                    recentPairText.contains("default agent", Qt::CaseInsensitive))) {
            advancePairSetup();
        } else if (pairStage == PairStage::Pairable &&
                   (recentPairText.contains("pairable on succeeded", Qt::CaseInsensitive) ||
                    recentPairText.contains("pairable on failed", Qt::CaseInsensitive) ||
                    recentPairText.contains("Failed to set pairable", Qt::CaseInsensitive))) {
            advancePairSetup();
        } else if (pairStage == PairStage::Pairing) {
            if (recentPairText.contains("Pairing successful", Qt::CaseInsensitive) ||
                recentPairText.contains("AlreadyExists", Qt::CaseInsensitive) ||
                recentPairText.contains("Already paired", Qt::CaseInsensitive) ||
                recentPairText.contains("Paired: yes", Qt::CaseInsensitive)) {
                pairStage = PairStage::Trusting;
                pairMessage = "Trusting";
                recentPairText.clear();
                sendPairLine("trust " + pairMac);
                fallbackPairStage(PairStage::Trusting, 3500);
            } else if (recentPairText.contains("Failed to pair", Qt::CaseInsensitive)) {
                finishPair();
            }
        } else if (pairStage == PairStage::Trusting) {
            if (recentPairText.contains("trust succeeded", Qt::CaseInsensitive) ||
                recentPairText.contains("Failed to trust", Qt::CaseInsensitive) ||
                recentPairText.contains("trust failed", Qt::CaseInsensitive)) {
                startConnectCommand();
            }
        } else if (pairStage == PairStage::Connecting) {
            if (recentPairText.contains("Connection successful", Qt::CaseInsensitive) ||
                recentPairText.contains("AlreadyConnected", Qt::CaseInsensitive) ||
                recentPairText.contains("Failed to connect", Qt::CaseInsensitive) ||
                recentPairText.contains("not available", Qt::CaseInsensitive)) {
                finishPair();
            }
        }
    }

    void startConnectCommand()
    {
        if (!pairProcess) return;
        pairStage = PairStage::Connecting;
        pairMessage = "Connecting";
        recentPairText.clear();
        sendPairLine("connect " + pairMac);
    }

    void handleAgentPrompt()
    {
        if (!pairProcess || pairStage == PairStage::Verifying) return;
        // Only inspect the last prompt; bluetoothctl frequently redraws its prompt.
        const QString tail = recentPairText.right(550);
        QRegularExpression confirm(QStringLiteral(
            "(?:Confirm passkey|Request confirmation)\\s*([0-9]{4,8})?\\s*\\(yes/no\\)\\s*:") ,
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpression input(QStringLiteral(
            "Enter\\s+(?:PIN code|passkey)[^:]*:") ,
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpression authorize(QStringLiteral(
            "Authorize service[^:]*\\(yes/no\\)\\s*:") ,
            QRegularExpression::CaseInsensitiveOption);
        QRegularExpression display(QStringLiteral(
            "Display\\s+(?:PIN code|passkey)[^:]*:\\s*([0-9]+)"),
            QRegularExpression::CaseInsensitiveOption);
        auto c = confirm.match(tail);
        auto a = authorize.match(tail);
        auto i = input.match(tail);
        auto d = display.match(tail);
        QString prompt;
        if (c.hasMatch()) prompt = c.captured(0);
        else if (a.hasMatch()) prompt = a.captured(0);
        else if (i.hasMatch()) prompt = i.captured(0);
        else if (d.hasMatch()) prompt = d.captured(0);
        if (prompt.isEmpty() || prompt == lastPrompt) return;
        lastPrompt = prompt; // Must happen BEFORE opening a modal dialog.
        if (c.hasMatch()) {
            const QString number = c.captured(1);
            const bool accepted = QMessageBox::question(this, "Confirm Bluetooth pairing",
                number.isEmpty() ? QString("Approve the pairing request from %1?").arg(pairName)
                                 : QString("Does %1 show this same code?\n\n%2")
                                       .arg(pairName, number),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
            sendPairLine(accepted ? "yes" : "no");
        } else if (a.hasMatch()) {
            const bool accepted = QMessageBox::question(this, "Bluetooth service access",
                QString("Allow %1 to use this Bluetooth service?").arg(pairName),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
            sendPairLine(accepted ? "yes" : "no");
        } else if (i.hasMatch()) {
            bool ok = false;
            QString code = QInputDialog::getText(this, "Bluetooth PIN / passkey",
                QString("Enter the PIN or passkey for %1:").arg(pairName),
                QLineEdit::Normal, {}, &ok).trimmed();
            // Only submit user-supplied data to the live agent prompt.
            sendPairLine(ok ? code : QString());
        } else if (d.hasMatch()) {
            QMessageBox::information(this, "Bluetooth PIN",
                QString("Enter this code on %1:\n\n%2").arg(pairName, d.captured(1)));
        }
    }

    void finishPair()
    {
        if (pairStage == PairStage::Idle || pairStage == PairStage::Verifying) return;
        pairStage = PairStage::Verifying;
        if (pairProcess) {
            QProcess *p = pairProcess;
            pairProcess = nullptr;
            p->disconnect(this);
            if (p->state() == QProcess::Running) p->kill();
            p->deleteLater();
        }
        const QString mac = pairMac;
        const QString name = pairName;
        const QString log = pairTranscript;
        runAsync("bluetoothctl", {"info", mac},
                 [this, name, log](const QString &info, bool) {
            const bool connected = yesPropertyBT(info, "Connected");
            const bool paired = yesPropertyBT(info, "Paired");
            pairStage = PairStage::Idle;
            connectInProgress = false;
            pairSpinTimer->stop();
            activeDeviceButton = nullptr;
            updateButtons();
            if (connected) {
                QMessageBox::information(this, "Bluetooth", "Connected to " + name + ".");
            } else if (paired) {
                QMessageBox::information(this, "Bluetooth",
                    name + " is paired, but it isn't currently connected.\n\n"
                    "Some devices connect only when a supported service is used. "
                    "For audio, make sure PipeWire/WirePlumber or the appropriate "
                    "PulseAudio Bluetooth module is installed. You can tap the device to retry.");
            } else {
                QMessageBox::warning(this, "Bluetooth",
                    "Couldn't pair or connect to " + name + ".\n\n" + bluetoothErrorBT(log));
            }
            pendingRefresh = false;
            refreshDevices();
        }, 5000);
    }
};

// Exported factory function expected by osm-settings.
extern "C" QWidget *make_page(QStackedWidget *stack)
{
    return new BluetoothPage(stack);
}
