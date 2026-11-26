#include <QApplication>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QScrollArea>
#include <QGridLayout>
#include <QPixmap>
#include <QImageReader>
#include <QImage>
#include <QButtonGroup>
#include <QResizeEvent>
#include <QProcess>
#include <QTimer>
#include <QSplitter>
#include <QFont>
#include <QFrame>
#include <QScroller>
#include <QScrollerProperties>
#include <QVector>
#include <QSettings>
#include <QFile>
#include <QScrollBar>
#include <QThreadPool>
#include <QRunnable>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDateTime>
#include <QMessageBox>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <QIntValidator>
#include <QRandomGenerator>
#include <QCollator>
#include <algorithm>
#include <functional>
#include <utility>

static QString osmPaperConfigPath()
{
    const QString configDir = QDir::homePath() + "/.config/Alternix";
    QDir().mkpath(configDir);
    return configDir + "/osm-paper.conf";
}

static QString expandWallpaperHome(const QString &rawPath)
{
    const QString path = rawPath.trimmed();
    if (path == "~") return QDir::homePath();
    if (path.startsWith("~/")) return QDir::homePath() + path.mid(1);
    return path;
}

// Decode and cache small thumbnails on worker threads, never on Qt's GUI thread.
// The cache key includes the image's modification time and size, so edits
// invalidate stale previews automatically.
static QImage loadWallpaperThumbnail(const QString &path, const QSize &size)
{
    const QFileInfo file(path);
    if (!file.isFile()) return {};

    const QByteArray identity = file.absoluteFilePath().toUtf8() + '\n'
        + QByteArray::number(file.size()) + '\n'
        + QByteArray::number(file.lastModified().toMSecsSinceEpoch()) + '\n'
        + QByteArray::number(size.width()) + 'x' + QByteArray::number(size.height());
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex());
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/thumbnails");
    const QString cacheFile = cacheDir + QLatin1Char('/') + key + QStringLiteral(".png");

    QImage cached(cacheFile);
    if (!cached.isNull()) return cached;

    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize decodedSize = reader.size();
    if (decodedSize.isValid()) {
        decodedSize.scale(size, Qt::KeepAspectRatio);
        reader.setScaledSize(decodedSize);
    }
    QImage thumbnail = reader.read();
    if (thumbnail.isNull()) return {};
    if (thumbnail.size().width() > size.width() || thumbnail.size().height() > size.height())
        thumbnail = thumbnail.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    if (QDir().mkpath(cacheDir)) {
        QSaveFile output(cacheFile);
        if (output.open(QIODevice::WriteOnly)) {
            if (thumbnail.save(&output, "PNG")) output.commit();
            else output.cancelWriting();
        }
    }
    return thumbnail;
}

class ThumbnailJob final : public QRunnable
{
public:
    explicit ThumbnailJob(std::function<void()> task) : m_task(std::move(task)) {}
    void run() override { m_task(); }
private:
    std::function<void()> m_task;
};

