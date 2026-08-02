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
#include <QCheckBox>
#include <QRegularExpression>
#include <algorithm>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

class FileBrowser : public QWidget {
public:

    // ================= THEME =================
    //
    // Every widget here sets its own stylesheet with literal colours, so a
    // global palette would simply be overridden. Instead each stylesheet is
    // written once in the dark palette and passed through themed(), which
    // swaps the colours when light mode is on. One table, no duplicated
    // stylesheets, and nothing to keep in step by hand.
    //
    // The two palettes deliberately share no colour values, so the swap is
    // reversible and can be applied to a string more than once without damage.
    // If a colour is ever added to this table, keep that true: a light value
    // that also appears as a dark value would be translated twice.

    static bool &themeIsLight() { static bool light = false; return light; }

    static QString themed(QString css) {
        if (css.isEmpty()) return css;

        static const char *pal[][2] = {
            // dark          light
            { "white",   "#1a1a1a" },   // button and label text
            { "#ffffff", "#101010" },   // checked-button border
            { "#DDDDDD", "#2b2b2b" },   // entry text
            { "#CCCCCC", "#4d4d4d" },   // secondary label text

            { "#282828", "#f5f5f5" },   // window background
            { "#2a2a2a", "#f2f2f2" },
            { "#333333", "#ececec" },
            { "#333",    "#ececec" },   // list and entry background
            { "#3a3a3a", "#e6e6e6" },
            { "#222",    "#fbfbfb" },   // disabled / pressed background
            { "#4a4a4a", "#d6d6d6" },
            { "#444",    "#dcdcdc" },   // list item face / pressed
            { "#5a5a5a", "#c8c8c8" },
            { "#555",    "#cdcdcd" },   // button face
            { "#6a6a6a", "#bababa" },
            { "#666",    "#bebebe" },   // hover
            { "#7a7a7a", "#a9a9a9" },
            { "#777",    "#adadad" },   // selected item
            { "#888",    "#9b9b9b" },

            { "#2a82da", "#90caf9" },   // accent
            { "#3a92ea", "#a6d4fb" },
            { "#1a72ca", "#64b5f6" },

            { "#cc0000", "#ef9a9a" },   // delete / danger
            { "#dd3333", "#ffb3b3" },
            { "#aa0000", "#e57373" },
            { "#cc3333", "#eb8f8f" },
            { "#aa2222", "#e07070" },
            { "#881818", "#d05555" },
        };
        const int n = (int)(sizeof(pal) / sizeof(pal[0]));

        static QHash<QString, QString> toLight, toDark;
        if (toLight.isEmpty()) {
            for (int i = 0; i < n; ++i) {
                toLight.insert(QString(pal[i][0]).toLower(), QString(pal[i][1]));
                toDark.insert(QString(pal[i][1]).toLower(), QString(pal[i][0]));
            }
        }
        const QHash<QString, QString> &map = themeIsLight() ? toLight : toDark;

        // Rewrite colour by colour so a short token such as "#333" can never
        // corrupt a longer one such as "#333333".
        static const QRegularExpression re(
            "(#[0-9a-fA-F]{6}\\b|#[0-9a-fA-F]{3}\\b|\\bwhite\\b)");

        QString out;
        int last = 0;
        QRegularExpressionMatchIterator it = re.globalMatch(css);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += css.mid(last, m.capturedStart() - last);
            const QString token = m.captured(1);
            out += map.value(token.toLower(), token);
            last = m.capturedEnd();
        }
        out += css.mid(last);
        return out;
    }

    // Re-colour a window that has already been built. Widgets created after a
    // switch are correct from the start because every stylesheet goes through
    // themed(), so this is only needed for the live toggle.
    static void themeTree(QWidget *root) {
        if (!root) return;
        root->setStyleSheet(themed(root->styleSheet()));
        const QList<QWidget *> kids = root->findChildren<QWidget *>();
        for (QWidget *w : kids) {
            const QString css = w->styleSheet();
            if (!css.isEmpty()) w->setStyleSheet(themed(css));
        }
    }
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
        // SETTINGS (store in ~/.config/Alternix/osm-files.conf)
        // Read first: every stylesheet below is written for the dark palette
        // and translated by themed(), so the choice has to be known already.
        settings = new QSettings(QDir::homePath() + "/.config/Alternix/osm-files.conf",
                                 QSettings::IniFormat);
        themeIsLight() = settings->value("ui/lightMode", false).toBool();

        setStyleSheet(themed("background:#282828; color:white;"));

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
        backBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; font-weight:bold; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
        pathRow->addWidget(backBtn, 0);

        // Refresh button
        refreshBtn = new QPushButton("⟳");
        refreshBtn->setFixedSize(50, 50);
        refreshBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
        pathRow->addWidget(refreshBtn, 0);

        // Home button
        homeBtn = new QPushButton("🏡");
        homeBtn->setFixedSize(50, 50);
        homeBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
        pathRow->addWidget(homeBtn, 0);

        connect(homeBtn, &QPushButton::clicked, this, [this]() {
            listDirectory(QDir::homePath());
        });

        // Network button
        networkBtn = new QPushButton("🌐");
        networkBtn->setFixedSize(50, 50);
        networkBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
        pathRow->addWidget(networkBtn, 0);

        connect(networkBtn, &QPushButton::clicked, this, &FileBrowser::showNetworkDialog);

        // USB button
        usbBtn = new QPushButton("🖴");
        usbBtn->setFixedSize(50, 50);
        usbBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
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
        pathBtn->setStyleSheet(themed(pathBtnNormalStyle));
        pathBtn->setMinimumHeight(50);
        pathBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        pathRow->addWidget(pathBtn, 1);

        // View mode toggle
        viewToggleBtn = new QPushButton("☴");
        viewToggleBtn->setFixedSize(50, 50);
        viewToggleBtn->setCheckable(true);
        viewToggleBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
            "QPushButton:checked { background:#2a82da; }"
        ));
        pathRow->addWidget(viewToggleBtn, 0);

        // Shortcuts button
        shortcutsBtn = new QPushButton("⭐");
        shortcutsBtn->setFixedSize(50, 50);
        shortcutsBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; "
            "border-radius:10px; font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
        pathRow->addWidget(shortcutsBtn, 0);
        // Light / dark toggle
        themeBtn = new QPushButton(themeIsLight() ? "\u2600" : "\U0001F319");
        themeBtn->setFixedSize(50, 50);
        themeBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; "
            "border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
        pathRow->addWidget(themeBtn, 0);

        connect(themeBtn, &QPushButton::clicked, this, [this]() {
            themeIsLight() = !themeIsLight();
            if (settings) {
                settings->setValue("ui/lightMode", themeIsLight());
                settings->sync();
            }
            themeTree(this);
            themeBtn->setText(themeIsLight() ? "\u2600" : "\U0001F319");
        });

        root->addLayout(pathRow);

        // ========= TOP TOOLBAR ROW (Hidden, NewDir, NewFile, Copy, Cut, Paste...) =========
        QHBoxLayout *bar = new QHBoxLayout;
        bar->setSpacing(10);

        auto makeTopButton = [&](const QString &text) -> QPushButton* {
            QPushButton *b = new QPushButton(text);
            b->setFixedHeight(40);
            b->setMinimumWidth(60);
            b->setStyleSheet(themed(
                "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:14px; }"
                "QPushButton:hover:enabled { background:#666; }"
                "QPushButton:pressed:enabled { background:#444; }"
                "QPushButton:disabled { background:#222; color:#555; }"
                "QPushButton:checked { background:#2a82da; color:white; border:3px solid #ffffff; }"
            ));
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
        deleteBtn->setStyleSheet(themed(
            "QPushButton { background:#222; color:#555; border:none; border-radius:10px; font-size:14px; }"
            "QPushButton:pressed:enabled { background:#aa0000; }"
            "QPushButton:hover:enabled { background:#dd3333; }"
        ));

        extractBtn     = makeTopButton("Extract");
        openWithBtn    = makeTopButton("OpenWith");
        defaultsBtn    = makeTopButton("Defaults");
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
        bar->addWidget(defaultsBtn, 0);
        bar->addWidget(propsBtn, 0);
        bar->addWidget(multiSelectBtn, 0);
        bar->addWidget(unselectBtn, 0);

        QWidget *btnContainer = new QWidget;
        btnContainer->setFixedHeight(60);
        btnContainer->setStyleSheet(themed("background:transparent;"));
        btnContainer->setLayout(bar);

        QScrollArea *btnScroll = new QScrollArea;
        QScroller::grabGesture(btnScroll, QScroller::LeftMouseButtonGesture);
        btnScroll->setWidgetResizable(true);
        btnScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        btnScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        btnScroll->setFrameShape(QFrame::NoFrame);
        btnScroll->setStyleSheet(themed("background:transparent;"));
        btnScroll->setWidget(btnContainer);

        btnScroll->setStyleSheet(themed("QScrollArea { padding:0; margin:0; border:0; }"));
        btnScroll->widget()->setContentsMargins(0,0,0,0);
        btnScroll->viewport()->setContentsMargins(0,0,0,0);
        bar->setContentsMargins(0,0,0,0);

        root->addWidget(btnScroll);
        btnScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        // --- File list scroll area ---
        scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setStyleSheet(themed("background:#282828; border:none;"));
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
        statusLabel->setStyleSheet(themed("QLabel { color:#CCCCCC; font-size:12px; }"));
        statusRow->addWidget(statusLabel, 1);
        root->addLayout(statusRow);

        // Thumbnail timer
        thumbTimer = new QTimer(this);
        thumbTimer->setInterval(45);
        connect(thumbTimer, &QTimer::timeout, this, &FileBrowser::processNextThumbnail);

        // Path Menu
        pathMenu = new QWidget(this, Qt::Popup);
        pathMenu->setStyleSheet(themed("background:#222; border:2px solid #555; border-radius:14px;"));
        pathMenuLayout = new QVBoxLayout(pathMenu);
        pathMenuLayout->setContentsMargins(10,10,10,10);
        pathMenuLayout->setSpacing(6);

        // SHORTCUTS PANEL (floating overlay)
        shortcutsPanel = new QWidget(this, Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        shortcutsPanel->setFixedWidth(320);
        shortcutsPanel->setStyleSheet(themed(
            "background:rgba(30,30,30,0.92); border-left:3px solid #444;"
        ));
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
        addShortcutBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border-radius:12px; "
            "font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));

        removeShortcutBtn = new QPushButton("🗑️");
        removeShortcutBtn->setFixedHeight(60);
        removeShortcutBtn->setCheckable(true);
        removeShortcutBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border-radius:12px; "
            "font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:checked { background:#aa0000; }"
        ));

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
        connect(defaultsBtn,  &QPushButton::clicked, this, &FileBrowser::showDefaultAppsDialog);
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
    QPushButton *defaultsBtn = nullptr;
    QPushButton *themeBtn = nullptr;
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
            b->setStyleSheet(themed(
                "QPushButton { background:#333; color:white; border:none; "
                "border-radius:14px; padding:10px; font-size:15px; text-align:left; }"
                "QPushButton:hover { background:#444; }"
                "QPushButton:pressed { background:#222; }"
            ));
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
        btn->setStyleSheet(themed(currentNormalStyle));

        if (gridMode) {
            btn->setMinimumHeight(220);
            btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

            QVBoxLayout *v = new QVBoxLayout(btn);
            v->setContentsMargins(8,8,8,8);
            v->setSpacing(6);

            QLabel *thumb = new QLabel(btn);
            thumb->setAlignment(Qt::AlignCenter);
            thumb->setMinimumHeight(140);
            thumb->setStyleSheet(themed("background:transparent;"));

            QLabel *nameLbl = new QLabel(name, btn);
            nameLbl->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            nameLbl->setWordWrap(true);
            nameLbl->setStyleSheet(themed("background:transparent; font-size:20px;"));
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
                else openFilePath(p);
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
            b->setStyleSheet(themed(
                "QPushButton { background:#444; color:white; border:none; border-radius:10px; "
                "padding:16px; font-size:15px; text-align:left;}"
                "QPushButton:hover { background:#555; }"
                "QPushButton:pressed { background:#333; }"
            ));
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
            b->setStyleSheet(themed(sel ? currentSelectedStyle : currentNormalStyle));
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
            deleteBtn->setStyleSheet(themed(
                "QPushButton { background:#cc0000; color:white; border:none; "
                "border-radius:10px; font-size:18px; }"
                "QPushButton:hover { background:#dd3333; }"
                "QPushButton:pressed { background:#aa0000; }"
            ));
        } else {
            deleteBtn->setEnabled(false);
            deleteBtn->setStyleSheet(themed(
                "QPushButton { background:#222; color:#555; border:none; "
                "border-radius:10px; font-size:18px; }"
            ));
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
        pathBtn->setStyleSheet(themed(QString(
            "QPushButton { "
            "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
            "stop:0 #2a82da, stop:%1 #2a82da, stop:%1 #333333, stop:1 #333333); "
            "color:white; border-radius:8px; padding:10px; font-size:15px; text-align:left; }"
        ).arg(stop)));
    }

    void resetPathBar() {
        pathBtn->setStyleSheet(themed(pathBtnNormalStyle));
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
        confirm.setStyleSheet(themed(
            "QMessageBox { background:#282828; }"
            "QMessageBox QLabel { color:white; font-size:18px; }"
            "QPushButton { background:#555; color:white; border:none; border-radius:8px; "
            "padding:8px 24px; font-size:16px; min-width:80px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
        if (QPushButton *yes = qobject_cast<QPushButton*>(confirm.button(QMessageBox::Yes))) {
            yes->setText("Delete");
            yes->setStyleSheet(themed(
                "QPushButton { background:#aa2222; color:white; border:none; border-radius:8px; "
                "padding:8px 24px; font-size:16px; min-width:80px; }"
                "QPushButton:hover { background:#cc3333; }"
                "QPushButton:pressed { background:#881818; }"
            ));
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
        dlg.setStyleSheet(themed("QDialog { background:#282828; color:white; }"));
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
                L->setStyleSheet(themed("QLabel { color:white; font-size:20px; }"));
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
            sum->setStyleSheet(themed("QLabel { color:white; font-size:20px; }"));
            sum->setWordWrap(true);
            layout->addWidget(sum);
        }

        QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok);
        bb->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; border-radius:8px; padding:8px 20px; font-size:15px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
        connect(bb,&QDialogButtonBox::accepted,&dlg,&QDialog::accept);
        layout->addWidget(bb);

        dlg.exec();
        clearSelection(true);
        updateActionButtons();
    }

    // ================= DEFAULT APPLICATIONS =================

    // Extension key used for remembering default applications ("txt", "png").
    // Files with no extension get an empty key and are never remembered.
    static QString extensionKeyFor(const QString &filePath) {
        return QFileInfo(filePath).suffix().toLower();
    }

    QString defaultExecFor(const QString &ext) {
        if (!settings || ext.isEmpty()) return QString();
        settings->beginGroup("DefaultApps");
        const QString tmpl = settings->value(ext).toString();
        settings->endGroup();
        return tmpl;
    }

    QString defaultNameFor(const QString &ext) {
        if (!settings || ext.isEmpty()) return QString();
        settings->beginGroup("DefaultAppNames");
        const QString name = settings->value(ext).toString();
        settings->endGroup();
        return name;
    }

    void setDefaultAppFor(const QString &ext, const QString &name, const QString &execTemplate) {
        if (!settings || ext.isEmpty() || execTemplate.isEmpty()) return;
        settings->beginGroup("DefaultApps");
        settings->setValue(ext, execTemplate);
        settings->endGroup();
        settings->beginGroup("DefaultAppNames");
        settings->setValue(ext, name.isEmpty() ? execTemplate : name);
        settings->endGroup();
        settings->sync();
    }

    void clearDefaultAppFor(const QString &ext) {
        if (!settings || ext.isEmpty()) return;
        settings->beginGroup("DefaultApps");
        settings->remove(ext);
        settings->endGroup();
        settings->beginGroup("DefaultAppNames");
        settings->remove(ext);
        settings->endGroup();
        settings->sync();
    }

    // Open a file with the application remembered for its type, falling back
    // to osm-viewer when nothing has been chosen.
    void openFilePath(const QString &filePath) {
        const QString tmpl = defaultExecFor(extensionKeyFor(filePath));
        if (!tmpl.isEmpty()) {
            QProcess::startDetached("sh", QStringList()
                                    << "-c" << buildExecCommand(tmpl, filePath));
            return;
        }
        QProcess::startDetached("osm-viewer", QStringList() << filePath);
    }

    // Manage the remembered file-type -> application choices.
    void showDefaultAppsDialog() {
        QDialog dlg(this);
        dlg.setWindowTitle("Default applications");
        dlg.setStyleSheet(themed("QDialog { background:#282828; color:white; }"));
        dlg.setMinimumSize(560, 640);

        QVBoxLayout *lay = new QVBoxLayout(&dlg);
        lay->setSpacing(12);

        QLabel *info = new QLabel("File types that always open in a chosen application.");
        info->setWordWrap(true);
        info->setStyleSheet(themed("QLabel { color:#CCCCCC; font-size:18px; }"));
        lay->addWidget(info);

        QListWidget *list = new QListWidget;
        list->setStyleSheet(themed(
            "QListWidget { background:#333; color:white; font-size:22px; border:none; border-radius:8px; }"
            "QListWidget::item { padding:16px; }"
            "QListWidget::item:selected { background:#2a82da; }"
        ));
        list->setMinimumHeight(420);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        QScroller::ungrabGesture(list);
        lay->addWidget(list, 1);

        std::function<void()> reload = [&]() {
            list->clear();
            if (!settings) return;

            settings->beginGroup("DefaultApps");
            QStringList exts = settings->childKeys();
            settings->endGroup();
            exts.sort();

            if (exts.isEmpty()) {
                QListWidgetItem *empty = new QListWidgetItem(
                    "Nothing set yet - use OpenWith on a file and tick "
                    "\"Always open ... with this\".");
                empty->setData(Qt::UserRole, "");
                list->addItem(empty);
                return;
            }

            for (const QString &ext : exts) {
                const QString name = defaultNameFor(ext);
                const QString tmpl = defaultExecFor(ext);
                QListWidgetItem *it = new QListWidgetItem(
                    "." + ext + "      " + (name.isEmpty() ? tmpl : name));
                it->setData(Qt::UserRole, ext);
                list->addItem(it);
            }
        };
        reload();

        auto bigBtn = [](const QString &text) {
            QPushButton *b = new QPushButton(text);
            b->setMinimumHeight(64);
            b->setStyleSheet(themed(
                "QPushButton { background:#555; color:white; border:none; border-radius:12px; "
                "padding:10px 20px; font-size:20px; }"
                "QPushButton:hover { background:#666; }"
                "QPushButton:pressed { background:#444; }"
            ));
            return b;
        };

        QPushButton *removeBtn = bigBtn("Remove");
        QPushButton *clearBtn  = bigBtn("Remove all");
        QPushButton *closeBtn  = bigBtn("Close");

        QHBoxLayout *row = new QHBoxLayout;
        row->setSpacing(12);
        row->addWidget(removeBtn);
        row->addWidget(clearBtn);
        row->addStretch(1);
        row->addWidget(closeBtn);
        lay->addLayout(row);

        connect(removeBtn, &QPushButton::clicked, &dlg, [&]() {
            QListWidgetItem *cur = list->currentItem();
            if (!cur) return;
            const QString ext = cur->data(Qt::UserRole).toString();
            if (ext.isEmpty()) return;
            clearDefaultAppFor(ext);
            reload();
        });

        connect(clearBtn, &QPushButton::clicked, &dlg, [&]() {
            QMessageBox confirm(&dlg);
            confirm.setWindowTitle("Default applications");
            confirm.setText("Forget every remembered file type?");
            confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            confirm.setDefaultButton(QMessageBox::No);
            if (confirm.exec() != QMessageBox::Yes) return;

            if (settings) {
                settings->beginGroup("DefaultApps");
                settings->remove("");
                settings->endGroup();
                settings->beginGroup("DefaultAppNames");
                settings->remove("");
                settings->endGroup();
                settings->sync();
            }
            reload();
        });

        connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

        dlg.exec();
    }

    void openWithSelection() {
        if (selectedPaths.size() != 1) return;

        QString filePath = *selectedPaths.begin();
        if (!QFileInfo(filePath).isFile()) return;

        const QString ext = extensionKeyFor(filePath);

        QDialog dlg(this);
        dlg.setWindowTitle("Open with");
        dlg.setStyleSheet(themed("QDialog { background:#282828; color:white; }"));
        dlg.setMinimumSize(560, 760);

        QVBoxLayout *layout = new QVBoxLayout(&dlg);
        layout->setSpacing(12);

        QLabel *heading = new QLabel(QFileInfo(filePath).fileName());
        heading->setWordWrap(true);
        heading->setStyleSheet(themed("QLabel { color:#DDDDDD; font-size:20px; }"));
        layout->addWidget(heading);

        const QString currentDefault = defaultNameFor(ext);
        if (!currentDefault.isEmpty()) {
            QLabel *cur = new QLabel(
                QString(".%1 files currently open in %2").arg(ext, currentDefault));
            cur->setWordWrap(true);
            cur->setStyleSheet(themed("QLabel { color:#CCCCCC; font-size:16px; }"));
            layout->addWidget(cur);
        }

        QListWidget *list = new QListWidget;
        list->setStyleSheet(themed(
            "QListWidget { background:#333; color:white; font-size:22px; border:none; border-radius:8px; }"
            "QListWidget::item { padding:16px; }"
            "QListWidget::item:selected { background:#2a82da; }"
        ));
        list->setMinimumHeight(460);
        list->setIconSize(QSize(44, 44));
        layout->addWidget(list, 1);

        QScroller::ungrabGesture(list);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

        QVector<DesktopApp> apps = loadDesktopApps();
        std::sort(apps.begin(), apps.end(),
                  [](const DesktopApp &a, const DesktopApp &b) {
                      return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
                  });

        const QString currentExec = defaultExecFor(ext);
        for (const DesktopApp &app : apps) {
            QListWidgetItem *item = new QListWidgetItem(app.name, list);
            item->setData(Qt::UserRole, app.exec);
            if (!app.icon.isEmpty()) {
                QIcon ic = QIcon::fromTheme(app.icon);
                if (!ic.isNull()) item->setIcon(ic);
            }
            if (!currentExec.isEmpty() && app.exec == currentExec)
                list->setCurrentItem(item);   // preselect the remembered app
        }

        QLineEdit *cmdEdit = new QLineEdit;
        cmdEdit->setPlaceholderText("Custom command (e.g. gimp %f)");
        cmdEdit->setMinimumHeight(56);
        cmdEdit->setStyleSheet(themed(
            "QLineEdit { background:#333; color:#DDDDDD; border-radius:8px; padding:10px; font-size:20px; }"
        ));
        layout->addWidget(cmdEdit);

        QCheckBox *always = new QCheckBox(
            ext.isEmpty() ? QString("Always use this application")
                          : QString("Always open .%1 files with this").arg(ext));
        always->setEnabled(!ext.isEmpty());
        always->setStyleSheet(themed(
            "QCheckBox { color:white; font-size:20px; spacing:14px; padding:6px; }"
            "QCheckBox::indicator { width:34px; height:34px; }"
            "QCheckBox::indicator:unchecked { background:#333; border:2px solid #666; border-radius:6px; }"
            "QCheckBox::indicator:checked { background:#2a82da; border:2px solid #2a82da; border-radius:6px; }"
            "QCheckBox:disabled { color:#666; }"
        ));
        layout->addWidget(always);

        auto bigBtn = [](const QString &text, bool accent = false) {
            QPushButton *b = new QPushButton(text);
            b->setMinimumHeight(72);
            b->setMinimumWidth(150);
            if (accent) {
                b->setStyleSheet(themed(
                    "QPushButton { background:#2a82da; color:white; border:none; border-radius:12px; "
                    "padding:10px 24px; font-size:24px; }"
                    "QPushButton:hover { background:#3a92ea; }"
                    "QPushButton:pressed { background:#1a72ca; }"
                ));
            } else {
                b->setStyleSheet(themed(
                    "QPushButton { background:#555; color:white; border:none; border-radius:12px; "
                    "padding:10px 24px; font-size:24px; }"
                    "QPushButton:hover { background:#666; }"
                    "QPushButton:pressed { background:#444; }"
                ));
            }
            return b;
        };

        QPushButton *manageBtn = bigBtn("Defaults");
        QPushButton *cancelBtn = bigBtn("Cancel");
        QPushButton *openBtn   = bigBtn("Open", true);

        QHBoxLayout *btnRow = new QHBoxLayout;
        btnRow->setSpacing(12);
        btnRow->addWidget(manageBtn);
        btnRow->addStretch(1);
        btnRow->addWidget(cancelBtn);
        btnRow->addWidget(openBtn);
        layout->addLayout(btnRow);

        connect(manageBtn, &QPushButton::clicked, &dlg, [this]() {
            showDefaultAppsDialog();
        });
        connect(openBtn,   &QPushButton::clicked, &dlg, &QDialog::accept);
        connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(list, &QListWidget::itemDoubleClicked, &dlg,
                [&dlg](QListWidgetItem *) { dlg.accept(); });

        if (dlg.exec() != QDialog::Accepted) return;

        QString cmd;
        QString chosenTemplate;
        QString chosenName;

        QListWidgetItem *cur = list->currentItem();
        QString custom = cmdEdit->text().trimmed();

        if (!custom.isEmpty()) {
            // a typed command wins over the highlighted list entry
            chosenTemplate = custom.contains('%') ? custom : custom + " %f";
            chosenName     = custom.section(' ', 0, 0);
            cmd            = buildExecCommand(chosenTemplate, filePath);
        } else if (cur) {
            chosenTemplate = cur->data(Qt::UserRole).toString();
            chosenName     = cur->text();
            cmd            = buildExecCommand(chosenTemplate, filePath);
        } else {
            return;
        }

        if (always->isChecked() && !ext.isEmpty())
            setDefaultAppFor(ext, chosenName, chosenTemplate);

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

    // True if the disk (e.g. "sdb" or "mmcblk1") is storage the user can
    // remove: USB-attached, flagged removable, or an SD/TF card in a slot.
    // Uses /sys/block, which works regardless of lsblk version.
    static bool isUsbOrRemovableDisk(const QString &diskName) {
        QString sysPath = "/sys/block/" + diskName;

        // eMMC boot and rpmb areas are never user storage
        if (diskName.contains("boot") || diskName.contains("rpmb")) return false;

        // SD/TF cards sit behind an MMC host controller, so they report
        // removable=0 (exactly like soldered eMMC) and their device path
        // contains "/mmc_host", not "/usb". The card type node tells the two
        // apart: "SD" is a card in a slot, "MMC" is the internal eMMC.
        if (diskName.startsWith("mmcblk")) {
            QFile ty(sysPath + "/device/type");
            if (ty.open(QIODevice::ReadOnly)) {
                QString t = QString::fromLatin1(ty.readAll()).trimmed();
                return t.compare("SD", Qt::CaseInsensitive) == 0;
            }
            // No type node: the SD Configuration Register only exists on
            // real SD cards, so its presence is a reliable second opinion.
            return QFile::exists(sysPath + "/device/scr");
        }

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
            usbBtn->setStyleSheet(themed(
                "QPushButton { background:#2a82da; color:white; border:none; border-radius:10px; font-size:18px; }"
                "QPushButton:hover { background:#3a92ea; }"
                "QPushButton:pressed { background:#1a72ca; }"
            ));
            if (statusLabel) statusLabel->setText("USB device attached");
        }
    }

    void resetUsbButtonStyle() {
        usbBtn->setStyleSheet(themed(
            "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:18px; }"
            "QPushButton:hover { background:#666; }"
            "QPushButton:pressed { background:#444; }"
        ));
    }

    void showUsbDialog() {
        resetUsbButtonStyle();

        QVector<UsbPartition> parts = enumerateUsbPartitions();

        QDialog dlg(this);
        dlg.setWindowTitle("Removable Drives");
        dlg.setStyleSheet(themed("background:#282828; color:white;"));
        dlg.setMinimumWidth(460);

        QVBoxLayout *lay = new QVBoxLayout(&dlg);
        lay->setSpacing(10);

        if (parts.isEmpty()) {
            QLabel *none = new QLabel("No USB drives or SD cards detected.");
            none->setStyleSheet(themed("font-size:15px; padding:20px;"));
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
            b->setStyleSheet(themed(
                "QPushButton { background:#444; color:white; border:none; border-radius:10px;"
                " padding:10px; font-size:14px; text-align:left; }"
                "QPushButton:hover { background:#555; }"
                "QPushButton:pressed { background:#333; }"
            ));
            row->addWidget(b, 1);

            // Eject button (only useful when mounted, but always shown;
            // greyed out when there is nothing to unmount)
            QPushButton *eject = new QPushButton("⏏");
            eject->setFixedSize(64, 64);
            eject->setEnabled(!p.mountPoint.isEmpty());
            eject->setStyleSheet(themed(
                "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:22px; }"
                "QPushButton:hover:enabled { background:#dd3333; }"
                "QPushButton:pressed:enabled { background:#aa0000; }"
                "QPushButton:disabled { background:#222; color:#555; }"
            ));
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
            b->setStyleSheet(themed(
                "QPushButton { background:#555; color:white; border:none; border-radius:10px; font-size:14px; }"
                "QPushButton:hover { background:#666; }"
                "QPushButton:pressed { background:#444; }"
            ));
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
        // A whole-disk filesystem (no partition suffix) is already the disk;
        // stripping digits would turn /dev/mmcblk1 into /dev/mmcblk.
        QString disk = p.devPath;
        if (!QFile::exists("/sys/block/" + QFileInfo(disk).fileName())) {
            while (!disk.isEmpty() && disk.back().isDigit()) disk.chop(1);
            if (disk.endsWith('p') && disk.contains("mmcblk")) disk.chop(1);   // mmcblk0p1 -> mmcblk0
        }
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
        dlg.setStyleSheet(themed("QDialog { background:#282828; color:white; }"));
        dlg.setMinimumWidth(420);
        QVBoxLayout *layout = new QVBoxLayout(&dlg);
        layout->setSpacing(10);

        QLabel *title = new QLabel("Saved servers");
        title->setStyleSheet(themed("QLabel { color:#CCCCCC; font-size:14px; }"));
        layout->addWidget(title);

        QListWidget *list = new QListWidget;
        list->setStyleSheet(themed(
            "QListWidget { background:#333; color:white; font-size:16px; border:none; border-radius:8px; }"
            "QListWidget::item { padding:8px; }"
            "QListWidget::item:selected { background:#555; }"
        ));
        QScroller::grabGesture(list, QScroller::LeftMouseButtonGesture);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        for (const QString &s : savedServers)
            list->addItem(s);
        layout->addWidget(list, 1);

        // Address entry
        QLabel *addrLbl = new QLabel("Server address");
        addrLbl->setStyleSheet(themed("QLabel { color:#CCCCCC; font-size:14px; }"));
        layout->addWidget(addrLbl);

        QLineEdit *addrEdit = new QLineEdit;
        addrEdit->setPlaceholderText("smb://server/share, sftp://host, ftp://host ...");
        addrEdit->setStyleSheet(themed(
            "QLineEdit { background:#333; color:#DDDDDD; border-radius:6px; padding:8px; font-size:16px; }"
        ));
        layout->addWidget(addrEdit);

        // Find servers on the network instead of typing an address
        QPushButton *scanBtn = new QPushButton("🔍  Scan for NAS");
        scanBtn->setFixedHeight(44);
        scanBtn->setStyleSheet(themed(
            "QPushButton { background:#2a82da; color:white; border:none; border-radius:8px; "
            "padding:8px 20px; font-size:15px; }"
            "QPushButton:hover { background:#3a92ea; }"
            "QPushButton:pressed { background:#1a72ca; }"
        ));
        layout->addWidget(scanBtn);
        connect(scanBtn, &QPushButton::clicked, &dlg, [this, addrEdit]() {
            showNasScanDialog(addrEdit);
        });

        // Optional credentials
        QHBoxLayout *credRow = new QHBoxLayout;
        QLineEdit *userEdit = new QLineEdit;
        userEdit->setPlaceholderText("Username (optional)");
        QLineEdit *passEdit = new QLineEdit;
        passEdit->setPlaceholderText("Password (optional)");
        passEdit->setEchoMode(QLineEdit::Password);
        QString credStyle =
            "QLineEdit { background:#333; color:#DDDDDD; border-radius:6px; padding:8px; font-size:16px; }";
        userEdit->setStyleSheet(themed(credStyle));
        passEdit->setStyleSheet(themed(credStyle));
        credRow->addWidget(userEdit);
        credRow->addWidget(passEdit);
        layout->addLayout(credRow);

        // Buttons
        QHBoxLayout *btnRow = new QHBoxLayout;
        auto makeDlgBtn = [](const QString &text) {
            QPushButton *b = new QPushButton(text);
            b->setFixedHeight(44);
            b->setStyleSheet(themed(
                "QPushButton { background:#555; color:white; border:none; border-radius:8px; "
                "padding:8px 20px; font-size:15px; }"
                "QPushButton:hover { background:#666; }"
                "QPushButton:pressed { background:#444; }"
            ));
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
        // smb needs a share name: smb://host alone mounts nothing browsable
        if (url.startsWith("smb://", Qt::CaseInsensitive) &&
            url.section("://", 1).section('/', 1, 1).isEmpty()) {
            QMessageBox::warning(this, "Network",
                "Please include a share name, e.g.\n\n"
                "smb://" + url.section("://", 1).section('/', 0, 0)
                         + "/videos");
            return;
        }
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
        host = host.toLower();                          // gvfs lowercases the server name
        QString share = rest.section('/', 1, 1).toLower();

        QDir root(gvfsRoot());

        // gvfs mount directory names are deterministic - try building the
        // path directly, which works even if scanning the root misbehaves
        if (url.startsWith("smb://", Qt::CaseInsensitive) && !share.isEmpty()) {
            QString direct = root.absoluteFilePath(
                QString("smb-share:server=%1,share=%2").arg(host, share));
            if (QFileInfo(direct).isDir()) return direct;
        }

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
    // ================= NAS DISCOVERY =================

    // Local IPv4 addresses with prefix, e.g. "192.168.1.42/24".
    // Read from iproute2 so no extra Qt module is needed.
    static QStringList localIPv4Cidrs() {
        QStringList out;
        QProcess p;
        p.start("ip", QStringList() << "-o" << "-4" << "addr" << "show");
        if (!p.waitForStarted(2000) || !p.waitForFinished(4000)) { p.kill(); return out; }
        const QStringList lines =
            QString::fromUtf8(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QStringList f = line.split(' ', Qt::SkipEmptyParts);
            int i = f.indexOf("inet");
            if (i < 0 || i + 1 >= f.size()) continue;
            const QString cidr = f.at(i + 1);
            if (cidr.startsWith("127.")) continue;
            if (!out.contains(cidr)) out << cidr;
        }
        return out;
    }

    // Non-blocking TCP connect sweep across the local /24.
    // Returns the addresses that accepted a connection on 'port'.
    static QStringList sweepPort(const QString &localIp, quint16 port, int timeoutMs) {
        QStringList found;
        const QStringList o = localIp.split('.');
        if (o.size() != 4) return found;
        const QString prefix = o[0] + "." + o[1] + "." + o[2] + ".";

        QVector<int> fds;
        QVector<QString> addrs;

        for (int host = 1; host <= 254; ++host) {
            const QString ip = prefix + QString::number(host);
            if (ip == localIp) continue;

            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            if (fd >= FD_SETSIZE) { ::close(fd); continue; }   // select() cannot watch it

            int fl = ::fcntl(fd, F_GETFL, 0);
            ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);

            struct sockaddr_in sa;
            ::memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port   = htons(port);
            if (::inet_pton(AF_INET, ip.toLatin1().constData(), &sa.sin_addr) != 1) {
                ::close(fd);
                continue;
            }

            int rc = ::connect(fd, (struct sockaddr *)&sa, sizeof(sa));
            if (rc == 0) {                       // accepted immediately
                found << ip;
                ::close(fd);
                continue;
            }
            if (rc < 0 && errno != EINPROGRESS) { ::close(fd); continue; }

            fds.append(fd);
            addrs.append(ip);
        }

        QElapsedTimer timer;
        timer.start();
        while (!fds.isEmpty() && timer.elapsed() < timeoutMs) {
            fd_set wfds;
            FD_ZERO(&wfds);
            int maxFd = -1;
            for (int fd : fds) { FD_SET(fd, &wfds); if (fd > maxFd) maxFd = fd; }

            int remain = timeoutMs - (int)timer.elapsed();
            if (remain < 0) remain = 0;
            int slice = qMin(remain, 100);
            struct timeval tv;
            tv.tv_sec  = slice / 1000;
            tv.tv_usec = (slice % 1000) * 1000;

            int n = ::select(maxFd + 1, nullptr, &wfds, nullptr, &tv);
            if (n > 0) {
                QVector<int> stillFds;
                QVector<QString> stillAddrs;
                for (int i = 0; i < fds.size(); ++i) {
                    int fd = fds[i];
                    if (!FD_ISSET(fd, &wfds)) {
                        stillFds.append(fd);
                        stillAddrs.append(addrs[i]);
                        continue;
                    }
                    int err = 0;
                    socklen_t len = sizeof(err);
                    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0)
                        found << addrs[i];
                    ::close(fd);
                }
                fds   = stillFds;
                addrs = stillAddrs;
            }
            QCoreApplication::processEvents();
        }

        for (int fd : fds) ::close(fd);

        // return them in address order, not in the order select() woke up
        QStringList ordered;
        for (int host = 1; host <= 254; ++host) {
            const QString ip = prefix + QString::number(host);
            if (found.contains(ip)) ordered << ip;
        }
        return ordered;
    }

    // Best-effort friendly name for a discovered server (NetBIOS, then DNS).
    // Every lookup is bounded so a silent host cannot stall the scan.
    static QString nasHostName(const QString &ip) {
        QProcess nb;
        nb.start("nmblookup", QStringList() << "-A" << ip);
        if (nb.waitForStarted(1000) && nb.waitForFinished(2500)) {
            const QStringList lines =
                QString::fromUtf8(nb.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
            for (const QString &l : lines) {
                const QString t = l.trimmed();
                if (!t.contains("<20>") && !t.contains("<00>")) continue;
                if (t.contains("GROUP", Qt::CaseInsensitive)) continue;
                const QString n = t.section('<', 0, 0).trimmed();
                if (!n.isEmpty()) return n;
            }
        } else {
            nb.kill();
        }

        QProcess ge;
        ge.start("getent", QStringList() << "hosts" << ip);
        if (ge.waitForStarted(1000) && ge.waitForFinished(1500) && ge.exitCode() == 0) {
            const QStringList f =
                QString::fromUtf8(ge.readAllStandardOutput()).trimmed()
                    .split(' ', Qt::SkipEmptyParts);
            if (f.size() > 1) {
                const QString n = f.at(1).section('.', 0, 0);
                if (!n.isEmpty()) return n;
            }
        } else {
            ge.kill();
        }

        return QString();
    }

    // Shares exported by an SMB host. Uses smbclient when it is installed and
    // falls back to gvfs, which osm-files already needs for network browsing.
    static QStringList listSmbShares(const QString &host) {
        QStringList shares;

        QProcess sc;
        sc.setProcessChannelMode(QProcess::MergedChannels);
        sc.start("smbclient", QStringList() << "-L" << host << "-N" << "-g");
        if (sc.waitForStarted(1500) && sc.waitForFinished(8000)) {
            const QStringList lines =
                QString::fromUtf8(sc.readAll()).split('\n', Qt::SkipEmptyParts);
            for (const QString &l : lines) {
                if (!l.startsWith("Disk|")) continue;      // grepable output
                const QString n = l.section('|', 1, 1).trimmed();
                if (n.isEmpty() || n.endsWith('$')) continue;   // hide admin shares
                if (!shares.contains(n)) shares << n;
            }
        } else {
            sc.kill();
        }
        if (!shares.isEmpty()) return shares;

        QProcess gm;
        gm.setProcessChannelMode(QProcess::MergedChannels);
        gm.start("gio", QStringList() << "mount" << ("smb://" + host));
        if (gm.waitForStarted(1500)) {
            gm.write("\n\n\n");                             // try anonymous
            gm.closeWriteChannel();
            if (!gm.waitForFinished(10000)) gm.kill();
        }

        QProcess gl;
        gl.start("gio", QStringList() << "list" << ("smb://" + host));
        if (gl.waitForStarted(1500) && gl.waitForFinished(8000)) {
            const QStringList lines =
                QString::fromUtf8(gl.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
            for (const QString &l : lines) {
                QString n = l.trimmed();
                while (n.endsWith('/')) n.chop(1);
                if (n.isEmpty() || n.endsWith('$')) continue;
                if (!shares.contains(n)) shares << n;
            }
        } else {
            gl.kill();
        }
        return shares;
    }

    // Scan the local network for SMB file servers and let the user pick one.
    // The chosen address is written into the Network dialog's address box.
    void showNasScanDialog(QLineEdit *addrEdit) {
        QDialog dlg(this);
        dlg.setWindowTitle("Scan for NAS");
        dlg.setStyleSheet(themed("QDialog { background:#282828; color:white; }"));
        dlg.setMinimumWidth(420);
        dlg.setMinimumHeight(420);

        QVBoxLayout *lay = new QVBoxLayout(&dlg);
        lay->setSpacing(10);

        QLabel *status = new QLabel("Ready to scan.");
        status->setWordWrap(true);
        status->setStyleSheet(themed("QLabel { color:#CCCCCC; font-size:14px; }"));
        lay->addWidget(status);

        QListWidget *list = new QListWidget;
        list->setStyleSheet(themed(
            "QListWidget { background:#333; color:white; font-size:16px; border:none; border-radius:8px; }"
            "QListWidget::item { padding:10px; }"
            "QListWidget::item:selected { background:#555; }"
        ));
        QScroller::grabGesture(list, QScroller::LeftMouseButtonGesture);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        lay->addWidget(list, 1);

        QHBoxLayout *btnRow = new QHBoxLayout;
        auto mkBtn = [](const QString &text) {
            QPushButton *b = new QPushButton(text);
            b->setFixedHeight(44);
            b->setStyleSheet(themed(
                "QPushButton { background:#555; color:white; border:none; border-radius:8px; "
                "padding:8px 20px; font-size:15px; }"
                "QPushButton:hover { background:#666; }"
                "QPushButton:pressed { background:#444; }"
            ));
            return b;
        };
        QPushButton *rescanBtn = mkBtn("Scan again");
        QPushButton *closeBtn  = mkBtn("Close");
        btnRow->addWidget(rescanBtn);
        btnRow->addStretch(1);
        btnRow->addWidget(closeBtn);
        lay->addLayout(btnRow);

        std::function<void()> runScan = [&]() {
            list->clear();
            rescanBtn->setEnabled(false);
            status->setText("Scanning your network for file servers ...");
            QCoreApplication::processEvents();

            const QStringList cidrs = localIPv4Cidrs();
            if (cidrs.isEmpty()) {
                status->setText("No network connection found.\n"
                                "Connect to Wi-Fi or Ethernet, then scan again.");
                rescanBtn->setEnabled(true);
                return;
            }

            QStringList hosts;
            for (const QString &cidr : cidrs) {
                const QString ip = cidr.section('/', 0, 0);
                for (const QString &h : sweepPort(ip, 445, 1500))
                    if (!hosts.contains(h)) hosts << h;
            }
            if (hosts.isEmpty()) {
                // older servers that still only listen on the NetBIOS port
                for (const QString &cidr : cidrs) {
                    const QString ip = cidr.section('/', 0, 0);
                    for (const QString &h : sweepPort(ip, 139, 1500))
                        if (!hosts.contains(h)) hosts << h;
                }
            }

            if (hosts.isEmpty()) {
                status->setText("No file servers found on this network.\n"
                                "If your NAS is switched on, you can still type its "
                                "address into the Network window.");
                rescanBtn->setEnabled(true);
                return;
            }

            status->setText(QString("Found %1 server(s). Tap one to see its shared folders.")
                                .arg(hosts.size()));
            QCoreApplication::processEvents();

            for (const QString &ip : hosts) {
                const QString name = nasHostName(ip);
                QListWidgetItem *it = new QListWidgetItem(
                    name.isEmpty() ? ("🖧  " + ip) : ("🖧  " + name + "   (" + ip + ")"));
                it->setData(Qt::UserRole,     ip);      // host
                it->setData(Qt::UserRole + 1, "");      // share (none yet)
                it->setData(Qt::UserRole + 2, false);   // shares already listed?
                list->addItem(it);
                QCoreApplication::processEvents();
            }
            rescanBtn->setEnabled(true);
        };

        connect(list, &QListWidget::itemClicked, &dlg, [&](QListWidgetItem *item) {
            const QString host  = item->data(Qt::UserRole).toString();
            const QString share = item->data(Qt::UserRole + 1).toString();

            if (!share.isEmpty()) {                      // a share was tapped
                addrEdit->setText("smb://" + host + "/" + share);
                dlg.accept();
                return;
            }
            if (host.isEmpty()) return;
            if (item->data(Qt::UserRole + 2).toBool()) return;   // already expanded
            item->setData(Qt::UserRole + 2, true);

            status->setText("Reading shared folders from " + host + " ...");
            QCoreApplication::processEvents();

            const QStringList shares = listSmbShares(host);
            int at = list->row(item) + 1;

            if (shares.isEmpty()) {
                QListWidgetItem *none = new QListWidgetItem(
                    "        no shared folders visible - a username and password "
                    "may be needed");
                none->setData(Qt::UserRole,     "");
                none->setData(Qt::UserRole + 1, "");
                none->setData(Qt::UserRole + 2, true);
                list->insertItem(at, none);
                status->setText("Tap a server to see its shared folders.");
                return;
            }

            for (const QString &s : shares) {
                QListWidgetItem *si = new QListWidgetItem("        📁  " + s);
                si->setData(Qt::UserRole,     host);
                si->setData(Qt::UserRole + 1, s);
                si->setData(Qt::UserRole + 2, true);
                list->insertItem(at++, si);
            }
            status->setText("Tap a shared folder to use it.");
        });

        connect(rescanBtn, &QPushButton::clicked, &dlg, [&]() { runScan(); });
        connect(closeBtn,  &QPushButton::clicked, &dlg, &QDialog::reject);

        QTimer::singleShot(0, &dlg, [&]() { runScan(); });
        dlg.exec();
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
