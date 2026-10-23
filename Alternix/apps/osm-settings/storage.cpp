#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QTimer>
#include <QProcess>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QStackedWidget>
#include <QSlider>
#include <QCoreApplication>
#include <QResizeEvent>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QJsonParseError>
#include <QHash>
#include <functional>

#include <sys/statvfs.h>
#include <unistd.h>

// -----------------------------------------------------
// Helpers
// -----------------------------------------------------
static QString humanSize(qulonglong bytes) {
    double b = bytes;
    const char *u[] = {"B","KB","MB","GB","TB"};
    int i = 0;
    while (b >= 1024 && i < 4) { b /= 1024; i++; }
    return QString::number(b, 'f', (i==0?0:1)) + " " + u[i];
}

// The same desktop-session mount order as osm-files: udisksctl, then pmount.
// Commands always receive arguments separately (never interpolate device names in sh).
static bool runUtility(const QString &program, const QStringList &args,
                       QString &output, int timeoutMs = 15000)
{
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(2000)) {
        output = "Could not start " + program + ". Is it installed?";
        return false;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        output = QString::fromLocal8Bit(p.readAll()) + "\n" + program + " timed out.";
        return false;
    }
    output = QString::fromLocal8Bit(p.readAll());
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

// lsblk is also the mountpoint source for osm-files. It resolves device aliases
// and returns unescaped paths, unlike naive string matching in /proc/mounts.
static QString findMount(const QString &dev)
{
    QString output;
    if (!runUtility("lsblk", {"-n", "-o", "MOUNTPOINT", dev}, output, 3000))
        return QString();
    for (const QString &line : output.split('\n')) {
        QString mp = line.trimmed();
        if (!mp.isEmpty()) return mp;
    }
    return QString();
}

static bool mountDevice(const QString &dev, QString &mountPoint, QString &output)
{
    mountPoint = findMount(dev);
    output.clear();
    if (!mountPoint.isEmpty()) return true;

    QString step;
    const bool udisksOk = runUtility("udisksctl", {"mount", "-b", dev}, step);
    output += step;
    mountPoint = findMount(dev);
    if (mountPoint.isEmpty() && udisksOk) {
        // Same fallback used in osm-files for a just-created udisks mount.
        int at = step.lastIndexOf(" at ");
        if (at >= 0) {
            QString parsed = step.mid(at + 4).trimmed();
            if (parsed.endsWith('.')) parsed.chop(1);
            if (QDir(parsed).exists()) mountPoint = parsed;
        }
    }
    if (!mountPoint.isEmpty()) return true;

    const bool pmountOk = runUtility("pmount", {dev}, step);
    if (!step.trimmed().isEmpty()) output += "\n" + step;
    mountPoint = findMount(dev);
    if (mountPoint.isEmpty() && pmountOk) {
        const QString expected = "/media/" + QFileInfo(dev).fileName();
        if (QDir(expected).exists()) mountPoint = expected;
    }
    return !mountPoint.isEmpty();
}

static bool unmountDevice(const QString &dev, const QString &mountPoint, QString &output)
{
    Q_UNUSED(mountPoint);
    output.clear();
    if (findMount(dev).isEmpty()) return true;

    QString step;
    runUtility("udisksctl", {"unmount", "-b", dev}, step);
    output = step;
    if (findMount(dev).isEmpty()) return true;

    runUtility("pumount", {dev}, step);
    if (!step.trimmed().isEmpty()) output += "\n" + step;
    return findMount(dev).isEmpty();
}

// Match the USB/SD classification from osm-files. A SATA disk named sda must
// not be classified as removable simply because its name begins with 'sd'.
static bool isUsbOrRemovableDisk(const QString &diskName)
{
    const QString sysPath = "/sys/block/" + diskName;
    if (diskName.contains("boot") || diskName.contains("rpmb")) return false;

    if (diskName.startsWith("mmcblk")) {
        QFile type(sysPath + "/device/type");
        if (type.open(QIODevice::ReadOnly))
            return QString::fromLatin1(type.readAll()).trimmed()
                       .compare("SD", Qt::CaseInsensitive) == 0;
        return QFile::exists(sysPath + "/device/scr");
    }

    QFile flag(sysPath + "/removable");
    if (flag.open(QIODevice::ReadOnly) && flag.readAll().trimmed() == "1")
        return true;

    return QFileInfo(sysPath).canonicalFilePath().contains("/usb");
}

