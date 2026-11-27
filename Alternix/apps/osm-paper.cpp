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
#include <QPointer>
#include <QLockFile>
#include <QGuiApplication>
#include <QShowEvent>
#include <algorithm>
#include <sys/resource.h>
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

static QString wallpaperStartDir()
{
    const QString alt = QDir::homePath() + "/Alternix/Wallpapers";
    if (QDir(alt).exists()) return alt;
    const QString pics = QDir::homePath() + "/Pictures/wallpapers";
    if (QDir(pics).exists()) return pics;
    return QDir::homePath();
}

// Images in a folder, in natural alphanumeric order (img2 before img10).
static QStringList listWallpapers(const QString &dirPath)
{
    QDir dir(dirPath);
    if (!dir.exists()) return {};
    const QStringList filters = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif", "*.webp"};
    QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(files.begin(), files.end(), [&collator](const QFileInfo &a, const QFileInfo &b) {
        return collator.compare(a.fileName(), b.fileName()) < 0;
    });
    QStringList paths;
    paths.reserve(files.size());
    for (const QFileInfo &file : files) paths << file.absoluteFilePath();
    return paths;
}

static QString normalizeWallpaperMode(const QString &mode)
{
    if (mode == "fit" || mode == "fill") return "fill";   // "fit" was the old name
    if (mode == "tile") return "tile";
    return "zoom";                                          // also maps old "center"
}

static QString wallpaperModeArgument(const QString &mode)
{
    const QString m = normalizeWallpaperMode(mode);
    if (m == "fill") return "--stretch";
    if (m == "tile") return "--tile";
    return "--zoom";
}