class WallpaperBrowser : public QWidget
{
public:
    explicit WallpaperBrowser(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setWindowTitle("Wallpapers");

        loadLastWallpaperSettings();
        loadLastFolder();

        // Match the compact header card used by osm-kernels.
        auto *headerFrame = new QFrame;
        headerFrame->setObjectName("card");
        auto *headerLayout = new QVBoxLayout(headerFrame);
        headerLayout->setContentsMargins(16, 12, 16, 12);
        headerLayout->setSpacing(4);
        auto *headerTitle = new QLabel("Wallpapers");
        headerTitle->setObjectName("big");
        auto *headerNote = new QLabel("Select a wallpaper, then choose its fill type.");
        headerNote->setObjectName("small");
        headerLayout->addWidget(headerTitle);
        headerLayout->addWidget(headerNote);

        // ────────────────────────────── Top bar ──────────────────────────────
        auto *topLayout  = new QHBoxLayout;
        topLayout->setSpacing(8);
        auto *folderLbl  = new QLabel("Folder:");
        pathEdit         = new QLineEdit;
        auto *browseBtn  = new QPushButton("Browse…");

        topLayout->addWidget(folderLbl);
        topLayout->addWidget(pathEdit);
        topLayout->addWidget(browseBtn);

        // ───────────────── THUMBNAIL GRID (scrollable) ──────────────────
        scrollArea = new QScrollArea;
        scrollArea->setObjectName("wallpaperScroll");
        scrollArea->setWidgetResizable(false); // needed for QScroller stability
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setAlignment(Qt::AlignHCenter | Qt::AlignTop); // center grid widget

        gridWidget = new QWidget;
        gridWidget->setObjectName("wallpaperGrid");
        scrollArea->viewport()->setStyleSheet("background: #1d2021;");
        gridLayout = new QGridLayout(gridWidget);
        gridLayout->setContentsMargins(10, 10, 10, 10);
        gridLayout->setHorizontalSpacing(10);
        gridLayout->setVerticalSpacing(10);
        scrollArea->setWidget(gridWidget);

        // Keep kinetic scrolling for touch swipes. The left-mouse scroller
        // gesture intercepted ordinary clicks/taps on thumbnail buttons.
        QScroller::grabGesture(scrollArea->viewport(), QScroller::TouchGesture);
        auto *scroller = QScroller::scroller(scrollArea->viewport());
        QScrollerProperties scrollProperties = scroller->scrollerProperties();
        scrollProperties.setScrollMetric(QScrollerProperties::MousePressEventDelay, 0.0);
        scroller->setScrollerProperties(scrollProperties);

        // ───────────────── BUTTON PANEL (2×2) ─────────────────
        zoomBtn   = new QPushButton("Zoom");
        fitBtn    = new QPushButton("Fit");
        tileBtn   = new QPushButton("Tile");
        centerBtn = new QPushButton("Center");

        // Identical checked-button highlight to osm-kernels' navigation.
        modeGroup = new QButtonGroup(this);
        modeGroup->setExclusive(true);
        for (QPushButton *button : {zoomBtn, fitBtn, tileBtn, centerBtn}) {
            button->setCheckable(true);
            button->setMinimumHeight(48);
            button->setFocusPolicy(Qt::NoFocus);
            modeGroup->addButton(button);
        }
        selectedModeButton(lastMode)->setChecked(true);

        auto *btnGrid = new QGridLayout;
        btnGrid->setContentsMargins(6,6,6,6);
        btnGrid->setSpacing(6);

        btnGrid->addWidget(zoomBtn,   0,0);
        btnGrid->addWidget(fitBtn,    0,1);
        btnGrid->addWidget(tileBtn,   1,0);
        btnGrid->addWidget(centerBtn, 1,1);

        auto *visorBtn = new QPushButton("Set as Visor Background");
        visorBtn->setMinimumHeight(48);
        visorBtn->setFocusPolicy(Qt::NoFocus);
        btnGrid->addWidget(visorBtn, 2, 0, 1, 2);

        // Timed shuffle controls: the interval is shown beside its toggle.
        auto *shuffleRow = new QWidget;
        auto *shuffleLayout = new QHBoxLayout(shuffleRow);
        shuffleLayout->setContentsMargins(0, 0, 0, 0);
        shuffleLayout->setSpacing(6);
        shuffleBtn = new QPushButton("Shuffle");
        shuffleBtn->setCheckable(true);
        shuffleBtn->setMinimumHeight(48);
        shuffleBtn->setFocusPolicy(Qt::NoFocus);
        shuffleMinutesEdit = new QLineEdit("5");
        shuffleMinutesEdit->setValidator(new QIntValidator(1, 1440, shuffleMinutesEdit));
        shuffleMinutesEdit->setAlignment(Qt::AlignCenter);
        shuffleMinutesEdit->setPlaceholderText("Minutes");
        shuffleMinutesEdit->setMaximumWidth(90);
        shuffleMinutesEdit->setToolTip("Minutes between shuffled wallpapers");
        auto *minutesLabel = new QLabel("min");
        shuffleLayout->addWidget(shuffleBtn, 1);
        shuffleLayout->addWidget(shuffleMinutesEdit);
        shuffleLayout->addWidget(minutesLabel);
        btnGrid->addWidget(shuffleRow, 3, 0, 1, 2);

        sequentialBtn = new QPushButton("Sequential  →");
        sequentialBtn->setMinimumHeight(48);
        sequentialBtn->setFocusPolicy(Qt::NoFocus);
        btnGrid->addWidget(sequentialBtn, 4, 0, 1, 2);

        auto *buttonsFrame = new QFrame;
        buttonsFrame->setObjectName("card");
        buttonsFrame->setLayout(btnGrid);

        int buttonHeight = 380;
        buttonsFrame->setMinimumHeight(buttonHeight);
        buttonsFrame->setMaximumHeight(buttonHeight);

        // ───────────────── SPLITTER (FIXED, NO HANDLE) ─────────────────
        splitter = new QSplitter(Qt::Vertical);
        splitter->addWidget(scrollArea);
        splitter->addWidget(buttonsFrame);
        splitter->setHandleWidth(0);
        splitter->setChildrenCollapsible(false);

        QTimer::singleShot(0, this, [this, buttonHeight]() {
            splitter->setSizes({
                height() - buttonHeight,
                buttonHeight
            });
            splitter->setStretchFactor(0, 0);
            splitter->setStretchFactor(1, 0);
        });

        // Root layout
        rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(14, 14, 14, 14);
        rootLayout->setSpacing(10);
        rootLayout->addWidget(headerFrame);
        rootLayout->addLayout(topLayout);
        rootLayout->addWidget(splitter, 1);

        buttonGroup = new QButtonGroup(this);
        buttonGroup->setExclusive(true);

        columns   = 2;                  // default until first width-based recompute
        thumbSize = QSize(260, 174);    // preview size
        // Must fit thumbnail + 48px filename + spacing + margins; the old
        // 210px cards let labels spill over adjacent rows and intercept taps.
        cardSize  = QSize(300, 260);

        // Never decode full-sized images in the GUI thread. Limit parallel
        // jobs to prevent thumbnail work starving rendering and touch input.
        thumbPool.setMaxThreadCount(2);
        thumbRequestTimer = new QTimer(this);
        thumbRequestTimer->setSingleShot(true);
        thumbRequestTimer->setInterval(20);
        connect(thumbRequestTimer, &QTimer::timeout,
                this, &WallpaperBrowser::loadVisibleThumbnails);
        cardBuildTimer = new QTimer(this);
        cardBuildTimer->setSingleShot(true);
        connect(cardBuildTimer, &QTimer::timeout,
                this, &WallpaperBrowser::buildNextCardBatch);
        connect(scrollArea->verticalScrollBar(), &QScrollBar::valueChanged,
                this, [this](int) { scheduleVisibleThumbnails(); });

        // ─────────── Signals ───────────

        connect(browseBtn, &QPushButton::clicked, this, [this]() {
            QString dir = QFileDialog::getExistingDirectory(
                this,
                "Select Wallpaper Folder",
                lastFolder.isEmpty() ? defaultStartDir() : lastFolder
            );

            if (!dir.isEmpty()) {
                pathEdit->setText(dir);
                saveLastFolder(dir);
                loadImagesFrom(dir);
            }
        });

        connect(pathEdit, &QLineEdit::returnPressed, this, [this]() {
            saveLastFolder(pathEdit->text());
            loadImagesFrom(pathEdit->text());
        });

        connect(buttonGroup, &QButtonGroup::idClicked, this,
                [this](int id) { selectWallpaper(id); });

        // transparent preview fade
        auto fade = [this]() {
            setWindowOpacity(0.25);
            QTimer::singleShot(1500, this, [this]() {
                setWindowOpacity(1.0);
            });
        };

        connect(zoomBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("zoom")) fade();
        });
        connect(fitBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("fit")) fade();
        });
        connect(tileBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("tile")) fade();
        });
        connect(centerBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("center")) fade();
        });
        connect(visorBtn, &QPushButton::clicked, this, [this]() {
            setVisorBackground();
        });

        shuffleTimer = new QTimer(this);
        connect(shuffleBtn, &QPushButton::toggled, this, [this](bool enabled) {
            if (enabled) startShuffleTimer();
            else shuffleTimer->stop();
        });
        connect(shuffleMinutesEdit, &QLineEdit::editingFinished, this, [this]() {
            if (shuffleBtn->isChecked()) startShuffleTimer();
        });
        connect(shuffleTimer, &QTimer::timeout, this, [this]() {
            advanceWallpaper(true);
        });
        connect(sequentialBtn, &QPushButton::clicked, this, [this]() {
            advanceWallpaper(false);
        });

        // Load initial folder
        QString defaultDir = lastFolder.isEmpty() ? defaultStartDir() : lastFolder;
        pathEdit->setText(defaultDir);
        loadImagesFrom(defaultDir);

        applyLastWallpaperOnStartup();
        // The folder path should not steal the initial keyboard focus.
        pathEdit->setFocusPolicy(Qt::ClickFocus);
        QTimer::singleShot(0, this, [this]() { focusSelectedWallpaper(); });
    }

    ~WallpaperBrowser() override
    {
        // Each worker posts its result only while this widget is alive.
        // At most two image decodes can be running when closing.
        thumbPool.waitForDone();
    }

