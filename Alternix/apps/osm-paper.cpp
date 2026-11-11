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

        auto *buttonsFrame = new QFrame;
        buttonsFrame->setObjectName("card");
        buttonsFrame->setLayout(btnGrid);

        int buttonHeight = 220;
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

        // Async thumbnail loader
        thumbTimer = new QTimer(this);
        thumbTimer->setInterval(40);
        connect(thumbTimer, &QTimer::timeout, this, &WallpaperBrowser::loadNextThumbnail);

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
            if (applyWallpaper("zoom", "xwallpaper --zoom")) fade();
        });
        connect(fitBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("fit", "xwallpaper --stretch")) fade();
        });
        connect(tileBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("tile", "xwallpaper --tile")) fade();
        });
        connect(centerBtn, &QPushButton::clicked, this, [this, fade]() {
            if (applyWallpaper("center", "xwallpaper --center")) fade();
        });

        // Load initial folder
        QString defaultDir = lastFolder.isEmpty() ? defaultStartDir() : lastFolder;
        pathEdit->setText(defaultDir);
        loadImagesFrom(defaultDir);

        applyLastWallpaperOnStartup();
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
    QTimer      *thumbTimer{};

    // State
    QStringList imagePaths;
    QString     currentImagePath;
    int         columns;
    QSize       thumbSize;
    QSize       cardSize;
    QVector<QPushButton*> thumbButtons;
    QVector<QLabel*>      thumbImageLabels;
    int         nextThumbIndex = 0;

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

    QString modeToCommand(const QString &modeName)
    {
        if (modeName == "zoom")   return "xwallpaper --zoom";
        if (modeName == "fit")    return "xwallpaper --stretch";
        if (modeName == "tile")   return "xwallpaper --tile";
        if (modeName == "center") return "xwallpaper --center";
        return "xwallpaper --zoom";
    }

    void applyWallpaperDirect(const QString &modeName, const QString &path)
    {
        if (path.isEmpty()) return;
        QString cmd = modeToCommand(modeName) + " \"" + path + "\"";
        QProcess::startDetached("/bin/sh", {"-c", cmd});
    }

    bool applyWallpaper(const QString &modeName,
                        const QString &cmdPrefix)
    {
        if (currentImagePath.isEmpty()) {
            selectedModeButton(lastMode)->setChecked(true);
            return false;
        }

        QString cmd = cmdPrefix + " \"" + currentImagePath + "\"";
        if (!QProcess::startDetached("/bin/sh", {"-c", cmd})) {
            selectedModeButton(lastMode)->setChecked(true);
            return false;
        }

        saveLastWallpaper(modeName, currentImagePath);
        return true;
    }

    // ───────────────────── Grid management ───────────────────────────

    void clearGrid()
    {
        thumbTimer->stop();
        nextThumbIndex = 0;
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
    }

    void loadImagesFrom(const QString &dirPath)
    {
        thumbTimer->stop();
        nextThumbIndex = 0;
        thumbButtons.clear();
        thumbImageLabels.clear();

        QDir dir(dirPath);
        if (!dir.exists()) {
            clearGrid();
            imagePaths.clear();
            currentImagePath.clear();
            return;
        }

        QStringList filters = {"*.png","*.jpg","*.jpeg","*.bmp","*.gif","*.webp"};
        QFileInfoList files =
            dir.entryInfoList(filters, QDir::Files, QDir::Name);

        imagePaths.clear();
        clearGrid();
        currentImagePath.clear();
        scrollArea->verticalScrollBar()->setValue(0);

        for (const QFileInfo &fi : files)
            imagePaths << fi.absoluteFilePath();

        buildPlaceholdersAndStartLoader();
    }

    void buildPlaceholdersAndStartLoader()
    {
        if (imagePaths.isEmpty()) return;

        for (int i = 0; i < imagePaths.size(); ++i)
        {
            // Card button
            QPushButton *card = new QPushButton;
            card->setCheckable(true);
            card->setFocusPolicy(Qt::NoFocus);
            card->setObjectName("wallpaperCard");
            card->setToolTip(imagePaths.at(i));

            card->setFixedSize(cardSize);

            // Inner layout: thumbnail + filename
            QVBoxLayout *inner = new QVBoxLayout(card);
            inner->setContentsMargins(12, 12, 12, 12);
            inner->setSpacing(6);
            inner->setAlignment(Qt::AlignCenter);

            // Thumbnail label
            QLabel *thumbLabel = new QLabel("Loading…", card);
            thumbLabel->setAlignment(Qt::AlignCenter);
            thumbLabel->setFixedSize(thumbSize);
            // Labels are decorative: they must not steal mouse/touch events
            // from their enclosing button, including while thumbnails fade in.
            thumbLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            inner->addWidget(thumbLabel);

            // Filename label (wrapped, centred)
            QLabel *nameLabel = new QLabel(QFileInfo(imagePaths[i]).fileName(), card);
            nameLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            nameLabel->setObjectName("wallpaperFilename");
            nameLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            nameLabel->setWordWrap(true);
            nameLabel->setFixedHeight(48); // up to two lines, contained within card
            inner->addWidget(nameLabel);

            // Track thumbnail labels so we can set pixmaps later
            thumbImageLabels.append(thumbLabel);

            buttonGroup->addButton(card, i);
            thumbButtons.append(card);
        }

        // FIX: columns are computed from the available width instead of
        // being fixed at 2; relayoutGrid() places the cards.
        columns = computeColumns();
        relayoutGrid();

        // If the applied wallpaper is in this folder, highlight it. Do not
        // mislabel the first wallpaper as current when a different folder opens.
        const int selectedIndex = lastWallpaperPath.isEmpty()
            ? 0 : imagePaths.indexOf(lastWallpaperPath);
        if (selectedIndex >= 0)
            selectWallpaper(selectedIndex);

        nextThumbIndex = 0;
        thumbTimer->start();
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
        // Detach all items from the layout without deleting the widgets.
        QLayoutItem *item;
        while ((item = gridLayout->takeAt(0)))
            delete item;

        int row = 0, col = 0;
        for (QPushButton *card : thumbButtons) {
            gridLayout->addWidget(card, row, col);
            if (++col >= columns) { col = 0; ++row; }
        }

        // Fix the grid widget's width to exactly fit the columns so the
        // scroll area's AlignHCenter keeps it centred.
        int spacing = gridLayout->horizontalSpacing();
        int lm = gridLayout->contentsMargins().left();
        int rm = gridLayout->contentsMargins().right();
        int contentWidth = columns * cardSize.width()
                         + (columns - 1) * spacing
                         + lm + rm;

        gridWidget->setMinimumWidth(contentWidth);
        gridWidget->setMaximumWidth(contentWidth);

        // Height must be allowed to SHRINK too (fewer rows after widening),
        // so reset the minimum before re-measuring.
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

    // ───────────────────── Async thumbnail loader ────────────────────

    void loadNextThumbnail()
    {
        if (nextThumbIndex >= imagePaths.size() ||
            nextThumbIndex >= thumbImageLabels.size()) {
            thumbTimer->stop();
            return;
        }

        QString path = imagePaths.at(nextThumbIndex);
        QLabel *thumbLabel = thumbImageLabels.at(nextThumbIndex);

        // Decode at approximately the displayed size where supported,
        // avoiding the long UI stalls caused by full-resolution wallpapers.
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QSize decodeSize = reader.size();
        if (decodeSize.isValid()) {
            decodeSize.scale(thumbSize, Qt::KeepAspectRatio);
            reader.setScaledSize(decodeSize);
        }
        const QImage decoded = reader.read();
        if (!decoded.isNull()) {
            QPixmap thumb = QPixmap::fromImage(decoded).scaled(
                thumbSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

            thumbLabel->setText("");
            thumbLabel->setPixmap(thumb);

            // Keep the pixmap in QLabel's normal widget paint path.
            // QGraphicsOpacityEffect renders the thumbnail to an off-screen
            // cache: on Qt5, that cache can remain at the old viewport
            // position when QScroller scrolls the QScrollArea by blitting.
            // Removing the effect makes both the card and its image scroll
            // together, including images visible when the swipe begins.
            thumbLabel->update();
        }
        else {
            thumbLabel->setText("Error");
        }

        ++nextThumbIndex;
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
