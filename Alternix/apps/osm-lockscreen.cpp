#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QDateTime>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTouchEvent>
#include <QEvent>
#include <QScreen>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTextStream>
#include <QProcess>
#include <QGraphicsOpacityEffect>
#include <QFontMetrics>
#include <QCloseEvent>
#include <QShowEvent>
#include <QResizeEvent>
#include <QPointer>
#include <QtMath>
#include <QWindow>
#include <QLockFile>
#include <QCryptographicHash>

// Shared, session-scoped overlay coordination. QLockFile automatically clears
// a marker when its owning process exits and checks for stale markers after a
// crash. This needs only QtCore/QtWidgets, not a new X11 library dependency.
static QString overlayMarkerPath(const QString &name) {
    const QByteArray display = qgetenv("DISPLAY").isEmpty()
        ? qgetenv("WAYLAND_DISPLAY") : qgetenv("DISPLAY");
    const QByteArray session = QCryptographicHash::hash(display, QCryptographicHash::Sha256)
                                   .toHex().left(16);
    const QString dir = QDir::homePath() + "/.cache/Alternix/overlay-locks";
    QDir().mkpath(dir);
    return dir + "/" + name + "-" + QString::fromLatin1(session) + ".lock";
}

static bool overlayMarkerActive(const QString &name) {
    const QString marker = overlayMarkerPath(name);
    if (!QFileInfo::exists(marker))
        return false;
    QLockFile probe(marker);
    probe.setStaleLockTime(0); // never treat a live, long-running lock as expired
    if (probe.tryLock(0)) {
        probe.unlock(); // absent or stale marker
        return false;
    }
    return probe.error() == QLockFile::LockFailedError;
}

