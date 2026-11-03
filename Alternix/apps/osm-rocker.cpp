#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QGuiApplication>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QSettings>
#include <QProcess>
#include <QMouseEvent>
#include <QVector>
#include <QDebug>
#include <QTimer>
#include <QThread>
#include <QGraphicsDropShadowEffect>

class OverlayPanel : public QWidget {
public:
    OverlayPanel() {
        setWindowFlags(Qt::FramelessWindowHint
                       | Qt::WindowStaysOnTopHint
                       | Qt::BypassWindowManagerHint
                       | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::NoFocus);

        QRect screenGeom = QGuiApplication::primaryScreen()->geometry();
        setGeometry(screenGeom);

        auto *rootLayout = new QHBoxLayout(this);
        rootLayout->setContentsMargins(0,0,0,0);

        QWidget *rightContainer = new QWidget(this);
        auto *rightLayout = new QVBoxLayout(rightContainer);
        rightLayout->setContentsMargins(0,0,0,0);

        panel = new QWidget(rightContainer);
        panel->setObjectName("overlayPanel");

        auto *panelLayout = new QVBoxLayout(panel);
        panelLayout->setContentsMargins(20,20,20,20);
        panelLayout->setSpacing(12);

        panel->setStyleSheet(
            "#overlayPanel {"
            " background-color: #80708099;"
            //" border: 2px solid #000000;"
            " border:none;"
            " border-top-left-radius:26px;"
            " border-bottom-left-radius:26px;"
            " border-top-right-radius:0px;"
            " border-bottom-right-radius:0px;"
            "}"
        );

        // Shadow behind panel — same style as osm-running
        auto *shadow = new QGraphicsDropShadowEffect(panel);
        shadow->setOffset(0, 0);
        shadow->setBlurRadius(32);
        shadow->setColor(QColor(0, 0, 0, 220));
        panel->setGraphicsEffect(shadow);

        QStringList items = {
            "🔊 Volume",
            "🐁 Mouse scroll",
          //  "Keyboard scroll",
            "📸 Screenshot",
            "📛 Power Menu"
        };

        for (const QString &t : items) {
            QLabel *lbl = new QLabel(t, panel);
            lbl->setAlignment(Qt::AlignLeft);
            lbl->setStyleSheet("color:white; font-size:28px;");
            panelLayout->addWidget(lbl);
            menuLabels.append(lbl);
        }

        int screenH = QGuiApplication::primaryScreen()->geometry().height();

        // Top spacer = 15% of screen height
        rightLayout->addSpacing(screenH * 0.15);

        rightLayout->addWidget(panel);

        // Bottom takes remaining space
        rightLayout->addStretch();


        rootLayout->addStretch();
        rootLayout->addWidget(rightContainer);

        currentIndex = 0;
        updateHighlight();
    }

    void gpioUp()      { moveSelection(-1); }
    void gpioDown()    { moveSelection(+1); }
    void gpioSelect()  { activateCurrent(); }

protected:

    // No blackout overlay — background stays visible
    void paintEvent(QPaintEvent *) override {}

    void keyPressEvent(QKeyEvent *e) override {
        switch (e->key()) {
        case Qt::Key_Up:
        case Qt::Key_VolumeUp:
            moveSelection(-1);
            break;

        case Qt::Key_Down:
        case Qt::Key_VolumeDown:
            moveSelection(+1);
            break;

        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            activateCurrent();
            break;

        case Qt::Key_Escape:
            close();
            break;

        default:
            QWidget::keyPressEvent(e);
            break;
        }
    }

    // ───────────────────────────────────────────────
    // Click-to-close + click-to-activate + pressed highlight
    QLabel *pressedLabel = nullptr;

    void mousePressEvent(QMouseEvent *e) override {
        QPoint p = e->pos();

        // Convert panel rect into overlay coordinates
        QRect panelRect = panel->geometry();
        panelRect.moveTopLeft(panel->mapTo(this, QPoint(0,0)));

        // Outside panel → close
        if (!panelRect.contains(p)) {
            close();
            return;
        }

        // Identify pressed label
        pressedLabel = nullptr;

        for (int i = 0; i < menuLabels.size(); i++) {
            QLabel *lbl = menuLabels[i];

            QRect r = lbl->geometry();
            r.moveTopLeft(lbl->mapTo(this, QPoint(0,0)));

            if (r.contains(p)) {
                pressedLabel = lbl;
                lbl->setStyleSheet(
                    "color:white; font-size:24px; background:#3a3a3a; border-radius:12px;"
                );
                return;
            }
        }

        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        QPoint p = e->pos();

        if (!pressedLabel) {
            e->accept();
            return;
        }

        QLabel *released = nullptr;

        for (int i = 0; i < menuLabels.size(); i++) {
            QLabel *lbl = menuLabels[i];

            QRect r = lbl->geometry();
            r.moveTopLeft(lbl->mapTo(this, QPoint(0,0)));

            if (r.contains(p)) {
                released = lbl;
                break;
            }
        }

        updateHighlight();

        if (released && released == pressedLabel) {
            currentIndex = menuLabels.indexOf(released);
            updateHighlight();
            activateCurrent();
        }

        pressedLabel = nullptr;
        e->accept();
    }

private:
    QWidget *panel;
    QVector<QLabel*> menuLabels;
    int currentIndex;