private:
    // ───────────────────── DEFAULT START DIR ─────────────────────
    QString defaultStartDir()
    {
        QString alt = QDir::homePath() + "/Alternix/Wallpapers";
        if (QDir(alt).exists()) return alt;

        QString pics = QDir::homePath() + "/Pictures/wallpapers";
        if (QDir(pics).exists()) return pics;

        return QDir::homePath();
    }

    // Widgets
    QLineEdit   *pathEdit{};
    QLineEdit   *shuffleMinutesEdit{};
    QPushButton *shuffleBtn{};
    QPushButton *sequentialBtn{};
    QTimer      *shuffleTimer{};
    QScrollArea *scrollArea{};
    QWidget     *gridWidget{};
    QGridLayout *gridLayout{};
    QButtonGroup *buttonGroup{};
    QButtonGroup *modeGroup{};
    QPushButton *zoomBtn{};
    QPushButton *fitBtn{};
    QPushButton *tileBtn{};
    QPushButton *centerBtn{};
    QSplitter   *splitter{};
    QVBoxLayout *rootLayout{};
    QTimer      *thumbRequestTimer{};
    QTimer      *cardBuildTimer{};
    QThreadPool  thumbPool;

    // State
    QStringList imagePaths;
    QString     currentImagePath;
    int         columns;
    QSize       thumbSize;
    QSize       cardSize;
    QVector<QPushButton*> thumbButtons;
    QVector<QLabel*>      thumbImageLabels;
    QVector<quint8> thumbStates;     // 0=not requested; 1=loading; 2=done
    int         inFlight = 0;
    int         loadGeneration = 0;     // ignore results from a previous folder
    int         nextCardIndex = 0;

    // Persistence
    QString lastWallpaperPath;
    QString lastMode;
    QString lastFolder;

    // ───────────────────── Selection ───────────────────────
    QPushButton *selectedModeButton(const QString &mode) const
    {
        if (mode == "fit") return fitBtn;
        if (mode == "tile") return tileBtn;
        if (mode == "center") return centerBtn;
        return zoomBtn;
    }

    void selectWallpaper(int index)
    {
        if (index < 0 || index >= imagePaths.size()) return;
        currentImagePath = imagePaths.at(index);
        if (auto *button = buttonGroup->button(index))
            button->setChecked(true);
    }

    void focusSelectedWallpaper()
    {
        const int index = imagePaths.indexOf(currentImagePath);
        if (index >= 0) {
            if (QPushButton *button = buttonGroup->button(index)) {
                button->setFocus(Qt::OtherFocusReason);
                return;
            }
        }
        if (!thumbButtons.isEmpty()) thumbButtons.first()->setFocus(Qt::OtherFocusReason);
    }

    int shuffleIntervalMs() const
    {
        bool ok = false;
        const int minutes = shuffleMinutesEdit ? shuffleMinutesEdit->text().toInt(&ok) : 5;
        return qBound(1, ok ? minutes : 5, 1440) * 60 * 1000;
    }

    void startShuffleTimer()
    {
        if (!shuffleTimer || imagePaths.size() < 2) {
            if (shuffleBtn) shuffleBtn->setChecked(false);
            return;
        }
        shuffleTimer->start(shuffleIntervalMs());
    }

    void advanceWallpaper(bool random)
    {
        if (imagePaths.size() < 2) return;
        const int currentIndex = imagePaths.indexOf(currentImagePath);
        int nextIndex = 0;
        if (random) {
            do {
                nextIndex = QRandomGenerator::global()->bounded(imagePaths.size());
            } while (nextIndex == currentIndex);
        } else {
            nextIndex = currentIndex < 0 ? 0 : (currentIndex + 1) % imagePaths.size();
        }
        selectWallpaper(nextIndex);
        const QString mode = lastMode.isEmpty() ? QStringLiteral("zoom") : lastMode;
        applyWallpaper(mode);
    }

    // ───────────────────── Persistence ───────────────────────
    void saveLastFolder(const QString &folder)
    {
        QSettings s(osmPaperConfigPath(), QSettings::IniFormat);
        s.beginGroup("last");
        s.setValue("folder", folder);
        s.endGroup();
        lastFolder = folder;
    }

    void loadLastFolder()
    {
        QSettings s(osmPaperConfigPath(), QSettings::IniFormat);
        s.beginGroup("last");
        lastFolder = expandWallpaperHome(s.value("folder").toString());
        s.endGroup();
    }

    void saveLastWallpaper(const QString &modeName, const QString &path)
    {
        QSettings s(osmPaperConfigPath(), QSettings::IniFormat);
        s.beginGroup("last");
        s.setValue("wallpaper", path);
        s.setValue("mode", modeName);
        s.endGroup();

        lastWallpaperPath = path;
        lastMode = modeName;
        selectedModeButton(modeName)->setChecked(true);
    }

    void loadLastWallpaperSettings()
    {
        QSettings s(osmPaperConfigPath(), QSettings::IniFormat);
        s.beginGroup("last");
        lastWallpaperPath = expandWallpaperHome(s.value("wallpaper").toString());
        lastMode = s.value("mode").toString();
        s.endGroup();
    }

    void applyLastWallpaperOnStartup()
    {
        if (lastWallpaperPath.isEmpty() || !QFile::exists(lastWallpaperPath))
            return;
        QString mode = lastMode.isEmpty() ? "zoom" : lastMode;
        applyWallpaperDirect(mode, lastWallpaperPath);
    }

    // ───────────────────── Wallpaper apply ───────────────────────

    QString modeToArgument(const QString &modeName) const
    {
        if (modeName == "fit")    return "--stretch";
        if (modeName == "tile")   return "--tile";
        if (modeName == "center") return "--center";
        return "--zoom";
    }

    // Always clear the previous X root pixmap. After a screen rotation it
    // may still have the old landscape/portrait dimensions, leading to
    // corrupted margins until the entire wallpaper is redrawn.
    bool redrawWallpaper(const QString &modeName, const QString &path)
    {
        if (path.isEmpty()) return false;
        // Pass arguments directly: wallpaper filenames can contain spaces,
        // shell quotes and other characters that must not be interpreted.
        return QProcess::startDetached(
            "xwallpaper", QStringList{"--clear", modeToArgument(modeName), path});
    }

    void applyWallpaperDirect(const QString &modeName, const QString &path)
    {
        redrawWallpaper(modeName, path);
    }

    bool applyWallpaper(const QString &modeName)
    {
        if (currentImagePath.isEmpty()) {
            selectedModeButton(lastMode)->setChecked(true);
            return false;
        }

        if (!redrawWallpaper(modeName, currentImagePath)) {
            selectedModeButton(lastMode)->setChecked(true);
            return false;
        }

        saveLastWallpaper(modeName, currentImagePath);
        return true;
    }

    // ───────────────────── Visor boot background ─────────────────────
    QString detectVisorEsp() const
    {
        QProcess findmnt;
        findmnt.start("findmnt", {"-rn", "-o", "TARGET,FSTYPE"});
        if (!findmnt.waitForFinished(3000) || findmnt.exitCode() != 0)
            return {};

        const QStringList lines = QString::fromLocal8Bit(findmnt.readAllStandardOutput())
            .split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QStringList fields = line.trimmed().split(QRegularExpression("\\s+"));
            if (fields.size() < 2 || fields.last() != "vfat") continue;
            const QString mountPoint = fields.first();
            const QString visorDir = mountPoint + "/EFI/visor";
            if (QFileInfo(visorDir + "/boot.conf").isFile()
                && QDir(visorDir + "/backgrounds").exists())
                return mountPoint;
        }
        return {};
    }

    void setVisorBackground()
    {
        if (currentImagePath.isEmpty()) {
            QMessageBox::warning(this, "Visor Background", "Select a wallpaper first.");
            return;
        }

        const QString esp = detectVisorEsp();
        if (esp.isEmpty()) {
            QMessageBox::warning(this, "Visor Background",
                "Could not detect a mounted EFI System Partition containing Visor.\n"
                "Make sure the ESP is mounted and Visor is installed.");
            return;
        }

        QTemporaryFile temp(QDir::tempPath() + "/osm-paper-visor-XXXXXX.png");
        temp.setAutoRemove(false);
        if (!temp.open()) {
            QMessageBox::warning(this, "Visor Background", "Could not create a temporary image file.");
            return;
        }
        const QString tempImage = temp.fileName();
        temp.close();
        QImage image(currentImagePath);
        if (image.isNull() || !image.save(tempImage, "PNG")) {
            QFile::remove(tempImage);
            QMessageBox::warning(this, "Visor Background", "Could not convert the selected wallpaper to PNG.");
            return;
        }

        const QString pythonScript = QStringLiteral(
            "import os,sys,shutil,re\n"
            "esp,src=sys.argv[1],sys.argv[2]\n"
            "dest=os.path.join(esp,'EFI','visor','backgrounds','alternix-wallpaper.png')\n"
            "conf=os.path.join(esp,'EFI','visor','boot.conf')\n"
            "os.makedirs(os.path.dirname(dest),exist_ok=True)\n"
            "shutil.copy2(src,dest)\n"
            "with open(conf,'r',encoding='utf-8') as f: lines=f.readlines()\n"
            "key=r'\\EFI\\visor\\backgrounds\\alternix-wallpaper.png'\n"
            "out=[]; replaced=False\n"
            "for line in lines:\n"
            "    if re.match(r'^\\s*background\\s*=',line):\n"
            "        out.append('background='+key+'\\n'); replaced=True\n"
            "    else: out.append(line)\n"
            "if not replaced: out.append('background='+key+'\\n')\n"
            "with open(conf,'w',encoding='utf-8') as f: f.writelines(out)\n");

        QProcess process;
        process.start("pkexec", {"python3", "-c", pythonScript, esp, tempImage});
        if (!process.waitForFinished(-1) || process.exitCode() != 0) {
            QFile::remove(tempImage);
            const QString error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            QMessageBox::warning(this, "Visor Background",
                error.isEmpty() ? "Failed to update Visor's background." : error);
            return;
        }
        QFile::remove(tempImage);
        QMessageBox::information(this, "Visor Background",
            "Wallpaper saved to Visor's background folder and boot.conf updated.\n"
            "The selected image will appear in Visor at the next boot.");
    }


    // ───────────────────── Grid management ───────────────────────────

    void clearGrid()
    {
        cardBuildTimer->stop();
        thumbRequestTimer->stop();
        thumbPool.clear();           // cancel jobs still queued, not running jobs
        ++loadGeneration;
        nextCardIndex = 0;
        inFlight = 0;
        thumbStates.clear();
        thumbButtons.clear();
        thumbImageLabels.clear();

        for (auto *btn : buttonGroup->buttons())
            buttonGroup->removeButton(btn);

        QLayoutItem *item;
        while ((item = gridLayout->takeAt(0))) {
            if (item->widget()) {
                item->widget()->hide();
                item->widget()->deleteLater();
            }
            delete item;
        }
        gridWidget->setMinimumHeight(0);
    }

    void loadImagesFrom(const QString &dirPath)
    {
        clearGrid();
        imagePaths.clear();
        currentImagePath.clear();
        scrollArea->verticalScrollBar()->setValue(0);

        QDir dir(dirPath);
        if (!dir.exists()) return;

        const QStringList filters = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif", "*.webp"};
        QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(files.begin(), files.end(), [&collator](const QFileInfo &a, const QFileInfo &b) {
            return collator.compare(a.fileName(), b.fileName()) < 0;
        });
        imagePaths.reserve(files.size());
        for (const QFileInfo &file : files)
            imagePaths << file.absoluteFilePath();

        buildPlaceholdersAndStartLoader();
    }

    void buildPlaceholdersAndStartLoader()
    {
        if (imagePaths.isEmpty()) {
            if (shuffleTimer) shuffleTimer->stop();
            if (shuffleBtn) shuffleBtn->setChecked(false);
            return;
        }
        if (imagePaths.size() < 2 && shuffleBtn) {
            shuffleBtn->setChecked(false);
            if (shuffleTimer) shuffleTimer->stop();
        }

        // The image list is cheap. Construct the actual button widgets in
        // small chunks so large wallpaper folders cannot block startup.
        thumbStates.fill(0, imagePaths.size());
        columns = computeColumns();
        int selectedIndex = lastWallpaperPath.isEmpty()
            ? 0 : imagePaths.indexOf(lastWallpaperPath);
        if (selectedIndex < 0) selectedIndex = 0;
        currentImagePath = imagePaths.at(selectedIndex);

        buildNextCardBatch();       // initial visible cards before first paint
    }

    void buildNextCardBatch()
    {
        // First batch covers several rows; subsequent batches yield back
        // to Qt's event loop so the window remains interactive.
        const int batchEnd = qMin(nextCardIndex + 24, imagePaths.size());
        for (int i = nextCardIndex; i < batchEnd; ++i) {
            auto *card = new QPushButton;
            card->setCheckable(true);
            card->setFocusPolicy(Qt::StrongFocus);
            card->setObjectName(QStringLiteral("wallpaperCard"));
            card->setToolTip(imagePaths.at(i));
            card->setFixedSize(cardSize);

            auto *inner = new QVBoxLayout(card);
            inner->setContentsMargins(12, 12, 12, 12);
            inner->setSpacing(6);
            inner->setAlignment(Qt::AlignCenter);

            auto *thumbLabel = new QLabel(QStringLiteral("Loading…"), card);
            thumbLabel->setAlignment(Qt::AlignCenter);
            thumbLabel->setFixedSize(thumbSize);
            thumbLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            inner->addWidget(thumbLabel);

            auto *nameLabel = new QLabel(QFileInfo(imagePaths.at(i)).fileName(), card);
            nameLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            nameLabel->setObjectName(QStringLiteral("wallpaperFilename"));
            nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            nameLabel->setWordWrap(true);
            nameLabel->setFixedHeight(48);
            inner->addWidget(nameLabel);

            thumbImageLabels.append(thumbLabel);
            buttonGroup->addButton(card, i);
            thumbButtons.append(card);
            gridLayout->addWidget(card, i / columns, i % columns);
            if (imagePaths.at(i) == currentImagePath) {
                card->setChecked(true);
                card->setFocus(Qt::OtherFocusReason);
            }
        }
        nextCardIndex = batchEnd;
        updateGridGeometry();
        scheduleVisibleThumbnails();
        if (nextCardIndex < imagePaths.size()) cardBuildTimer->start(5);
    }

    // How many card columns fit in the scroll area's current width.
    int computeColumns() const
    {
        int avail = scrollArea && scrollArea->viewport()
                        ? scrollArea->viewport()->width()
                        : width();
        int spacing = gridLayout->horizontalSpacing();
        int lm = gridLayout->contentsMargins().left();
        int rm = gridLayout->contentsMargins().right();

        int usable = avail - lm - rm;
        // n cards need n*cardW + (n-1)*spacing  ⇒  n = (usable+spacing)/(cardW+spacing)
        int cols = (usable + spacing) / (cardSize.width() + spacing);
        return qMax(1, cols);
    }

    // Repositions the existing cards for the current column count —
    // no card is recreated, so thumbnails and selection are untouched.
    void relayoutGrid()
    {
        QLayoutItem *item;
        while ((item = gridLayout->takeAt(0)))
            delete item;

        int row = 0, col = 0;
        for (QPushButton *card : thumbButtons) {
            gridLayout->addWidget(card, row, col);
            if (++col >= columns) { col = 0; ++row; }
        }
        updateGridGeometry();
        scheduleVisibleThumbnails();
    }

    void updateGridGeometry()
    {
        const int spacing = gridLayout->horizontalSpacing();
        const QMargins margins = gridLayout->contentsMargins();
        const int contentWidth = columns * cardSize.width()
            + (columns - 1) * spacing + margins.left() + margins.right();
        gridWidget->setFixedWidth(contentWidth);
        gridWidget->setMinimumHeight(0);
        gridLayout->activate();
        gridWidget->adjustSize();
        gridWidget->setMinimumHeight(gridWidget->sizeHint().height());
    }