struct StorageVolume {
    QString dev, label, size, fstype, mountPoint;
};

struct StorageDisk {
    QString dev, size;
    bool removable = false;
    QVector<StorageVolume> volumes;
};

static bool readStorageDisks(QVector<StorageDisk> &disks, QString &rootDisk,
                             QString &rootPartition, QString &error)
{
    QString output;
    if (!runUtility("lsblk", {"-J", "-o", "NAME,SIZE,LABEL,MOUNTPOINT,TYPE,FSTYPE"},
                    output, 5000)) {
        error = "Unable to list drives with lsblk:\n" + output;
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(output.toUtf8(), &parseError);
    if (!json.isObject() || parseError.error != QJsonParseError::NoError) {
        error = "Unable to read lsblk JSON: " + parseError.errorString();
        return false;
    }

    disks.clear();
    rootDisk.clear();
    rootPartition.clear();
    const QJsonArray blockdevices = json.object().value("blockdevices").toArray();
    for (const QJsonValue &entry : blockdevices) {
        const QJsonObject diskJson = entry.toObject();
        if (diskJson.value("type").toString() != "disk") continue;
        const QString name = diskJson.value("name").toString();
        if (name.isEmpty() || name.contains("boot") || name.contains("rpmb")) continue;

        StorageDisk disk;
        disk.dev = "/dev/" + name;
        disk.size = diskJson.value("size").toString();
        disk.removable = isUsbOrRemovableDisk(name);

        std::function<void(const QJsonObject&)> collect = [&](const QJsonObject &node) {
            const QString nodeName = node.value("name").toString();
            const QString fs = node.value("fstype").toString();
            const QString mp = node.value("mountpoint").toString();
            const QString type = node.value("type").toString();
            const QString devicePath = "/dev/" + nodeName;

            if (mp == "/") {
                rootDisk = disk.dev;
                rootPartition = devicePath;
            }

            // Mirrors osm-files: only expose mountable filesystems, not raw
            // partition table containers, swap, or eMMC boot/RPMB devices.
            if (!nodeName.isEmpty() && !fs.isEmpty() && fs.compare("swap", Qt::CaseInsensitive) != 0 &&
                !fs.startsWith("crypto", Qt::CaseInsensitive) &&
                fs != "LVM2_member" && fs != "linux_raid_member" &&
                type != "crypto" && !nodeName.contains("boot0") &&
                !nodeName.contains("boot1") && !nodeName.contains("rpmb")) {
                StorageVolume volume;
                volume.dev = devicePath;
                volume.label = node.value("label").toString();
                volume.size = node.value("size").toString();
                volume.fstype = fs;
                volume.mountPoint = mp;
                disk.volumes.append(volume);
            }

            for (const QJsonValue &child : node.value("children").toArray())
                collect(child.toObject());
        };
        collect(diskJson); // also supports a filesystem directly on a whole disk
        disks.append(disk);
    }
    return true;
}

static bool protectedSystemMount(const QString &mountPoint)
{
    if (mountPoint == "/") return true;
    static const QStringList protectedPaths = {
        "/boot", "/home", "/usr", "/var", "/etc", "/opt", "/srv",
        "/run", "/proc", "/sys", "/dev", "/tmp"
    };
    for (const QString &path : protectedPaths)
        if (mountPoint == path || mountPoint.startsWith(path + "/")) return true;
    return false;
}

static bool getSpace(const QString &mp, qulonglong &total, qulonglong &free)
{
    struct statvfs st;
    if (statvfs(mp.toUtf8().constData(), &st) != 0)
        return false;
    total = (qulonglong)st.f_blocks * st.f_frsize;
    free  = (qulonglong)st.f_bavail * st.f_frsize;
    return true;
}

// -----------------------------------------------------
// Styles
// -----------------------------------------------------
static QString altBtnStyle(const QString &txtColor)
{
    return QString(
        "QPushButton {"
        " background:#444444;"
        " color:%1;"
        " border:1px solid #222222;"
        " border-radius:14px;"
        " font-size:20px;"
        " font-weight:bold;"
        " padding:4px 10px;"
        "}"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
    ).arg(txtColor);
}

static QPushButton* makeBtn(const QString &txt, const QString &color = "white")
{
    QPushButton *b = new QPushButton(txt);
    b->setStyleSheet(altBtnStyle(color));
    b->setMinimumSize(110, 48);
    b->setMaximumWidth(150);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

// -----------------------------------------------------
// Data structs
// -----------------------------------------------------
struct PartitionCard {
    QString dev;
    QString mountPoint;

    QFrame *frame = nullptr;
    QLabel *info  = nullptr;

    QPushButton *btnMount = nullptr;
    QPushButton *btnOpen  = nullptr;
    QPushButton *btnEject = nullptr;
};

struct DeviceCard {
    QString dev;
    QString type;
    QFrame *frame = nullptr;
    QLabel *space = nullptr;
    QVector<PartitionCard*> parts;
};

// -----------------------------------------------------
// StoragePage
// -----------------------------------------------------
class StoragePage : public QWidget
{
public:
    explicit StoragePage(QStackedWidget *stack)
        : QWidget(stack), m_stack(stack)
    {
        setStyleSheet("background:#282828; color:white; font-family:Sans;");
        setMinimumWidth(720);

        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(30, 30, 30, 30);
        root->setSpacing(10);

        QLabel *title = new QLabel("Storage");
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:40px; font-weight:bold;");
        root->addWidget(title);

        // Scroll
        m_scroll = new QScrollArea(this);
        m_scroll->setWidgetResizable(true);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(m_scroll->viewport(), QScroller::LeftMouseButtonGesture);

        m_wrap = new QWidget(m_scroll);
        QVBoxLayout *wrapLay = new QVBoxLayout(m_wrap);
        wrapLay->setSpacing(10);
        wrapLay->setContentsMargins(0,0,0,0);

        // Outer card
        m_outer = new QFrame(m_wrap);
        m_outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:32px; }");
        QVBoxLayout *outerLay = new QVBoxLayout(m_outer);
        outerLay->setContentsMargins(20, 20, 20, 20);
        outerLay->setSpacing(20);

        // Internal Storage Card
        QFrame *intCard = new QFrame(m_outer);
        intCard->setStyleSheet("QFrame { background:#444444; border-radius:26px; }");
        QVBoxLayout *intLay = new QVBoxLayout(intCard);
        intLay->setContentsMargins(16, 20, 16, 20);
        intLay->setSpacing(12);

        m_internalTitle = new QLabel("Internal Storage");
        m_internalTitle->setAlignment(Qt::AlignCenter);
        m_internalTitle->setStyleSheet("font-size:24px; font-weight:bold;");
        intLay->addWidget(m_internalTitle);

        m_usage = new QSlider(Qt::Horizontal, intCard);
        m_usage->setEnabled(false);
        m_usage->setRange(0, 100);
        m_usage->setFixedHeight(26);
        m_usage->setStyleSheet(
            "QSlider::groove:horizontal {"
            " height:20px; margin:3px 8px;"
            " background:#2a2a2a; border-radius:10px;"
            "}"
            "QSlider::sub-page:horizontal {"
            " background:#4da3ff; border-radius:10px;"
            "}"
            "QSlider::add-page:horizontal {"
            " background:#2a2a2a; border-radius:10px;"
            "}"
            "QSlider::handle:horizontal {"
            " background:transparent; width:0px;"
            "}"
        );
        intLay->addWidget(m_usage);

        m_info = new QLabel("...");
        m_info->setAlignment(Qt::AlignCenter);
        m_info->setStyleSheet(
            "QLabel { background:#383838; border-radius:14px;"
            " font-size:22px; padding:8px 20px; }"
        );
        intLay->addWidget(m_info);

        m_internalGrid = new QGridLayout();
        m_internalGrid->setSpacing(12);
        m_internalGrid->setContentsMargins(0,8,0,0);
        intLay->addLayout(m_internalGrid);

        QPushButton *openInt = makeBtn("Open");
        intLay->addWidget(openInt, 0, Qt::AlignCenter);
        connect(openInt, &QPushButton::clicked, this, [this]() {
            launchFiles("/");
        });

        outerLay->addWidget(intCard);

        // Devices
        m_devicesWidget = new QWidget(m_outer);
        m_devicesLayout = new QVBoxLayout(m_devicesWidget);
        m_devicesLayout->setSpacing(16);
        m_devicesLayout->setContentsMargins(0,8,0,0);
        outerLay->addWidget(m_devicesWidget);

        wrapLay->addWidget(m_outer);
        wrapLay->addStretch();

        m_scroll->setWidget(m_wrap);
        root->addWidget(m_scroll);

        // Back
        QPushButton *back = makeBtn("❮");
        back->setFixedSize(130, 54);
        connect(back, &QPushButton::clicked, this, [this]() {
            if (m_stack) m_stack->setCurrentIndex(0);
        });
        root->addWidget(back, 0, Qt::AlignCenter);

        m_timer = new QTimer(this);
        m_timer->setInterval(4000);
        connect(m_timer, &QTimer::timeout, this, &StoragePage::refreshAll);

        if (m_stack) {
            connect(m_stack, &QStackedWidget::currentChanged, this, [this](int idx){
                if (m_stack->widget(idx) == this) {
                    refreshAll();
                    m_timer->start();
                } else {
                    m_timer->stop();
                }
            });
            if (m_stack->currentWidget() == this)
                m_timer->start();
        }

        refreshAll();
    }

protected:
    void resizeEvent(QResizeEvent *ev) override
    {
        QWidget::resizeEvent(ev);
        refreshAll();
    }

private:
    QStackedWidget *m_stack = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_wrap = nullptr;
    QFrame  *m_outer = nullptr;

    QLabel  *m_internalTitle = nullptr;
    QSlider *m_usage = nullptr;
    QLabel  *m_info  = nullptr;

    QGridLayout *m_internalGrid = nullptr;
    QVector<PartitionCard*> m_internalParts;

    QWidget *m_devicesWidget = nullptr;
    QVBoxLayout *m_devicesLayout = nullptr;
    QVector<DeviceCard*> m_deviceCards;

    QTimer *m_timer = nullptr;

    QString m_rootDev;
    QString m_rootBase;
    QVector<StorageDisk> m_disks;

    // -----------------------------------------------------
    // Width helper
    // -----------------------------------------------------
    int effectiveWidth() const
    {
        int w = width();
        if (w < 720)
            w = 720;
        return w - 90;   // reduced margins for 720px fit
    }

    // -----------------------------------------------------
    // DEV scanning
    // -----------------------------------------------------
    QStringList scanPartitions(const QString &baseDevPath) const
    {
        QStringList parts;
        for (const StorageDisk &disk : m_disks) {
            if (disk.dev != baseDevPath) continue;
            for (const StorageVolume &volume : disk.volumes)
                parts << volume.dev;
            break;
        }
        return parts;
    }

    const StorageVolume *volumeFor(const QString &device) const
    {
        for (const StorageDisk &disk : m_disks)
            for (const StorageVolume &volume : disk.volumes)
                if (volume.dev == device) return &volume;
        return nullptr;
    }

    const StorageDisk *diskFor(const QString &device) const
    {
        for (const StorageDisk &disk : m_disks)
            if (disk.dev == device) return &disk;
        return nullptr;
    }

    void launchFiles(const QString &path)
    {
        if (!QDir(path).exists()) {
            QMessageBox::warning(this, "Open drive", "The folder is no longer available:\n" + path);
            return;
        }
        if (!QProcess::startDetached("osm-files", QStringList() << path)) {
            QMessageBox::warning(this, "Open drive",
                                 "Could not start osm-files. Make sure it is installed and on PATH.");
        }
    }

    // A mounted drive opens immediately. On an unmounted drive, Open mounts it
    // using the same path as the Mount button, then launches osm-files there.
    void openVolume(const QString &device)
    {
        QString mp = findMount(device);
        if (mp.isEmpty()) {
            QString output;
            if (!mountDevice(device, mp, output)) {
                QMessageBox::warning(this, "Mount failed",
                    "Could not mount:\n" + device + "\n\n" + output.trimmed() +
                    "\n\nInstall udisks2 (with elogind) or pmount, and check your "
                    "desktop session's mounting permissions.");
                refreshAll();
                return;
            }
        }
        launchFiles(mp);
        refreshAll();
    }

    void toggleMount(const QString &device)
    {
        const QString mp = findMount(device);
        QString output;
        if (mp.isEmpty()) {
            QString newMountPoint;
            if (!mountDevice(device, newMountPoint, output))
                QMessageBox::warning(this, "Mount failed",
                    "Could not mount:\n" + device + "\n\n" + output.trimmed() +
                    "\n\nInstall udisks2 (with elogind) or pmount, and check your "
                    "desktop session's mounting permissions.");
        } else if (!protectedSystemMount(mp)) {
            if (!unmountDevice(device, mp, output))
                QMessageBox::warning(this, "Unmount failed",
                    "Could not unmount:\n" + mp + "\n\n" + output.trimmed() +
                    "\n\nClose files or applications using this drive and try again.");
        }
        refreshAll();
    }

    void ejectDisk(const QString &device)
    {
        const StorageDisk *disk = diskFor(device);
        if (!disk || !disk->removable) return;
        const QStringList partitions = scanPartitions(device);
        QString errors;
        for (const QString &partition : partitions) {
            const QString mp = findMount(partition);
            if (mp.isEmpty()) continue;
            if (protectedSystemMount(mp)) {
                errors += "System mount cannot be ejected: " + mp + "\n";
                continue;
            }
            QString output;
            if (!unmountDevice(partition, mp, output))
                errors += partition + ": " + output.trimmed() + "\n";
        }
        // Power off only after every filesystem is confirmed unmounted.
        if (errors.isEmpty()) {
            bool stillMounted = false;
            for (const QString &partition : partitions)
                if (!findMount(partition).isEmpty()) stillMounted = true;
            if (!stillMounted) {
                QString ignored;
                runUtility("udisksctl", {"power-off", "-b", device}, ignored, 8000);
            }
        }
        if (!errors.isEmpty())
            QMessageBox::warning(this, "Eject failed", errors.trimmed());
        refreshAll();
    }

    // -----------------------------------------------------
    // Clear helpers
    // -----------------------------------------------------
    void clearInternalParts()
    {
        for (PartitionCard *pc : m_internalParts) {
            if (pc->frame) pc->frame->deleteLater();
            delete pc;
        }
        m_internalParts.clear();

        QLayoutItem *it;
        while ((it = m_internalGrid->takeAt(0)) != nullptr) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
    }

    void clearDeviceCards()
    {
        for (DeviceCard *dc : m_deviceCards) {
            for (PartitionCard *pc : dc->parts) {
                if (pc->frame) pc->frame->deleteLater();
                delete pc;
            }
            if (dc->frame) dc->frame->deleteLater();
            delete dc;
        }
        m_deviceCards.clear();

        QLayoutItem *it;
        while ((it = m_devicesLayout->takeAt(0)) != nullptr) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
    }

    // -----------------------------------------------------
    // Partition Card
    // -----------------------------------------------------
    PartitionCard* createPartitionCard(const QString &dev, bool internal)
    {
        PartitionCard *pc = new PartitionCard();
        pc->dev = dev;

        QFrame *frame = new QFrame();
        frame->setStyleSheet("QFrame { background:#555555; border-radius:18px; }");
        frame->setMinimumWidth(200);
        frame->setMaximumWidth(240);

        QVBoxLayout *lay = new QVBoxLayout(frame);
        lay->setContentsMargins(8, 12, 8, 12);
        lay->setSpacing(8);

        QLabel *lbl = new QLabel(QFileInfo(dev).fileName());
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet("font-size:22px;");
        lay->addWidget(lbl);

        QLabel *info = new QLabel("...");
        info->setAlignment(Qt::AlignCenter);
        info->setStyleSheet("font-size:18px;");
        info->setWordWrap(true);
        lay->addWidget(info);

        QHBoxLayout *btnRow = new QHBoxLayout();
        btnRow->setSpacing(6);

        pc->btnMount = makeBtn("Mount");
        pc->btnOpen  = makeBtn("Open");

        btnRow->addWidget(pc->btnMount);
        btnRow->addWidget(pc->btnOpen);

        if (!internal) {
            pc->btnEject = makeBtn("Eject", "#CC6666");
            btnRow->addWidget(pc->btnEject);
        }

        lay->addLayout(btnRow);

        pc->frame = frame;
        pc->info  = info;
        return pc;
    }

    // -----------------------------------------------------
    // Update partition card
    // -----------------------------------------------------
    void updatePartitionCard(PartitionCard *pc, bool internalRoot)
    {
        const StorageVolume *volume = volumeFor(pc->dev);
        const QString mp = volume ? volume->mountPoint : findMount(pc->dev);
        pc->mountPoint = mp;
        if (internalRoot) {
            qulonglong total = 0, free = 0;
            getSpace("/", total, free);
            pc->mountPoint = "/";
            pc->info->setText(
                QString("<span style='color:#7CFC00;'>Mounted at /</span><br>"
                        "%1 free / %2 total")
                        .arg(humanSize(free), humanSize(total)));
            if (pc->btnMount) pc->btnMount->hide();
            if (pc->btnOpen) pc->btnOpen->hide(); // use Internal Storage Open
            if (pc->btnEject) pc->btnEject->hide();
            return;
        }

        const QString detail = volume
            ? QString("%1%2 (%3)").arg(
                  volume->label.isEmpty() ? "" : volume->label.toHtmlEscaped() + "<br>",
                  volume->size.toHtmlEscaped(), volume->fstype.toHtmlEscaped())
            : QString();
        if (mp.isEmpty()) {
            pc->info->setText(detail + "<br>Not mounted");
            pc->btnMount->setText("Mount");
            pc->btnMount->setStyleSheet(altBtnStyle("white"));
        } else {
            qulonglong total = 0, free = 0;
            const bool hasSpace = getSpace(mp, total, free);
            pc->info->setText(
                QString("<span style='color:#7CFC00;'>Mounted</span><br>%1<br>%2")
                    .arg(mp.toHtmlEscaped(),
                         hasSpace ? QString("%1 free / %2 total")
                                        .arg(humanSize(free), humanSize(total))
                                  : detail));
            pc->btnMount->setText("Unmount");
            pc->btnMount->setStyleSheet(altBtnStyle("#CC6666"));
        }
        // Never let Storage tear down critical OS mount points. All other
        // mounted and unmounted filesystems retain a working Open button.
        pc->btnMount->setVisible(mp.isEmpty() || !protectedSystemMount(mp));
        pc->btnOpen->show();
        if (pc->btnEject && protectedSystemMount(mp)) pc->btnEject->hide();
    }

    // -----------------------------------------------------
    // Internal storage grid
    // -----------------------------------------------------
    void updateInternalUsage()
    {
        qulonglong total=0, free=0;
        if (!getSpace("/", total, free))
            return;

        qulonglong used = total - free;
        int pct = (total > 0) ? (int)((used * 100) / total) : 0;
        m_usage->setValue(pct);

        m_info->setText(
            humanSize(total) + "\n" +
            humanSize(free) + " free"
        );
    }

    void buildInternalGrid()
    {
        clearInternalParts();
        if (m_rootBase.isEmpty()) return;

        const QString baseDevPath = "/dev/" + m_rootBase;
        const QStringList parts = scanPartitions(baseDevPath);
        const int cols = qMax(2, effectiveWidth() / 225);
        int row = 0, col = 0;

        for (const QString &partition : parts) {
            const bool root = (partition == m_rootDev);
            PartitionCard *pc = createPartitionCard(partition, true);
            updatePartitionCard(pc, root);
            if (!root) {
                connect(pc->btnMount, &QPushButton::clicked, this,
                        [this, partition]() { toggleMount(partition); });
                connect(pc->btnOpen, &QPushButton::clicked, this,
                        [this, partition]() { openVolume(partition); });
            }
            m_internalGrid->addWidget(pc->frame, row, col);
            m_internalParts.append(pc);
            if (++col >= cols) { col = 0; ++row; }
        }
    }

    // -----------------------------------------------------
    // External devices
    // -----------------------------------------------------
    void buildDeviceCards()
    {
        clearDeviceCards();
        bool anyDisks = false;
        for (const StorageDisk &disk : m_disks) {
            const QString dev = disk.dev;
            const QString base = QFileInfo(dev).fileName();
            if (!m_rootBase.isEmpty() && base == m_rootBase) continue;
            anyDisks = true;

            DeviceCard *dc = new DeviceCard();
            dc->dev = dev;
            if (!disk.removable) dc->type = "Internal Drive";
            else if (base.startsWith("mmcblk")) dc->type = "SD Card";
            else if (QFileInfo("/sys/block/" + base).canonicalFilePath().contains("/usb"))
                dc->type = "USB Drive";
            else dc->type = "Removable Drive";

            QFrame *card = new QFrame();
            card->setStyleSheet("QFrame { background:#3a3a3a; border-radius:26px; }");
            QVBoxLayout *devLay = new QVBoxLayout(card);
            devLay->setContentsMargins(24, 20, 24, 20);
            devLay->setSpacing(14);
            QLabel *title = new QLabel(dc->type + ": " + base);
            title->setAlignment(Qt::AlignCenter);
            title->setStyleSheet("font-size:26px; font-weight:bold;");
            devLay->addWidget(title);

            QLabel *space = new QLabel("Not mounted — " + disk.size);
            space->setAlignment(Qt::AlignCenter);
            space->setWordWrap(true);
            space->setStyleSheet("font-size:18px;");
            devLay->addWidget(space);
            dc->space = space;

            QGridLayout *grid = new QGridLayout();
            grid->setSpacing(14);
            grid->setContentsMargins(0, 0, 0, 0);
            const int cols = qMax(2, effectiveWidth() / 225);
            int row = 0, col = 0;
            bool anyMounted = false;

            for (const StorageVolume &volume : disk.volumes) {
                const QString partition = volume.dev;
                PartitionCard *pc = createPartitionCard(partition, !disk.removable);
                updatePartitionCard(pc, partition == m_rootDev);
                if (!pc->mountPoint.isEmpty()) anyMounted = true;

                connect(pc->btnMount, &QPushButton::clicked, this,
                        [this, partition]() { toggleMount(partition); });
                connect(pc->btnOpen, &QPushButton::clicked, this,
                        [this, partition]() { openVolume(partition); });

                if (pc->btnEject) {
                    connect(pc->btnEject, &QPushButton::clicked, this,
                            [this, dev]() { ejectDisk(dev); });
                }
                grid->addWidget(pc->frame, row, col);
                dc->parts.append(pc);
                if (++col >= cols) { col = 0; ++row; }
            }

            if (disk.volumes.isEmpty()) {
                QLabel *noFs = new QLabel("No mountable filesystem found");
                noFs->setAlignment(Qt::AlignCenter);
                noFs->setStyleSheet("font-size:18px;");
                devLay->addWidget(noFs);
            } else if (anyMounted) {
                for (PartitionCard *pc : dc->parts) {
                    if (pc->mountPoint.isEmpty()) continue;
                    qulonglong total = 0, free = 0;
                    if (getSpace(pc->mountPoint, total, free)) {
                        space->setText(QString("<span style='color:#7CFC00;'>Mounted</span> — "
                                               "%1 free / %2 total")
                                           .arg(humanSize(free), humanSize(total)));
                    } else {
                        space->setText("Mounted");
                    }
                    break;
                }
            }
            devLay->addLayout(grid);
            m_devicesLayout->addWidget(card);
            dc->frame = card;
            m_deviceCards.append(dc);
        }
        if (!anyDisks) {
            QLabel *none = new QLabel("No additional storage devices found");
            none->setAlignment(Qt::AlignCenter);
            none->setStyleSheet("font-size:24px;");
            m_devicesLayout->addWidget(none);
        }
        m_devicesLayout->addStretch();
    }

    // -----------------------------------------------------
    // Master refresh
    // -----------------------------------------------------
    void refreshAll()
    {
        QVector<StorageDisk> disks;
        QString rootDisk, rootPartition, error;
        if (!readStorageDisks(disks, rootDisk, rootPartition, error)) {
            m_info->setText("Storage scan failed: " + error.toHtmlEscaped());
            return;
        }

        m_disks = disks;
        m_rootDev = rootPartition;
        m_rootBase = rootDisk.isEmpty() ? QString() : QFileInfo(rootDisk).fileName();
        m_internalTitle->setText(m_rootBase.isEmpty()
            ? "Internal Storage" : "Internal Storage (" + m_rootBase + ")");
        updateInternalUsage();
        buildInternalGrid();
        buildDeviceCards();
    }

};

// -----------------------------------------------------
// Factory
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new StoragePage(stack);
}
