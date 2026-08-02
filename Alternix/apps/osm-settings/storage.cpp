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
#include <QTextStream>
#include <QSlider>
#include <QCoreApplication>
#include <QResizeEvent>
#include <QMessageBox>

#include <sys/statvfs.h>
#include <dirent.h>
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

static bool runCmdWithOutput(const QString &cmd, QString &output)
{
    QProcess p;
    p.start("/bin/sh", {"-c", cmd});
    if (!p.waitForFinished())
        return false;
    output = QString::fromLocal8Bit(p.readAllStandardOutput())
           + QString::fromLocal8Bit(p.readAllStandardError());
    return (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
}

static QString findMount(const QString &dev)
{
    QFile f("/proc/mounts");
    if (!f.open(QIODevice::ReadOnly))
        return "";
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine();
        QStringList parts = line.split(' ');
        if (parts.size() < 2)
            continue;
        if (parts[0] == dev)
            return parts[1];
    }
    return "";
}

// Mount a removable partition the way a desktop session does.
//
// Plain "mount" is not permitted for an ordinary user, and "sudo mount" on its
// own leaves FAT/exFAT volumes owned by root, so the user cannot write to their
// own SD card or USB stick. Try the privileged helpers first (udisks2 through
// polkit, then pmount) and only fall back to sudo, passing the ownership
// options that FAT-family filesystems need.
//
// The mount point is always read back from /proc/mounts rather than assumed,
// because udisks2 and pmount both choose it themselves.
static bool mountDevice(const QString &dev, QString &mountPoint, QString &output)
{
    output.clear();
    mountPoint.clear();

    QString out;

    // 1) udisks2 + polkit: mounts to /run/media/<user>/<label>
    runCmdWithOutput(QString("udisksctl mount -b %1").arg(dev), out);
    output += out;
    mountPoint = findMount(dev);
    if (!mountPoint.isEmpty()) return true;

    // 2) pmount: mounts under /media as the calling user
    runCmdWithOutput(QString("pmount %1").arg(dev), out);
    output += out;
    mountPoint = findMount(dev);
    if (!mountPoint.isEmpty()) return true;

    // 3) sudo: an ordinary user cannot create a directory in /media, so the
    //    mount point has to be made with sudo as well
    const QString tgt = "/media/" + QFileInfo(dev).fileName();

    QString fsType;
    runCmdWithOutput(QString("lsblk -no FSTYPE %1").arg(dev), fsType);
    fsType = fsType.trimmed();

    QString opts;
    if (fsType == "vfat" || fsType == "msdos" || fsType == "exfat" ||
        fsType == "ntfs" || fsType == "ntfs3") {
        opts = QString("-o uid=%1,gid=%2,umask=0022 ")
                   .arg((uint)getuid())
                   .arg((uint)getgid());
    }

    const QString mountCmd =
        "mkdir -p '" + tgt + "' && mount " + opts + dev + " '" + tgt + "'";

    // -n first: without a terminal, a missing NOPASSWD rule must fail straight
    // away instead of blocking on a password prompt nobody can answer
    runCmdWithOutput("sudo -n sh -c \"" + mountCmd + "\"", out);
    output += out;
    mountPoint = findMount(dev);
    if (!mountPoint.isEmpty()) return true;

    if (qEnvironmentVariableIsSet("SUDO_ASKPASS")) {
        runCmdWithOutput("sudo -A sh -c \"" + mountCmd + "\"", out);
        output += out;
        mountPoint = findMount(dev);
        if (!mountPoint.isEmpty()) return true;
    }

    return false;
}