protected:
    // Re-flow the grid whenever the window width allows a different
    // number of columns.
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        int newCols = computeColumns();
        if (newCols != columns) {
            columns = newCols;
            if (!thumbButtons.isEmpty())
                relayoutGrid();
        }
    }

private:
    // ───────────────────── Visible-first, background thumbnail loading ─────────
    void scheduleVisibleThumbnails()
    {
        if (thumbRequestTimer && !thumbRequestTimer->isActive())
            thumbRequestTimer->start();
    }

    void loadVisibleThumbnails()
    {
        if (thumbButtons.isEmpty() || inFlight >= 2) return;

        const int rowHeight = cardSize.height() + gridLayout->verticalSpacing();
        const int scrollY = scrollArea->verticalScrollBar()->value();
        const int viewportHeight = qMax(1, scrollArea->viewport()->height());
        const int firstRow = qMax(0, scrollY / rowHeight);
        const int lastRow = (scrollY + viewportHeight) / rowHeight;

        // Always prioritise what is *currently* visible. Only decode one
        // row before and two after when spare background capacity remains.
        auto dispatchRow = [this](int row) {
            if (row < 0) return;
            for (int col = 0; col < columns && inFlight < 2; ++col) {
                const int index = row * columns + col;
                if (index >= thumbButtons.size()) return;
                if (thumbStates.at(index) != 0) continue;

                thumbStates[index] = 1;
                ++inFlight;
                const int generation = loadGeneration;
                const QString path = imagePaths.at(index);
                const QSize size = thumbSize;
                thumbPool.start(new ThumbnailJob([this, generation, index, path, size]() {
                    const QImage image = loadWallpaperThumbnail(path, size);
                    QMetaObject::invokeMethod(this, [this, generation, index, image]() {
                        // A folder switch may have replaced all the buttons
                        // while this thumbnail was being decoded.
                        if (generation != loadGeneration) return;
                        --inFlight;
                        if (index >= thumbImageLabels.size()) return;
                        thumbStates[index] = 2;
                        QLabel *label = thumbImageLabels.at(index);
                        if (image.isNull()) label->setText(QStringLiteral("Error"));
                        else {
                            label->clear();
                            label->setPixmap(QPixmap::fromImage(image));
                        }
                        // No opacity effect: images remain attached to their
                        // cards while the viewport scrolls (Qt5 blitting).
                        scheduleVisibleThumbnails();
                    }, Qt::QueuedConnection);
                }));
            }
        };

        for (int row = firstRow; row <= lastRow && inFlight < 2; ++row)
            dispatchRow(row);
        for (int row = lastRow + 1; row <= lastRow + 2 && inFlight < 2; ++row)
            dispatchRow(row);
        if (inFlight < 2) dispatchRow(firstRow - 1);
    }

};

