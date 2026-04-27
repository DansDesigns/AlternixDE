#include <QApplication>
#include <QMainWindow>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QPushButton>
#include <QLabel>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QProcess>
#include <QLibrary>
#include <QTextStream>
#include <QFile>
#include <QScrollBar>
#include <QScreen>
#include <QMouseEvent>
#include <QScroller>
#include <QSettings>
#include <functional>

static const int CARD_PADDING = 22;
static const int ICON_COLUMN_WIDTH = 54;
static const int ICON_TEXT_SPACING = 18;
static const int CARD_WIDTH = 620;

typedef QWidget* (*PageFactory)(QStackedWidget*);

// ------------------------------------------------------
// Flick-enabled scroll area
class TouchScrollArea : public QScrollArea {
public:
    TouchScrollArea(QWidget *parent = nullptr)
        : QScrollArea(parent)
    {
        setWidgetResizable(true);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setStyleSheet(
            "QScrollArea { background:#282828;  font-family:Sans; border:none; }"
            "QWidget { background:#282828; font-family:Sans; }"
            "QLabel { color:white; font-family:Sans;}"
            "QMessageBox QLabel { color:white; font-family:Sans; }"
        );

        QScroller::grabGesture(this, QScroller::LeftMouseButtonGesture);
    }
};

// ------------------------------------------------------
// Clickable card with tap-detection
class ClickableCard : public QFrame {
public:
    explicit ClickableCard(QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setCursor(Qt::PointingHandCursor);
    }

    std::function<void()> onClick;

protected:
    QPoint pressPos;
    bool pressed = false;

    void mousePressEvent(QMouseEvent *event) override {
        pressed = true;
        pressPos = event->globalPos();
        QFrame::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (!pressed) return;
        pressed = false;

        QPoint delta = event->globalPos() - pressPos;
        if (delta.manhattanLength() < 6) {
            if (onClick) onClick();
        }

        QFrame::mouseReleaseEvent(event);
    }
};

// ------------------------------------------------------
QString runCommand(const QString &cmd) {
    QProcess p;
    p.start("bash", {"-c", cmd});
    p.waitForFinished(1500);
    return QString::fromUtf8(p.readAllStandardOutput());
}

QString getSSID() {
    QString ssid = runCommand("iwgetid -r 2>/dev/null").trimmed();
    return ssid.isEmpty() ? "N/C" : ssid;
}

QString getEthernetStatus() {
    QDir netDir("/sys/class/net");
    QStringList ifs = netDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString &iface : ifs) {
        if (iface.startsWith("e")) {
            QFile f("/sys/class/net/" + iface + "/operstate");
            if (!f.open(QIODevice::ReadOnly)) continue;
            QString s = QString(f.readAll()).trimmed();
            if (s == "up") return "Connected";
            return "N/C";
        }
    }
    return "Unknown";
}

// ------------------------------------------------------
class SettingsHub : public QMainWindow {
public:
    SettingsHub() {
        QScreen *screen = QGuiApplication::primaryScreen();
        QRect avail = screen->availableGeometry();

        int w = 800;
        int h = 1280;

        if (h > avail.height()) {
            double scale = double(avail.height()) / double(h);
            w *= scale;
            h = avail.height();
        }

        resize(w, h);
        move(avail.center() - rect().center());

        setWindowTitle("Settings");

        QApplication::setFont(QFont("Noto Color Emoji"));
        setStyleSheet("background:#282828;");

        stack = new QStackedWidget(this);
        setCentralWidget(stack);

        stack->addWidget(makeMainMenu());
    }

protected:
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Backspace && stack->currentIndex() != 0)
            stack->setCurrentIndex(0);

        QMainWindow::keyPressEvent(e);
    }