// The lockscreen owns pointer input while locked. Request that the resident
// osm-status process show its read-only notification tray; do not start a
// second process or grant a notification permission to unlock the device.
static void requestLockedStatusTray() {
    QFile f(overlayMarkerPath("osm-status-request"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("show\n");
        f.close();
    }
}

// Utility
static QString readFirstLine(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QTextStream in(&f);
    return in.readLine().trimmed();
}

// ─────────────────────────────────────────────
// LockScreenWidget
// ─────────────────────────────────────────────
class LockScreenWidget : public QWidget {
public:
    explicit LockScreenWidget(QWidget *parent = nullptr)
        : QWidget(parent),
          wifiActive(false),
          btActive(false),
          batteryPercent(-1),
          sliderOffset(0.0),
          sliding(false),
          slidingBack(false),
          lockMarker(overlayMarkerPath("osm-lockscreen"))
    {
        // === FULLSCREEN LOCKDOWN MODE ===
        setWindowFlags(Qt::FramelessWindowHint
                       | Qt::WindowStaysOnTopHint
                       | Qt::BypassWindowManagerHint);

        setWindowModality(Qt::ApplicationModal);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_ShowWithoutActivating, false);
        // Handle touchscreen swipes directly instead of relying on
        // platform-specific touch-to-mouse synthesis.
        setAttribute(Qt::WA_AcceptTouchEvents, true);

        // Set geometry to screen size
        QScreen *scr = QGuiApplication::primaryScreen();
        QRect geo = scr->geometry();
        setGeometry(geo);

        // Load assets first
        loadWallpaper();
        loadIcons();

        // Root container for layout
        layoutRoot = new QWidget(this);
        layoutRoot->setGeometry(rect());
        // The wallpaper, clock and status labels are visual-only.  Let all
        // mouse presses/drags pass through them to this full-screen widget,
        // so a swipe can begin ANYWHERE rather than only on the padlock.
        layoutRoot->setAttribute(Qt::WA_TransparentForMouseEvents, true);

        rootLayout = new QVBoxLayout(layoutRoot);
        QVBoxLayout *mainLayout = rootLayout;
        mainLayout->setContentsMargins(40,40,40,40);
        mainLayout->setSpacing(15);

        // === TOP ROW: WIFI - BATTERY - BT ===
        QHBoxLayout *topRow = new QHBoxLayout();
        topRow->setSpacing(20);

        wifiLabel = new QLabel(this);
        wifiLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        batteryLabel = new QLabel("Battery: --%", this);
        batteryLabel->setAlignment(Qt::AlignCenter);
        batteryLabel->setStyleSheet("color:white;");

        btLabel = new QLabel(this);
        btLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        wifiLabel->setStyleSheet("color:grey;");
        btLabel->setStyleSheet("color:grey;");

        wifiEffect = new QGraphicsOpacityEffect(this);
        btEffect   = new QGraphicsOpacityEffect(this);
        wifiLabel->setGraphicsEffect(wifiEffect);
        btLabel->setGraphicsEffect(btEffect);

        topRow->addWidget(wifiLabel);
        topRow->addStretch(1);
        topRow->addWidget(batteryLabel);
        topRow->addStretch(1);
        topRow->addWidget(btLabel);

        mainLayout->addLayout(topRow);

        // Spacer
        mainLayout->addStretch(1);

        // === CLOCK ===
        timeLabel = new QLabel("--:--", this);
        timeLabel->setAlignment(Qt::AlignCenter);
        timeLabel->setStyleSheet("color:white;");
        mainLayout->addWidget(timeLabel, 0, Qt::AlignCenter);

        // Spacer
        mainLayout->addStretch(2);

        // === SLIDE TEXT ===
        slideTextLabel = new QLabel("Slide up to unlock", this);
        slideTextLabel->setAlignment(Qt::AlignCenter | Qt::AlignBottom);
        slideTextLabel->setStyleSheet("color:white;");
        slideTextLabel->setContentsMargins(0, 20, 0, 0);
		mainLayout->addWidget(slideTextLabel);


        // === Timers for clock + status ===
        QTimer *clockTimer = new QTimer(this);
        connect(clockTimer, &QTimer::timeout, this, &LockScreenWidget::updateClock);
        clockTimer->start(1000);
        updateClock();

        QTimer *statusTimer = new QTimer(this);
        connect(statusTimer, &QTimer::timeout, this, &LockScreenWidget::updateStatus);
        statusTimer->start(5000);
        updateStatus();

        // Slide-back animation timer
        slideBackTimer = new QTimer(this);
        slideBackTimer->setInterval(16); // ~60fps
        connect(slideBackTimer, &QTimer::timeout, this, &LockScreenWidget::onSlideBackStep);

        // A lock must intercept BOTH input streams: the Qtile root
        // shortcuts (keyboard), and osm-notify's edge handles (mouse).
        // Additionally, an override-redirect panel may try to raise itself
        // above us later, so refresh the lockscreen's X11 stacking position.
        // osm-power advertises a short-lived overlay marker; while it is
        // running we yield both grabs and stacking priority to that menu.
        lockMarker.setStaleLockTime(0);
        lockMarker.tryLock(0);
        enforcementTimer = new QTimer(this);
        enforcementTimer->setInterval(120);
        connect(enforcementTimer, &QTimer::timeout, this,
                [this]() { enforceLock(); });
        enforcementTimer->start();

        // The fullscreen lock owns the pointer grab, so a separate status
        // badge cannot reliably receive a click. Draw the small notification
        // indicator directly on the lockscreen and check only its file count.
        notificationPollTimer = new QTimer(this);
        notificationPollTimer->setInterval(700);
        connect(notificationPollTimer, &QTimer::timeout, this, [this]() {
            const int count = QDir(QDir::homePath() + "/.osm-notify")
                .entryList(QStringList() << "*.txt",
                           QDir::Files | QDir::Readable).size();
            if (count != lockedNotificationCount) {
                lockedNotificationCount = count;
                update();
            }
        });
        notificationPollTimer->start();
        lockedNotificationCount = QDir(QDir::homePath() + "/.osm-notify")
            .entryList(QStringList() << "*.txt",
                       QDir::Files | QDir::Readable).size();

        adjustScaling();

        // Qtile does not reliably notify override-redirect fullscreen windows
        // when xrandr changes orientation. Track the live QScreen geometry as
        // osm-launcher does, with a cheap fallback poll on affected devices.
        connect(qApp, &QGuiApplication::primaryScreenChanged, this,
                [this](QScreen *) { syncScreenGeometry(); });
        orientationTimer = new QTimer(this);
        orientationTimer->setInterval(200);
        connect(orientationTimer, &QTimer::timeout, this,
                [this]() { syncScreenGeometry(); });
        orientationTimer->start();
    }

protected:

    // ─── PREVENT CLOSE ───
    void closeEvent(QCloseEvent *ev) override {
        ev->ignore();
    }

    // ─── SWALLOW ALL KEYBOARD INPUT ───
    void keyPressEvent(QKeyEvent *e) override {
        // The physical Power key must still be able to open the approved
        // power menu even when the lockscreen has the global keyboard grab.
        if (e->key() == Qt::Key_PowerOff && !e->isAutoRepeat()
            && !powerMenuActive)
            QProcess::startDetached("osm-power", QStringList());
        e->accept();
    }
    void keyReleaseEvent(QKeyEvent *e) override {
        e->accept();
    }

    // ─── PAINT EVENT (Wallpaper + slider) ───
    void paintEvent(QPaintEvent *ev) override {
        Q_UNUSED(ev);
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform,true);

        p.fillRect(rect(), Qt::black);

        // Draw the already-resized wallpaper. A rotation rebuilds this once;
        // ordinary repaint/slide frames never rescale the full-screen image.
        if (!wallpaperCover.isNull()) {
            const qreal dpr = wallpaperCover.devicePixelRatioF();
            const qreal coverW = wallpaperCover.width() / dpr;
            const qreal coverH = wallpaperCover.height() / dpr;
            p.drawPixmap(QPointF((width() - coverW) * 0.5,
                                 (height() - coverH) * 0.5), wallpaperCover);
        }

        // Dark overlay for readability
        p.fillRect(rect(), QColor(0,0,0,80));

        // Match osm-status's small right-edge unread-count badge. This is
        // visual-only; pointer/touch events are still handled by the lock.
        if (lockedNotificationCount > 0) {
            p.setRenderHint(QPainter::Antialiasing, true);
            const int x = width() - 28;
            const int y = int(height() * 0.15) + 24;
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(112, 128, 153, 128));
            p.drawRoundedRect(QRectF(x, y, 24, 54), 6, 6);
            p.setBrush(QColor(0, 160, 220));
            p.drawEllipse(QPointF(x + 14, y), 14, 14);
            QFont countFont = font();
            countFont.setBold(true);
            countFont.setPointSize(16);
            p.setFont(countFont);
            p.setPen(Qt::white);
            p.drawText(QRectF(x, y - 14, 28, 28), Qt::AlignCenter,
                       QString::number(lockedNotificationCount));
        }

        // Draw slider (PNG or "^")
        if (slideTextLabel) {
            QRect tg = slideTextLabel->geometry();
            const int baseY = tg.top() - qMax(22, qMin(width(), height()) / 25);
            const int arrowY = baseY + sliderOffset;
            const int cx = width()/2;

            if (sliderIconAvailable && !sliderIconScaled.isNull()) {
                const int x = cx - sliderIconScaled.width() / 2;
                const int y = arrowY - sliderIconScaled.height() / 2;
                p.drawPixmap(x, y, sliderIconScaled);
            } else {
                QFont f = font();
                f.setPixelSize(qBound(30, height() / 16, 90));
                p.setFont(f);
                p.setPen(Qt::white);
                QRect r(cx-40, arrowY-30, 80, 60);
                p.drawText(r, Qt::AlignTop, "🔒");
            }
        }
    }

    void showEvent(QShowEvent *ev) override {
        QWidget::showEvent(ev);
        // The native QWindow may not exist until the first show(). Rebind if
        // the window moves to a different screen, then sync after fullscreen
        // geometry has had a chance to settle in Qt/X11.
        if (!windowScreenSignalConnected && windowHandle()) {
            windowScreenSignalConnected = true;
            connect(windowHandle(), &QWindow::screenChanged, this,
                    [this](QScreen *) { syncScreenGeometry(); });
        }
        QTimer::singleShot(0, this, [this]() { syncScreenGeometry(); });
    }

    void resizeEvent(QResizeEvent *ev) override {
        QWidget::resizeEvent(ev);
        if (ev->oldSize() != ev->size())
            cancelGestureForScreenChange();
        // This child is a manual, full-window layout host. The old code only
        // resized the outer lockscreen: the layout stayed landscape-sized and
        // clipped labels when rotating to portrait (or the reverse).
        if (layoutRoot)
            layoutRoot->setGeometry(rect());
        adjustScaling();
    }

    // ─── FULL-SCREEN SWIPE-TO-UNLOCK (MOUSE + TOUCH) ───
    // A press anywhere on the lockscreen can start the swipe; only a
    // sufficiently long UPWARD drag can open the existing PIN/pattern dialog.
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && e->pos().x() >= width() - 32) {
            statusEdgeGesture = true;
            statusEdgeStart = e->pos();
            e->accept();
            return;
        }
        if (e->button() == Qt::LeftButton) {
            beginSlide(e->pos().y());
            e->accept();
            return;
        }
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (statusEdgeGesture) {
            if ((e->buttons() & Qt::LeftButton) &&
                statusEdgeStart.x() - e->pos().x() > 12) {
                requestLockedStatusTray();
                statusEdgeGesture = false;
            }
            e->accept();
            return;
        }
        if (sliding && activeTouchId == -1 && (e->buttons() & Qt::LeftButton))
            moveSlide(e->pos().y());
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (statusEdgeGesture) {
            if (e->button() == Qt::LeftButton && lockedNotificationCount > 0 &&
                e->pos().y() >= int(height() * 0.15) &&
                e->pos().y() <= int(height() * 0.15) + 110)
                requestLockedStatusTray();
            statusEdgeGesture = false;
            e->accept();
            return;
        }
        if (e->button() == Qt::LeftButton && sliding && activeTouchId == -1) {
            moveSlide(e->pos().y());
            finishSlide();
        }
        e->accept();
    }

    // Qt 5 touch events work on Wayland/X11 touchscreens without requiring
    // the system to convert each touch into a synthetic mouse drag.
    bool event(QEvent *e) override {
        switch (e->type()) {
        case QEvent::TouchBegin: {
            auto *touch = static_cast<QTouchEvent *>(e);
            if (activeTouchId == -1 && !touch->touchPoints().isEmpty()) {
                const auto &point = touch->touchPoints().first();
                if (point.pos().x() >= width() - 32) {
                    statusEdgeGesture = true;
                    statusEdgeStart = point.pos().toPoint();
                    statusEdgeTouchId = point.id();
                    e->accept();
                    return true;
                }
                activeTouchId = point.id();
                beginSlide(point.pos().y());
            }
            e->accept();
            return true;
        }
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd: {
            auto *touch = static_cast<QTouchEvent *>(e);
            if (statusEdgeTouchId >= 0) {
                const bool ended = e->type() == QEvent::TouchEnd;
                for (const auto &point : touch->touchPoints()) {
                    if (point.id() != statusEdgeTouchId) continue;
                    if (statusEdgeStart.x() - point.pos().x() > 12) {
                        requestLockedStatusTray();
                        statusEdgeGesture = false;
                        statusEdgeTouchId = -1;
                    } else if (ended || (point.state() & Qt::TouchPointReleased)) {
                        if (lockedNotificationCount > 0 &&
                            point.pos().y() >= int(height() * 0.15) &&
                            point.pos().y() <= int(height() * 0.15) + 110)
                            requestLockedStatusTray();
                        statusEdgeGesture = false;
                        statusEdgeTouchId = -1;
                    }
                    break;
                }
                if (ended) { statusEdgeGesture = false; statusEdgeTouchId = -1; }
                e->accept();
                return true;
            }
            bool released = (e->type() == QEvent::TouchEnd);
            for (const auto &point : touch->touchPoints()) {
                if (point.id() != activeTouchId)
                    continue;
                moveSlide(point.pos().y());
                if (point.state() & Qt::TouchPointReleased)
                    released = true;
                break;
            }
            if (released && activeTouchId != -1) {
                activeTouchId = -1;
                finishSlide();
            }
            e->accept();
            return true;
        }
        case QEvent::TouchCancel:
            statusEdgeGesture = false;
            statusEdgeTouchId = -1;
            activeTouchId = -1;
            if (sliding) {
                sliding = false;
                startSlideBack();
            }
            e->accept();
            return true;
        default:
            return QWidget::event(e);
        }
    }