// Unmount a removable partition, using the same escalation ladder as
// mountDevice. Success is confirmed against /proc/mounts, not exit codes,
// because udisksctl reports an error for a volume it did not mount itself.
static bool unmountDevice(const QString &dev, const QString &mountPoint, QString &output)
{
    output.clear();

    QString out;

    runCmdWithOutput(QString("udisksctl unmount -b %1").arg(dev), out);
    output += out;
    if (findMount(dev).isEmpty()) return true;

    runCmdWithOutput(QString("pumount %1").arg(dev), out);
    output += out;
    if (findMount(dev).isEmpty()) return true;

    const QString target = mountPoint.isEmpty() ? dev : mountPoint;

    runCmdWithOutput("umount '" + target + "'", out);
    output += out;
    if (findMount(dev).isEmpty()) return true;

    runCmdWithOutput("sudo -n umount '" + target + "'", out);
    output += out;
    if (findMount(dev).isEmpty()) return true;

    if (qEnvironmentVariableIsSet("SUDO_ASKPASS")) {
        runCmdWithOutput("sudo -A umount '" + target + "'", out);
        output += out;
        if (findMount(dev).isEmpty()) return true;
    }

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

static qulonglong partBytes(const QString &dev)
{
    QFile f("/sys/class/block/" + QFileInfo(dev).fileName() + "/size");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    bool ok = false;
    qulonglong sectors = f.readAll().trimmed().toULongLong(&ok);
    return ok ? sectors * 512ULL : 0;
}

// root device for "/"
static QString detectRootDev()
{
    QFile f("/proc/mounts");
    if (!f.open(QIODevice::ReadOnly))
        return "";
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine();
        QStringList parts = line.split(' ');
        if (parts.size() < 2)
            continue;
        if (parts[1] == "/")
            return parts[0];
    }
    return "";
}

