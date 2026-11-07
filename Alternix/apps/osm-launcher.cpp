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
#include <QDebug>
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
#include <QScrollBar>
#include <QTouchEvent>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QPainterPath>
#include <QPointer>
#include <QFile>
#include <QMessageBox>
#include <QByteArray>
#include <unistd.h>

// ──────────────────────────────  Window with built-in top/bottom fade
class LauncherWindow : public QWidget {
    bool opening = false;
    bool dismissing = false;
    qreal openingProgress = 0.0;
    qreal dismissProgress = 0.0;
    QPixmap transitionFrame;
    QPointer<QTimer> iconLoader;
    QVector<QPointer<QWidget>> openingChildren;

public:
    using QWidget::QWidget;

    bool isTransitioning() const { return opening || dismissing; }
    void setIconLoader(QTimer *timer) { iconLoader = timer; }

    // Inverse of the dismiss animation: rise/expand from the bottom centre
    // while fading in. The fullscreen window itself never changes geometry,
    // which avoids fighting Qtile's fullscreen window management.
    // Call immediately after showFullScreen(), before the event loop starts,
    // so the unanimated launcher is never painted to the screen first.
    void openWithAnimation() {
        if (isTransitioning()) return;

        transitionFrame = grab(); // Render the finished layout, including child widgets.
        if (transitionFrame.isNull()) return;

        openingChildren.clear();
        const auto children = findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget *child : children) {
            if (child->isVisible()) {
                openingChildren.append(QPointer<QWidget>(child));
                child->hide();
            }
        }

        opening = true;
        openingProgress = 0.0;
        QVariantAnimation *animation = new QVariantAnimation(this);
        animation->setDuration(320);
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(animation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &v) {
            openingProgress = v.toDouble();
            update();
        });
        connect(animation, &QVariantAnimation::finished, this, [this]() {
            opening = false;
            for (const QPointer<QWidget> &child : openingChildren)
                if (child) child->show();
            openingChildren.clear();
            transitionFrame = QPixmap();
            update();
        });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
        update();
    }

    // Keep the existing pull-down-to-dismiss animation unchanged.
    void dismissWithAnimation() {
        if (isTransitioning()) return;

        transitionFrame = grab(); // Capture all tiles, fades and the close button.
        dismissing = true;
        if (iconLoader) iconLoader->stop();

        // The snapshot replaces the live child widgets during the transition.
        const auto children = findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        for (QWidget *child : children) child->hide();

        QVariantAnimation *animation = new QVariantAnimation(this);
        animation->setDuration(320);
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        animation->setEasingCurve(QEasingCurve::InCubic);
        connect(animation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &v) {
            dismissProgress = v.toDouble();
            update();
        });
        connect(animation, &QVariantAnimation::finished, this, [this]() {
            close();
        });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (isTransitioning()) {
            // Both directions use identical geometry. During opening the
            // closing progress simply runs backwards from 1 to 0.
            if (!transitionFrame.isNull()) {
                const qreal t = dismissing ? dismissProgress : 1.0 - openingProgress;
                const qreal scale = 1.0 - 0.82 * t;
                const qreal w = width() * scale;
                const qreal h = height() * scale;
                const QRectF target((width() - w) / 2.0,
                                    height() - h + height() * 0.08 * t,
                                    w, h);
                p.setRenderHint(QPainter::SmoothPixmapTransform);
                p.setOpacity(1.0 - t);
                QPainterPath rounded;
                rounded.addRoundedRect(target, 28.0 * t, 28.0 * t);
                p.setClipPath(rounded);
                p.drawPixmap(target, transitionFrame, QRectF(transitionFrame.rect()));
            }
            return;
        }

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

// App label colour, read in main() from the UI settings config
static QString g_labelColor = "#ffffff";

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

// Existing Alternix authentication: PolicyKit uses /etc/pam.d/polkit-1,
// while sudo / osm-sudo use /etc/pam.d/sudo.  Do NOT replace either with a
// launcher-specific sudo hack; keep the real policy authorization step.
static bool isPolkitName(const QString &path) {
    const QString name = QFileInfo(path).fileName().toLower();
    return name == "pkexec" || name.endsWith("-pkexec");
}

static bool needsPolkit(const QString &command) {
    const QStringList args = QProcess::splitCommand(command);
    for (const QString &arg : args)
        if (isPolkitName(arg)) return true;
    return false;
}