    void moveSelection(int delta) {
        currentIndex += delta;
        if (currentIndex < 0) currentIndex = menuLabels.size()-1;
        if (currentIndex >= menuLabels.size()) currentIndex = 0;
        updateHighlight();
    }

    void updateHighlight() {
        for (int i = 0; i < menuLabels.size(); i++) {
            QLabel *lbl = menuLabels[i];
            if (i == currentIndex)
                lbl->setStyleSheet("color:white; font-size:30px; background:#282828; border-radius:12px;");
            else
                lbl->setStyleSheet("color:white; font-size:24px; background:transparent;");
        }
    }

    void activateCurrent() {
        QString item = menuLabels[currentIndex]->text();

        if (item == "🔊 Volume")                 { setGpioMode("volume"); close(); return; }
        if (item == "🐁 Mouse scroll")           { setGpioMode("scroll"); close(); return; }
        //if (item == "Keyboard scroll")           { setGpioMode("keys");   close(); return; }
        if (item == "📸 Screenshot")             { doScreenshot();        return; }
        if (item == "📛 Power Menu")             { openPowerMenu();       close(); return; }
    }

    void setGpioMode(const QString &mode) {
        QSettings s(QDir::homePath()+"/.config/Alternix/.osm-gpio-mode.ini",
                    QSettings::IniFormat);
        s.setValue("mode", mode);
        s.sync();
    }

    QWidget* buildToast(const QString &message) {
        QWidget *toast = new QWidget(nullptr);
        toast->setWindowFlags(Qt::FramelessWindowHint |
                              Qt::WindowStaysOnTopHint |
                              Qt::Tool);

        toast->setAttribute(Qt::WA_TranslucentBackground);

        QRect g = QGuiApplication::primaryScreen()->geometry();
        toast->setGeometry(g.x(), g.y()+10, g.width(), 50);

        QWidget *box = new QWidget(toast);
        box->setGeometry(20, 0, g.width()-40, 50);
        box->setStyleSheet(
            "background-color: #282828;"
            "border-radius: 10px;"
        );

        auto *layout = new QHBoxLayout(box);
        layout->setContentsMargins(15, 5, 15, 5);

        QLabel *tick = new QLabel("✔", box);
        tick->setStyleSheet("color:#00FF66; font-size:22px; font-weight:bold;");

        QLabel *msg = new QLabel(message, box);
        msg->setStyleSheet("color:white; font-size:20px;");

        layout->addWidget(tick);
        layout->addSpacing(10);
        layout->addWidget(msg);

        return toast;
    }

    void doScreenshot() {
        setWindowOpacity(0.0);
        panel->hide();
        QApplication::processEvents();
        QThread::msleep(60);

        QScreen *scr = QGuiApplication::primaryScreen();
        if (!scr) { close(); return; }

        QPixmap pix = scr->grabWindow(0);

        QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (dir.isEmpty()) dir = QDir::homePath();
        QDir d(dir);
        if (!d.exists()) d.mkpath(".");

        QString name = "screenshot-" +
            QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".png";

        QString full = d.filePath(name);
        pix.save(full);

        QWidget *toast = buildToast("Screenshot saved to ~/Pictures");
        toast->show();
        toast->raise();

        QTimer::singleShot(2000, toast, [toast](){
            toast->close();
            toast->deleteLater();
        });

        close();
    }

    void openPowerMenu() {
        // Launch the AlternixOS power application directly instead of relying
        // on the legacy Super+P keyboard shortcut in the window manager.
        QString program = QStandardPaths::findExecutable("osm-power");
        if (program.isEmpty()) {
            qWarning() << "osm-rocker: osm-power executable not found in PATH";
            return;
        }

        if (!QProcess::startDetached(program, QStringList())) {
            qWarning() << "osm-rocker: failed to launch" << program;
        }
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    OverlayPanel o;

    // sized to screen WITHOUT fullscreen mode
    QRect g = QGuiApplication::primaryScreen()->geometry();
    o.setGeometry(g);

    o.show();   // NOT showFullScreen()
    o.raise();
    o.activateWindow();
    o.setFocus();

    return app.exec();
}
