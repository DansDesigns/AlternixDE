#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QDateTime>
#include <QPainter>
#include <QMouseEvent>
#include <QScreen>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QProcess>
#include <QGraphicsOpacityEffect>
#include <QFontMetrics>
#include <QCloseEvent>
#include <QtMath>
#include <QWindow>

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
          slidingBack(false)
    {
        // === FULLSCREEN LOCKDOWN MODE ===
        setWindowFlags(Qt::FramelessWindowHint
                       | Qt::WindowStaysOnTopHint
                       | Qt::BypassWindowManagerHint);

        setWindowModality(Qt::ApplicationModal);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_ShowWithoutActivating, false);

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

        // KEYBOARD GRAB — DO NOT REMOVE
        // Qtile's shortcuts (Win+Enter for a terminal and the rest) are
        // grabbed on the root window, so they fire whichever window has
        // focus. Swallowing keys in keyPressEvent cannot stop them. An
        // active keyboard grab does: while this window holds it, every
        // key comes here and Qtile's shortcuts cannot trigger. It also
        // stops typing reaching the window underneath. The grab only
        // works once the window is on screen, and fails while another
        // program holds one, so keep retrying until it succeeds.
        grabTimer = new QTimer(this);
        grabTimer->setInterval(200);
        connect(grabTimer, &QTimer::timeout, this, [this]() {
            if (windowHandle() && windowHandle()->setKeyboardGrabEnabled(true))
                grabTimer->stop();
        });
        grabTimer->start();

        adjustScaling();
    }

protected:

    // ─── PREVENT CLOSE ───
    void closeEvent(QCloseEvent *ev) override {
        ev->ignore();
    }

    // ─── SWALLOW ALL KEYBOARD INPUT ───
    void keyPressEvent(QKeyEvent *e) override {
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

    // ─── MOUSE EVENTS FOR SLIDER ───
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton)
            return;

        int cx = width()/2;
        QRect tg = slideTextLabel->geometry();
		// Icon position on screem: og: 30..
        int baseY = tg.top() - (50 * scaleFactor);
        int arrowY = baseY + sliderOffset;

        int rad = int(60 * scaleFactor);
        QRect handle(cx-rad, arrowY-rad, rad*2, rad*2);

        if (handle.contains(e->pos())) {
            sliding = true;
            slidingBack = false;
            slideBackTimer->stop();
            lastPos = e->pos();
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!sliding)
            return;

        int dy = e->pos().y() - lastPos.y();
        lastPos = e->pos();

        sliderOffset += dy;
        double maxUp = -height()*0.3;
        if (sliderOffset < maxUp) sliderOffset = maxUp;
        if (sliderOffset > 0) sliderOffset = 0;

        update();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        Q_UNUSED(e);

        if (!sliding)
            return;
        sliding = false;

        if (-sliderOffset > height()*0.2) {
            triggerUnlock();
        } else {
            startSlideBack();
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
    QPoint lastPos;
    QTimer *slideBackTimer;
    QTimer *grabTimer;

    qreal scaleFactor = 1.0; // devicePixelRatio()

    void loadWallpaper() {
        QString cfg = QDir::homePath() + "/.config/Alternix/osm-paper.conf";
        QString path;

        if (QFile::exists(cfg)) {
            QFile f(cfg);
            if (f.open(QIODevice::ReadOnly|QIODevice::Text)) {
                QTextStream in(&f);
                while (!in.atEnd()) {
                    QString line = in.readLine().trimmed();
                    if (line.startsWith("wallpaper=")) {
                        path = line.section('=',1);
                        break;
                    }
                }
            }
        }

        if (!path.isEmpty() && QFile::exists(path))
            wallpaper.load(path);
        else
            wallpaper = QPixmap();
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
        // Let go of the keyboard while osm-lock asks for the pattern or
        // PIN, so it can take its own grab and receive typed PINs.
        grabTimer->stop();
        if (windowHandle())
            windowHandle()->setKeyboardGrabEnabled(false);

        QProcess proc;
        proc.start("osm-lock", QStringList() << "--auth");
        proc.waitForFinished(-1);

        if (proc.exitCode() == 0) {
            QFile flag("/tmp/osm_unlock_success");
            flag.open(QIODevice::WriteOnly);
            flag.write("1");
            flag.close();

            QApplication::quit();
        } else {
            sliderOffset = 0;
            update();
            grabTimer->start();   // locked again: take the keyboard back
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

    return app.exec();
}