private:

    QLabel *wifiLabel;
    QLabel *btLabel;
    QLabel *batteryLabel;
    QLabel *timeLabel;
    QLabel *slideTextLabel;

    QGraphicsOpacityEffect *wifiEffect;
    QGraphicsOpacityEffect *btEffect;

    QWidget *layoutRoot = nullptr;
    QVBoxLayout *rootLayout = nullptr;
    QPixmap wallpaper;
    QPixmap wallpaperCover;
    QSize wallpaperCoverSize;
    qreal wallpaperCoverDpr = 0.0;
    QPixmap wifiIcon;
    QPixmap btIcon;
    QPixmap sliderIcon;
    QPixmap sliderIconScaled;
    bool sliderIconAvailable = false;

    QTimer *orientationTimer = nullptr;
    QPointer<QScreen> observedScreen;
    QMetaObject::Connection screenGeometryConnection;
    QRect lastScreenGeometry;
    int geometryRepairAttempts = 0;
    unsigned int geometryPollCount = 0;
    bool windowScreenSignalConnected = false;

    bool wifiActive;
    bool btActive;
    int batteryPercent;

    // Slider movement
    int sliderOffset;
    bool sliding;
    bool slidingBack;
    qreal swipeStartY = 0.0;
    qreal swipeStartOffset = 0.0;
    int activeTouchId = -1;
    QTimer *slideBackTimer;
    QTimer *enforcementTimer;
    QLockFile lockMarker;
    bool powerMenuActive = false;
    bool unlockDialogActive = false;
    bool haveKeyboardGrab = false;
    bool haveMouseGrab = false;
    unsigned int enforcementCount = 0;
    QTimer *notificationPollTimer = nullptr;
    int lockedNotificationCount = 0;
    bool statusEdgeGesture = false;
    int statusEdgeTouchId = -1;
    QPoint statusEdgeStart;

    // The Qt native grabs are released only for osm-power and osm-lock auth.
    void releaseInputGrabs() {
        if (windowHandle()) {
            windowHandle()->setMouseGrabEnabled(false);
            windowHandle()->setKeyboardGrabEnabled(false);
        }
        haveKeyboardGrab = false;
        haveMouseGrab = false;
    }

    void enforceLock() {
        if (unlockDialogActive)
            return; // osm-lock --auth must receive its own input

        const bool powerVisible = overlayMarkerActive("osm-power");
        if (powerVisible) {
            if (!powerMenuActive) {
                powerMenuActive = true;
                releaseInputGrabs();
            }
            return; // osm-power, not osm-notify, is the permitted top overlay
        }
        if (powerMenuActive) {
            powerMenuActive = false;
            haveKeyboardGrab = false;
            haveMouseGrab = false;
        }

        // Only a live osm-status popout may temporarily receive pointer input
        // over the lock. Its V7 read-only window must keep a fullscreen input
        // shield above other applications. The lock retains keyboard control.
        // Power takes precedence above this branch as before.
        if (overlayMarkerActive("osm-status")) {
            if (haveMouseGrab && windowHandle())
                windowHandle()->setMouseGrabEnabled(false);
            haveMouseGrab = false;
            if (windowHandle() && !haveKeyboardGrab)
                haveKeyboardGrab = windowHandle()->setKeyboardGrabEnabled(true);
            return;
        }

        // Override-redirect windows are outside Qtile's regular 'above'
        // layer. Raising periodically keeps the lockscreen above transient
        // osm-notify edge handles without editing osm-notify itself.
        raise();
        if (windowHandle()) {
            // Reassert occasionally as grabs may be lost across suspend or
            // an X11 activation change. Keep retrying if another grab wins.
            const bool retry = (++enforcementCount % 5) == 0;
            if (!haveKeyboardGrab || retry)
                haveKeyboardGrab = windowHandle()->setKeyboardGrabEnabled(true);
            if (!haveMouseGrab || (retry && !sliding && activeTouchId == -1))
                haveMouseGrab = windowHandle()->setMouseGrabEnabled(true);
        }
    }

    void cancelGestureForScreenChange() {
        // Coordinates measured in the previous orientation are invalid after
        // rotating. Never accidentally complete a stale swipe into auth.
        if (!sliding && !slidingBack && sliderOffset == 0 &&
            activeTouchId == -1 && !statusEdgeGesture && statusEdgeTouchId == -1)
            return;
        slideBackTimer->stop();
        sliding = false;
        slidingBack = false;
        sliderOffset = 0;
        activeTouchId = -1;
        statusEdgeGesture = false;
        statusEdgeTouchId = -1;
        swipeStartY = 0;
        swipeStartOffset = 0;
        update();
    }

    void syncScreenGeometry() {
        QScreen *screen = windowHandle() && windowHandle()->screen()
                          ? windowHandle()->screen()
                          : QGuiApplication::primaryScreen();
        if (!screen) return;

        const bool screenObjectChanged = screen != observedScreen;
        if (screenObjectChanged) {
            QObject::disconnect(screenGeometryConnection);
            observedScreen = screen;
            screenGeometryConnection = connect(screen, &QScreen::geometryChanged,
                this, [this](const QRect &) { syncScreenGeometry(); });
        }

        const QRect target = screen->geometry();
        if (!target.isValid()) return;
        const bool screenChanged = target != lastScreenGeometry;
        if (screenChanged) {
            lastScreenGeometry = target;
            geometryRepairAttempts = 0;
            geometryPollCount = 0;
            cancelGestureForScreenChange();
        }

        // Use the actual SCREEN bounds, not the previous window width/height.
        // With Qt::BypassWindowManagerHint, some Qtile/xrandr combinations leave
        // fullscreen at its old dimensions. Adjust the QWidget AND QWindow,
        // without showNormal()/hiding the lock or releasing either input grab.
        if (isVisible() && geometry() != target) {
            if (geometryRepairAttempts < 5 || (geometryPollCount % 10) == 0) {
                ++geometryRepairAttempts;
                setGeometry(target);
                if (windowHandle()) windowHandle()->setGeometry(target);
            }
        } else if (screenChanged || screenObjectChanged ||
                   qAbs(wallpaperCoverDpr - devicePixelRatioF()) > 0.001) {
            // Qt may have updated the window before signalling us. A move to
            // a different-DPI screen also needs a fresh cached wallpaper.
            if (layoutRoot) layoutRoot->setGeometry(rect());
            adjustScaling();
        }
        ++geometryPollCount;
    }

    void rebuildWallpaperCover() {
        const QSize viewport = size();
        const qreal dpr = devicePixelRatioF();
        if (viewport == wallpaperCoverSize && qAbs(dpr - wallpaperCoverDpr) < 0.001)
            return;
        wallpaperCoverSize = viewport;
        wallpaperCoverDpr = dpr;
        wallpaperCover = QPixmap();
        if (!wallpaper.isNull() && viewport.isValid()) {
            const QSize pixels(qMax(1, qRound(viewport.width() * dpr)),
                               qMax(1, qRound(viewport.height() * dpr)));
            wallpaperCover = wallpaper.scaled(pixels, Qt::KeepAspectRatioByExpanding,
                                             Qt::SmoothTransformation);
            wallpaperCover.setDevicePixelRatio(dpr);
        }
    }

    void beginSlide(qreal y) {
        sliding = true;
        slidingBack = false;
        slideBackTimer->stop();
        swipeStartY = y;
        swipeStartOffset = sliderOffset;
    }

    void moveSlide(qreal y) {
        if (!sliding)
            return;
        // Keep the padlock tracking the drag; the wallpaper and the rest
        // of the lockscreen remain fixed, just as with the original slider.
        const qreal distance = swipeStartOffset + y - swipeStartY;
        sliderOffset = qRound(qBound(-height() * 0.3, distance, 0.0));
        update();
    }

    void finishSlide() {
        if (!sliding)
            return;
        sliding = false;
        // Preserve the original distance threshold and auth behaviour.
        if (-sliderOffset > height() * 0.2)
            triggerUnlock();
        else
            startSlideBack();
    }

    void loadWallpaper() {
        // Shared wallpaper configuration used by osm-paper and
        // osm-paper-restore, including on the first installed launch.
        const QString home = QDir::homePath();
        const QString cfg = home + "/.config/Alternix/osm-paper.conf";
        QSettings settings(cfg, QSettings::IniFormat);

        QString path = settings.value("last/wallpaper").toString().trimmed();
        if (path.isEmpty())
            path = settings.value("wallpaper").toString().trimmed();

        // The installed default can contain a home-relative path.
        if (path == "~")
            path = home;
        else if (path.startsWith("~/"))
            path = home + path.mid(1);

        wallpaper = QPixmap();
        if (!path.isEmpty() && QFileInfo(path).isFile())
            wallpaper.load(path);
    }

    // ──────────────────────────────────────
    // Load icons from ~/.config/qtile/images
    // ──────────────────────────────────────
    void loadIcons() {
        QString dir = QDir::homePath() + "/.config/qtile/images/";

        QString wp = dir + "wifi.png";
        if (QFile::exists(wp)) wifiIcon.load(wp);

        QString bp = dir + "bt.png";
        if (QFile::exists(bp)) btIcon.load(bp);

        QString sp = dir + "slider.png";
        sliderIconAvailable = QFile::exists(sp);
        if (sliderIconAvailable) sliderIcon.load(sp);
    }

    // ──────────────────────────────────────
    void adjustScaling() {
        const int W = qMax(1, width());
        const int H = qMax(1, height());
        if (!layoutRoot || !rootLayout || !timeLabel) return;

        layoutRoot->setGeometry(rect());
        // Work in Qt logical pixels: devicePixelRatio is applied by the paint
        // system already. Multiplying font point sizes by DPR clipped portrait
        // layouts on HiDPI tablets.
        const int margin = qBound(12, qMin(W, H) / 20, 40);
        rootLayout->setContentsMargins(margin, margin, margin, margin);
        rootLayout->setSpacing(qBound(6, qMin(W, H) / 50, 15));

        QFont clockFont = timeLabel->font();
        clockFont.setPixelSize(qBound(28, qMin(H / 9, W / 4), 140));
        timeLabel->setFont(clockFont);

        QFont statusFont = batteryLabel->font();
        statusFont.setPixelSize(qBound(12, qMin(H / 44, W / 30), 30));
        batteryLabel->setFont(statusFont);
        wifiLabel->setFont(statusFont);
        btLabel->setFont(statusFont);
        const int iconH = qMax(12, QFontMetrics(statusFont).height());

        if (!wifiIcon.isNull())
            wifiLabel->setPixmap(wifiIcon.scaledToHeight(iconH, Qt::SmoothTransformation));
        else
            wifiLabel->setText("WiFi");
        if (!btIcon.isNull())
            btLabel->setPixmap(btIcon.scaledToHeight(iconH, Qt::SmoothTransformation));
        else
            btLabel->setText("BT");

        QFont swipeFont = slideTextLabel->font();
        swipeFont.setPixelSize(qBound(13, qMin(H / 52, W / 23), 26));
        slideTextLabel->setFont(swipeFont);
        slideTextLabel->setContentsMargins(0, qBound(8, H / 40, 20), 0, 0);

        if (sliderIconAvailable && !sliderIcon.isNull()) {
            const int desiredHeight = qBound(24, qMin(H / 10, W / 3), 150);
            if (sliderIconScaled.height() != desiredHeight)
                sliderIconScaled = sliderIcon.scaledToHeight(desiredHeight,
                                                             Qt::SmoothTransformation);
        } else {
            sliderIconScaled = QPixmap();
        }

        rebuildWallpaperCover();
        rootLayout->invalidate();
        rootLayout->activate();
        update();
    }

    // ──────────────────────────────────────
    // STATUS UPDATE
    // ──────────────────────────────────────
    void updateClock() {
        timeLabel->setText(QTime::currentTime().toString("HH:mm"));
    }

    void updateStatus() {
        wifiActive = detectWifiActive();
        btActive   = detectBtActive();
        batteryPercent = readBattery();

        if (batteryPercent >= 0)
            batteryLabel->setText(QString("🔋%1%").arg(batteryPercent));
        else
            batteryLabel->setText("🔋--%");

        wifiEffect->setOpacity(wifiActive ? 1.0 : 0.3);
        btEffect->setOpacity(btActive ? 1.0 : 0.3);

        update();
    }

    bool detectWifiActive() {
        QDir d("/sys/class/net");
        if (!d.exists()) return false;
        QStringList devs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &iface : devs) {
            QString w = d.absoluteFilePath(iface + "/wireless");
            if (!QDir(w).exists()) continue;
            QString op = readFirstLine(d.absoluteFilePath(iface + "/operstate"));
            if (op == "up") return true;
        }
        return false;
    }

    bool detectBtActive() {
        QDir d("/sys/class/bluetooth");
        return d.exists() && !d.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty();
    }

    int readBattery() {
        QDir d("/sys/class/power_supply");
        if (!d.exists()) return -1;
        QStringList ps = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QString name;
        for (const QString &n : ps) {
            if (n.toUpper().startsWith("BAT")) { name=n; break; }
        }
        if (name.isEmpty()) {
            for (const QString &n : ps) {
                QString t = readFirstLine(d.absoluteFilePath(n+"/type")).toLower();
                if (t=="battery") { name=n; break; }
            }
        }
        if (name.isEmpty()) return -1;
        QString cap = readFirstLine(d.absoluteFilePath(name+"/capacity"));
        bool ok=false;
        int v = cap.toInt(&ok);
        return ok ? v : -1;
    }

    // ──────────────────────────────────────
    // SLIDER UNLOCK LOGIC
    // ──────────────────────────────────────
    void triggerUnlock() {
        // The trusted osm-lock pattern/PIN prompt needs mouse/touch as well
        // as keyboard. Suspend stacking enforcement while it is on screen,
        // then regain both grabs after any denial or timeout.
        unlockDialogActive = true;
        releaseInputGrabs();

        QProcess proc;
        proc.start("osm-lock", QStringList() << "--auth");

        // BOUNDED WAIT — DO NOT GO BACK TO waitForFinished(-1)
        // This blocks the lockscreen while osm-lock runs, so if the prompt
        // never appeared (Qtile stuck after waking) the slider froze at
        // the top for good. After 90 s give up and slide back, so the
        // unlock can be tried again.
        if (!proc.waitForFinished(90000)) {
            proc.kill();
            proc.waitForFinished(2000);
        }

        if (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0) {
            QFile flag("/tmp/osm_unlock_success");
            flag.open(QIODevice::WriteOnly);
            flag.write("1");
            flag.close();

            QApplication::quit();
        } else {
            sliderOffset = 0;
            update();
            unlockDialogActive = false;
            enforceLock(); // immediately restore priority and both input grabs
        }
    }

    void startSlideBack() {
        slidingBack = true;
        slideBackTimer->start();
    }

    void onSlideBackStep() {
        if (!slidingBack) {
            slideBackTimer->stop();
            return;
        }
        sliderOffset += qMax(8, height() / 65);
        if (sliderOffset >= 0) {
            sliderOffset = 0;
            slidingBack = false;
            slideBackTimer->stop();
        }
        update();
    }
};

// ─────────────────────────────────────────────
// main()
// ─────────────────────────────────────────────
int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);

    LockScreenWidget w;
    w.show();
    w.raise();
    w.activateWindow();
    w.showFullScreen();
    QTimer::singleShot(0, &w, [&w]() { w.raise(); w.activateWindow(); });

    return app.exec();
}
