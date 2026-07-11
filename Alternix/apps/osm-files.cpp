#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScroller>
#include <QDir>
#include <QFileInfo>
#include <QFileIconProvider>
#include <QProcess>
#include <QThread>
#include <QScreen>
#include <QGuiApplication>
#include <QLineEdit>
#include <QInputDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QTimer>
#include <QSet>
#include <QHash>
#include <QVector>
#include <QFile>
#include <QDirIterator>
#include <QEvent>
#include <QMouseEvent>
#include <QLocale>
#include <QImage>
#include <QPixmap>
#include <QSettings>
#include <QListWidget>
#include <QIcon>
#include <QAbstractItemView>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QFileDialog>
#include <QMessageBox>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <functional>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <unistd.h>

class FileBrowser : public QWidget {
public:
    explicit FileBrowser(const QString &startPath, QWidget *parent = nullptr)
        : QWidget(parent),
          currentPath(startPath),
          scroll(nullptr),
          listContainer(nullptr),
          listLayout(nullptr),
          refreshBtn(nullptr),
          backBtn(nullptr),
          homeBtn(nullptr),
          networkBtn(nullptr),
          usbBtn(nullptr),
          usbPollTimer(nullptr),
          pathBtn(nullptr),
          pathMenu(nullptr),
          pathMenuLayout(nullptr),
          viewToggleBtn(nullptr),
          gridMode(false),
          hiddenBtn(nullptr),
          showHidden(false),
          mkdirBtn(nullptr),
          newFileBtn(nullptr),
          copyBtn(nullptr),
          cutBtn(nullptr),
          pasteBtn(nullptr),
          renameBtn(nullptr),
          moveBtn(nullptr),
          deleteBtn(nullptr),
          extractBtn(nullptr),
          openWithBtn(nullptr),
          propsBtn(nullptr),
          multiSelectBtn(nullptr),
          unselectBtn(nullptr),
          multiSelectMode(false),
          clipboardCutMode(false),
          thumbTimer(nullptr),
          statusLabel(nullptr),
          currentItemCount(0),
          shortcutsBtn(nullptr),
          shortcutsPanel(nullptr),
          shortcutsLayout(nullptr),
          addShortcutBtn(nullptr),
          removeShortcutBtn(nullptr),
          shortcutDeleteMode(false),
          settings(nullptr),
          shortcutsAnim(nullptr),
          shortcutsTargetVisible(false)
    {
        setStyleSheet("background:#282828; color:white;");

        // SETTINGS (store in ~/.config/Alternix/osm-files.conf)
        settings = new QSettings(QDir::homePath() + "/.config/Alternix/osm-files.conf",
                                 QSettings::IniFormat);
        loadShortcuts();

        // Card-style list (wide)
        listNormalStyle =
            "QPushButton {"
            " background:#444;"
            " color:white;"
            " border:none;"
            " border-radius:8px;"
            " padding:10px;"
            " font-size:15px;"
            " text-align:left;"
            "}"
            "QPushButton:hover { background:#555; }"
            "QPushButton:pressed { background:#333; }";

        listSelectedStyle =
            "QPushButton {"
            " background:#777;"
            " color:white;"
            " border:none;"
            " border-radius:8px;"
            " padding:10px;"
            " font-size:15px;"
            " text-align:left;"
            "}"
            "QPushButton:hover { background:#888; }"
            "QPushButton:pressed { background:#666; }";

        // Card-style grid (tiles)
        gridNormalStyle =
            "QPushButton {"
            " background:#3a3a3a;"
            " color:white;"
            " border:none;"
            " border-radius:12px;"
            " padding:10px;"
            " font-size:15px;"
            " text-align:center;"
            "}"
            "QPushButton:hover { background:#4a4a4a; }"
            "QPushButton:pressed { background:#2a2a2a; }";

        gridSelectedStyle =
            "QPushButton {"
            " background:#6a6a6a;"
            " color:white;"
            " border:none;"
            " border-radius:12px;"
            " padding:10px;"
            " font-size:15px;"
            " text-align:center;"
            "}"
            "QPushButton:hover { background:#7a7a7a; }"
            "QPushButton:pressed { background:#5a5a5a; }";

        // Start in list mode styles
        currentNormalStyle = listNormalStyle;
        currentSelectedStyle = listSelectedStyle;

        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(20,20,20,20);
        root->setSpacing(10);

        // ========== FIRST ROW (Back, Refresh, Home, Path, View Toggle, Shortcuts) ==========
        QHBoxLayout *pathRow = new QHBoxLayout;
        pathRow->setSpacing(10);

        // Back button
        backBtn = new QPushButton("⇑");
        backBtn->setFixedSize(50, 50);
        backBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; font-weight:bold; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
        pathRow->addWidget(backBtn, 0);

        // Refresh button
        refreshBtn = new QPushButton("⟳");
        refreshBtn->setFixedSize(50, 50);
        refreshBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
        pathRow->addWidget(refreshBtn, 0);

        // Home button
        homeBtn = new QPushButton("🏡");
        homeBtn->setFixedSize(50, 50);
        homeBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
        pathRow->addWidget(homeBtn, 0);

        connect(homeBtn, &QPushButton::clicked, this, [this]() {
            listDirectory(QDir::homePath());
        });

        // Network button
        networkBtn = new QPushButton("🌐");
        networkBtn->setFixedSize(50, 50);
        networkBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
        pathRow->addWidget(networkBtn, 0);

        connect(networkBtn, &QPushButton::clicked, this, &FileBrowser::showNetworkDialog);

        // USB button
        usbBtn = new QPushButton("🖴");
        usbBtn->setFixedSize(50, 50);
        usbBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
        pathRow->addWidget(usbBtn, 0);

        connect(usbBtn, &QPushButton::clicked, this, &FileBrowser::showUsbDialog);

        // Hotplug poll: check every 3s for newly attached USB devices
        usbPollTimer = new QTimer(this);
        usbPollTimer->setInterval(3000);
        connect(usbPollTimer, &QTimer::timeout, this, &FileBrowser::pollUsbDevices);
        knownUsbDevices = enumerateUsbDevicePaths();   // baseline, no flash on startup
        usbPollTimer->start();

        // Path dropdown button
        pathBtn = new QPushButton(currentPath);
        pathBtnNormalStyle =
            "QPushButton { background:#333; color:#DDDDDD; border-radius:8px; "
            "padding:10px; font-size:15px; text-align:left; }"
            "QPushButton:hover { background:#444; }"
            "QPushButton:pressed { background:#222; }";
        pathBtn->setStyleSheet(pathBtnNormalStyle);
        pathBtn->setMinimumHeight(50);
        pathBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        pathRow->addWidget(pathBtn, 1);

        // View mode toggle
        viewToggleBtn = new QPushButton("☴");
        viewToggleBtn->setFixedSize(50, 50);
        viewToggleBtn->setCheckable(true);
        viewToggleBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
            "QPushButton:checked { background:#2a82da; }"
        );
        pathRow->addWidget(viewToggleBtn, 0);

        // Shortcuts button
        shortcutsBtn = new QPushButton("⭐");
        shortcutsBtn->setFixedSize(50, 50);
        shortcutsBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; "
            "border-radius:10px; font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
        pathRow->addWidget(shortcutsBtn, 0);

        root->addLayout(pathRow);

        // ========= TOP TOOLBAR ROW (Hidden, NewDir, NewFile, Copy, Cut, Paste...) =========
        QHBoxLayout *bar = new QHBoxLayout;
        bar->setSpacing(10);

        auto makeTopButton = [&](const QString &text) -> QPushButton* {
            QPushButton *b = new QPushButton(text);
            b->setFixedHeight(40);
            b->setMinimumWidth(60);
            b->setStyleSheet(
                "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:14px; }"
                "QPushButton:hover:enabled { background:#666; }"
                "QPushButton:pressed:enabled { background:#444; }"
                "QPushButton:disabled { background:#222; color:#555; }"
                "QPushButton:checked { background:#2a82da; color:white; border:3px solid #ffffff; }"
            );
            return b;
        };

        hiddenBtn      = makeTopButton("Hidden");
        mkdirBtn       = makeTopButton("NewDir");
        newFileBtn     = makeTopButton("NewFile");
        copyBtn        = makeTopButton("Copy");
        cutBtn         = makeTopButton("Cut");
        pasteBtn       = makeTopButton("Paste");
        renameBtn      = makeTopButton("Rename");
        moveBtn        = makeTopButton("Move");

        // DELETE BUTTON (red when enabled)
        deleteBtn = new QPushButton("Delete");
        deleteBtn->setFixedHeight(40);
        deleteBtn->setMinimumWidth(60);
        deleteBtn->setStyleSheet(
            "QPushButton { background:#222; color:#555; border:none; border-radius:10px; font-size:14px; }"
            "QPushButton:pressed:enabled { background:#aa0000; }"
            "QPushButton:hover:enabled { background:#dd3333; }"
        );

        extractBtn     = makeTopButton("Extract");
        openWithBtn    = makeTopButton("OpenWith");
        propsBtn       = makeTopButton("Details");
        multiSelectBtn = makeTopButton("Select");
        unselectBtn    = makeTopButton("Unselect");

        hiddenBtn->setCheckable(true);
        multiSelectBtn->setCheckable(true);

        bar->addWidget(hiddenBtn, 0);
        bar->addWidget(mkdirBtn, 0);
        bar->addWidget(newFileBtn, 0);
        bar->addWidget(copyBtn, 0);
        bar->addWidget(cutBtn, 0);
        bar->addWidget(pasteBtn, 0);
        bar->addWidget(renameBtn, 0);
        bar->addWidget(moveBtn, 0);
        bar->addWidget(deleteBtn, 0);
        bar->addWidget(extractBtn, 0);
        bar->addWidget(openWithBtn, 0);
        bar->addWidget(propsBtn, 0);
        bar->addWidget(multiSelectBtn, 0);
        bar->addWidget(unselectBtn, 0);

        QWidget *btnContainer = new QWidget;
        btnContainer->setFixedHeight(60);
        btnContainer->setStyleSheet("background:transparent;");
        btnContainer->setLayout(bar);