// Always clear the previous X root pixmap. After a screen rotation it may
// still have the old dimensions, leading to corrupted margins.
static bool applyWallpaperFile(const QString &mode, const QString &path)
{
    if (path.isEmpty()) return false;
    // Arguments are passed directly: filenames may contain spaces or quotes.
    return QProcess::startDetached(
        "xwallpaper", QStringList{"--clear", wallpaperModeArgument(mode), path});
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

struct AutoConfig
{
    QString mode;          // "shuffle", "sequential" or empty (off)
    int     minutes = 5;
    QString folder;
    QString wallpaper;     // wallpaper currently applied
    QString wallpaperMode; // fill / tile / zoom
    bool active() const { return mode == "shuffle" || mode == "sequential"; }
};

static AutoConfig readAutoConfig()
{
    QSettings s(osmPaperConfigPath(), QSettings::IniFormat);
    AutoConfig c;
    c.mode = s.value("auto/mode").toString();
    c.minutes = qBound(1, s.value(c.mode == "shuffle" ? "auto/shuffleMinutes"
                                                      : "auto/sequentialMinutes", 5).toInt(), 1440);
    c.folder = expandWallpaperHome(s.value("last/folder").toString());
    if (c.folder.isEmpty()) c.folder = wallpaperStartDir();
    c.wallpaper = expandWallpaperHome(s.value("last/wallpaper").toString());
    c.wallpaperMode = normalizeWallpaperMode(s.value("last/mode").toString());
    return c;
}

// Headless background mode (`osm-paper --daemon`). It owns the timed
// Shuffle / Sequential changes, so they keep working after the window is
// closed. It follows the settings file written by the GUI and exits by
// itself once auto-change is switched off.
class WallpaperDaemon : public QObject
{
public:
    WallpaperDaemon()
    {
        tickTimer = new QTimer(this);
        connect(tickTimer, &QTimer::timeout, this, [this]() { tick(); });
        pollTimer = new QTimer(this);
        pollTimer->setInterval(2000);
        connect(pollTimer, &QTimer::timeout, this, [this]() { poll(); });
        poll();
        pollTimer->start();
    }

private:
    QTimer *tickTimer{};
    QTimer *pollTimer{};
    QString activeMode;
    int     activeMinutes = 0;

    static void quitApp()
    {
        QMetaObject::invokeMethod(qApp, []() { QCoreApplication::quit(); }, Qt::QueuedConnection);
    }

    // Picks up changes made in the GUI (mode, minutes, turning it off).
    void poll()
    {
        const AutoConfig c = readAutoConfig();
        if (!c.active()) { quitApp(); return; }
        if (c.mode != activeMode || c.minutes != activeMinutes) {
            activeMode = c.mode;
            activeMinutes = c.minutes;
            tickTimer->start(c.minutes * 60 * 1000);
        }
    }

    void tick()
    {
        const AutoConfig c = readAutoConfig();
        if (!c.active()) { quitApp(); return; }
        const QStringList paths = listWallpapers(c.folder);
        if (paths.size() < 2) return;

        const int current = paths.indexOf(c.wallpaper);
        int next = 0;
        if (c.mode == "shuffle") {
            do {
                next = QRandomGenerator::global()->bounded(paths.size());
            } while (next == current);
        } else {
            next = current < 0 ? 0 : (current + 1) % paths.size();
        }

        if (!applyWallpaperFile(c.wallpaperMode, paths.at(next))) return;

        // Remember it so a restart (and the GUI) sees the new one.
        QSettings s(osmPaperConfigPath(), QSettings::IniFormat);
        s.setValue("last/wallpaper", paths.at(next));
        s.sync();
    }
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
        loadAutoSettings();

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
        // The folder field only gets focus when clicked, never on startup.
        pathEdit->setFocusPolicy(Qt::ClickFocus);
        browseBtn->setFocusPolicy(Qt::NoFocus);

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

        // ───────────────── BUTTON PANEL (2 rows × 3) ─────────────────
        //   fill     tile        zoom
        //   shuffle  sequential  visor
        fillBtn = new QPushButton("Fill");
        tileBtn = new QPushButton("Tile");
        zoomBtn = new QPushButton("Zoom");

        // Identical checked-button highlight to osm-kernels' navigation.
        modeGroup = new QButtonGroup(this);
        modeGroup->setExclusive(true);
        for (QPushButton *button : {fillBtn, tileBtn, zoomBtn}) {
            button->setCheckable(true);
            modeGroup->addButton(button);
        }
        selectedModeButton(lastMode)->setChecked(true);

        // Timed auto-change toggles. Each has its own minutes field inside
        // the button, shown only while that mode is switched on. Only one
        // of the two can be active at a time.
        shuffleBtn = makeAutoButton("Shuffle",
            "Change to a random wallpaper every N minutes",
            shuffleMinutes, shuffleMinutesEdit, shuffleMinutesLabel);
        sequentialBtn = makeAutoButton("Sequential",
            "Change to the next picture (alphanumeric order) every N minutes",
            sequentialMinutes, sequentialMinutesEdit, sequentialMinutesLabel);
        auto *visorBtn = new QPushButton("Visor");
        visorBtn->setToolTip("Set as Visor boot background");

        auto *btnGrid = new QGridLayout;
        btnGrid->setContentsMargins(6,6,6,6);
        btnGrid->setSpacing(6);

        const QVector<QPushButton*> panelButtons = {
            fillBtn, tileBtn, zoomBtn, shuffleBtn, sequentialBtn, visorBtn };
        for (int i = 0; i < panelButtons.size(); ++i) {
            QPushButton *button = panelButtons.at(i);
            button->setMinimumHeight(48);
            button->setFocusPolicy(Qt::NoFocus);
            // Ignore size hints so all three columns stay equal width.
            button->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            btnGrid->addWidget(button, i / 3, i % 3);
        }
        for (int col = 0; col < 3; ++col) btnGrid->setColumnStretch(col, 1);

        auto *buttonsFrame = new QFrame;
        buttonsFrame->setObjectName("card");
        buttonsFrame->setLayout(btnGrid);

        int buttonHeight = 2 * 48 + 3 * 6 + 2;
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

        connect(fillBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("fill")) fade();
        });
        connect(tileBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("tile")) fade();
        });
        connect(zoomBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("zoom")) fade();
        });
        connect(visorBtn, &QPushButton::clicked, this, [this]() {
            setVisorBackground();
        });

        // Shuffle / Sequential run in the background process (--daemon), so
        // here the buttons only show and save the settings.
        connect(shuffleBtn, &QPushButton::toggled, this, [this](bool on) {
            shuffleMinutesEdit->setVisible(on);
            shuffleMinutesLabel->setVisible(on);
            if (on && sequentialBtn->isChecked()) sequentialBtn->setChecked(false);
        });
        connect(sequentialBtn, &QPushButton::toggled, this, [this](bool on) {
            sequentialMinutesEdit->setVisible(on);
            sequentialMinutesLabel->setVisible(on);
            if (on && shuffleBtn->isChecked()) shuffleBtn->setChecked(false);
        });
        // Only user clicks are saved, so an empty or missing folder at
        // startup can never overwrite the remembered choice.
        auto autoClicked = [this]() {
            saveAutoSettings();
            if (shuffleBtn->isChecked() || sequentialBtn->isChecked())
                startDaemon();
        };
        connect(shuffleBtn, &QPushButton::clicked, this, autoClicked);
        connect(sequentialBtn, &QPushButton::clicked, this, autoClicked);
        connect(shuffleMinutesEdit, &QLineEdit::editingFinished, this, [this]() {
            commitAutoMinutes(shuffleMinutesEdit, shuffleMinutes);
        });
        connect(sequentialMinutesEdit, &QLineEdit::editingFinished, this, [this]() {
            commitAutoMinutes(sequentialMinutesEdit, sequentialMinutes);
        });

        // Keep the selection in step with wallpapers the daemon applies.
        syncTimer = new QTimer(this);
        syncTimer->setInterval(2000);
        connect(syncTimer, &QTimer::timeout, this, [this]() { syncWithDaemon(); });

        // Load initial folder
        QString defaultDir = lastFolder.isEmpty() ? defaultStartDir() : lastFolder;
        pathEdit->setText(defaultDir);
        loadImagesFrom(defaultDir);

        applyLastWallpaperOnStartup();

        // Restore the remembered auto-change mode (starts its timer).
        if (savedAutoMode == "shuffle") shuffleBtn->setChecked(true);
        else if (savedAutoMode == "sequential") sequentialBtn->setChecked(true);
        // Make sure the background process is running if auto-change is on.
        if (shuffleBtn->isChecked() || sequentialBtn->isChecked()) startDaemon();
        configWallpaperSeen = lastWallpaperPath;
        syncTimer->start();
    }

    ~WallpaperBrowser() override
    {
        // Each worker posts its result only while this widget is alive.
        // At most two image decodes can be running when closing.
        thumbPool.waitForDone();
    }

