#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QDateTime>
#include <QPainter>
#include <QMouseEvent>
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QDebug>

class PowerMenuWindow : public QWidget {
public:
    explicit PowerMenuWindow(QWidget *parent = nullptr)
        : QWidget(parent),
          panel(nullptr),
          helloLabel(nullptr),
          timeLabel(nullptr),
          statsPanel(nullptr)
    {
        // Fullscreen, no decorations, overlay-style
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground, true);

        // Take up the full screen
        QScreen *scr = QGuiApplication::primaryScreen();
        if (scr) setGeometry(scr->geometry());

        // --- Central rounded "screen" panel ---
        panel = new QWidget(this);
        panel->setObjectName("panel");
        panel->setAutoFillBackground(false);

        QVBoxLayout *panelLayout = new QVBoxLayout(panel);
        panelLayout->setContentsMargins(30, 30, 30, 30);
        panelLayout->setSpacing(20);

        // Top row: Hello $USER    HH:MM
        QHBoxLayout *topRow = new QHBoxLayout();
        helloLabel = new QLabel(this);
        QString user = QString::fromLocal8Bit(qgetenv("USER"));
        if (user.isEmpty())
            user = QDir::homePath().split("/").last();
        helloLabel->setText("🐧 Hello " + user);
        helloLabel->setStyleSheet("color: white;");

        timeLabel = new QLabel("--:--", this);
        timeLabel->setStyleSheet("color: white;");
        timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        topRow->addWidget(helloLabel);
        topRow->addStretch(1);
        topRow->addWidget(timeLabel);

        panelLayout->addLayout(topRow);

        // Spacer
        panelLayout->addSpacing(10);

        // Icon row
        QHBoxLayout *iconRow = new QHBoxLayout();
        iconRow->setSpacing(30);
        iconRow->setAlignment(Qt::AlignHCenter);

        QWidget *lockWidget   = createIconButton("Lock",     QDir::homePath() + "/.config/qtile/images/lock.png");
        QWidget *sleepWidget  = createIconButton("Sleep",    QDir::homePath() + "/.config/qtile/images/sleep.png");
        QWidget *rebootWidget = createIconButton("Reboot",   QDir::homePath() + "/.config/qtile/images/restart.png");
        QWidget *powerWidget  = createIconButton("Power Off",QDir::homePath() + "/.config/qtile/images/shutdown.png");

        // Connect actions
        connect(lockWidget->findChild<QPushButton*>("btn"), &QPushButton::clicked,
                this, &PowerMenuWindow::doLock);
        connect(sleepWidget->findChild<QPushButton*>("btn"), &QPushButton::clicked,
                this, &PowerMenuWindow::doSleep);
        connect(rebootWidget->findChild<QPushButton*>("btn"), &QPushButton::clicked,
                this, &PowerMenuWindow::doReboot);
        connect(powerWidget->findChild<QPushButton*>("btn"), &QPushButton::clicked,
                this, &PowerMenuWindow::doPowerOff);

        iconRow->addStretch(1);
        iconRow->addWidget(lockWidget);
        iconRow->addWidget(sleepWidget);
        iconRow->addWidget(rebootWidget);
        iconRow->addWidget(powerWidget);
        iconRow->addStretch(1);

        panelLayout->addLayout(iconRow);

        // Spacer between icons and stats block
        panelLayout->addStretch(1);

        // Stats panel placeholder
        statsPanel = new QWidget(this);
        statsPanel->setObjectName("statsPanel");
        QVBoxLayout *statsLayout = new QVBoxLayout(statsPanel);
        statsLayout->setContentsMargins(30, 30, 30, 30);

        QLabel *statsText = new QLabel(
            "Htop or built in graphs\nshowing CPU, RAM,\nNetwork etc...",
            this);
        statsText->setAlignment(Qt::AlignCenter);
        statsText->setStyleSheet("color: white;");
        statsLayout->addWidget(statsText);

        panelLayout->addWidget(statsPanel);