        QScrollArea *btnScroll = new QScrollArea;
        QScroller::grabGesture(btnScroll, QScroller::LeftMouseButtonGesture);
        btnScroll->setWidgetResizable(true);
        btnScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        btnScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        btnScroll->setFrameShape(QFrame::NoFrame);
        btnScroll->setStyleSheet("background:transparent;");
        btnScroll->setWidget(btnContainer);

        btnScroll->setStyleSheet("QScrollArea { padding:0; margin:0; border:0; }");
        btnScroll->widget()->setContentsMargins(0,0,0,0);
        btnScroll->viewport()->setContentsMargins(0,0,0,0);
        bar->setContentsMargins(0,0,0,0);

        root->addWidget(btnScroll);
        btnScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        // --- File list scroll area ---
        scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setStyleSheet("background:#282828; border:none;");
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(scroll, QScroller::LeftMouseButtonGesture);

        listContainer = new QWidget;
        listLayout = new QVBoxLayout(listContainer);
        listLayout->setContentsMargins(0,10,0,10);
        listLayout->setSpacing(10);

        scroll->setWidget(listContainer);
        root->addWidget(scroll);

        // --- Status bar ---
        QHBoxLayout *statusRow = new QHBoxLayout;
        statusRow->setSpacing(5);
        statusLabel = new QLabel("0 items");
        statusLabel->setStyleSheet("QLabel { color:#CCCCCC; font-size:12px; }");
        statusRow->addWidget(statusLabel, 1);
        root->addLayout(statusRow);

        // Thumbnail timer
        thumbTimer = new QTimer(this);
        thumbTimer->setInterval(45);
        connect(thumbTimer, &QTimer::timeout, this, &FileBrowser::processNextThumbnail);

        // Path Menu
        pathMenu = new QWidget(this, Qt::Popup);
        pathMenu->setStyleSheet("background:#222; border:2px solid #555; border-radius:14px;");
        pathMenuLayout = new QVBoxLayout(pathMenu);
        pathMenuLayout->setContentsMargins(10,10,10,10);
        pathMenuLayout->setSpacing(6);

        // SHORTCUTS PANEL (floating overlay)
        shortcutsPanel = new QWidget(this, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        shortcutsPanel->setFixedWidth(320);
        shortcutsPanel->setStyleSheet(
            "background:rgba(30,30,30,0.92); border-left:3px solid #444;"
        );
        shortcutsPanel->hide();

        shortcutsLayout = new QVBoxLayout(shortcutsPanel);
        shortcutsLayout->setContentsMargins(20,20,20,20);
        shortcutsLayout->setSpacing(10);

        // Rebuild list before adding bottom buttons
        rebuildShortcutsPanel();

        // Bottom + / bin buttons
        QHBoxLayout *bottomBtns = new QHBoxLayout;
        addShortcutBtn = new QPushButton("+");
        addShortcutBtn->setFixedHeight(60);
        addShortcutBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border-radius:12px; "
            "font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );

        removeShortcutBtn = new QPushButton("🗑️");
        removeShortcutBtn->setFixedHeight(60);
        removeShortcutBtn->setCheckable(true);
        removeShortcutBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border-radius:12px; "
            "font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:checked { background:#aa0000; }"
        );

        bottomBtns->addWidget(addShortcutBtn);
        bottomBtns->addWidget(removeShortcutBtn);
        shortcutsLayout->addLayout(bottomBtns);

        // Animation for shortcuts panel
        shortcutsAnim = new QPropertyAnimation(shortcutsPanel, "geometry", this);
        shortcutsAnim->setDuration(150);
        shortcutsAnim->setEasingCurve(QEasingCurve::OutCubic);
        shortcutsTargetVisible = false;
        connect(shortcutsAnim, &QPropertyAnimation::finished, this, [this]() {
            if (!shortcutsTargetVisible && shortcutsPanel)
                shortcutsPanel->hide();
        });

        // ===== Connections =====

        connect(shortcutsBtn, &QPushButton::clicked, this, [this]() {
            slideShortcutsPanel(!shortcutsPanel->isVisible());
        });

        connect(addShortcutBtn, &QPushButton::clicked, this, [this]() {
            QString dir = QFileDialog::getExistingDirectory(
                this, "Select Folder", QDir::homePath()
            );
            if (!dir.isEmpty()) {
                if (!shortcutsList.contains(dir)) {
                    shortcutsList.append(dir);
                    saveShortcuts();
                    rebuildShortcutsPanel();
                }
            }
        });

        connect(removeShortcutBtn, &QPushButton::toggled, this,
                [this](bool on){ shortcutDeleteMode = on; });

        connect(refreshBtn, &QPushButton::clicked, this, [this]() {
            listDirectory(currentPath);
        });

        connect(backBtn, &QPushButton::clicked, this, [this]() {
            QDir dir(currentPath);
            QString parent = dir.absolutePath();
            if (parent == "/" || parent == dir.rootPath()) {
                qApp->quit();
                return;
            }
            dir.cdUp();
            listDirectory(dir.absolutePath());
        });

        connect(viewToggleBtn, &QPushButton::toggled, this, [this](bool checked) {
            gridMode = checked;
            viewToggleBtn->setText(checked ? "☷" : "☴");
            currentNormalStyle  = gridMode ? gridNormalStyle  : listNormalStyle;
            currentSelectedStyle = gridMode ? gridSelectedStyle : listSelectedStyle;
            listDirectory(currentPath);
        });

        connect(pathBtn, &QPushButton::clicked, this, [this]() {
            rebuildPathMenu();
            QPoint pos = pathBtn->mapToGlobal(QPoint(0, pathBtn->height()));
            pathMenu->resize(pathBtn->width(), pathMenu->sizeHint().height());
            pathMenu->move(pos);
            pathMenu->show();
        });

        connect(copyBtn,      &QPushButton::clicked, this, &FileBrowser::copySelection);
        connect(cutBtn,       &QPushButton::clicked, this, &FileBrowser::cutSelection);
        connect(pasteBtn,     &QPushButton::clicked, this, &FileBrowser::pasteClipboard);
        connect(renameBtn,    &QPushButton::clicked, this, &FileBrowser::renameSelection);
        connect(moveBtn,      &QPushButton::clicked, this, &FileBrowser::moveSelection);
        connect(deleteBtn,    &QPushButton::clicked, this, &FileBrowser::deleteSelection);
        connect(propsBtn,     &QPushButton::clicked, this, &FileBrowser::showPropertiesDialog);
        connect(openWithBtn,  &QPushButton::clicked, this, &FileBrowser::openWithSelection);
        connect(mkdirBtn,     &QPushButton::clicked, this, &FileBrowser::createDirectory);
        connect(newFileBtn,   &QPushButton::clicked, this, &FileBrowser::createNewFile);
        connect(extractBtn,   &QPushButton::clicked, this, &FileBrowser::extractSelection);

        connect(multiSelectBtn, &QPushButton::toggled, this, [this](bool checked) {
            multiSelectMode = checked;
            if (!checked) clearSelection(false);
            updateActionButtons();
        });

        connect(unselectBtn, &QPushButton::clicked, this, [this]() {
            clearSelection(true);
            updateActionButtons();
        });

        connect(hiddenBtn, &QPushButton::toggled, this, [this](bool checked) {
            showHidden = checked;
            listDirectory(currentPath);
        });

        // Initial position for shortcuts panel (off-screen to the right)
        if (shortcutsPanel) {
            int w = shortcutsPanel->width();
            QPoint topRight = this->mapToGlobal(QPoint(width(), 0));
            shortcutsPanel->setGeometry(topRight.x(), topRight.y(), w, height());
        }

        listDirectory(currentPath);
    }

protected:
    bool event(QEvent *e) override {
        if (e->type() == QEvent::ToolTip)
            return true;
        return QWidget::event(e);
    }

    bool eventFilter(QObject *w, QEvent *e) override {
        // This eventFilter is used only for file buttons (long-press).
        QPushButton *btn = qobject_cast<QPushButton*>(w);
        if (!btn) return QWidget::eventFilter(w,e);
        if (!btn->property("fullPath").isValid()) return QWidget::eventFilter(w,e);

        if (e->type() == QEvent::MouseButtonPress) {
            QMouseEvent *me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::LeftButton) {
                QTimer *t = new QTimer(this);
                t->setSingleShot(true);
                connect(t,&QTimer::timeout,this,[this,btn](){
                    holdTimers.remove(btn);
                    handleLongPress(btn);
                });
                holdTimers.insert(btn,t);
                btn->setProperty("longPressTriggered",false);
                t->start(600);
            }
        } else if (e->type()==QEvent::MouseButtonRelease ||
                   e->type()==QEvent::MouseMove) {
            auto it = holdTimers.find(btn);
            if (it != holdTimers.end()) {
                it.value()->stop();
                it.value()->deleteLater();
                holdTimers.erase(it);
            }
        }

        return QWidget::eventFilter(w,e);
    }

    void mousePressEvent(QMouseEvent *e) override {
        // Clicking anywhere in the main window (outside the shortcuts panel) closes it
        if (shortcutsPanel && shortcutsPanel->isVisible()) {
            QPoint globalPos = e->globalPos();
            if (!shortcutsPanel->geometry().contains(globalPos)) {
                slideShortcutsPanel(false);
                if (removeShortcutBtn) {
                    removeShortcutBtn->setChecked(false);
                    shortcutDeleteMode = false;
                }
            }
        }
        QWidget::mousePressEvent(e);
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        if (!shortcutsPanel) return;

        int w = shortcutsPanel->width();
        QPoint topRight = this->mapToGlobal(QPoint(width(), 0));

        if (shortcutsPanel->isVisible() && shortcutsTargetVisible) {
            shortcutsPanel->setGeometry(topRight.x() - w, topRight.y(), w, height());
        } else {
            shortcutsPanel->setGeometry(topRight.x(), topRight.y(), w, height());
        }
    }

