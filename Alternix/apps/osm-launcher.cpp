#include <QApplication>
#include <QWidget>
#include <QScrollArea>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QDirIterator>
#include <QSettings>
#include <QIcon>
#include <QPixmap>
#include <QProcess>
#include <QFrame>
#include <QFileInfo>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QScroller>
#include <QScrollerProperties>
#include <QScreen>
#include <QPainter>
#include <QLinearGradient>
#include <QVector>
#include <QDir>
#include <QLockFile>
#include <algorithm>

// ──────────────────────────────  Window with built-in top/bottom fade
class LauncherWindow : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(0,0,0,140));

        int w = width();
        int h = height();

        const int fadeH = 75;

        // Top fade — black → transparent
        QLinearGradient top(0, 0, 0, fadeH * 2);
        top.setColorAt(0.0, QColor(0, 0, 0, 255));
        top.setColorAt(0.5, QColor(0,0,0,255));   // <- extend black
        top.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.fillRect(0, 0, w, fadeH * 3, top);

        // Bottom fade — transparent → black
        QLinearGradient bottom(0, h - fadeH * 2, 0, h);
        bottom.setColorAt(0.0, QColor(0, 0, 0, 0));
        bottom.setColorAt(0.5, QColor(0,0,0,255)); // <- start fade later
        bottom.setColorAt(1.0, QColor(0, 0, 0, 255));
        p.fillRect(0, h - fadeH * 2, w, fadeH * 2, bottom);
    }
};

// ──────────────────────────────  Clean exec fields (unchanged)
static QString cleanExec(const QString &exec) {
    QString s = exec;
    for (auto rep : {"%U","%u","%F","%f","%i","%c","%k"}) s.replace(rep, "");
    return s.trimmed();
}

struct AppEntry {
    QString name;
    QString exec;
    QString icon;
};

static QStringList standardDesktopDirs() {
    QStringList dirs;
    dirs << QDir::homePath() + "/.local/share/applications"
         << "/usr/share/applications"
         << QDir::homePath() + "/.local/share/flatpak/exports/share/applications"
         << "/var/lib/flatpak/exports/share/applications"
         << "/var/lib/snapd/desktop/applications";
    return dirs;
}

static QList<AppEntry> loadDesktopEntries() {
    QList<AppEntry> apps;
    for (const QString &dir : standardDesktopDirs()) {
        QDirIterator it(dir, QStringList() << "*.desktop", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QString path = it.next();
            QSettings s(path, QSettings::IniFormat);
            s.beginGroup("Desktop Entry");
            QString name = s.value("Name").toString();
            QString exec = s.value("Exec").toString();
            QString icon = s.value("Icon").toString();
            QString nodisplay = s.value("NoDisplay", "false").toString().toLower();
            s.endGroup();
            if (name.isEmpty() || exec.isEmpty() || nodisplay == "true") continue;
            apps.append({name, cleanExec(exec), icon});
        }
    }
    std::sort(apps.begin(), apps.end(),
              [](const AppEntry &a, const AppEntry &b){ return a.name.toLower() < b.name.toLower(); });
    return apps;
}

// ──────────────────────────────  AppTile (lazy icon loading)
class AppTile : public QFrame {
    AppEntry entry;
    bool dragging = false;
    QPoint startPos;
    QLabel *iconLabel;

public:
    explicit AppTile(const AppEntry &e, QWidget *parent=nullptr)
        : QFrame(parent), entry(e), iconLabel(nullptr)
    {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAutoFillBackground(false);
        setStyleSheet("QFrame { background: #00000099; border: none; }");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setMinimumSize(150, 160);

        QWidget *hoverBox = new QWidget(this);
        hoverBox->setObjectName("hoverBox");
        hoverBox->setStyleSheet(
            "#hoverBox { background: #80708099; border-radius: 20px; }"
            "#hoverBox:hover { background-color: #282828; border: 1px solid #ffffff; }"
        );

        QVBoxLayout *v = new QVBoxLayout(hoverBox);
        v->setContentsMargins(16,16,16,16);
        v->setSpacing(8);
        v->setAlignment(Qt::AlignCenter);

        int iconSize = QApplication::primaryScreen()->size().width() <= 720 ? 32 : 32;

        iconLabel = new QLabel(hoverBox);
        iconLabel->setAlignment(Qt::AlignCenter);

        // Transparent placeholder to keep layout stable
        QPixmap placeholder(iconSize, iconSize);
        placeholder.fill(Qt::transparent);
        iconLabel->setPixmap(placeholder);

        v->addWidget(iconLabel);

        QLabel *lbl = new QLabel(e.name, hoverBox);
        lbl->setWordWrap(true);
        lbl->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        lbl->setMinimumHeight(60);
        lbl->setMaximumHeight(200);
        lbl->setStyleSheet("color: white; font-size: 15pt; line-height: 110%;");
        v->addWidget(lbl);

        QVBoxLayout *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0,0,0,0);
        outer->addWidget(hoverBox);
    }

    // Called later by the timer to actually load the icon
    void loadIcon() {
        if (!iconLabel)
            return;

        int iconSize = QApplication::primaryScreen()->size().width() <= 720 ? 32 : 32;

        // If there is no icon defined at all, go straight to the puzzle placeholder
        if (entry.icon.trimmed().isEmpty()) {
            iconLabel->setPixmap(QPixmap());
            iconLabel->setText("🧩");
            iconLabel->setStyleSheet("font-size:48px;");
            return;
        }

        QPixmap pix;
        QIcon ic = QIcon::fromTheme(entry.icon);
        if (!ic.isNull())
            pix = ic.pixmap(iconSize, iconSize);
        if (pix.isNull() && QFileInfo(entry.icon).exists())
            pix.load(entry.icon);

        if (!pix.isNull()) {
            iconLabel->setText(QString());
            iconLabel->setStyleSheet(QString());
            iconLabel->setPixmap(
                pix.scaled(iconSize, iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            );
        } else {
            // fallback if icon cannot be resolved
            iconLabel->setPixmap(QPixmap());
            iconLabel->setText("🧩");
            iconLabel->setStyleSheet("font-size:48px;");
        }
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        dragging = false;
        startPos = e->pos();
        QFrame::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if ((e->pos() - startPos).manhattanLength() > 10)
            dragging = true;
        QFrame::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (!dragging && e->button() == Qt::LeftButton) {
            QStringList args = entry.exec.split(' ');
            if (!args.isEmpty()) {
                QString prog = args.takeFirst();
                QProcess::startDetached(prog, args);
            }
            if (QWidget *w = window()) w->close();
        }
        QFrame::mouseReleaseEvent(e);
    }
};

