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
#include <QScreen>
#include <QButtonGroup>
#include <QResizeEvent>
#include <QStyleFactory>
#include <QProcess>
#include <QTimer>
#include <QSplitter>
#include <QFont>
#include <QFrame>
#include <QScroller>
#include <QScrollerProperties>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QVector>
#include <QSettings>
#include <QFile>

class WallpaperBrowser : public QWidget
{
public:
    explicit WallpaperBrowser(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setWindowTitle("Wallpaper Selector");

        // Slightly transparent background + global font (18pt)
        setStyleSheet("background-color: rgba(40,40,40,200); color:white; font-size:18pt;");

        QFont f = font();
        f.setPointSize(18);
        setFont(f);

        loadLastWallpaperSettings();
        loadLastFolder();

        // ────────────────────────────── Top bar ──────────────────────────────
        auto *topLayout  = new QHBoxLayout;
        auto *folderLbl  = new QLabel("Folder:");
        pathEdit         = new QLineEdit;
        auto *browseBtn  = new QPushButton("Browse…");

        topLayout->addWidget(folderLbl);
        topLayout->addWidget(pathEdit);
        topLayout->addWidget(browseBtn);

        // ───────────────── THUMBNAIL GRID (scrollable) ──────────────────
        scrollArea = new QScrollArea;
        scrollArea->setWidgetResizable(false); // needed for QScroller stability
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setAlignment(Qt::AlignHCenter | Qt::AlignTop); // center grid widget

        gridWidget = new QWidget;
        // Opaque-ish background to avoid "ghost" artifacts when scrolling
        gridWidget->setStyleSheet("background-color: rgba(40,40,40,230);");
        gridLayout = new QGridLayout(gridWidget);
        gridLayout->setContentsMargins(10, 10, 10, 10);
        gridLayout->setHorizontalSpacing(10);
        gridLayout->setVerticalSpacing(10);
        scrollArea->setWidget(gridWidget);

        // Smooth kinetic scrolling
        QScroller::grabGesture(scrollArea->viewport(), QScroller::TouchGesture);
        QScroller::grabGesture(scrollArea->viewport(), QScroller::LeftMouseButtonGesture);

        // ───────────────── BUTTON PANEL (2×2) ─────────────────
        zoomBtn   = new QPushButton("Zoom");
        fitBtn    = new QPushButton("Fit");
        tileBtn   = new QPushButton("Tile");
        centerBtn = new QPushButton("Center");

        auto *btnGrid = new QGridLayout;
        btnGrid->setContentsMargins(6,6,6,6);
        btnGrid->setSpacing(6);

        btnGrid->addWidget(zoomBtn,   0,0);
        btnGrid->addWidget(fitBtn,    0,1);
        btnGrid->addWidget(tileBtn,   1,0);
        btnGrid->addWidget(centerBtn, 1,1);

        auto *buttonsFrame = new QFrame;
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
        rootLayout->addLayout(topLayout);
        rootLayout->addWidget(splitter);

        buttonGroup = new QButtonGroup(this);
        buttonGroup->setExclusive(true);

        columns   = 2;                  // default until first width-based recompute
        thumbSize = QSize(260, 180);    // preview size
        cardSize  = QSize(300, 210);    // card size (bigger so rounded corners show)

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

        connect(buttonGroup, &QButtonGroup::idClicked, this, [this](int id) {
            if (id >= 0 && id < imagePaths.size())
                currentImagePath = imagePaths.at(id);
        });

        // transparent preview fade
        auto fade = [this]() {
            setWindowOpacity(0.25);
            QTimer::singleShot(1500, this, [this]() {
                setWindowOpacity(1.0);
            });
        };

        connect(zoomBtn, &QPushButton::clicked, this, [this, fade]() {
            applyWallpaper("zoom", "xwallpaper --zoom");
            fade();
        });
        connect(fitBtn, &QPushButton::clicked, this, [this, fade]() {
            applyWallpaper("fit", "xwallpaper --stretch");
            fade();
        });
        connect(tileBtn, &QPushButton::clicked, this, [this, fade]() {
            applyWallpaper("tile", "xwallpaper --tile");
            fade();
        });
        connect(centerBtn, &QPushButton::clicked, this, [this, fade]() {
            applyWallpaper("center", "xwallpaper --center");
            fade();
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
    QVector<QLabel*>      thumbImageLabels;  // NEW: holds thumbnail labels
    int         nextThumbIndex = 0;

    // Persistence
    QString lastWallpaperPath;
    QString lastMode;
    QString lastFolder;

    // ───────────────────── Persistence ───────────────────────
    void saveLastFolder(const QString &folder)
    {
        QSettings s(QDir::homePath() + "/.config/Alternix/osm-paper.conf", QSettings::IniFormat);
        s.beginGroup("last");
        s.setValue("folder", folder);
        s.endGroup();
        lastFolder = folder;
    }

    void loadLastFolder()
    {
        QSettings s(QDir::homePath() + "/.config/Alternix/osm-paper.conf", QSettings::IniFormat);
        s.beginGroup("last");
        lastFolder = s.value("folder").toString();
        s.endGroup();
    }

    void saveLastWallpaper(const QString &modeName, const QString &path)
    {
        QSettings s(QDir::homePath() + "/.config/Alternix/osm-paper.conf", QSettings::IniFormat);
        s.beginGroup("last");
        s.setValue("wallpaper", path);
        s.setValue("mode", modeName);
        s.endGroup();

        lastWallpaperPath = path;
        lastMode = modeName;
    }

    void loadLastWallpaperSettings()
    {
        QSettings s(QDir::homePath() + "/.config/Alternix/osm-paper.conf", QSettings::IniFormat);
        s.beginGroup("last");
        lastWallpaperPath = s.value("wallpaper").toString();
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

    void applyWallpaper(const QString &modeName,
                        const QString &cmdPrefix)
    {
        if (currentImagePath.isEmpty()) return;

        QString cmd = cmdPrefix + " \"" + currentImagePath + "\"";
        QProcess::startDetached("/bin/sh", {"-c", cmd});

        saveLastWallpaper(modeName, currentImagePath);
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
            if (item->widget()) item->widget()->deleteLater();
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
            return;
        }

        QStringList filters = {"*.png","*.jpg","*.jpeg","*.bmp","*.gif","*.webp"};
        QFileInfoList files =
            dir.entryInfoList(filters, QDir::Files, QDir::Name);

        imagePaths.clear();
        clearGrid();

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
            card->setFlat(true);
            card->setFocusPolicy(Qt::NoFocus);

            card->setStyleSheet(
                "QPushButton {"
                "   background-color: #80708099;"
                "   border-radius: 20px;"
                "   border: none;"
                "}"
                "QPushButton:checked {"
                "   background-color: #282828;"
                "   border: 1px solid white;"
                "}"
            );

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
            thumbLabel->setStyleSheet("color:white;");
            inner->addWidget(thumbLabel);

            // Filename label (wrapped, centred)
            QLabel *nameLabel = new QLabel(QFileInfo(imagePaths[i]).fileName(), card);
            nameLabel->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            nameLabel->setStyleSheet("color:white; font-size:14pt;");
            nameLabel->setWordWrap(true);
            nameLabel->setMinimumHeight(48); // approx 2 lines
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

        // Select last used wallpaper
        int selectedIndex = 0;
        if (!lastWallpaperPath.isEmpty()) {
            int idx = imagePaths.indexOf(lastWallpaperPath);
            if (idx != -1) selectedIndex = idx;
        }

        if (!imagePaths.isEmpty()) {
            if (auto *btn = buttonGroup->button(selectedIndex))
                btn->setChecked(true);
            currentImagePath = imagePaths.at(selectedIndex);
        }

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

        QPixmap pix(path);
        if (!pix.isNull()) {
            QPixmap thumb = pix.scaled(
                thumbSize,
                Qt::KeepAspectRatio,
                Qt::SmoothTransformation);

            thumbLabel->setText("");
            thumbLabel->setPixmap(thumb);

            auto *effect = new QGraphicsOpacityEffect(thumbLabel);
            thumbLabel->setGraphicsEffect(effect);

            auto *anim = new QPropertyAnimation(effect, "opacity", thumbLabel);
            anim->setDuration(300);
            anim->setStartValue(0.0);
            anim->setEndValue(1.0);
            anim->start(QPropertyAnimation::DeleteWhenStopped);
        }
        else {
            thumbLabel->setText("Error");
        }

        ++nextThumbIndex;
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    if (QStyleFactory::keys().contains("kvantum", Qt::CaseInsensitive))
        QApplication::setStyle(QStyleFactory::create("kvantum"));

    WallpaperBrowser w;
    w.resize(720, 1560);
    w.show();

    return app.exec();
}