private:
    // ───────────────────── DEFAULT START DIR ─────────────────────
    QString defaultStartDir() { return wallpaperStartDir(); }

    // Widgets
    QLineEdit   *pathEdit{};
    QLineEdit   *shuffleMinutesEdit{};
    QLabel      *shuffleMinutesLabel{};
    QPushButton *shuffleBtn{};
    QPushButton *sequentialBtn{};
    QLineEdit   *sequentialMinutesEdit{};
    QLabel      *sequentialMinutesLabel{};
    QTimer      *syncTimer{};
    QScrollArea *scrollArea{};
    QWidget     *gridWidget{};
    QGridLayout *gridLayout{};
    QButtonGroup *buttonGroup{};
    QButtonGroup *modeGroup{};
    QPushButton *fillBtn{};
    QPushButton *tileBtn{};
    QPushButton *zoomBtn{};
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
    int         shuffleMinutes = 5;
    int         sequentialMinutes = 5;
    QString     savedAutoMode;           // "shuffle", "sequential" or empty
    QString     configWallpaperSeen;     // last wallpaper value seen in the settings file
    bool        focusWhenBuilt = false;  // focus the selected card once it exists
    bool        initialFocusDone = false;

    // Persistence
    QString lastWallpaperPath;
    QString lastMode;
    QString lastFolder;

    // ───────────────────── Selection ───────────────────────
    QPushButton *selectedModeButton(const QString &mode) const
    {
        if (mode == "fill" || mode == "fit") return fillBtn;
        if (mode == "tile") return tileBtn;
        return zoomBtn;
    }

    void selectWallpaper(int index, bool reveal = false)
    {
        if (index < 0 || index >= imagePaths.size()) return;
        currentImagePath = imagePaths.at(index);
        if (auto *button = buttonGroup->button(index)) {
            button->setChecked(true);
            if (reveal) {
                button->setFocus(Qt::OtherFocusReason);
                scrollArea->ensureWidgetVisible(button);
            }
        }
    }

    // Give keyboard focus to the currently selected wallpaper card and
    // scroll it into view. If the card isn't built yet, buildNextCardBatch()
    // does it as soon as it is.
    void focusSelectedWallpaper()
    {
        const int index = imagePaths.indexOf(currentImagePath);
        QAbstractButton *button = index >= 0 ? buttonGroup->button(index) : nullptr;
        if (!button) {
            focusWhenBuilt = true;
            return;
        }
        focusWhenBuilt = false;
        button->setFocus(Qt::OtherFocusReason);
        QPointer<QAbstractButton> guard(button);
        QTimer::singleShot(0, this, [this, guard]() {
            if (guard) scrollArea->ensureWidgetVisible(guard);
        });
    }

    QPushButton *makeAutoButton(const QString &title, const QString &tip, int minutes,
                                QLineEdit *&edit, QLabel *&unit)
    {
        auto *btn = new QPushButton;
        btn->setObjectName("autoBtn");
        btn->setCheckable(true);
        btn->setToolTip(tip);
        auto *inner = new QHBoxLayout(btn);
        inner->setContentsMargins(8, 0, 8, 0);
        inner->setSpacing(4);
        auto *lbl = new QLabel(title);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        edit = new QLineEdit(QString::number(minutes));
        edit->setObjectName("autoMinutes");
        edit->setValidator(new QIntValidator(1, 1440, edit));
        edit->setAlignment(Qt::AlignCenter);
        edit->setFixedSize(46, 32);
        edit->setFocusPolicy(Qt::ClickFocus);
        edit->setToolTip("Minutes between wallpaper changes");
        unit = new QLabel("min");
        unit->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        edit->hide();
        unit->hide();
        inner->addWidget(lbl, 1);
        inner->addWidget(edit);
        inner->addWidget(unit);
        return btn;
    }

    void loadAutoSettings()
    {
        QSettings s(osmPaperConfigPath(), QSettings::IniFormat);
        s.beginGroup("auto");
        savedAutoMode = s.value("mode").toString();
        shuffleMinutes = qBound(1, s.value("shuffleMinutes", 5).toInt(), 1440);
        sequentialMinutes = qBound(1, s.value("sequentialMinutes", 5).toInt(), 1440);
        s.endGroup();
    }

    void saveAutoSettings()
    {
        QSettings s(osmPaperConfigPath(), QSettings::IniFormat);
        s.beginGroup("auto");
        s.setValue("mode", shuffleBtn->isChecked() ? "shuffle"
                           : sequentialBtn->isChecked() ? "sequential" : "");
        s.setValue("shuffleMinutes", shuffleMinutes);
        s.setValue("sequentialMinutes", sequentialMinutes);
        s.endGroup();
        s.sync();
    }

    void commitAutoMinutes(QLineEdit *edit, int &minutes)
    {
        bool ok = false;
        const int typed = edit->text().toInt(&ok);
        minutes = qBound(1, ok ? typed : minutes, 1440);
        edit->setText(QString::number(minutes));
        saveAutoSettings();   // the daemon notices the new value within seconds
    }

    // Start the background process. If one is already running it finds the
    // lock held and exits at once, so calling this repeatedly is harmless.
    void startDaemon()
    {
        QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                QStringList{QStringLiteral("--daemon")});
    }

    // Reflect a wallpaper the daemon applied: move the selection to it.
    void syncWithDaemon()
    {
        QSettings s(osmPaperConfigPath(), QSettings::IniFormat);
        const QString path = expandWallpaperHome(s.value("last/wallpaper").toString());
        if (path.isEmpty() || path == configWallpaperSeen) return;
        configWallpaperSeen = path;
        const int index = imagePaths.indexOf(path);
        if (index < 0) return;
        lastWallpaperPath = path;
        selectWallpaper(index);
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
        configWallpaperSeen = path;
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
        // Older versions stored "fit" (now Fill) and "center" (removed).
        if (!lastMode.isEmpty()) lastMode = normalizeWallpaperMode(lastMode);
    }

    void applyLastWallpaperOnStartup()
    {
        if (lastWallpaperPath.isEmpty() || !QFile::exists(lastWallpaperPath))
            return;
        QString mode = lastMode.isEmpty() ? "zoom" : lastMode;
        applyWallpaperDirect(mode, lastWallpaperPath);
    }

    // ───────────────────── Wallpaper apply ───────────────────────

    bool redrawWallpaper(const QString &modeName, const QString &path)
    {
        return applyWallpaperFile(modeName, path);
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
        focusWhenBuilt = true;
        scrollArea->verticalScrollBar()->setValue(0);

        imagePaths = listWallpapers(dirPath);

        buildPlaceholdersAndStartLoader();
    }

    void buildPlaceholdersAndStartLoader()
    {
        if (imagePaths.isEmpty()) return;

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
                if (focusWhenBuilt) {
                    focusWhenBuilt = false;
                    card->setFocus(Qt::OtherFocusReason);
                    QPointer<QPushButton> guard(card);
                    QTimer::singleShot(0, this, [this, guard]() {
                        if (guard) scrollArea->ensureWidgetVisible(guard);
                    });
                }
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
    // Start with the selected wallpaper focused, not the folder field.
    void showEvent(QShowEvent *e) override
    {
        QWidget::showEvent(e);
        if (initialFocusDone) return;
        initialFocusDone = true;
        QTimer::singleShot(0, this, [this]() { focusSelectedWallpaper(); });
    }

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
QPushButton#autoBtn { padding: 0px 8px; }
QPushButton#autoBtn QLabel { background: transparent; }
QPushButton#autoBtn:checked QLabel { color: #fbf1c7; }
QLineEdit#autoMinutes { padding: 2px 4px; border-radius: 6px; }
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
    // Background mode: timed Shuffle / Sequential changes, no window.
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) != QLatin1String("--daemon")) continue;
        // Background housekeeping must never compete with the desktop. Set
        // before any worker thread exists so the threads inherit it.
        setpriority(PRIO_PROCESS, 0, 19);
        QGuiApplication app(argc, argv);
        app.setApplicationName(QStringLiteral("osm-paper"));
        QLockFile lock(QFileInfo(osmPaperConfigPath()).absolutePath()
                       + QStringLiteral("/osm-paper-daemon.lock"));
        lock.setStaleLockTime(0);              // stale only if its owner died
        if (!lock.tryLock(0)) return 0;        // a daemon is already running
        WallpaperDaemon daemon;
        return app.exec();
    }

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