// ──────────────────────────────  Escape-to-close (unchanged)
class KeyFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (ev->type() == QEvent::KeyPress) {
            QKeyEvent *ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape) {
                if (auto w = qobject_cast<QWidget*>(obj)) {
                    while (w && !w->isWindow()) w = w->parentWidget();
                    if (w) w->close();
                    return true;
                }
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

// ──────────────────────────────  main
int main(int argc, char **argv) {
    QApplication a(argc, argv);

    // ───────── Single-instance guard using QLockFile
    QDir cacheDir(QDir::homePath() + "/Alternix/.cache");
    cacheDir.mkpath(".");  // ensure directory exists

    QString lockPath = cacheDir.absoluteFilePath("osm-launcher.lock");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);  // never auto-steal stale locks

    if (!lock.tryLock(0)) {
        // Another osm-launcher is already running
        return 0;
    }

    QList<AppEntry> apps = loadDesktopEntries();

    LauncherWindow window;        // ← WINDOW TYPE WITH BUILT-IN FADE
    window.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    window.setAttribute(Qt::WA_TranslucentBackground, true);
    window.setAutoFillBackground(false);
    window.setStyleSheet("background:transparent;");

    QVBoxLayout *main = new QVBoxLayout(&window);
    main->setContentsMargins(20,5,20,5);
    main->setSpacing(12);

    QScrollArea *scroll = new QScrollArea(&window);
    scroll->setWidgetResizable(false);
    scroll->setStyleSheet("border: none; background:transparent;");
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget *container = new QWidget();
    container->setAttribute(Qt::WA_TranslucentBackground, true);
    container->setAutoFillBackground(false);
    container->setStyleSheet("background:transparent;");

    QGridLayout *grid = new QGridLayout(container);
    grid->setSpacing(12);
    grid->setContentsMargins(10,10,10,10);

    for (int i = 0; i < 4; ++i)
        grid->setColumnStretch(i, 1);

    int cols = 4;
    int idx = 0;

    QVector<AppTile*> tiles;
    tiles.reserve(apps.size());

    // Create all tiles with lightweight placeholders only
    for (const AppEntry &e : apps) {
        AppTile *tile = new AppTile(e, container);
        grid->addWidget(tile, idx / cols, idx % cols);
        tiles.append(tile);
        ++idx;
    }

    container->setLayout(grid);
    container->adjustSize();

    int screenWidth = QApplication::primaryScreen()->size().width();

    int contentWidth;
    if (screenWidth <= 800)
        contentWidth = screenWidth - 40;
    else
        contentWidth = static_cast<int>(screenWidth * 0.9);

    scroll->setFixedWidth(contentWidth);
    container->setMinimumWidth(contentWidth);
    container->setMinimumHeight(container->sizeHint().height() + 200);

    scroll->setWidget(container);

    QScroller::grabGesture(scroll->viewport(), QScroller::TouchGesture);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
    QScrollerProperties prop;
    prop.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.1);
    prop.setScrollMetric(QScrollerProperties::MaximumVelocity, 0.4);
    prop.setScrollMetric(QScrollerProperties::SnapPositionRatio, 0.5);
    prop.setScrollMetric(QScrollerProperties::SnapTime, 0.3);
    QScroller::scroller(scroll->viewport())->setScrollerProperties(prop);

    main->addWidget(scroll, 1, Qt::AlignHCenter);

    QHBoxLayout *closeRow = new QHBoxLayout();
    closeRow->addStretch();
    QPushButton *closeBtn = new QPushButton(" ❌ ");
    closeBtn->setFixedHeight(40);
    closeBtn->setStyleSheet(
        "QPushButton { background-color: #66000000; color: red; font-size: 32pt; padding: 6px 18px; border-radius: 8px; }"
        "QPushButton:hover { background-color: #282828; }");
    QObject::connect(closeBtn, &QPushButton::clicked, &window, &QWidget::close);
    closeRow->addWidget(closeBtn);
    closeRow->addStretch();
    main->addLayout(closeRow);

    KeyFilter *kf = new KeyFilter();
    window.installEventFilter(kf);

    window.showFullScreen();

    // ──────────────────────────────  Lazy icon loader
    QTimer *iconTimer = new QTimer(&window);
    iconTimer->setInterval(30);   // 30ms per icon (row-order)
    int current = 0;

    QObject::connect(iconTimer, &QTimer::timeout, &window,
                     [iconTimer, &tiles, &current]() mutable {
        if (current >= tiles.size()) {
            iconTimer->stop();
            return;
        }
        tiles[current]->loadIcon();
        ++current;
    });

    iconTimer->start();

    return a.exec();
}