private:
    QString currentPath;
    QScrollArea *scroll;
    QWidget *listContainer;
    QVBoxLayout *listLayout;

    QPushButton *refreshBtn;
    QPushButton *backBtn;
    QPushButton *homeBtn;
    QPushButton *networkBtn;
    QPushButton *usbBtn;
    QTimer *usbPollTimer;
    QSet<QString> knownUsbDevices;
    QStringList savedServers;
    QPushButton *pathBtn;
    QWidget *pathMenu;
    QVBoxLayout *pathMenuLayout;

    QPushButton *viewToggleBtn;
    bool gridMode;

    QPushButton *hiddenBtn;
    bool showHidden;

    QPushButton *mkdirBtn;
    QPushButton *newFileBtn;
    QPushButton *copyBtn;
    QPushButton *cutBtn;
    QPushButton *pasteBtn;
    QPushButton *renameBtn;
    QPushButton *moveBtn;
    QPushButton *deleteBtn;
    QPushButton *extractBtn;
    QPushButton *openWithBtn;
    QPushButton *propsBtn;
    QPushButton *multiSelectBtn;
    QPushButton *unselectBtn;

    // Shortcuts overlay
    QPushButton *shortcutsBtn;
    QWidget *shortcutsPanel;
    QVBoxLayout *shortcutsLayout;
    QPushButton *addShortcutBtn;
    QPushButton *removeShortcutBtn;
    bool shortcutDeleteMode;
    QSettings *settings;
    QStringList shortcutsList;
    QPropertyAnimation *shortcutsAnim;
    bool shortcutsTargetVisible;

    QString listNormalStyle;
    QString listSelectedStyle;
    QString gridNormalStyle;
    QString gridSelectedStyle;
    QString currentNormalStyle;
    QString currentSelectedStyle;
    QString pathBtnNormalStyle;
    QElapsedTimer planThrottle;

    QHash<QString, QPushButton*> pathToButton;
    QSet<QString> selectedPaths;
    bool multiSelectMode;

    QStringList clipboardPaths;
    bool clipboardCutMode;

    QHash<QObject*, QTimer*> holdTimers;

    QVector<QPushButton*> imageButtons;
    QTimer *thumbTimer;

    QLabel *statusLabel;
    int currentItemCount;

    // For grid-mode thumbnails and labels
    QHash<QPushButton*, QLabel*> gridThumbLabel;
    QHash<QPushButton*, QLabel*> gridNameLabel;
    // ---- Helpers ----
    static bool isImageFile(const QString &fileName) {
        QString ext = QFileInfo(fileName).suffix().toLower();
        return ext == "png" || ext == "jpg" || ext == "jpeg" ||
               ext == "bmp" || ext == "gif" || ext == "webp";
    }

    static bool isArchiveFilePath(const QString &filePath) {
        QString lower = filePath.toLower();
        return lower.endsWith(".zip") ||
               lower.endsWith(".tar") ||
               lower.endsWith(".tar.gz") ||
               lower.endsWith(".tgz") ||
               lower.endsWith(".tar.xz") ||
               lower.endsWith(".tar.bz2");
    }

    static QString quoteFilePath(const QString &path) {
        QString p = path;
        p.replace("\"", "\\\"");
        return "\"" + p + "\"";
    }

    static QString buildExecCommand(const QString &tmpl, const QString &filePath) {
        QString cmd = tmpl;
        QString quoted = quoteFilePath(filePath);

        cmd.replace("%f", quoted);
        cmd.replace("%F", quoted);
        cmd.replace("%u", quoted);
        cmd.replace("%U", quoted);

        cmd.replace("%i", "");
        cmd.replace("%c", "");
        cmd.replace("%k", "");

        return cmd;
    }

    struct DesktopApp {
        QString name;
        QString exec;
        QString icon;
    };

    static QVector<DesktopApp> loadDesktopApps() {
        QVector<DesktopApp> apps;
        QStringList dirs;
        dirs << "/usr/share/applications"
             << QDir::homePath() + "/.local/share/applications";

        for (const QString &dirPath : dirs) {
            QDir d(dirPath);
            if (!d.exists())
                continue;

            QStringList files = d.entryList(QStringList() << "*.desktop", QDir::Files);
            for (const QString &file : files) {
                QString full = d.absoluteFilePath(file);
                QSettings s(full, QSettings::IniFormat);
                s.beginGroup("Desktop Entry");
                QString name = s.value("Name").toString();
                QString exec = s.value("Exec").toString();
                QString icon = s.value("Icon").toString();
                s.endGroup();

                if (name.isEmpty() || exec.isEmpty())
                    continue;

                DesktopApp app;
                app.name = name;
                app.exec = exec;
                app.icon = icon;
                apps.push_back(app);
            }
        }

        return apps;
    }

    bool selectedSingleIsFile() const {
        if (selectedPaths.size() != 1)
            return false;
        QString p = *selectedPaths.begin();
        return QFileInfo(p).isFile();
    }

    // ================= SHORTCUTS =====================
    void loadShortcuts() {
        shortcutsList.clear();
        if (settings) {
            settings->beginGroup("Shortcuts");
            shortcutsList = settings->value("paths").toStringList();
            settings->endGroup();
        }

        if (shortcutsList.isEmpty()) {
            QString home = QDir::homePath();
            QString doc = home + "/Documents";
            QString dl  = home + "/Downloads";
            QString pic = home + "/Pictures";

            if (QDir(doc).exists()) shortcutsList << doc;
            if (QDir(dl).exists())  shortcutsList << dl;
            if (QDir(pic).exists()) shortcutsList << pic;
        }
    }

    void saveShortcuts() {
        if (!settings) return;
        settings->beginGroup("Shortcuts");
        settings->setValue("paths", shortcutsList);
        settings->endGroup();
        settings->sync();
    }

    void rebuildShortcutsPanel() {
        if (!shortcutsLayout || !shortcutsPanel) return;

        // Preserve bottom "+ / bin" bar if it already exists
        int count = shortcutsLayout->count();
        int bottomIndex = -1;

        if (count > 0 && addShortcutBtn && removeShortcutBtn) {
            QLayoutItem *lastItem = shortcutsLayout->itemAt(count - 1);
            if (lastItem) {
                QLayout *sub = lastItem->layout();
                if (sub) {
                    for (int i = 0; i < sub->count(); ++i) {
                        QWidget *w = sub->itemAt(i)->widget();
                        if (w == addShortcutBtn) {
                            bottomIndex = count - 1;
                            break;
                        }
                    }
                }
            }
        }

        QLayoutItem *bottomItem = nullptr;
        if (bottomIndex >= 0) {
            bottomItem = shortcutsLayout->takeAt(bottomIndex);
        }

        while (shortcutsLayout->count() > 0) {
            QLayoutItem *it = shortcutsLayout->takeAt(0);
            if (!it) continue;
            if (it->widget()) it->widget()->deleteLater();
            if (it->layout()) delete it->layout();
            delete it;
        }

        for (const QString &path : shortcutsList) {
            QString labelText;
            QDir d(path);
            QString base = d.dirName();
            if (base.isEmpty())
                base = path;

            QString home = QDir::homePath();
            if (path == home + "/Documents")      labelText = "📄 Documents";
            else if (path == home + "/Downloads") labelText = "📥 Downloads";
            else if (path == home + "/Pictures")  labelText = "🖼️ Pictures";
            else                                  labelText = "📁 " + base;

            QPushButton *b = new QPushButton(labelText, shortcutsPanel);
            b->setStyleSheet(
                "QPushButton { background:#333; color:white; border:none; "
                "border-radius:14px; padding:10px; font-size:15px; text-align:left; }"
                "QPushButton:hover { background:#444; }"
                "QPushButton:pressed { background:#222; }"
            );
            b->setProperty("shortcutPath", path);

            connect(b, &QPushButton::clicked, this, [this, path]() {
                if (shortcutDeleteMode) {
                    shortcutsList.removeAll(path);
                    saveShortcuts();
                    rebuildShortcutsPanel();
                } else {
                    listDirectory(path);
                    slideShortcutsPanel(false);
                    if (removeShortcutBtn) {
                        removeShortcutBtn->setChecked(false);
                        shortcutDeleteMode = false;
                    }
                }
            });

            shortcutsLayout->addWidget(b);
        }

        if (bottomItem) {
            shortcutsLayout->addItem(bottomItem);
        }
    }

    void slideShortcutsPanel(bool show) {
        if (!shortcutsPanel || !shortcutsAnim) return;

        int panelW = shortcutsPanel->width();
        QPoint topRight = this->mapToGlobal(QPoint(width(), 0));

        QRect startRect, endRect;

        if (show) {
            shortcutsPanel->show();
            shortcutsPanel->raise();
            startRect = QRect(topRight.x(), topRight.y(), panelW, height());
            endRect   = QRect(topRight.x() - panelW, topRight.y(), panelW, height());
        } else {
            startRect = shortcutsPanel->geometry();
            QPoint offRight = this->mapToGlobal(QPoint(width(), 0));
            endRect   = QRect(offRight.x(), offRight.y(), panelW, height());
        }

        shortcutsTargetVisible = show;
        shortcutsAnim->stop();
        shortcutsAnim->setStartValue(startRect);
        shortcutsAnim->setEndValue(endRect);
        shortcutsAnim->start();
    }

    void updateStatusBar() {
        if (!statusLabel) return;
        int total = currentItemCount;
        int sel = selectedPaths.size();
        QString text = QString("%1 item%2").arg(total).arg(total == 1 ? "" : "s");
        if (sel > 0) text += QString(" — %1 selected").arg(sel);
        statusLabel->setText(text);
    }

    void clearList() {
        if (thumbTimer && thumbTimer->isActive()) thumbTimer->stop();

        for (auto it = holdTimers.begin(); it != holdTimers.end(); ++it) {
            if (it.value()) {
                it.value()->stop();
                it.value()->deleteLater();
            }
        }
        holdTimers.clear();

        imageButtons.clear();
        gridThumbLabel.clear();
        gridNameLabel.clear();

        while (QLayoutItem *i = listLayout->takeAt(0)) {
            if (QWidget *w = i->widget()) w->deleteLater();
            delete i;
        }

        pathToButton.clear();
        selectedPaths.clear();
    }

    int calculateGridColumns() const {
        int w = scroll->viewport()->width();
        if (w <= 0) return 2;

        if (w < 360) return 2;
        if (w < 720) return 3;
        return 4;
    }

    QPushButton* createFileButton(const QFileInfo &fi, QFont &entryFont) {
        bool isDir = fi.isDir();
        QString name = fi.fileName();
        QString fullPath = fi.absoluteFilePath();
        bool isImg = (!isDir && isImageFile(name));

        QPushButton *btn = new QPushButton;
        btn->setFont(entryFont);
        btn->setStyleSheet(currentNormalStyle);

        if (gridMode) {
            btn->setMinimumHeight(220);
            btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

            QVBoxLayout *v = new QVBoxLayout(btn);
            v->setContentsMargins(8,8,8,8);
            v->setSpacing(6);

            QLabel *thumb = new QLabel(btn);
            thumb->setAlignment(Qt::AlignCenter);
            thumb->setMinimumHeight(140);
            thumb->setStyleSheet("background:transparent;");

            QLabel *nameLbl = new QLabel(name, btn);
            nameLbl->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            nameLbl->setWordWrap(true);
            nameLbl->setStyleSheet("background:transparent; font-size:20px;");
            nameLbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

            v->addWidget(thumb);
            v->addWidget(nameLbl);

            QFont iconFont = thumb->font();
            iconFont.setPointSize(40);
            thumb->setFont(iconFont);

            if (isDir) {
                thumb->setText("📁");
            } else if (isImg) {
                thumb->setText("⏳");
            } else {
                thumb->setText("📄");
            }

            gridThumbLabel.insert(btn, thumb);
            gridNameLabel.insert(btn, nameLbl);

        } else {
            QString display;
            if (isDir)
                display = QString("📁  %1").arg(name);
            else if (isImg)
                display = QString("⏳  %1").arg(name);
            else
                display = QString("📄  %1").arg(name);

            btn->setText(display);
            btn->setMinimumHeight(90);
        }

        btn->setProperty("fullPath", fullPath);
        btn->setProperty("isDir", isDir);
        btn->setProperty("isImage", isImg);
        btn->setProperty("baseName", name);
        btn->setProperty("thumbDone", false);
        btn->setProperty("longPressTriggered", false);

        btn->installEventFilter(this);

        connect(btn,&QPushButton::clicked,this,[this,btn](){
            QString p = btn->property("fullPath").toString();
            bool isDir = btn->property("isDir").toBool();
            bool lp = btn->property("longPressTriggered").toBool();

            if (lp) {
                btn->setProperty("longPressTriggered",false);
                return;
            }

            if (multiSelectMode) {
                toggleSelection(p);
            } else {
                if (isDir) listDirectory(p);
                else QProcess::startDetached("osm-viewer", QStringList() << p);
            }
        });

        pathToButton.insert(fullPath, btn);
        if (isImg) imageButtons.append(btn);

        return btn;
    }

    void rebuildPathMenu() {
        if (!pathMenu || !pathMenuLayout) return;

        while (QLayoutItem *it = pathMenuLayout->takeAt(0)) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }

        QStringList parts = currentPath.split("/", Qt::SkipEmptyParts);
        QString accum = "/";

        auto makeEntry = [this](const QString &label, const QString &path) {
            QPushButton *b = new QPushButton(label, pathMenu);
            b->setStyleSheet(
                "QPushButton { background:#444; color:white; border:none; border-radius:10px; "
                "padding:16px; font-size:15px; text-align:left;}"
                "QPushButton:hover { background:#555; }"
                "QPushButton:pressed { background:#333; }"
            );
            connect(b, &QPushButton::clicked, this, [this, path]() {
                pathMenu->hide();
                listDirectory(path);
            });
            pathMenuLayout->addWidget(b);
        };

        makeEntry("/", "/");

        for (const QString &p : parts) {
            if (accum != "/") accum += "/";
            accum += p;
            makeEntry(p, accum);
        }
    }

    void listDirectory(const QString &path) {
        QDir dir(path);
        if (!dir.exists()) return;

        currentPath = dir.absolutePath();
        pathBtn->setText(currentPath);
        rebuildPathMenu();

        clearList();

        // gvfs FUSE entries can stat oddly; include QDir::System so they
        // are not silently filtered out of the listing
        QDir::Filters extra = path.startsWith(gvfsRoot())
                                  ? QDir::System : QDir::Filters();

        if (showHidden)
            dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | extra);
        else
            dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | extra);

        dir.setSorting(QDir::DirsFirst | QDir::IgnoreCase);

        QFileInfoList list = dir.entryInfoList();
        currentItemCount = list.size();

        QFont entryFont("DejaVu Sans");
        entryFont.setPointSize(gridMode ? 22 : 26);

        if (!gridMode) {
            for (const QFileInfo &fi : list) {
                listLayout->addWidget(createFileButton(fi, entryFont));
            }
        } else {
            QWidget *gridContainer = new QWidget;
            QGridLayout *grid = new QGridLayout(gridContainer);
            grid->setContentsMargins(0,0,0,0);
            grid->setSpacing(10);

            int cols = calculateGridColumns();
            int row = 0, col = 0;

            for (const QFileInfo &fi : list) {
                QPushButton *btn = createFileButton(fi, entryFont);
                grid->addWidget(btn, row, col);
                col++;
                if (col >= cols) {
                    col = 0;
                    row++;
                }
            }

            listLayout->addWidget(gridContainer);
        }

        listLayout->addStretch(1);

        if (!imageButtons.isEmpty())
            thumbTimer->start();

        updateActionButtons();
        updateStatusBar();
    }

    void handleLongPress(QPushButton *btn) {
        if (!btn) return;
        QString p = btn->property("fullPath").toString();
        if (p.isEmpty()) return;

        multiSelectMode = true;
        multiSelectBtn->setChecked(true);
        btn->setProperty("longPressTriggered",true);

        if (!selectedPaths.contains(p)) {
            selectedPaths.insert(p);
            applySelectionStyle(p,true);
        }
        updateActionButtons();
        updateStatusBar();
    }

    void applySelectionStyle(const QString &p, bool sel) {
        if (pathToButton.contains(p)) {
            QPushButton *b = pathToButton[p];
            b->setStyleSheet(sel ? currentSelectedStyle : currentNormalStyle);
        }
    }

    void toggleSelection(const QString &p) {
        if (selectedPaths.contains(p)) {
            selectedPaths.remove(p);
            applySelectionStyle(p,false);
        } else {
            selectedPaths.insert(p);
            applySelectionStyle(p,true);
        }

        if (multiSelectMode && selectedPaths.isEmpty()) {
            multiSelectMode = false;
            multiSelectBtn->setChecked(false);
        }

        updateActionButtons();
        updateStatusBar();
    }

    void clearSelection(bool resetMulti) {
        for (const QString &p : selectedPaths)
            applySelectionStyle(p,false);
        selectedPaths.clear();

        if (resetMulti) {
            multiSelectMode = false;
            multiSelectBtn->setChecked(false);
        }
        updateActionButtons();
        updateStatusBar();
    }

    void updateDeleteButton(bool enabled) {
        if (enabled) {
            deleteBtn->setEnabled(true);
            deleteBtn->setStyleSheet(
                "QPushButton { background:#cc0000; color:white; border:none; "
                "border-radius:10px; font-size:18px; }"
                "QPushButton:hover { background:#dd3333; }"
                "QPushButton:pressed { background:#aa0000; }"
            );
        } else {
            deleteBtn->setEnabled(false);
            deleteBtn->setStyleSheet(
                "QPushButton { background:#222; color:#555; border:none; "
                "border-radius:10px; font-size:18px; }"
            );
        }
    }

    void updateActionButtons() {
        int n = selectedPaths.size();
        bool hasSel = n > 0;
        bool hasClip = !clipboardPaths.isEmpty();
        bool singleFileSel = selectedSingleIsFile();

        pasteBtn->setEnabled(hasClip);

        bool canExtract = false;
        if (singleFileSel) {
            QString p = *selectedPaths.begin();
            canExtract = isArchiveFilePath(p);
        }

        if (!multiSelectMode) {
            copyBtn->setEnabled(false);
            cutBtn->setEnabled(false);
            updateDeleteButton(false);
            renameBtn->setEnabled(false);
            moveBtn->setEnabled(false);
            propsBtn->setEnabled(false);
            unselectBtn->setEnabled(false);
            openWithBtn->setEnabled(false);
            extractBtn->setEnabled(false);
            return;
        }

        copyBtn->setEnabled(hasSel);
        cutBtn->setEnabled(hasSel);
        updateDeleteButton(hasSel);
        renameBtn->setEnabled(n == 1);
        moveBtn->setEnabled(hasSel);
        propsBtn->setEnabled(hasSel);
        unselectBtn->setEnabled(true);
        openWithBtn->setEnabled(singleFileSel);
        extractBtn->setEnabled(canExtract);
    }

    QStringList selectedPathList() const {
        return QStringList(selectedPaths.begin(), selectedPaths.end());
    }

    // Live feedback during the planning tree-walk. On big folders the scan
    // itself is the slow phase (before any step runs), and previously it
    // showed nothing — making a folder delete look like a silent freeze.
    void planTick(const QString &label, int count) {
        if (planThrottle.elapsed() < 50) return;
        pathBtn->setText(QString("%1  (scanning… %2 items)").arg(label).arg(count));
        QCoreApplication::processEvents();
        planThrottle.restart();
    }

    // Build an ordered list of copy steps (dirs created before their contents,
    // so each step can run independently and still produce a valid tree).
    void planCopy(const QString &src, const QString &dst,
                  QVector<std::function<void()>> &steps, const QString &label) {
        QFileInfo s(src);
        if (s.isDir()) {
            steps.append([dst]() { QDir().mkpath(dst); });
            planTick(label, steps.size());
            QDir d(src);
            QFileInfoList list = d.entryInfoList(QDir::NoDotAndDotDot|QDir::AllEntries);
            for (const QFileInfo &f : list)
                planCopy(f.absoluteFilePath(), dst + "/" + f.fileName(), steps, label);
        } else {
            steps.append([src, dst]() { QFile::copy(src, dst); });
            planTick(label, steps.size());
        }
    }

    // Build a post-order list of delete steps (children removed before the
    // directory that contains them, so rmdir succeeds once we reach it).
    void planDelete(const QString &path,
                    QVector<std::function<void()>> &steps, const QString &label) {
        QFileInfo info(path);
        if (info.isDir() && !info.isSymLink()) {
            QDir d(path);
            QFileInfoList list = d.entryInfoList(QDir::NoDotAndDotDot|QDir::AllEntries);
            for (const QFileInfo &f : list)
                planDelete(f.absoluteFilePath(), steps, label);
            steps.append([path]() { QDir().rmdir(path); });
        } else {
            steps.append([path]() { QFile::remove(path); });
        }
        planTick(label, steps.size());
    }

    // Puts the path bar into busy mode before planning begins, so there is
    // visible feedback from the very first moment of the operation.
    void beginPathBarBusy(const QString &label) {
        pathBtn->setEnabled(false);
        setPathBarProgress(0.0, label);
        pathBtn->setText(label + "  (scanning…)");
        QCoreApplication::processEvents();
        planThrottle.start();
    }

    // Turns the address bar into a fill gauge: a gradient split at `frac`
    // sweeps across it left-to-right, with a live label + percentage.
    void setPathBarProgress(double frac, const QString &label) {
        frac = qBound(0.0, frac, 1.0);
        QString stop = QString::number(frac, 'f', 4);
        pathBtn->setText(QString("%1  (%2%)").arg(label).arg(int(frac * 100)));
        pathBtn->setStyleSheet(QString(
            "QPushButton { "
            "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
            "stop:0 #2a82da, stop:%1 #2a82da, stop:%1 #333333, stop:1 #333333); "
            "color:white; border-radius:8px; padding:10px; font-size:15px; text-align:left; }"
        ).arg(stop));
    }

    void resetPathBar() {
        pathBtn->setStyleSheet(pathBtnNormalStyle);
        pathBtn->setText(currentPath);
        pathBtn->setEnabled(true);
    }

    // Runs a flat list of file-operation steps one at a time, animating the
    // address bar as progress and keeping the UI responsive throughout.
    // Call beginPathBarBusy(label) before planning, then this to execute.
    void runWithProgress(const QString &label, const QVector<std::function<void()>> &steps) {
        if (steps.isEmpty()) {
            resetPathBar();
            return;
        }

        int total = steps.size();

        setPathBarProgress(0.0, label);
        QCoreApplication::processEvents();

        QElapsedTimer throttle;
        throttle.start();

        for (int i = 0; i < total; ++i) {
            steps[i]();

            bool last = (i == total - 1);
            if (last || throttle.elapsed() >= 40) {
                setPathBarProgress(double(i + 1) / double(total), label);
                QCoreApplication::processEvents();
                throttle.restart();
            }
        }

        resetPathBar();
    }

    void copySelection() {
        clipboardPaths = selectedPathList();
        clipboardCutMode = false;
        clearSelection(true);
        updateActionButtons();
    }

    void cutSelection() {
        clipboardPaths = selectedPathList();
        clipboardCutMode = true;
        clearSelection(true);
        updateActionButtons();
    }

    void pasteClipboard() {
        if (clipboardPaths.isEmpty()) return;

        QDir d(currentPath);
        if (!d.exists()) return;

        // Resolve every destination name up front so collisions are settled
        // before any work starts (renaming mid-operation would shift names).
        struct PasteItem { QString src, dst; };
        QVector<PasteItem> items;
        for (const QString &src : clipboardPaths) {
            QFileInfo info(src);
            QString base = info.fileName();
            QString dst = d.absoluteFilePath(base);

            int i = 1;
            while (QFileInfo::exists(dst)) {
                dst = d.absoluteFilePath(base + "_" + QString::number(i));
                ++i;
            }
            items.append({src, dst});
        }

        bool cutMode = clipboardCutMode;
        QString label = cutMode ? "Moving files…" : "Copying files…";

        beginPathBarBusy(label);

        QVector<std::function<void()>> steps;
        for (const PasteItem &it : items) {
            if (cutMode) {
                QString src = it.src, dst = it.dst;
                steps.append([src, dst]() { QFile::rename(src, dst); });
            } else {
                planCopy(it.src, it.dst, steps, label);
            }
        }

        runWithProgress(label, steps);

        if (cutMode) {
            clipboardPaths.clear();
            clipboardCutMode = false;
        }

        listDirectory(currentPath);
        clearSelection(true);
        updateActionButtons();
    }

    void renameSelection() {
        if (selectedPaths.size() != 1) return;
        QString p = *selectedPaths.begin();
        QFileInfo info(p);

        bool ok = false;
        QString newName = QInputDialog::getText(
            this,"Rename","New name:",QLineEdit::Normal,
            info.fileName(),&ok
        );
        if (!ok || newName.trimmed().isEmpty()) return;

        QFile::rename(p, info.dir().absoluteFilePath(newName.trimmed()));
        listDirectory(currentPath);
        clearSelection(true);
        updateActionButtons();
    }

    void moveSelection() {
        if (selectedPaths.isEmpty()) return;

        bool ok = false;
        QString dest = QInputDialog::getText(
            this,"Move","Destination:",QLineEdit::Normal,
            currentPath,&ok
        );
        if (!ok || dest.trimmed().isEmpty()) return;

        QDir d(dest.trimmed());
        if (!d.exists()) return;

        beginPathBarBusy("Moving files…");

        QVector<std::function<void()>> steps;
        for (const QString &src : selectedPathList()) {
            QFileInfo info(src);
            QString dstPath = d.absoluteFilePath(info.fileName());
            steps.append([src, dstPath]() { QFile::rename(src, dstPath); });
        }

        runWithProgress("Moving files…", steps);

        listDirectory(currentPath);
        clearSelection(true);
        updateActionButtons();
    }

    void deleteSelection() {
        QStringList sel = selectedPathList();
        if (sel.isEmpty()) return;

        // Confirmation prompt — deletes are permanent (no trash), so make
        // it clear what's about to go before doing anything.
        int files = 0, dirs = 0;
        for (const QString &p : sel) {
            if (QFileInfo(p).isDir()) dirs++;
            else files++;
        }

        QString what;
        if (sel.size() == 1) {
            what = "\"" + QFileInfo(sel.first()).fileName() + "\"";
        } else {
            QStringList parts;
            if (files > 0) parts << QString("%1 file%2").arg(files).arg(files == 1 ? "" : "s");
            if (dirs  > 0) parts << QString("%1 folder%2").arg(dirs).arg(dirs == 1 ? "" : "s");
            what = parts.join(" and ");
        }
        if (dirs > 0)
            what += "\n(folders are deleted with everything inside them)";

        QMessageBox confirm(this);
        confirm.setWindowTitle("Delete");
        confirm.setText("Permanently delete " + what + "?\n\nThis cannot be undone.");
        confirm.setIcon(QMessageBox::Warning);
        confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.setStyleSheet(
            "QMessageBox { background:#282828; }"
            "QMessageBox QLabel { color:white; font-size:18px; }"
            "QPushButton { background:#555; color:white; border:none; border-radius:8px; "
            "padding:8px 24px; font-size:16px; min-width:80px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
        if (QPushButton *yes = qobject_cast<QPushButton*>(confirm.button(QMessageBox::Yes))) {
            yes->setText("Delete");
            yes->setStyleSheet(
                "QPushButton { background:#aa2222; color:white; border:none; border-radius:8px; "
                "padding:8px 24px; font-size:16px; min-width:80px; }"
                "QPushButton:hover { background:#cc3333; }"
                "QPushButton:pressed { background:#881818; }"
            );
        }

        if (confirm.exec() != QMessageBox::Yes) return;

        beginPathBarBusy("Deleting files…");

        QVector<std::function<void()>> steps;
        for (const QString &p : sel)
            planDelete(p, steps, "Deleting files…");

        runWithProgress("Deleting files…", steps);

        listDirectory(currentPath);
        clearSelection(true);
        updateActionButtons();
    }

    void createDirectory() {
        bool ok = false;
        QString name = QInputDialog::getText(
            this,"New Folder","Folder name:",QLineEdit::Normal,
            "New Folder",&ok
        );
        if (!ok) return;
        name = name.trimmed();
        if (name.isEmpty()) return;

        QString target = QDir(currentPath).absoluteFilePath(name);
        int i = 1;
        while (QFileInfo::exists(target)) {
            target = QDir(currentPath).absoluteFilePath(name + "_" + QString::number(i));
            i++;
        }

        QDir().mkdir(target);
        listDirectory(currentPath);
    }

    void createNewFile() {
        bool ok = false;
        QString name = QInputDialog::getText(
            this,"New File","File name:",QLineEdit::Normal,
            "newfile.txt",&ok
        );
        if (!ok) return;
        name = name.trimmed();
        if (name.isEmpty()) return;

        QString target = QDir(currentPath).absoluteFilePath(name);
        int i = 1;

        while (QFileInfo::exists(target)) {
            int dot = name.lastIndexOf('.');
            QString withIndex;
            if (dot > 0)
                withIndex = name.left(dot) + "_" + QString::number(i++) + name.mid(dot);
            else
                withIndex = name + "_" + QString::number(i++);

            target = QDir(currentPath).absoluteFilePath(withIndex);
        }

        QFile f(target);
        if (f.open(QIODevice::WriteOnly)) f.close();

        listDirectory(currentPath);
    }

    void extractSelection() {
        if (selectedPaths.size() != 1)
            return;

        QString path = *selectedPaths.begin();
        QFileInfo info(path);
        if (!info.isFile()) return;

        QString lower = path.toLower();
        if (!isArchiveFilePath(path)) return;

        QString fileName = info.fileName();
        QString baseName = info.completeBaseName();

        if (lower.endsWith(".tar.gz")) baseName = fileName.left(fileName.size() - 7);
        else if (lower.endsWith(".tar.xz")) baseName = fileName.left(fileName.size() - 7);
        else if (lower.endsWith(".tar.bz2")) baseName = fileName.left(fileName.size() - 8);

        QString outDir = QDir(currentPath).absoluteFilePath(baseName + "_extracted");
        int i = 1;
        while (QFileInfo::exists(outDir))
            outDir = QDir(currentPath).absoluteFilePath(baseName + "_extracted_" + QString::number(i++));

        QDir().mkpath(outDir);

        QString cmd;
        if (lower.endsWith(".zip"))
            cmd = QString("unzip -o %1 -d %2").arg(quoteFilePath(path)).arg(quoteFilePath(outDir));
        else if (lower.endsWith(".gz") || lower.endsWith(".tgz"))
            cmd = QString("tar -xzf %1 -C %2").arg(quoteFilePath(path)).arg(quoteFilePath(outDir));
        else if (lower.endsWith(".xz"))
            cmd = QString("tar -xJf %1 -C %2").arg(quoteFilePath(path)).arg(quoteFilePath(outDir));
        else if (lower.endsWith(".bz2"))
            cmd = QString("tar -xjf %1 -C %2").arg(quoteFilePath(path)).arg(quoteFilePath(outDir));
        else
            cmd = QString("tar -xf %1 -C %2").arg(quoteFilePath(path)).arg(quoteFilePath(outDir));

        QProcess::startDetached("sh", QStringList() << "-c" << cmd);
    }

    void showPropertiesDialog() {
        if (selectedPaths.isEmpty()) return;

        QStringList sel = selectedPathList();

        QDialog dlg(this);
        dlg.setWindowTitle("Properties");
        dlg.setStyleSheet("QDialog { background:#282828; color:white; }");
        QVBoxLayout *layout = new QVBoxLayout(&dlg);

        if (sel.size() == 1) {
            QString p = sel.first();
            QFileInfo info(p);

            QString type = info.isDir() ? "Folder" : "File";
            QString size = info.isDir() ? "N/A" : QString::number(info.size()) + " bytes";

            QFile::Permissions pm = info.permissions();
            QString perms;
            perms += (pm & QFile::ReadUser)  ? "r" : "-";
            perms += (pm & QFile::WriteUser) ? "w" : "-";
            perms += (pm & QFile::ExeUser)   ? "x" : "-";
            perms += " ";
            perms += (pm & QFile::ReadGroup)  ? "r" : "-";
            perms += (pm & QFile::WriteGroup) ? "w" : "-";
            perms += (pm & QFile::ExeGroup)   ? "x" : "-";
            perms += " ";
            perms += (pm & QFile::ReadOther)  ? "r" : "-";
            perms += (pm & QFile::WriteOther) ? "w" : "-";
            perms += (pm & QFile::ExeOther)   ? "x" : "-";

            QString mod = info.lastModified()
                              .toString(QLocale().dateTimeFormat(QLocale::ShortFormat));

            for (QString s : {
                "Name: " + info.fileName(),
                "Path: " + info.absoluteFilePath(),
                "Type: " + type,
                "Size: " + size,
                "Permissions: " + perms,
                "Modified: " + mod
            }) {
                QLabel *L = new QLabel(s);
                L->setStyleSheet("QLabel { color:white; font-size:20px; }");
                L->setWordWrap(true);
                layout->addWidget(L);
            }

        } else {
            int files=0, dirs=0;
            qint64 total=0;

            for (const QString &p : sel) {
                QFileInfo info(p);
                if (info.isDir()) dirs++;
                else { files++; total += info.size(); }
            }

            QLabel *sum = new QLabel(
                QString("Selected: %1\nFiles: %2\nFolders: %3\nTotal size: %4 bytes")
                    .arg(sel.size()).arg(files).arg(dirs).arg(total)
            );
            sum->setStyleSheet("QLabel { color:white; font-size:20px; }");
            sum->setWordWrap(true);
            layout->addWidget(sum);
        }

        QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok);
        bb->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; border-radius:8px; padding:8px 20px; font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
        layout->addWidget(bb);

        dlg.exec();
        clearSelection(true);
        updateActionButtons();
    }

    void openWithSelection() {
        if (selectedPaths.size() != 1) return;

        QString filePath = *selectedPaths.begin();
        if (!QFileInfo(filePath).isFile()) return;

        QDialog dlg(this);
        dlg.setWindowTitle("Open with");
        dlg.setStyleSheet("QDialog { background:#282828; color:white; }");
        QVBoxLayout *layout = new QVBoxLayout(&dlg);

        QListWidget *list = new QListWidget;
        list->setStyleSheet(
            "QListWidget { background:#333; color:white; font-size:18px; border:none; }"
            "QListWidget::item { padding:6px; }"
            "QListWidget::item:selected { background:#555; }"
        );
        layout->addWidget(list);

        QScroller::ungrabGesture(list);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

        QVector<DesktopApp> apps = loadDesktopApps();
        for (const DesktopApp &app : apps) {
            QListWidgetItem *item = new QListWidgetItem(app.name, list);
            item->setData(Qt::UserRole, app.exec);
            if (!app.icon.isEmpty()) {
                QIcon ic = QIcon::fromTheme(app.icon);
                if (!ic.isNull()) item->setIcon(ic);
            }
        }

        QLineEdit *cmdEdit = new QLineEdit;
        cmdEdit->setPlaceholderText("Custom command (e.g. gimp %f)");
        cmdEdit->setStyleSheet(
            "QLineEdit { background:#333; color:#DDDDDD; border-radius:6px; padding:6px; font-size:18px; }"
        );
        layout->addWidget(cmdEdit);

        QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);
        bb->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; border-radius:8px; "
            "padding:8px 20px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
        connect(bb,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
        layout->addWidget(bb);

        if (dlg.exec() != QDialog::Accepted) return;

        QString cmd;
        QListWidgetItem *cur = list->currentItem();
        QString custom = cmdEdit->text().trimmed();

        if (cur) {
            QString execTemplate = cur->data(Qt::UserRole).toString();
            cmd = buildExecCommand(execTemplate, filePath);
        } else if (!custom.isEmpty()) {
            if (custom.contains("%f"))
                cmd = buildExecCommand(custom, filePath);
            else
                cmd = custom + " " + quoteFilePath(filePath);
        } else {
            return;
        }

        QProcess::startDetached("sh", QStringList() << "-c" << cmd);
        clearSelection(true);
        updateActionButtons();
    }

    // ================= NETWORK ACCESS =================

    static QString gvfsRoot() {
        // If launched via sudo/osm-sudo, getuid() is 0 but the gvfs daemon
        // (and its FUSE mount) belong to the desktop session user. FUSE
        // denies access to other users - including root - so network mounts
        // are invisible when elevated. Use the invoking user's uid if known.
        uid_t uid = getuid();
        if (uid == 0) {
            QByteArray sudoUid = qgetenv("SUDO_UID");
            QByteArray pkUid   = qgetenv("PKEXEC_UID");
            bool ok = false;
            uid_t real = sudoUid.toUInt(&ok);
            if (!ok) { real = pkUid.toUInt(&ok); }
            if (ok && real != 0) uid = real;
        }
        return QString("/run/user/%1/gvfs").arg(uid);
    }

    // True if we are running elevated: gvfs FUSE mounts will be unreadable
    static bool gvfsBlockedByPrivilege() {
        return getuid() == 0;
    }

    void loadSavedServers() {
        savedServers = settings->value("network/servers").toStringList();
    }

    void saveSavedServers() {
        settings->setValue("network/servers", savedServers);
        settings->sync();
    }

    // ================= USB DEVICES =================

    struct UsbPartition {
        QString devPath;     // /dev/sdb1
        QString label;       // volume label or empty
        QString size;        // "57.3G"
        QString fstype;      // vfat, exfat, ext4, ntfs...
        QString mountPoint;  // empty if not mounted
    };

    // True if the disk (e.g. "sdb") is USB-attached or removable.
    // Uses /sys/block, which works regardless of lsblk version.
    static bool isUsbOrRemovableDisk(const QString &diskName) {
        QString sysPath = "/sys/block/" + diskName;

        // removable flag (flash sticks, card readers)
        QFile rm(sysPath + "/removable");
        if (rm.open(QIODevice::ReadOnly)) {
            if (rm.readAll().trimmed() == "1") return true;
        }

        // USB-attached but rm=0 (SSDs in enclosures, many external HDDs):
        // the resolved device path contains "/usb"
        QFileInfo link(sysPath);
        QString real = link.canonicalFilePath();
        return real.contains("/usb");
    }

    // Enumerates partitions on removable/USB drives.
    // Uses only lsblk columns that exist on old util-linux versions,
    // and /sys/block for the USB check.
    QVector<UsbPartition> enumerateUsbPartitions() {
        QVector<UsbPartition> out;

        QProcess proc;
        proc.start("lsblk", QStringList()
                   << "-J" << "-o"
                   << "NAME,SIZE,LABEL,MOUNTPOINT,TYPE,FSTYPE");
        if (!proc.waitForFinished(4000)) { proc.kill(); return out; }

        QJsonDocument doc = QJsonDocument::fromJson(proc.readAllStandardOutput());
        QJsonArray devices = doc.object().value("blockdevices").toArray();

        for (const QJsonValue &dv : devices) {
            QJsonObject disk = dv.toObject();
            if (disk.value("type").toString() != "disk") continue;

            QString diskName = disk.value("name").toString();
            if (diskName.isEmpty()) continue;
            if (!isUsbOrRemovableDisk(diskName)) continue;

            auto addPart = [&](const QJsonObject &p) {
                UsbPartition up;
                up.devPath    = "/dev/" + p.value("name").toString();
                up.label      = p.value("label").toString();
                up.size       = p.value("size").toString();
                up.fstype     = p.value("fstype").toString();
                up.mountPoint = p.value("mountpoint").toString();
                if (up.fstype.isEmpty()) return;   // skip raw/extended/no-fs
                out.append(up);
            };

            QJsonArray children = disk.value("children").toArray();
            if (children.isEmpty()) {
                // filesystem directly on the disk (some flash drives)
                addPart(disk);
            } else {
                for (const QJsonValue &pv : children)
                    addPart(pv.toObject());
            }
        }
        return out;
    }

    // Just device paths, for cheap hotplug comparison
    QSet<QString> enumerateUsbDevicePaths() {
        QSet<QString> s;
        for (const UsbPartition &p : enumerateUsbPartitions())
            s.insert(p.devPath);
        return s;
    }

    void pollUsbDevices() {
        QSet<QString> now = enumerateUsbDevicePaths();
        bool added = false;
        for (const QString &d : now)
            if (!knownUsbDevices.contains(d)) { added = true; break; }
        knownUsbDevices = now;

        if (added) {
            // highlight the USB button and mention it in the status bar
            usbBtn->setStyleSheet(
                "QPushButton { background:#2a82da; color:white; border:none; border-radius:10px; font-size:18px; }"
                "QPushButton:hover { background:#3a92ea; }"
                "QPushButton:pressed { background:#1a72ca; }"
            );
            if (statusLabel) statusLabel->setText("USB device attached");
        }
    }

    void resetUsbButtonStyle() {
        usbBtn->setStyleSheet(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        );
    }

    void showUsbDialog() {
        resetUsbButtonStyle();

        QVector<UsbPartition> parts = enumerateUsbPartitions();

        QDialog dlg(this);
        dlg.setWindowTitle("USB Drives");
        dlg.setStyleSheet("background:#282828; color:white;");
        dlg.setMinimumWidth(460);

        QVBoxLayout *lay = new QVBoxLayout(&dlg);
        lay->setSpacing(10);

        if (parts.isEmpty()) {
            QLabel *none = new QLabel("No USB drives detected.");
            none->setStyleSheet("font-size:15px; padding:20px;");
            lay->addWidget(none);
        }

        for (const UsbPartition &p : parts) {
            QString name = p.label.isEmpty()
                           ? QFileInfo(p.devPath).fileName()   // sdb1
                           : p.label;
            QString state = p.mountPoint.isEmpty() ? "not mounted" : p.mountPoint;
            QString text = QString("🖴  %1   (%2, %3)\n      %4")
                               .arg(name, p.size, p.fstype, state);

            QHBoxLayout *row = new QHBoxLayout;
            row->setSpacing(8);

            QPushButton *b = new QPushButton(text);
            b->setMinimumHeight(64);
            b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            b->setStyleSheet(
                "QPushButton { background:#444; color:white; border:none; border-radius:10px;"
                " padding:10px; font-size:14px; text-align:left; }"
                "QPushButton:hover { background:#555; }"
                "QPushButton:pressed { background:#333; }"
            );
            row->addWidget(b, 1);

            // Eject button (only useful when mounted, but always shown;
            // greyed out when there is nothing to unmount)
            QPushButton *eject = new QPushButton("⏏");
            eject->setFixedSize(64, 64);
            eject->setEnabled(!p.mountPoint.isEmpty());
            eject->setStyleSheet(
                "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:22px; }"
                "QPushButton:hover:enabled { background:#dd3333; }"
                "QPushButton:pressed:enabled { background:#aa0000; }"
                "QPushButton:disabled { background:#222; color:#555; }"
            );
            row->addWidget(eject, 0);

            lay->addLayout(row);

            UsbPartition part = p;   // copy for capture
            connect(b, &QPushButton::clicked, &dlg, [this, part, &dlg]() {
                dlg.accept();
                openUsbPartition(part);
            });

            connect(eject, &QPushButton::clicked, &dlg, [this, part, &dlg]() {
                dlg.accept();
                ejectUsbPartition(part);
            });
        }

        // Rescan + close row
        QHBoxLayout *btnRow = new QHBoxLayout;
        QPushButton *rescan = new QPushButton("Rescan");
        QPushButton *close  = new QPushButton("Close");
        for (QPushButton *b : { rescan, close }) {
            b->setFixedHeight(44);
            b->setStyleSheet(
                "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:14px; }"
                "QPushButton:hover { background:#666; }"
                "QPushButton:pressed { background:#444; }"
            );
        }
        btnRow->addWidget(rescan);
        btnRow->addWidget(close);
        lay->addLayout(btnRow);

        connect(close, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(rescan, &QPushButton::clicked, &dlg, [this, &dlg]() {
            dlg.reject();
            // re-open with a fresh scan
            QTimer::singleShot(0, this, &FileBrowser::showUsbDialog);
        });

        dlg.exec();
    }

    void openUsbPartition(const UsbPartition &p) {
        // Already mounted? Just browse it.
        if (!p.mountPoint.isEmpty() && QDir(p.mountPoint).exists()) {
            addUsbShortcut(p);
            listDirectory(p.mountPoint);
            return;
        }

        if (statusLabel) statusLabel->setText("Mounting " + p.devPath + " ...");
        QCoreApplication::processEvents();

        // Try udisksctl first (udisks2 + elogind on Devuan)
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start("udisksctl", QStringList() << "mount" << "-b" << p.devPath);
        bool ran = proc.waitForStarted(2000) && proc.waitForFinished(15000);
        QString output = QString::fromUtf8(proc.readAll());

        QString mountPath;
        if (ran && proc.exitCode() == 0) {
            // Output: "Mounted /dev/sdb1 at /media/dan/SANDISK64"
            int at = output.lastIndexOf(" at ");
            if (at >= 0) {
                mountPath = output.mid(at + 4).trimmed();
                if (mountPath.endsWith('.')) mountPath.chop(1);
            }
        }

        // Fallback: pmount (mounts to /media/<basename>)
        if (mountPath.isEmpty()) {
            QProcess pm;
            pm.setProcessChannelMode(QProcess::MergedChannels);
            pm.start("pmount", QStringList() << p.devPath);
            if (pm.waitForStarted(2000) && pm.waitForFinished(15000) && pm.exitCode() == 0) {
                mountPath = "/media/" + QFileInfo(p.devPath).fileName();
            } else {
                output += "\n" + QString::fromUtf8(pm.readAll());
            }
        }

        if (mountPath.isEmpty() || !QDir(mountPath).exists()) {
            QMessageBox::warning(this, "USB",
                "Failed to mount " + p.devPath + "\n\n" + output.trimmed() +
                "\n\nInstall udisks2 (with elogind) or pmount, and ensure your "
                "user is in the 'plugdev' group.");
            if (statusLabel) statusLabel->setText(QString::number(currentItemCount) + " items");
            return;
        }

        addUsbShortcut(p, mountPath);
        listDirectory(mountPath);
    }

    void ejectUsbPartition(const UsbPartition &p) {
        if (p.mountPoint.isEmpty()) return;

        // If we are currently browsing inside the drive, leave it first
        if (currentPath.startsWith(p.mountPoint))
            listDirectory(QDir::homePath());

        if (statusLabel) statusLabel->setText("Ejecting " + p.devPath + " ...");
        QCoreApplication::processEvents();

        // Try udisksctl unmount, fall back to pumount
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start("udisksctl", QStringList() << "unmount" << "-b" << p.devPath);
        bool ok = proc.waitForStarted(2000) && proc.waitForFinished(15000)
                  && proc.exitCode() == 0;
        QString output = QString::fromUtf8(proc.readAll());

        if (!ok) {
            QProcess pm;
            pm.setProcessChannelMode(QProcess::MergedChannels);
            pm.start("pumount", QStringList() << p.devPath);
            ok = pm.waitForStarted(2000) && pm.waitForFinished(15000)
                 && pm.exitCode() == 0;
            output += "\n" + QString::fromUtf8(pm.readAll());
        }

        if (!ok) {
            QMessageBox::warning(this, "USB",
                "Failed to unmount " + p.devPath + "\n\n" + output.trimmed() +
                "\n\nMake sure no files on the drive are open.");
            if (statusLabel) statusLabel->setText(QString::number(currentItemCount) + " items");
            return;
        }

        // Power off the whole device so it is safe to pull (best effort)
        QString disk = p.devPath;
        while (!disk.isEmpty() && disk.back().isDigit()) disk.chop(1);
        if (disk.endsWith('p') && disk.contains("mmcblk")) disk.chop(1);   // mmcblk0p1 -> mmcblk0
        QProcess po;
        po.start("udisksctl", QStringList() << "power-off" << "-b" << disk);
        po.waitForFinished(8000);

        // Remove the stale shortcut if we added one
        if (shortcutsList.contains(p.mountPoint)) {
            shortcutsList.removeAll(p.mountPoint);
            saveShortcuts();
            rebuildShortcutsPanel();
        }

        if (statusLabel)
            statusLabel->setText("Safe to remove " + (p.label.isEmpty() ? p.devPath : p.label));
    }

    // Add the mounted drive to the shortcuts panel (deduped)
    void addUsbShortcut(const UsbPartition &p, const QString &mountPathOverride = QString()) {
        QString mp = mountPathOverride.isEmpty() ? p.mountPoint : mountPathOverride;
        if (mp.isEmpty()) return;
        if (!shortcutsList.contains(mp)) {
            shortcutsList.append(mp);
            saveShortcuts();
            rebuildShortcutsPanel();
        }
    }

    void showNetworkDialog() {
        loadSavedServers();

        QDialog dlg(this);
        dlg.setWindowTitle("Network");
        dlg.setStyleSheet("QDialog { background:#282828; color:white; }");
        dlg.setMinimumWidth(420);
        QVBoxLayout *layout = new QVBoxLayout(&dlg);
        layout->setSpacing(10);

        QLabel *title = new QLabel("Saved servers");
        title->setStyleSheet("QLabel { color:#CCCCCC; font-size:14px; }");
        layout->addWidget(title);

        QListWidget *list = new QListWidget;
        list->setStyleSheet(
            "QListWidget { background:#333; color:white; font-size:16px; border:none; border-radius:8px; }"
            "QListWidget::item { padding:8px; }"
            "QListWidget::item:selected { background:#555; }"
        );
        QScroller::grabGesture(list, QScroller::LeftMouseButtonGesture);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        for (const QString &s : savedServers)
            list->addItem(s);
        layout->addWidget(list, 1);

        // Address entry
        QLabel *addrLbl = new QLabel("Server address");
        addrLbl->setStyleSheet("QLabel { color:#CCCCCC; font-size:14px; }");
        layout->addWidget(addrLbl);

        QLineEdit *addrEdit = new QLineEdit;
        addrEdit->setPlaceholderText("smb://server/share, sftp://host, ftp://host ...");
        addrEdit->setStyleSheet(
            "QLineEdit { background:#333; color:#DDDDDD; border-radius:6px; padding:8px; font-size:16px; }"
        );
        layout->addWidget(addrEdit);

        // Optional credentials
        QHBoxLayout *credRow = new QHBoxLayout;
        QLineEdit *userEdit = new QLineEdit;
        userEdit->setPlaceholderText("Username (optional)");
        QLineEdit *passEdit = new QLineEdit;
        passEdit->setPlaceholderText("Password (optional)");
        passEdit->setEchoMode(QLineEdit::Password);
        QString credStyle =
            "QLineEdit { background:#333; color:#DDDDDD; border-radius:6px; padding:8px; font-size:16px; }";
        userEdit->setStyleSheet(credStyle);
        passEdit->setStyleSheet(credStyle);
        credRow->addWidget(userEdit);
        credRow->addWidget(passEdit);
        layout->addLayout(credRow);

        // Buttons
        QHBoxLayout *btnRow = new QHBoxLayout;
        auto makeDlgBtn = [](const QString &text) {
            QPushButton *b = new QPushButton(text);
            b->setFixedHeight(44);
            b->setStyleSheet(
                "QPushButton { background:#555; color:white; border:none; border-radius:8px; "
                "padding:8px 20px; font-size:15px; }"
                "QPushButton:hover { background:#666; }"
                "QPushButton:pressed { background:#444; }"
            );
            return b;
        };
        QPushButton *connectBtn = makeDlgBtn("Connect");
        QPushButton *forgetBtn  = makeDlgBtn("Forget");
        QPushButton *cancelBtn  = makeDlgBtn("Cancel");
        btnRow->addWidget(connectBtn);
        btnRow->addWidget(forgetBtn);
        btnRow->addStretch(1);
        btnRow->addWidget(cancelBtn);
        layout->addLayout(btnRow);

        // Selecting a saved server fills the address field
        connect(list, &QListWidget::itemClicked, &dlg, [addrEdit](QListWidgetItem *item) {
            addrEdit->setText(item->text());
        });
        // Double-click connects straight away
        connect(list, &QListWidget::itemDoubleClicked, &dlg, [addrEdit, &dlg](QListWidgetItem *item) {
            addrEdit->setText(item->text());
            dlg.accept();
        });

        connect(forgetBtn, &QPushButton::clicked, &dlg, [this, list]() {
            QListWidgetItem *cur = list->currentItem();
            if (!cur) return;
            savedServers.removeAll(cur->text());
            saveSavedServers();
            delete list->takeItem(list->row(cur));
        });

        connect(connectBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
        connect(cancelBtn,  &QPushButton::clicked, &dlg, &QDialog::reject);

        if (dlg.exec() != QDialog::Accepted) return;

        QString url = addrEdit->text().trimmed();
        if (url.isEmpty()) return;

        // Assume smb:// if no scheme given
        if (!url.contains("://"))
            url = "smb://" + url;

        connectToServer(url, userEdit->text().trimmed(), passEdit->text());
    }

    void connectToServer(const QString &url, const QString &user, const QString &pass) {
        if (gvfsBlockedByPrivilege()) {
            QMessageBox::warning(this, "Network",
                "osm-files is running as root. gvfs network mounts belong to "
                "the desktop user's session and FUSE hides them from root, so "
                "the share will mount but appear empty.\n\n"
                "Launch osm-files as the normal user to browse network shares.");
        }
        if (statusLabel) statusLabel->setText("Connecting to " + url + " ...");
        QCoreApplication::processEvents();

        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start("gio", QStringList() << "mount" << url);
        if (!proc.waitForStarted(3000)) {
            QMessageBox::warning(this, "Network",
                "Could not run 'gio'. Install gvfs (gvfs-backends) for network browsing.");
            if (statusLabel) statusLabel->setText(QString::number(currentItemCount) + " items");
            return;
        }

        // gio mount prompts on stdin: User, Domain, Password (when auth is required)
        if (!user.isEmpty() || !pass.isEmpty()) {
            proc.write((user + "\n").toUtf8());   // username
            proc.write("\n");                      // domain (default)
            proc.write((pass + "\n").toUtf8());   // password
        } else {
            // try anonymous/guest if it asks
            proc.write("\n\n\n");
        }
        proc.closeWriteChannel();

        if (!proc.waitForFinished(20000)) {
            proc.kill();
            QMessageBox::warning(this, "Network", "Connection to " + url + " timed out.");
            if (statusLabel) statusLabel->setText(QString::number(currentItemCount) + " items");
            return;
        }

        QString output = QString::fromUtf8(proc.readAll());
        bool alreadyMounted = output.contains("already mounted", Qt::CaseInsensitive);

        if (proc.exitCode() != 0 && !alreadyMounted) {
            QMessageBox::warning(this, "Network",
                "Failed to connect to " + url + "\n\n" + output.trimmed());
            if (statusLabel) statusLabel->setText(QString::number(currentItemCount) + " items");
            return;
        }

        // Remember successful server
        if (!savedServers.contains(url)) {
            savedServers.prepend(url);
            while (savedServers.size() > 20) savedServers.removeLast();
            saveSavedServers();
        }

        // The FUSE view of the mount can appear a moment after 'gio mount'
        // returns, so poll briefly instead of checking once
        QString mountPath;
        for (int attempt = 0; attempt < 20; ++attempt) {   // up to ~2s
            mountPath = findGvfsMount(url);
            if (!mountPath.isEmpty()) break;
            QThread::msleep(100);
            QCoreApplication::processEvents();
        }
        if (mountPath.isEmpty()) {
            // fall back to the gvfs root so the user can pick the mount manually
            mountPath = gvfsRoot();
        }

        if (!QDir(mountPath).exists()) {
            QMessageBox::warning(this, "Network",
                "Connected, but the mount point was not found under " + gvfsRoot());
            if (statusLabel) statusLabel->setText(QString::number(currentItemCount) + " items");
            return;
        }

        listDirectory(mountPath);
    }

    // Find the gvfs directory that corresponds to the given URL
    QString findGvfsMount(const QString &url) {
        QString rest = url.section("://", 1);          // host/share/path...
        QString host = rest.section('/', 0, 0);
        // strip user@ prefix and :port suffix
        if (host.contains('@')) host = host.section('@', 1);
        if (host.contains(':')) host = host.section(':', 0, 0);
        QString share = rest.section('/', 1, 1);

        QDir root(gvfsRoot());
        if (!root.exists()) return QString();

        QString hostMatch;
        for (const QString &entry :
             root.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System)) {
            // entries look like: smb-share:server=host,share=name  /  sftp:host=host,user=...
            if (!entry.contains(host, Qt::CaseInsensitive)) continue;
            if (!share.isEmpty() &&
                entry.contains("share=" + share, Qt::CaseInsensitive)) {
                return root.absoluteFilePath(entry);   // exact share match
            }
            if (hostMatch.isEmpty())
                hostMatch = root.absoluteFilePath(entry);
        }
        return hostMatch;
    }

    void processNextThumbnail() {
        if (imageButtons.isEmpty()) {
            thumbTimer->stop();
            return;
        }

        QRect vpRect = scroll->viewport()->rect();
        int visibleIndex = -1;

        for (int i = 0; i < imageButtons.size(); i++) {
            QPushButton *btn = imageButtons[i];
            if (!btn || btn->parent() == nullptr) continue;
            if (btn->property("thumbDone").toBool()) continue;

            QPoint topLeft = btn->mapTo(scroll->viewport(), QPoint(0,0));
            QRect btnRect(topLeft, btn->size());

            if (vpRect.intersects(btnRect)) {
                visibleIndex = i;
                break;
            }
        }

        int idx = visibleIndex;
        if (idx == -1) {
            for (int i = 0; i < imageButtons.size(); i++) {
                QPushButton *btn = imageButtons[i];
                if (!btn || btn->parent() == nullptr) continue;
                if (!btn->property("thumbDone").toBool()) {
                    idx = i;
                    break;
                }
            }
        }

        if (idx == -1) {
            thumbTimer->stop();
            return;
        }

        QPushButton *btn = imageButtons[idx];
        imageButtons.remove(idx);

        if (!btn || btn->parent() == nullptr) return;
        if (!btn->property("isImage").toBool()) return;
        if (btn->property("thumbDone").toBool()) return;

        QString fullPath = btn->property("fullPath").toString();
        QImage img(fullPath);
        if (!img.isNull()) {
            if (gridMode) {
                QLabel *thumb = gridThumbLabel.value(btn, nullptr);
                QLabel *nameLbl = gridNameLabel.value(btn, nullptr);

                if (thumb) {
                    int targetW = thumb->width();
                    if (targetW <= 0) targetW = 200;

                    QPixmap pm = QPixmap::fromImage(
                        img.scaled(targetW, targetW,
                                   Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation)
                    );
                    thumb->setPixmap(pm);
                    thumb->setText(QString());
                    thumb->setMinimumHeight(pm.height());
                }
                if (nameLbl) {
                    nameLbl->setText(btn->property("baseName").toString());
                }
            } else {
                QPixmap pm = QPixmap::fromImage(
                    img.scaled(120, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                );
                btn->setIcon(QIcon(pm));
                btn->setIconSize(QSize(120, 120));
                btn->setText(btn->property("baseName").toString());
            }
        }

        btn->setProperty("thumbDone", true);
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QString start;
    if (argc > 1)
        start = QString::fromLocal8Bit(argv[1]);
    if (start.isEmpty())
        start = QDir::homePath();

    FileBrowser fb(start);
    fb.setWindowTitle("Alternix Files");

    QScreen *s = QGuiApplication::primaryScreen();
    if (s) {
        QRect g = s->availableGeometry();
        fb.resize(g.width()*0.8, g.height()*0.8);
        fb.move(g.center() - QPoint(fb.width()/2, fb.height()/2));
    }

    fb.show();
    return app.exec();
}