        // Timer to update clock every second
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &PowerMenuWindow::updateClock);
        timer->start(1000);
        updateClock();

        updateStyles();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // Dim background overlay
        p.fillRect(rect(), QColor(0, 0, 0, 160));
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        if (!panel)
            return;

        // Center the panel and size it based on screen
        int w = width();
        int h = height();

        // Panel size: 80% height, 70% width (clamped a bit)
        int panelW = int(w * 0.75);
        int panelH = int(h * 0.80);
        if (panelW > 900) panelW = 900;
        if (panelH > 1000) panelH = 1000;

        int x = (w - panelW) / 2;
        int y = (h - panelH) / 2;

        panel->setGeometry(x, y, panelW, panelH);

        // Stats panel height: about 40% of panel
        if (statsPanel) {
            int statsH = int(panelH * 0.45);
            statsPanel->setMinimumHeight(statsH);
        }

        // Font scaling
        int base = panelH;
        int helloSize = qMax(14, base / 30);
        int timeSize  = qMax(14, base / 20);
        int statsTextSize = qMax(14, base / 30);

        QFont f = helloLabel->font();
        f.setPointSize(helloSize);
        helloLabel->setFont(f);

        QFont f2 = timeLabel->font();
        f2.setPointSize(timeSize);
        timeLabel->setFont(f2);

        // Set stats label font (child of statsPanel)
        QList<QLabel*> labels = statsPanel->findChildren<QLabel*>();
        for (QLabel *lab : labels) {
            QFont lf = lab->font();
            lf.setPointSize(statsTextSize);
            lab->setFont(lf);
        }

        // Adjust icon sizes
        int iconSize = qMax(48, base / 10);
        QList<QPushButton*> btns = panel->findChildren<QPushButton*>("btn");
        for (QPushButton *b : btns) {
            b->setIconSize(QSize(iconSize, iconSize));
            QFont bf = b->font();
            bf.setPointSize(qMax(14, base / 40));
            b->setFont(bf);
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        // If click is outside the panel, close the menu
        if (panel && !panel->geometry().contains(event->pos())) {
            close();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override {
        // Escape closes the menu
        if (event->key() == Qt::Key_Escape) {
            close();
            return;
        }
        QWidget::keyPressEvent(event);
    }

private:
    QWidget *panel;
    QLabel *helloLabel;
    QLabel *timeLabel;
    QWidget *statsPanel;

    QWidget* createIconButton(const QString &labelText, const QString &iconPath) {
        QWidget *wrapper = new QWidget(this);
        QVBoxLayout *v = new QVBoxLayout(wrapper);
        v->setContentsMargins(0,0,0,0);
        v->setSpacing(5);
        v->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);

        QPushButton *btn = new QPushButton(wrapper);
        btn->setObjectName("btn");
        btn->setFlat(true);
        btn->setCursor(Qt::PointingHandCursor);

        QFileInfo fi(iconPath);
        if (fi.exists()) {
            QIcon icon(iconPath);
            btn->setIcon(icon);
        } else {
            btn->setText(labelText);
        }

        QLabel *lab = new QLabel(labelText, wrapper);
        lab->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
        lab->setStyleSheet("color: white;");

        v->addWidget(btn, 0, Qt::AlignHCenter);
        v->addWidget(lab, 0, Qt::AlignHCenter);

        return wrapper;
    }

    void updateClock() {
        timeLabel->setText(QTime::currentTime().toString("HH:mm"));
    }

    void updateStyles() {
        // Rounded grey panel (screen area)
        QString panelStyle =
            "QWidget#panel {"
            "  background-color: #80708099;"
            "  border-radius: 40px;"
            "}";

        // Black rounded stats area
        QString statsStyle =
            "QWidget#statsPanel {"
            "  background-color: #000000;"
            "  border-radius: 35px;"
            "}";

        panel->setStyleSheet(panelStyle);
        statsPanel->setStyleSheet(statsStyle);
    }

private slots:
    void doLock() {
        QProcess::startDetached("osm-lockd", QStringList());
        close();
    }

    void doSleep() {
        QProcess::startDetached("osm-lockd", QStringList());
        QProcess::startDetached("osm-sudo", QStringList() << "pm-suspend");
#        QProcess::startDetached("sudo -S pm-suspend", QStringList());
        close();
    }

    void doReboot() {
        QProcess::startDetached("osm-sudo", QStringList() << "reboot");
#        QProcess::startDetached("sudo -S reboot", QStringList());
        close();
    }

    void doPowerOff() {
        QProcess::startDetached("osm-sudo", QStringList() << "poweroff");
#        QProcess::startDetached("sudo -S poweroff", QStringList());
        close();
    }

};

// ────────────────────────────────
// main
// ────────────────────────────────
int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);

    PowerMenuWindow w;
    w.showFullScreen();

    return app.exec();
}