private:
    QStackedWidget *stack;

    // ------------------------------------------------------
    QWidget *makeMainMenu() {
        QString ssid = getSSID();
        QString eth  = getEthernetStatus();

        struct Row { QString icon, title, sub, module; };

        QList<Row> list = {
            {"🛜", "Wireless", ssid, "wifi"},
            {"🔃", "Bluetooth", "Bluetooth Settings", "bluetooth"},
            {"📶", "Mobile Network", "Cellular, APN, Roaming", "mobile"},
            {"🔗", "Ethernet", eth, "ethernet"},
            {"📍", "Location", "GPS, Geolocation Services", "location"},
            {"🖥️", "Display", "Brightness, Sleep, Screen Info", "display"},
            {"🎨", "UI", "Font Size, Wallpaper", "ui"},
            {"🔊", "Sounds", "Output, Volume Levels", "sound"},
            {"🔋", "Battery", "Battery Level & Charging", "battery"},
            {"💾", "Storage", "Space, Usage & Cleanup", "storage"},
            {"📦", "Installed Applications", "apt, flatpak, snap", "apps"},
            {"🎮", "Emulation", "Android & Windows", "emulation"},
            {"🔐", "Security", "Lockscreen, Passwords, Firewall", "security"},
            {"👤", "Account Info", "User Account Stats", "accounts"},
            {"💽", "Kernel", "System Drivers & Kernel", "kernel"},
            {"⚙️", "System", "Device, OS, Hardware", "system"}
        };

        TouchScrollArea *scroll = new TouchScrollArea;

        QWidget *inner = new QWidget;
        QVBoxLayout *col = new QVBoxLayout(inner);
        col->setContentsMargins(40, 40, 40, 40);
        col->setSpacing(28);
        col->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

        for (auto &r : list) {
            ClickableCard *card = makeCard(r.icon, r.title, r.sub);

            card->setMinimumWidth(CARD_WIDTH);
            card->setMaximumWidth(CARD_WIDTH);

            card->onClick = [this, r]() {
                QWidget *p = loadPage(r.module, r.title);

                if (stack->count() > 1)
                    stack->removeWidget(stack->widget(1));

                stack->addWidget(p);
                stack->setCurrentIndex(1);
            };

            col->addWidget(card, 0, Qt::AlignHCenter);
        }

        col->addStretch();
        scroll->setWidget(inner);
        return scroll;
    }

    // ------------------------------------------------------
    ClickableCard *makeCard(const QString &icon,
                            const QString &title,
                            const QString &sub)
    {
        ClickableCard *card = new ClickableCard;
        card->setFixedHeight(130);

        QHBoxLayout *row = new QHBoxLayout(card);
        row->setContentsMargins(CARD_PADDING, 10, CARD_PADDING, 10);
        row->setSpacing(ICON_TEXT_SPACING);
        row->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        // Icon
        QWidget *iconWrapper = new QWidget;
        iconWrapper->setFixedWidth(ICON_COLUMN_WIDTH);
        QVBoxLayout *iconCol = new QVBoxLayout(iconWrapper);
        iconCol->setContentsMargins(0, 0, 0, 0);
        iconCol->setAlignment(Qt::AlignCenter);

        QLabel *ico = new QLabel(icon);
        ico->setStyleSheet("font-size:48px; color:white;");
        ico->setAlignment(Qt::AlignCenter);
        iconCol->addWidget(ico);

        // Text
        QWidget *textWrapper = new QWidget;
        QVBoxLayout *textCol = new QVBoxLayout(textWrapper);
        textCol->setContentsMargins(0, 0, 0, 0);
        textCol->setSpacing(0);
        textCol->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        QLabel *ttl = new QLabel(title);
        ttl->setStyleSheet("font-size:30px; font-weight:bold; color:white;");

        QLabel *subt = new QLabel(sub);
        subt->setStyleSheet("font-size:22px; color:#bbbbbb;");

        textCol->addWidget(ttl);
        textCol->addWidget(subt);

        row->addWidget(iconWrapper);
        row->addWidget(textWrapper, 1);

        // Card style
        card->setStyleSheet(
            "ClickableCard {"
            " background:#303030;"
            " border:3px dashed #777;"
            " border-radius:12px;"
            "}"
            "ClickableCard:hover { background:#3b3b3b; }"
            "ClickableCard:pressed { background:#505050; }"
        );

        return card;
    }

    // ------------------------------------------------------
    QWidget *loadPage(const QString &module, const QString &title)
    {
        QString path = QString("/usr/local/bin/%1.so").arg(module);
        QLibrary lib(path);

        // Try to load the module page
        if (lib.load()) {
            PageFactory factory = (PageFactory)lib.resolve("make_page");
            if (factory)
                return factory(stack);
        }

        //
        // FALLBACK PAGE — NOT IMPLEMENTED YET
        //
        QWidget *p = new QWidget;
        p->setStyleSheet("background:#282828;");

        QVBoxLayout *v = new QVBoxLayout(p);
        v->setContentsMargins(24, 24, 24, 24);
        v->setSpacing(20);
        v->setAlignment(Qt::AlignTop);

        // Title
        QLabel *lbl = new QLabel(title);
        lbl->setAlignment(Qt::AlignCenter);
        lbl->setStyleSheet("font-size:32px; color:white; font-weight:bold;");
        v->addWidget(lbl);

        // Message
        QLabel *msg = new QLabel("This Settings Feature has not been implemented yet.");
        msg->setAlignment(Qt::AlignCenter);
        msg->setStyleSheet("font-size:24px; color:#bbbbbb;");
        v->addWidget(msg);

        v->addStretch(1);

        // Penguin image
        QLabel *peng = new QLabel;
        QString penguinPath = QDir::homePath() + "/.config/qtile/images/Alternix_unknown.png";
        QPixmap px(penguinPath);

        peng->setPixmap(px.scaled(460, 460, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        peng->setAlignment(Qt::AlignRight);
        v->addWidget(peng, 0, Qt::AlignCenter);

        v->addSpacing(20);

        // NEW BACK BUTTON with Unicode arrow
        QPushButton *back = new QPushButton("❮");
        back->setFixedSize(200, 70);
        back->setStyleSheet(
            "QPushButton {"
            " background:#505050;"
            " color:white;"
            " font-size:36px;"
            " font-weight:bold;"
            " border:none;"
            " border-radius:16px;"
            "}"
            "QPushButton:hover { background:#5c5c5c; }"
            "QPushButton:pressed { background:#666; }"
        );

        QObject::connect(back, &QPushButton::clicked,
                         [this]() { stack->setCurrentIndex(0); });

        v->addWidget(back, 0, Qt::AlignCenter);

        v->addStretch();

        return p;
    }
};

// ------------------------------------------------------
int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    // Apply saved settings font size before any widgets are created
    {
        QString cfgPath = QDir::homePath() + "/.config/Alternix/osm-settings.conf";
        QSettings cfg(cfgPath, QSettings::IniFormat);
        // Default: 18pt on narrow screens (≤720px), 22pt otherwise
        QScreen *scr = QGuiApplication::primaryScreen();
        int defaultPt = (scr && scr->size().width() <= 720) ? 18 : 22;
        int pt = cfg.value("UI/SettingsFontSize", defaultPt).toInt();
        pt = qBound(14, pt, 36);
        QFont f = a.font();
        f.setPointSize(pt);
        a.setFont(f);
    }

    SettingsHub w;
    w.show();

    return a.exec();
}