// base device name from partition path
static QString baseDeviceName(const QString &devPath)
{
    QString name = QFileInfo(devPath).fileName();

    if (name.startsWith("mmcblk")) {
        int idx = name.indexOf('p');
        if (idx > 0)
            return name.left(idx);
        return name;
    }

    if (name.startsWith("sd") && name.size() >= 3)
        return name.left(3);

    return name;
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
        connect(openInt, &QPushButton::clicked, this, []() {
            QProcess::startDetached("osm-files", {"/"});
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
                if (m_stack->widget(idx) == this)
                    m_timer->start();
                else
                    m_timer->stop();
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
    QStringList scanBaseDevices()
    {
        QStringList devs;
        DIR *d = opendir("/dev");
        if (!d) return devs;

        while (auto *e = readdir(d)) {
            QString n = e->d_name;

            // eMMC boot0/boot1/rpmb are not user storage, only mmcblkN is
            if (n.startsWith("mmcblk") && !n.contains('p')
                && !n.contains("boot") && !n.contains("rpmb"))
                devs << "/dev/" + n;

            if (n.size() == 3 && n.startsWith("sd"))
                devs << "/dev/" + n;
        }
        closedir(d);
        devs.sort();
        return devs;
    }

    QStringList scanPartitions(const QString &baseDevPath)
    {
        QString baseName = QFileInfo(baseDevPath).fileName();
        QStringList parts;
        DIR *d = opendir("/dev");
        if (!d) return parts;

        while (auto *e = readdir(d)) {
            QString n = e->d_name;
            // eMMC boot0/boot1/rpmb areas are not usable partitions
            if (n.contains("boot") || n.contains("rpmb"))
                continue;
            if (n != baseName && n.startsWith(baseName))
                parts << "/dev/" + n;
        }
        closedir(d);
        parts.sort();
        return parts;
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
        if (internalRoot) {
            qulonglong total=0, free=0;
            getSpace("/", total, free);
            pc->mountPoint = "/";

            pc->info->setText(
                QString("<span style='color:#7CFC00;'>Mounted at /</span><br>"
                        "%1 free / %2 total")
                .arg(humanSize(free))
                .arg(humanSize(total))
            );

            if (pc->btnMount) pc->btnMount->hide();
            if (pc->btnOpen)  pc->btnOpen->hide();
            if (pc->btnEject) pc->btnEject->hide();
            return;
        }

        QString mp = findMount(pc->dev);
        pc->mountPoint = mp;

        if (mp.isEmpty()) {
            qulonglong size = partBytes(pc->dev);
            pc->info->setText(QString("Size: %1\nNot mounted").arg(humanSize(size)));
            pc->btnMount->setText("Mount");
            pc->btnMount->setStyleSheet(altBtnStyle("white"));
            pc->btnOpen->hide();
        } else {
            qulonglong t=0, f=0;
            getSpace(mp, t, f);
            pc->info->setText(
                QString("<span style='color:#7CFC00;'>Mounted</span><br>"
                        "%1 free / %2 total")
                .arg(humanSize(f))
                .arg(humanSize(t))
            );
            pc->btnMount->setText("Unmount");
            pc->btnMount->setStyleSheet(altBtnStyle("#CC6666"));
            pc->btnOpen->show();
        }
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

        if (m_rootBase.isEmpty())
            return;

        QString baseDevPath = "/dev/" + m_rootBase;
        QStringList parts = scanPartitions(baseDevPath);

        if (parts.isEmpty() && !m_rootDev.isEmpty())
            parts << m_rootDev;

        if (parts.isEmpty())
            return;

        PartitionCard *tmp = createPartitionCard(parts.first(), true);
        tmp->frame->adjustSize();
        int cardW = tmp->frame->sizeHint().width();
        if (cardW < 200) cardW = 200;
        tmp->frame->deleteLater();
        delete tmp;

        int avail = effectiveWidth();
        int cols = qMax(2, avail / cardW);

        int row = 0;
        int col = 0;

        for (const QString &p : parts) {
            bool isRootPart = (!m_rootDev.isEmpty() && p == m_rootDev);

            PartitionCard *pc = createPartitionCard(p, true);
            updatePartitionCard(pc, isRootPart);

            if (!isRootPart) {
                if (pc->btnMount) {
                    connect(pc->btnMount, &QPushButton::clicked, this, [this, pc]() {
                        if (pc->mountPoint.isEmpty()) {
                            QString mp, out;
                            if (!mountDevice(pc->dev, mp, out)) {
                                QMessageBox::warning(
                                    this, "Mount failed",
                                    "Could not mount:\n" + pc->dev + "\n\n" + out.trimmed() +
                                    "\n\nInstall udisks2 or pmount, and make sure this "
                                    "user is in the 'plugdev' group."
                                );
                            }
                        } else {
                            QString out;
                            if (!unmountDevice(pc->dev, pc->mountPoint, out)) {
                                QMessageBox::warning(
                                    this, "Unmount failed",
                                    "Could not unmount:\n" + pc->mountPoint + "\n\n" + out.trimmed() +
                                    "\n\nMake sure no files on the drive are open."
                                );
                            }
                        }
                        QCoreApplication::processEvents();
                        refreshAll();
                    });
                }
                if (pc->btnOpen) {
                    connect(pc->btnOpen, &QPushButton::clicked, this, [pc]() {
                        if (!pc->mountPoint.isEmpty())
                            QProcess::startDetached("osm-files", {pc->mountPoint});
                    });
                }
            }

            m_internalGrid->addWidget(pc->frame, row, col);
            m_internalParts.append(pc);

            col++;
            if (col >= cols) {
                col = 0;
                row++;
            }
        }
    }

    // -----------------------------------------------------
    // External devices
    // -----------------------------------------------------
    void buildDeviceCards()
    {
        clearDeviceCards();

        QStringList devs = scanBaseDevices();
        if (devs.isEmpty()) {
            QLabel *none = new QLabel("No storage devices found");
            none->setAlignment(Qt::AlignCenter);
            none->setStyleSheet("font-size:24px;");
            m_devicesLayout->addWidget(none);
            m_devicesLayout->addStretch();
            return;
        }

        for (const QString &dev : devs) {
            QString base = QFileInfo(dev).fileName();

            if (!m_rootBase.isEmpty() && base == m_rootBase)
                continue;

            DeviceCard *dc = new DeviceCard();
            dc->dev = dev;

            if (base.startsWith("mmcblk")) dc->type = "SD Card";
            else if (base.startsWith("sd")) dc->type = "USB Drive";
            else dc->type = "Device";

            QFrame *card = new QFrame();
            card->setStyleSheet("QFrame { background:#3a3a3a; border-radius:26px; }");
            QVBoxLayout *devLay = new QVBoxLayout(card);
            devLay->setContentsMargins(24, 20, 24, 20);
            devLay->setSpacing(14);

            QLabel *title = new QLabel(dc->type + ": " + base);
            title->setAlignment(Qt::AlignCenter);
            title->setStyleSheet("font-size:26px; font-weight:bold;");
            devLay->addWidget(title);

            QLabel *space = new QLabel("Not mounted");
            space->setAlignment(Qt::AlignCenter);
            space->setStyleSheet("font-size:18px;");
            devLay->addWidget(space);
            dc->space = space;

            QGridLayout *grid = new QGridLayout();
            grid->setSpacing(14);
            grid->setContentsMargins(0,0,0,0);

            QStringList parts = scanPartitions(dev);
            if (parts.isEmpty())
                parts << dev;

            PartitionCard *tmp = createPartitionCard(parts.first(), false);
            tmp->frame->adjustSize();
            int cardW = tmp->frame->sizeHint().width();
            if (cardW < 200) cardW = 200;
            tmp->frame->deleteLater();
            delete tmp;

            int avail = effectiveWidth();
            int cols = qMax(2, avail / cardW);

            int row = 0, col = 0;
            bool anyMounted = false;

            for (const QString &p : parts) {
                PartitionCard *pc = createPartitionCard(p, false);
                updatePartitionCard(pc, false);

                if (!pc->mountPoint.isEmpty())
                    anyMounted = true;

                // mount/unmount
                if (pc->btnMount) {
                    connect(pc->btnMount, &QPushButton::clicked, this, [this, pc]() {
                        if (pc->mountPoint.isEmpty()) {
                            QString mp, out;
                            if (!mountDevice(pc->dev, mp, out)) {
                                QMessageBox::warning(
                                    this, "Mount failed",
                                    "Could not mount:\n" + pc->dev + "\n\n" + out.trimmed() +
                                    "\n\nInstall udisks2 or pmount, and make sure this "
                                    "user is in the 'plugdev' group."
                                );
                            }
                        } else {
                            QString out;
                            if (!unmountDevice(pc->dev, pc->mountPoint, out)) {
                                QMessageBox::warning(
                                    this, "Unmount failed",
                                    "Could not unmount:\n" + pc->mountPoint + "\n\n" + out.trimmed() +
                                    "\n\nMake sure no files on the drive are open."
                                );
                            }
                        }
                        QCoreApplication::processEvents();
                        refreshAll();
                    });
                }

                if (pc->btnOpen) {
                    connect(pc->btnOpen, &QPushButton::clicked, this, [pc]() {
                        if (!pc->mountPoint.isEmpty())
                            QProcess::startDetached("osm-files", {pc->mountPoint});
                    });
                }

                if (pc->btnEject) {
                    connect(pc->btnEject, &QPushButton::clicked, this, [this, dc]() {
                        QString errors;
                        for (PartitionCard *pCard : dc->parts) {
                            if (pCard->mountPoint.isEmpty())
                                continue;
                            QString out;
                            if (!unmountDevice(pCard->dev, pCard->mountPoint, out))
                                errors += out.trimmed() + "\n";
                        }
                        if (!errors.isEmpty()) {
                            QMessageBox::warning(
                                this, "Eject failed",
                                "Some partitions could not be ejected:\n\n" + errors
                            );
                        }
                        QCoreApplication::processEvents();
                        refreshAll();
                    });
                }

                grid->addWidget(pc->frame, row, col);
                dc->parts.append(pc);

                col++;
                if (col >= cols) {
                    col = 0;
                    row++;
                }
            }

            if (anyMounted) {
                for (PartitionCard *pc : dc->parts) {
                    if (!pc->mountPoint.isEmpty()) {
                        qulonglong t=0, f=0;
                        if (getSpace(pc->mountPoint, t, f)) {
                            dc->space->setText(
                                QString("<span style='color:#7CFC00;'>Mounted</span> — "
                                        "%1 free / %2 total")
                                .arg(humanSize(f))
                                .arg(humanSize(t))
                            );
                        }
                        break;
                    }
                }
            }

            devLay->addLayout(grid);
            m_devicesLayout->addWidget(card);
            dc->frame = card;
            m_deviceCards.append(dc);
        }

        m_devicesLayout->addStretch();
    }

    // -----------------------------------------------------
    // Master refresh
    // -----------------------------------------------------
    void refreshAll()
    {
        m_rootDev  = detectRootDev();
        m_rootBase = m_rootDev.isEmpty() ? QString() : baseDeviceName(m_rootDev);

        if (!m_rootBase.isEmpty())
            m_internalTitle->setText("Internal Storage (" + m_rootBase + ")");
        else
            m_internalTitle->setText("Internal Storage");

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