// Same palette, typography, controls, and selected state as osm-kernels(6).
static const char *kStyle = R"(
QWidget { background: #282828; color: #ebdbb2; font-family: 'DejaVu Sans'; font-size: 11pt; }
QFrame#card { background: #32302f; border-radius: 12px; }
QFrame#card QLabel { background: transparent; }
QLabel { background: transparent; }
QLabel#big { font-size: 15pt; font-weight: bold; color: #fbf1c7; }
QLabel#small { font-size: 9pt; color: #a89984; }
QPushButton { background: #3c3836; border: 1px solid #504945; border-radius: 8px; padding: 8px 16px; }
QPushButton:pressed { background: #504945; }
QPushButton:checked { background: #458588; color: #fbf1c7; border-color: #83a598; }
QPushButton:disabled { color: #7c6f64; }
QLineEdit { background: #1d2021; color: #ebdbb2; border: 1px solid #3c3836; border-radius: 8px; padding: 8px; selection-background-color: #458588; }
QScrollArea#wallpaperScroll { background: #1d2021; border: 1px solid #3c3836; border-radius: 8px; }
QWidget#wallpaperGrid { background: #1d2021; }
QPushButton#wallpaperCard { background: #3c3836; border: 1px solid #504945; border-radius: 12px; padding: 0px; }
QPushButton#wallpaperCard:pressed { background: #504945; }
QPushButton#wallpaperCard:checked { background: #458588; color: #fbf1c7; border: 1px solid #83a598; }
QLabel#wallpaperFilename { color: #ebdbb2; background: transparent; font-size: 10pt; }
QScrollBar:vertical { width: 14px; background: #282828; }
QScrollBar::handle:vertical { background: #504945; border-radius: 6px; min-height: 40px; }
)";

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("osm-paper"));
    app.setFont(QFont(QStringLiteral("DejaVu Sans"), 11));
    app.setStyleSheet(QString::fromLatin1(kStyle));

    WallpaperBrowser w;
    w.resize(720, 1560);
    w.show();

    return app.exec();
}