static bool polkitPamUsesAlternix() {
    QFile f("/etc/pam.d/polkit-1");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QByteArray rules = f.readAll();
    return rules.contains("pam_exec.so") &&
           rules.contains("/usr/local/libexec/alternix-auth --pam");
}

static bool isKnownPolkitAgent(const QString &file) {
    const QString n = QFileInfo(file).fileName().toLower();
    return n == "lxpolkit" || n == "lxqt-policykit-agent" ||
           n == "polkit-mate-authentication-agent-1" ||
           n == "polkit-gnome-authentication-agent-1" ||
           n == "polkit-kde-authentication-agent-1" || n == "xfce-polkit";
}

// Use /proc rather than guessing from one package name. A package may have
// installed mate-polkit or policykit-1-gnome rather than lxpolkit.
static bool polkitAgentRunning() {
    const uid_t uid = getuid();
    QDirIterator it("/proc", QDir::Dirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString pidPath = it.next();
        const QString pidName = QFileInfo(pidPath).fileName();
        if (pidName.isEmpty() || !pidName.at(0).isDigit()) continue;
        if (QFileInfo(pidPath).ownerId() != uint(uid)) continue;
        QFile cmd(pidPath + "/cmdline");
        if (!cmd.open(QIODevice::ReadOnly)) continue;
        const QList<QByteArray> argv = cmd.read(8192).split('\0');
        for (const QByteArray &arg : argv) {
            if (arg.isEmpty()) continue;
            if (isKnownPolkitAgent(QString::fromLocal8Bit(arg))) return true;
        }
    }
    return false;
}

static bool startPolkitAgent() {
    if (polkitAgentRunning()) return true;
    // An installed agent is needed before pkexec can request graphical
    // authentication. Start it as this desktop user, never with sudo.
    // Xsession/XDG startup from alternix-auth-setup.sh covers other apps.
    const QStringList paths = {
        "/usr/bin/lxpolkit",
        "/usr/libexec/polkit-mate-authentication-agent-1",
        "/usr/lib/mate-polkit/polkit-mate-authentication-agent-1",
        "/usr/lib/x86_64-linux-gnu/polkit-mate/polkit-mate-authentication-agent-1",
        "/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1",
        "/usr/libexec/polkit-gnome-authentication-agent-1",
        "/usr/bin/lxqt-policykit-agent",
        "/usr/libexec/lxqt-policykit-agent",
        "/usr/libexec/xfce-polkit",
        "/usr/lib/polkit-kde-authentication-agent-1",
        "/usr/libexec/polkit-kde-authentication-agent-1"
    };
    for (const QString &path : paths) {
        if (QFileInfo(path).isExecutable() &&
            QProcess::startDetached(path, QStringList())) return true;
    }
    return false;
}

