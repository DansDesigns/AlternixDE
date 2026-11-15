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
        QWidget *root = new QWidget(this);
        root->setGeometry(rect());
        // The wallpaper, clock and status labels are visual-only.  Let all
        // mouse presses/drags pass through them to this full-screen widget,
        // so a swipe can begin ANYWHERE rather than only on the padlock.
        root->setAttribute(Qt::WA_TransparentForMouseEvents, true);

        QVBoxLayout *mainLayout = new QVBoxLayout(root);
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

        // Draw wallpaper
        if (!wallpaper.isNull()) {
            QPixmap scaled = wallpaper.scaled(size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            QPoint c = rect().center() - QPoint(scaled.width()/2, scaled.height()/2);
            p.drawPixmap(c, scaled);
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
            int baseY = tg.top() - (30 * scaleFactor);
            int arrowY = baseY + sliderOffset;
            int cx = width()/2;

            // ────────────────────────────────────────────────
            // ONLY CHANGE MADE HERE — proper aspect-ratio icon
            // ────────────────────────────────────────────────
            if (sliderIconAvailable && !sliderIcon.isNull()) {
                int desiredHeight = int((height() / 10.0) * scaleFactor);

                QPixmap sliderScaled = sliderIcon.scaledToHeight(
                    desiredHeight,
                    Qt::SmoothTransformation
                );

                int x = cx - sliderScaled.width() / 2;
                int y = arrowY - sliderScaled.height() / 2;

                p.drawPixmap(x, y, sliderScaled);

            } else {
                QFont f = font();
                f.setPointSize(int(36 * scaleFactor));
                p.setFont(f);
                p.setPen(Qt::white);
                QRect r(cx-40, arrowY-30, 80, 60);
                p.drawText(r, Qt::AlignTop, "🔒");
            }
        }
    }

    void resizeEvent(QResizeEvent *) override {
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

    QPixmap wallpaper;
    QPixmap wifiIcon;
    QPixmap btIcon;
    QPixmap sliderIcon;
    bool sliderIconAvailable = false;

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

    qreal scaleFactor = 1.0; // devicePixelRatio()

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
        scaleFactor = this->devicePixelRatio();

        int H = height();
        if (H <= 0) H = 800;

        // Clock font
        QFont f = timeLabel->font();
        f.setPointSize(int((H/12) * scaleFactor));
        timeLabel->setFont(f);

        // Battery + top row font
        QFont f2 = batteryLabel->font();
        f2.setPointSize(int((H/60) * scaleFactor));
        batteryLabel->setFont(f2);

        int iconH = QFontMetrics(f2).height();

        if (!wifiIcon.isNull())
            wifiLabel->setPixmap(wifiIcon.scaledToHeight(iconH, Qt::SmoothTransformation));
        else
            wifiLabel->setText("WiFi");

        if (!btIcon.isNull())
            btLabel->setPixmap(btIcon.scaledToHeight(iconH, Qt::SmoothTransformation));
        else
            btLabel->setText("BT");

        // Slide up to unlock text
        QFont f3 = slideTextLabel->font();
        f3.setPointSize(int((H/70) * scaleFactor));
        slideTextLabel->setFont(f3);

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
        sliderOffset += int(12 * scaleFactor);
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