// Launch a .desktop command without putting it through a shell by default.
// pkexec identifies the authorizing application by its *live parent process*.
// QProcess::startDetached() followed by closing this launcher leaves pkexec
// orphaned, causing "Refusing to render service to dead parents" on polkit.
// For PolicyKit launchers, a detached unprivileged shell stays alive and waits
// for the original command; the command and arguments are supplied as argv,
// never interpolated into executable shell text.
static bool launchDesktopCommand(const QString &command) {
    QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) return false;

    const QString program = parts.takeFirst();
    const QString basename = QFileInfo(program).fileName().toLower();
    const bool needsLiveParent = isPolkitName(basename) || needsPolkit(command);
    if (needsLiveParent) {
        QStringList shellArgs = {
            "-c", "\"$@\"; status=$?; exit \"$status\"", "alternix-app", program
        };
        shellArgs.append(parts);
        return QProcess::startDetached("/bin/sh", shellArgs);
    }
    return QProcess::startDetached(program, parts);
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

        int iconSize = QApplication::primaryScreen()->size().width() <= 720 ? 48 : 48;

        iconLabel = new QLabel(hoverBox);
        iconLabel->setAlignment(Qt::AlignCenter);

        // Transparent placeholder to keep layout stable
        QPixmap placeholder(iconSize, iconSize);
        placeholder.fill(Qt::transparent);
        iconLabel->setPixmap(placeholder);

        v->addWidget(iconLabel);

        QLabel *lbl = new QLabel(e.name, hoverBox);
        lbl->setStyleSheet(QString("color:%1; background:transparent;").arg(g_labelColor));
        lbl->setWordWrap(true);
        lbl->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        lbl->setMinimumHeight(60);
        lbl->setMaximumHeight(200);
        // Font size is inherited from QApplication font (set from UI settings config)
        v->addWidget(lbl);

        QVBoxLayout *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0,0,0,0);
        outer->addWidget(hoverBox);
    }

    // Called later by the timer to actually load the icon
    void loadIcon() {
        if (!iconLabel)
            return;

        int iconSize = QApplication::primaryScreen()->size().width() <= 720 ? 48 : 48;

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
            const QString command = entry.exec;
            if (needsPolkit(command)) {
                if (!polkitPamUsesAlternix()) {
                    QMessageBox::warning(this, "AlternixOS PolicyKit authentication",
                        "The PolicyKit PAM rule is not currently connected to "
                        "the existing Alternix pattern/PIN verifier.\n\n"
                        "The existing authentication setup may have rolled back "
                        "PolicyKit after its live test failed. Run the updated "
                        "alternix-auth-setup.sh from your normal desktop session.");
                    QFrame::mouseReleaseEvent(e);
                    return;
                }
                if (!polkitAgentRunning() && !startPolkitAgent()) {
                    QMessageBox::warning(this, "PolicyKit graphical agent unavailable",
                        "No installed graphical PolicyKit agent could be started "
                        "in the current Qtile session.\n\n"
                        "An existing PolicyKit agent is required before "
                        "Synaptic can request the Alternix pattern/PIN.");
                    QFrame::mouseReleaseEvent(e);
                    return;
                }
                // A newly spawned agent must register over the D-Bus session
                // before pkexec requests authentication. Check again without
                // freezing the desktop or leaving a sudo prompt behind.
                QPointer<QWidget> launcherWindow = window();
                QTimer::singleShot(1100, this, [this, command, launcherWindow]() {
                    if (!polkitAgentRunning()) {
                        QMessageBox::warning(this, "PolicyKit agent could not register",
                            "The graphical agent did not stay running. Check "
                            "the Qtile session D-Bus address and the PolicyKit "
                            "diagnostic output before retrying Synaptic.");
                        return;
                    }
                    if (launchDesktopCommand(command)) {
                        if (launcherWindow) launcherWindow->close();
                    } else {
                        qWarning() << "osm-launcher: failed to launch" << command;
                        QMessageBox::warning(this, "Could not start application",
                                             "Failed to start: " + command);
                    }
                });
                QFrame::mouseReleaseEvent(e);
                return;
            }
            if (launchDesktopCommand(command)) {
                if (QWidget *w = window()) w->close();
            } else {
                qWarning() << "osm-launcher: failed to start desktop command:" << command;
            }
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

// ──────────────────────────────  Pull-down-to-dismiss gesture
// An application-wide filter sees mouse/touch events even when the gesture
// starts on an app tile or its nested labels (not just on the scroll viewport).
// It does not consume ordinary events, so QScroller retains normal scrolling.
class PullDownDismissFilter : public QObject {
    enum Pointer { None, Mouse, Touch };
    LauncherWindow *launcher;
    QScrollArea *scroll;
    Pointer pointer = None;
    QPointF pressPosition;
    bool directionConfirmed = false;
    int touchId = -1;

    bool inAppList(QObject *object) const {
        const QWidget *widget = qobject_cast<QWidget*>(object);
        const QWidget *viewport = scroll->viewport();
        return widget && (widget == viewport || viewport->isAncestorOf(widget));
    }

    bool listIsAtTop() const {
        const QScrollBar *bar = scroll->verticalScrollBar();
        return bar->value() <= bar->minimum() + 1;
    }

    void start(Pointer type, const QPointF &globalPosition) {
        pointer = listIsAtTop() ? type : None;
        pressPosition = globalPosition;
        directionConfirmed = false;
    }

    void move(const QPointF &globalPosition) {
        if (pointer == None || launcher->isTransitioning()) return;
        // Only a pull that BEGINS with the list at the top may close it.
        if (!listIsAtTop()) { pointer = None; return; }

        const qreal dx = globalPosition.x() - pressPosition.x();
        const qreal dy = globalPosition.y() - pressPosition.y();

        // Never reinterpret a normal upward or sideways scroll as dismiss.
        if (!directionConfirmed && qAbs(dx) + qAbs(dy) >= 16.0) {
            if (dy <= 0 || dy < qAbs(dx) * 1.2) {
                pointer = None;
                return;
            }
            directionConfirmed = true;
        }

        // A deliberate pull: 75-125 logical pixels, scaled for screen height.
        const qreal threshold = qBound(75.0, launcher->height() * 0.11, 125.0);
        if (directionConfirmed && dy >= threshold && dy > qAbs(dx) * 1.2) {
            pointer = None;
            QScroller::scroller(scroll->viewport())->stop();
            launcher->dismissWithAnimation();
        }
    }

public:
    PullDownDismissFilter(LauncherWindow *window, QScrollArea *area, QObject *parent = nullptr)
        : QObject(parent), launcher(window), scroll(area) {}

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            if (launcher->isTransitioning()) return true;
            QMouseEvent *mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && pointer != Touch && inAppList(object))
                start(Mouse, mouse->screenPos());
            break;
        }
        case QEvent::MouseMove: {
            if (launcher->isTransitioning()) return true;
            QMouseEvent *mouse = static_cast<QMouseEvent*>(event);
            if (pointer == Mouse) {
                if (mouse->buttons() & Qt::LeftButton) move(mouse->screenPos());
                else pointer = None;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            if (launcher->isTransitioning()) return true;
            QMouseEvent *mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && pointer == Mouse) pointer = None;
            break;
        }
        case QEvent::TouchBegin: {
            if (launcher->isTransitioning()) return true;
            QTouchEvent *touch = static_cast<QTouchEvent*>(event);
            if (inAppList(object) && touch->touchPoints().size() == 1) {
                const auto point = touch->touchPoints().first();
                touchId = point.id();
                start(Touch, point.screenPos());
            }
            break;
        }
        case QEvent::TouchUpdate: {
            if (launcher->isTransitioning()) return true;
            if (pointer != Touch) break;
            QTouchEvent *touch = static_cast<QTouchEvent*>(event);
            if (touch->touchPoints().size() != 1 ||
                touch->touchPoints().first().id() != touchId) {
                pointer = None; // Multi-touch must not close the launcher.
                break;
            }
            move(touch->touchPoints().first().screenPos());
            break;
        }
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            if (launcher->isTransitioning()) return true;
            if (pointer == Touch) pointer = None;
            touchId = -1;
            break;
        default:
            break;
        }
        return QObject::eventFilter(object, event);
    }
};

// ──────────────────────────────  main
int main(int argc, char **argv) {
    QApplication a(argc, argv);

    // ───────── Read font size from UI settings
    {
        QString cfgPath = QDir::homePath() + "/.config/Alternix/osm-settings.conf";
        QSettings cfg(cfgPath, QSettings::IniFormat);
        int pt = cfg.value("UI/LauncherFontSize", 15).toInt();
        pt = qBound(14, pt, 36);
        QString col = cfg.value("UI/LauncherFontColor", "#ffffff").toString().trimmed();
        if (QColor(col).isValid())
            g_labelColor = col;
        QFont f = a.font();
        f.setPointSize(pt);
        a.setFont(f);
    }

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
    QPushButton *closeBtn = new QPushButton("❌");
    closeBtn->setFixedHeight(48);
    closeBtn->setStyleSheet(
        "QPushButton { background-color: #66000000; color: red; font-size: 30pt; padding: 6px 18px; border-radius: 8px; }"
        "QPushButton:hover { background-color: #282828; }");
    QObject::connect(closeBtn, &QPushButton::clicked, &window, &QWidget::close);
    closeRow->addWidget(closeBtn);
    closeRow->addStretch();
    main->addLayout(closeRow);

    KeyFilter *kf = new KeyFilter();
    window.installEventFilter(kf);

    // Best-effort only: don't block ordinary apps if PolicyKit is not installed.
    if (polkitPamUsesAlternix() && !polkitAgentRunning())
        startPolkitAgent();

    window.showFullScreen();
    window.openWithAnimation();  // Start before the first on-screen paint.

    // Observe touches and mouse drags anywhere in the scrollable app list.
    PullDownDismissFilter *pullFilter = new PullDownDismissFilter(&window, scroll, &window);
    a.installEventFilter(pullFilter);

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

    window.setIconLoader(iconTimer);
    iconTimer->start();

    return a.exec();
}
