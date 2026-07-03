// osm-clock — Alternix clock / alarms / timer / stopwatch
// Theming & interaction patterns mirror osm-settings.cpp
// Alarms are written to ~/.osm-alarms (consumed by osm-status AlarmWatcher):
//   HH:MM|Title|Body                one-shot today
//   yyyy-MM-dd HH:MM|Title|Body    one-shot on date
//   daily HH:MM|Title|Body         repeats every day
// Timer completion drops a file straight into ~/.osm-notify.
//
// Build:
//   g++ apps/osm-clock.cpp -o osm-clock -fPIC -ldl \
//       $(pkg-config --cflags --libs Qt5Widgets) -lX11

#include <QApplication>
#include <QMainWindow>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScroller>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QFrame>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QScreen>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QSettings>
#include <QFileDialog>
#include <QFileInfo>
#include <functional>

static const int CARD_PADDING = 22;
static const int ICON_COLUMN_WIDTH = 54;
static const int ICON_TEXT_SPACING = 18;
static const int CARD_WIDTH = 620;

// ------------------------------------------------------
// Flick-enabled scroll area (same as osm-settings)
class TouchScrollArea : public QScrollArea {
public:
    TouchScrollArea(QWidget *parent = nullptr)
        : QScrollArea(parent)
    {
        setWidgetResizable(true);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setStyleSheet(
            "QScrollArea { background:#282828; font-family:Sans; border:none; }"
            "QWidget { background:#282828; font-family:Sans; }"
            "QLabel { color:white; font-family:Sans; }"
        );
        QScroller::grabGesture(this, QScroller::LeftMouseButtonGesture);
    }
};

// ------------------------------------------------------
// Clickable card with tap-detection (same as osm-settings)
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
// Shared style snippets

static QString cardStyle() {
    return
        "ClickableCard {"
        " background:#303030;"
        " border:3px dashed #777;"
        " border-radius:12px;"
        "}"
        "ClickableCard:hover { background:#3b3b3b; }"
        "ClickableCard:pressed { background:#505050; }";
}

static QString bigButtonStyle() {
    return
        "QPushButton {"
        " background:#505050;"
        " color:white;"
        " font-size:36px;"
        " font-weight:bold;"
        " border:none;"
        " border-radius:16px;"
        "}"
        "QPushButton:hover { background:#5c5c5c; }"
        "QPushButton:pressed { background:#666; }";
}

static QPushButton *makeBackButton(std::function<void()> fn) {
    QPushButton *back = new QPushButton("❮");
    back->setFixedSize(200, 70);
    back->setStyleSheet(bigButtonStyle());
    QObject::connect(back, &QPushButton::clicked, fn);
    return back;
}

// ------------------------------------------------------
// Alarm model: read/write ~/.osm-alarms

struct Alarm {
    QString when;    // "HH:MM", "yyyy-MM-dd HH:MM", or "daily HH:MM"
    QString title;
    QString body;
    QString sound;   // optional per-alarm sound file

    bool daily() const { return when.startsWith("daily ", Qt::CaseInsensitive); }
    QString timePart() const {
        return daily() ? when.mid(6).trimmed() : when;
    }
    QString toLine() const {
        QString l = when + "|" + title + "|" + body;
        if (!sound.isEmpty()) l += "|" + sound;
        return l;
    }
};

static QString alarmFilePath() {
    return QDir::homePath() + "/.osm-alarms";
}

static QList<Alarm> loadAlarms(QStringList *commentsOut = nullptr) {
    QList<Alarm> out;
    QFile f(alarmFilePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;

    QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    for (const QString &raw : lines) {
        QString line = raw.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith('#')) {
            if (commentsOut) *commentsOut << raw;
            continue;
        }
        QStringList parts = line.split('|');
        Alarm a;
        a.when  = parts.value(0).trimmed();
        a.title = parts.value(1, "Alarm").trimmed();
        a.body  = parts.value(2).trimmed();
        a.sound = parts.value(3).trimmed();
        if (a.title.isEmpty()) a.title = "Alarm";
        if (!a.when.isEmpty()) out << a;
    }
    return out;
}

static void saveAlarms(const QList<Alarm> &alarms) {
    QStringList comments;
    loadAlarms(&comments);   // preserve existing comment header

    QFile f(alarmFilePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    QTextStream out(&f);
    if (comments.isEmpty()) {
        out << "# osm-status alarms\n"
               "# HH:MM|Title|Body            one-shot today\n"
               "# yyyy-MM-dd HH:MM|Title|Body one-shot on date\n"
               "# daily HH:MM|Title|Body      repeats every day\n";
    } else {
        for (const QString &c : comments) out << c << "\n";
    }
    for (const Alarm &a : alarms)
        out << a.toLine() << "\n";
}

// Direct drop into the osm-status notification folder (timer done, etc.)
static void dropNotification(const QString &title, const QString &body,
                             const QString &sound = QString()) {
    QDir d(QDir::homePath() + "/.osm-notify");
    if (!d.exists()) d.mkpath(".");
    QFile f(d.absoluteFilePath(
        QString("%1-clock.txt").arg(QDateTime::currentMSecsSinceEpoch())));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    QTextStream out(&f);
    out << title << "\n";
    if (!body.isEmpty()) out << body << "\n";
    if (!sound.trimmed().isEmpty()) out << "sound:" << sound.trimmed() << "\n";
}

// touch-friendly audio file picker; returns empty if cancelled
static QString pickSoundFile(QWidget *parent) {
    QString start = QDir::homePath() + "/Music";
    if (!QDir(start).exists()) start = QDir::homePath();
    return QFileDialog::getOpenFileName(parent, "Choose sound",
        start, "Audio files (*.wav *.ogg *.mp3 *.flac);;All files (*)");
}

static QString soundButtonText(const QString &sound) {
    if (sound.isEmpty()) return QString::fromUtf8("\xf0\x9f\x8e\xb5  Sound: Default");
    return QString::fromUtf8("\xf0\x9f\x8e\xb5  ") + QFileInfo(sound).fileName();
}

// ------------------------------------------------------
class ClockHub : public QMainWindow {
public:
    ClockHub() {
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

        setWindowTitle("Clock");
        QApplication::setFont(QFont("Noto Color Emoji"));
        setStyleSheet("background:#282828;");

        stack = new QStackedWidget(this);
        setCentralWidget(stack);

        stack->addWidget(makeMainMenu());     // 0
    }

protected:
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Backspace && stack->currentIndex() != 0)
            goHome();
        QMainWindow::keyPressEvent(e);
    }

private:
    QStackedWidget *stack;
    QLabel *bigTime = nullptr;
    QLabel *bigDate = nullptr;

    void goHome() {
        stack->setCurrentIndex(0);
        while (stack->count() > 1) {
            QWidget *w = stack->widget(1);
            stack->removeWidget(w);
            w->deleteLater();
        }
    }

    void openPage(QWidget *p) {
        while (stack->count() > 1) {
            QWidget *w = stack->widget(1);
            stack->removeWidget(w);
            w->deleteLater();
        }
        stack->addWidget(p);
        stack->setCurrentIndex(1);
    }

    // --------------------------------------------------
    QWidget *makeMainMenu() {
        TouchScrollArea *scroll = new TouchScrollArea;

        QWidget *inner = new QWidget;
        QVBoxLayout *col = new QVBoxLayout(inner);
        col->setContentsMargins(40, 40, 40, 40);
        col->setSpacing(28);
        col->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

        // ---- big live clock
        bigTime = new QLabel;
        bigTime->setAlignment(Qt::AlignCenter);
        bigTime->setStyleSheet("font-size:110px; font-weight:bold; color:white;");

        bigDate = new QLabel;
        bigDate->setAlignment(Qt::AlignCenter);
        bigDate->setStyleSheet("font-size:28px; color:#bbbbbb;");

        col->addSpacing(30);
        col->addWidget(bigTime);
        col->addWidget(bigDate);
        col->addSpacing(40);

        QTimer *tick = new QTimer(inner);
        tick->setInterval(500);
        auto updateClock = [this]() {
            QDateTime now = QDateTime::currentDateTime();
            if (bigTime) bigTime->setText(now.toString("HH:mm:ss"));
            if (bigDate) bigDate->setText(now.toString("dddd d MMMM yyyy"));
        };
        QObject::connect(tick, &QTimer::timeout, updateClock);
        tick->start();
        updateClock();

        // ---- nav cards (same look as settings hub)
        struct Row { QString icon, title, sub; std::function<QWidget*()> make; };
        QList<Row> rows = {
            {"⏰", "Alarms",    "Add & manage alarms",      [this]() { return makeAlarmsPage(); }},
            {"⏳", "Timer",     "Countdown timer",           [this]() { return makeTimerPage(); }},
            {"⏱️", "Stopwatch", "Elapsed time",              [this]() { return makeStopwatchPage(); }},
        };

        for (auto &r : rows) {
            ClickableCard *card = makeCard(r.icon, r.title, r.sub);
            card->setMinimumWidth(CARD_WIDTH);
            card->setMaximumWidth(CARD_WIDTH);
            auto make = r.make;
            card->onClick = [this, make]() { openPage(make()); };
            col->addWidget(card, 0, Qt::AlignHCenter);
        }

        col->addStretch();
        scroll->setWidget(inner);
        return scroll;
    }

    // --------------------------------------------------
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

        QWidget *iconWrapper = new QWidget;
        iconWrapper->setFixedWidth(ICON_COLUMN_WIDTH);
        QVBoxLayout *iconCol = new QVBoxLayout(iconWrapper);
        iconCol->setContentsMargins(0, 0, 0, 0);
        iconCol->setAlignment(Qt::AlignCenter);

        QLabel *ico = new QLabel(icon);
        ico->setStyleSheet("font-size:48px; color:white;");
        ico->setAlignment(Qt::AlignCenter);
        iconCol->addWidget(ico);

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

        card->setStyleSheet(cardStyle());
        return card;
    }

    // --------------------------------------------------
    // ALARMS — list page
    QWidget *makeAlarmsPage() {
        TouchScrollArea *scroll = new TouchScrollArea;
        QWidget *inner = new QWidget;
        QVBoxLayout *col = new QVBoxLayout(inner);
        col->setContentsMargins(40, 40, 40, 40);
        col->setSpacing(24);
        col->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

        QLabel *title = new QLabel("Alarms");
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:32px; font-weight:bold; color:white;");
        col->addWidget(title);

        QList<Alarm> alarms = loadAlarms();

        if (alarms.isEmpty()) {
            QLabel *msg = new QLabel("No alarms set.");
            msg->setAlignment(Qt::AlignCenter);
            msg->setStyleSheet("font-size:24px; color:#bbbbbb;");
            col->addWidget(msg);
        }

        for (int i = 0; i < alarms.size(); ++i) {
            const Alarm &a = alarms[i];

            // ---- state → border colour
            // daily: orange | time already passed: grey | active: green
            bool passed = false;
            if (!a.daily()) {
                QString w = a.when;
                if (w.contains(' ')) {
                    QDateTime dt = QDateTime::fromString(w, "yyyy-MM-dd HH:mm");
                    passed = dt.isValid() && dt < QDateTime::currentDateTime();
                } else {
                    QTime t = QTime::fromString(w, "HH:mm");
                    passed = t.isValid() && t < QTime::currentTime();
                }
            }
            QString borderCol = a.daily() ? "#e08a2e"          // orange
                              : passed    ? "#8a8a8a"          // grey
                                          : "#4fae4f";         // green

            QFrame *card = new QFrame;
            card->setObjectName("alarmCard");
            // only the card itself gets the background/border; every
            // child stays transparent so no dark corner boxes appear
            card->setStyleSheet(QString(
                "QFrame#alarmCard { background:#303030;"
                " border:3px dashed %1; border-radius:12px; }"
                "QFrame#alarmCard QWidget { background:transparent;"
                " border:none; }").arg(borderCol));
            card->setMinimumWidth(CARD_WIDTH);
            card->setMaximumWidth(CARD_WIDTH);
            card->setFixedHeight(130);

            QHBoxLayout *row = new QHBoxLayout(card);
            row->setContentsMargins(CARD_PADDING, 10, CARD_PADDING, 10);
            row->setSpacing(ICON_TEXT_SPACING);

            // 🎵 symbol next to the time when a custom sound is set
            QLabel *timeLbl = new QLabel(
                a.timePart() + (a.sound.isEmpty() ? "" : " \xF0\x9F\x8E\xB5"));
            timeLbl->setStyleSheet(
                "font-size:44px; font-weight:bold; color:white;");
            timeLbl->setFixedWidth(a.sound.isEmpty() ? 220 : 280);

            QWidget *textWrapper = new QWidget;
            QVBoxLayout *textCol = new QVBoxLayout(textWrapper);
            textCol->setContentsMargins(0, 0, 0, 0);
            textCol->setSpacing(0);

            QLabel *ttl = new QLabel(a.title);
            ttl->setStyleSheet(
                "font-size:26px; font-weight:bold; color:white;");
            QLabel *subt = new QLabel(
                a.daily() ? "\xF0\x9F\x94\x81 Daily"
                : passed  ? "One-shot (passed)"
                          : "One-shot");
            subt->setStyleSheet("font-size:20px; color:#bbbbbb;");
            textCol->addWidget(ttl);
            textCol->addWidget(subt);

            QPushButton *del = new QPushButton(" ❌");
            del->setFixedSize(64, 64);
            del->setStyleSheet(
                "QPushButton { border:none; font-size:26px;"
                " background:transparent; }"
                "QPushButton:hover { color:#ff1616; background:#ad1236;"
                " border-radius:18px; }"
                "QPushButton:pressed { color:#ffffff; background:#550000;"
                " border-radius:18px; }");

            int idx = i;
            QObject::connect(del, &QPushButton::clicked, [this, idx]() {
                QList<Alarm> all = loadAlarms();
                if (idx >= 0 && idx < all.size())
                    all.removeAt(idx);
                saveAlarms(all);
                openPage(makeAlarmsPage());   // rebuild list
            });

            row->addWidget(timeLbl);
            row->addWidget(textWrapper, 1);
            row->addWidget(del);

            col->addWidget(card, 0, Qt::AlignHCenter);
        }

        col->addSpacing(10);

        QPushButton *add = new QPushButton("➕  Add Alarm");
        add->setFixedSize(CARD_WIDTH, 90);
        add->setStyleSheet(bigButtonStyle());
        QObject::connect(add, &QPushButton::clicked, [this]() {
            openPage(makeAddAlarmPage());
        });
        col->addWidget(add, 0, Qt::AlignHCenter);

        col->addSpacing(20);
        col->addWidget(makeBackButton([this]() { goHome(); }),
                       0, Qt::AlignCenter);
        col->addStretch();

        scroll->setWidget(inner);
        return scroll;
    }

    // --------------------------------------------------
    // ALARMS — add page (touch friendly +/- spinners)
    QWidget *makeAddAlarmPage() {
        TouchScrollArea *scroll = new TouchScrollArea;
        QWidget *inner = new QWidget;
        QVBoxLayout *col = new QVBoxLayout(inner);
        col->setContentsMargins(40, 40, 40, 40);
        col->setSpacing(24);
        col->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

        QLabel *title = new QLabel("New Alarm");
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:32px; font-weight:bold; color:white;");
        col->addWidget(title);

        // shared state
        int *hour   = new int(QTime::currentTime().hour());
        int *minute = new int(0);
        bool *daily = new bool(false);

        QLabel *timeLbl = new QLabel;
        timeLbl->setAlignment(Qt::AlignCenter);
        timeLbl->setStyleSheet("font-size:96px; font-weight:bold; color:white;");
        auto refreshTime = [timeLbl, hour, minute]() {
            timeLbl->setText(QString("%1:%2")
                .arg(*hour, 2, 10, QChar('0'))
                .arg(*minute, 2, 10, QChar('0')));
        };
        refreshTime();

        // +/- rows for hour & minute
        auto makeSpin = [&](const QString &label, int *val, int mod, int step)
            -> QWidget*
        {
            QWidget *w = new QWidget;
            QHBoxLayout *h = new QHBoxLayout(w);
            h->setSpacing(20);
            h->setAlignment(Qt::AlignCenter);

            QPushButton *minus = new QPushButton("−");
            QPushButton *plus  = new QPushButton("+");
            for (QPushButton *b : {minus, plus}) {
                b->setFixedSize(110, 90);
                b->setStyleSheet(bigButtonStyle());
                b->setAutoRepeat(true);
                b->setAutoRepeatDelay(400);
                b->setAutoRepeatInterval(120);
            }

            QLabel *lbl = new QLabel(label);
            lbl->setStyleSheet("font-size:26px; color:#bbbbbb;");
            lbl->setFixedWidth(150);
            lbl->setAlignment(Qt::AlignCenter);

            QObject::connect(minus, &QPushButton::clicked,
                [val, mod, step, refreshTime]() {
                    *val = ((*val - step) % mod + mod) % mod;
                    refreshTime();
                });
            QObject::connect(plus, &QPushButton::clicked,
                [val, mod, step, refreshTime]() {
                    *val = (*val + step) % mod;
                    refreshTime();
                });

            h->addWidget(minus);
            h->addWidget(lbl);
            h->addWidget(plus);
            return w;
        };

        col->addWidget(timeLbl);
        col->addWidget(makeSpin("Hour",   hour,   24, 1));
        col->addWidget(makeSpin("Minute", minute, 60, 5));

        // title entry
        QLineEdit *name = new QLineEdit;
        name->setPlaceholderText("Alarm title (optional)");
        name->setFixedSize(CARD_WIDTH, 80);
        name->setStyleSheet(
            "QLineEdit { background:#303030; color:white; font-size:26px;"
            " border:3px dashed #777; border-radius:12px; padding:0 18px; }");
        col->addWidget(name, 0, Qt::AlignHCenter);

        // sound picker
        QString *sound = new QString;
        QPushButton *sndBtn = new QPushButton(soundButtonText(""));
        sndBtn->setFixedSize(CARD_WIDTH, 80);
        sndBtn->setStyleSheet(bigButtonStyle() +
                              "QPushButton { font-size:26px; }");
        QObject::connect(sndBtn, &QPushButton::clicked,
            [this, sound, sndBtn]() {
                QString f = pickSoundFile(this);
                if (!f.isEmpty()) {
                    *sound = f;
                    sndBtn->setText(soundButtonText(f));
                }
            });
        col->addWidget(sndBtn, 0, Qt::AlignHCenter);

        // daily toggle
        QPushButton *repeat = new QPushButton("🔁  Repeat daily: OFF");
        repeat->setFixedSize(CARD_WIDTH, 80);
        repeat->setCheckable(true);
        repeat->setStyleSheet(bigButtonStyle() +
            "QPushButton:checked { background:#2d5a2d; }");
        QObject::connect(repeat, &QPushButton::toggled,
            [repeat, daily](bool on) {
                *daily = on;
                repeat->setText(on ? "🔁  Repeat daily: ON"
                                   : "🔁  Repeat daily: OFF");
            });
        col->addWidget(repeat, 0, Qt::AlignHCenter);

        col->addSpacing(10);

        // save
        QPushButton *save = new QPushButton("✅  Save Alarm");
        save->setFixedSize(CARD_WIDTH, 90);
        save->setStyleSheet(bigButtonStyle());
        QObject::connect(save, &QPushButton::clicked,
            [this, hour, minute, daily, name, sound]() {
                QString hm = QString("%1:%2")
                    .arg(*hour, 2, 10, QChar('0'))
                    .arg(*minute, 2, 10, QChar('0'));

                Alarm a;
                if (*daily) {
                    a.when = "daily " + hm;
                } else {
                    // if the time already passed today, date it for tomorrow
                    QTime t(*hour, *minute);
                    QDate d = QDate::currentDate();
                    if (t <= QTime::currentTime()) d = d.addDays(1);
                    if (d == QDate::currentDate())
                        a.when = hm;
                    else
                        a.when = d.toString("yyyy-MM-dd") + " " + hm;
                }
                a.title = name->text().trimmed();
                if (a.title.isEmpty()) a.title = "Alarm";
                a.sound = *sound;

                QList<Alarm> all = loadAlarms();
                all << a;
                saveAlarms(all);
                openPage(makeAlarmsPage());
            });
        col->addWidget(save, 0, Qt::AlignHCenter);

        col->addSpacing(20);
        col->addWidget(makeBackButton([this]() { openPage(makeAlarmsPage()); }),
                       0, Qt::AlignCenter);
        col->addStretch();

        scroll->setWidget(inner);
        return scroll;
    }

    // --------------------------------------------------
    // TIMER
    QWidget *makeTimerPage() {
        TouchScrollArea *scroll = new TouchScrollArea;
        QWidget *inner = new QWidget;
        QVBoxLayout *col = new QVBoxLayout(inner);
        col->setContentsMargins(40, 40, 40, 40);
        col->setSpacing(24);
        col->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

        QLabel *title = new QLabel("Timer");
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:32px; font-weight:bold; color:white;");
        col->addWidget(title);

        int *remaining = new int(5 * 60);   // seconds
        int *setValue  = new int(5 * 60);
        bool *running  = new bool(false);

        QLabel *disp = new QLabel;
        disp->setAlignment(Qt::AlignCenter);
        disp->setStyleSheet("font-size:110px; font-weight:bold; color:white;");
        auto refresh = [disp, remaining]() {
            int m = *remaining / 60;
            int s = *remaining % 60;
            disp->setText(QString("%1:%2")
                .arg(m, 2, 10, QChar('0'))
                .arg(s, 2, 10, QChar('0')));
        };
        refresh();
        col->addWidget(disp);

        // adjust buttons
        QWidget *adj = new QWidget;
        QHBoxLayout *ah = new QHBoxLayout(adj);
        ah->setSpacing(20);
        ah->setAlignment(Qt::AlignCenter);
        struct Btn { QString label; int delta; };
        for (const Btn &b : QList<Btn>{
                {"−1m", -60}, {"−10s", -10}, {"+10s", 10}, {"+1m", 60}}) {
            QPushButton *pb = new QPushButton(b.label);
            pb->setFixedSize(130, 80);
            pb->setStyleSheet(bigButtonStyle() +
                              "QPushButton { font-size:26px; }");
            int delta = b.delta;
            QObject::connect(pb, &QPushButton::clicked,
                [remaining, setValue, running, delta, refresh]() {
                    if (*running) return;
                    *remaining = qBound(10, *remaining + delta, 24 * 3600);
                    *setValue = *remaining;
                    refresh();
                });
            ah->addWidget(pb);
        }
        col->addWidget(adj);

        // sound picker (persisted between launches)
        QSettings cfg(QDir::homePath() + "/.config/Alternix/osm-clock.conf",
                      QSettings::IniFormat);
        QString *tSound = new QString(cfg.value("Timer/Sound").toString());
        QPushButton *sndBtn = new QPushButton(soundButtonText(*tSound));
        sndBtn->setFixedSize(CARD_WIDTH, 80);
        sndBtn->setStyleSheet(bigButtonStyle() +
                              "QPushButton { font-size:26px; }");
        QObject::connect(sndBtn, &QPushButton::clicked,
            [this, tSound, sndBtn]() {
                QString f = pickSoundFile(this);
                if (!f.isEmpty()) {
                    *tSound = f;
                    sndBtn->setText(soundButtonText(f));
                    QSettings c(QDir::homePath()
                                + "/.config/Alternix/osm-clock.conf",
                                QSettings::IniFormat);
                    c.setValue("Timer/Sound", f);
                }
            });

        // start / pause / reset
        QPushButton *startBtn = new QPushButton("▶️  Start");
        startBtn->setFixedSize(CARD_WIDTH, 90);
        startBtn->setStyleSheet(bigButtonStyle());

        QPushButton *resetBtn = new QPushButton("🔄  Reset");
        resetBtn->setFixedSize(CARD_WIDTH, 80);
        resetBtn->setStyleSheet(bigButtonStyle() +
                                "QPushButton { font-size:28px; }");

        QTimer *tick = new QTimer(inner);
        tick->setInterval(1000);

        QObject::connect(startBtn, &QPushButton::clicked,
            [running, startBtn, tick]() {
                *running = !*running;
                if (*running) { tick->start(); startBtn->setText("⏸️  Pause"); }
                else          { tick->stop();  startBtn->setText("▶️  Start"); }
            });

        QObject::connect(resetBtn, &QPushButton::clicked,
            [remaining, setValue, running, startBtn, tick, refresh]() {
                tick->stop();
                *running = false;
                *remaining = *setValue;
                startBtn->setText("▶️  Start");
                refresh();
            });

        QObject::connect(tick, &QTimer::timeout,
            [remaining, setValue, running, startBtn, tick, refresh, tSound]() {
                if (*remaining > 0) --*remaining;
                refresh();
                if (*remaining <= 0) {
                    tick->stop();
                    *running = false;
                    startBtn->setText("▶️  Start");
                    *remaining = *setValue;
                    int m = *setValue / 60, s = *setValue % 60;
                    dropNotification("⏰ Timer finished",
                        QString("%1:%2 timer done")
                            .arg(m, 2, 10, QChar('0'))
                            .arg(s, 2, 10, QChar('0')),
                        *tSound);
                }
            });

        col->addWidget(sndBtn, 0, Qt::AlignHCenter);

        col->addSpacing(10);
        col->addWidget(startBtn, 0, Qt::AlignHCenter);
        col->addWidget(resetBtn, 0, Qt::AlignHCenter);

        col->addSpacing(20);
        col->addWidget(makeBackButton([this]() { goHome(); }),
                       0, Qt::AlignCenter);
        col->addStretch();

        scroll->setWidget(inner);
        return scroll;
    }

    // --------------------------------------------------
    // STOPWATCH
    QWidget *makeStopwatchPage() {
        TouchScrollArea *scroll = new TouchScrollArea;
        QWidget *inner = new QWidget;
        QVBoxLayout *col = new QVBoxLayout(inner);
        col->setContentsMargins(40, 40, 40, 40);
        col->setSpacing(24);
        col->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

        QLabel *title = new QLabel("Stopwatch");
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:32px; font-weight:bold; color:white;");
        col->addWidget(title);

        qint64 *accum = new qint64(0);      // ms accumulated while paused
        QElapsedTimer *et = new QElapsedTimer;
        bool *running = new bool(false);

        QLabel *disp = new QLabel("00:00.0");
        disp->setAlignment(Qt::AlignCenter);
        disp->setStyleSheet("font-size:110px; font-weight:bold; color:white;");
        col->addWidget(disp);

        QTimer *tick = new QTimer(inner);
        tick->setInterval(100);

        auto refresh = [disp, accum, et, running]() {
            qint64 ms = *accum + (*running ? et->elapsed() : 0);
            int minutes = int(ms / 60000);
            int seconds = int((ms / 1000) % 60);
            int tenths  = int((ms / 100) % 10);
            disp->setText(QString("%1:%2.%3")
                .arg(minutes, 2, 10, QChar('0'))
                .arg(seconds, 2, 10, QChar('0'))
                .arg(tenths));
        };
        QObject::connect(tick, &QTimer::timeout, refresh);

        QPushButton *startBtn = new QPushButton("▶️  Start");
        startBtn->setFixedSize(CARD_WIDTH, 90);
        startBtn->setStyleSheet(bigButtonStyle());
        QObject::connect(startBtn, &QPushButton::clicked,
            [accum, et, running, startBtn, tick, refresh]() {
                if (!*running) {
                    et->start();
                    *running = true;
                    tick->start();
                    startBtn->setText("⏸️  Pause");
                } else {
                    *accum += et->elapsed();
                    *running = false;
                    tick->stop();
                    startBtn->setText("▶️  Start");
                    refresh();
                }
            });

        QPushButton *resetBtn = new QPushButton("🔄  Reset");
        resetBtn->setFixedSize(CARD_WIDTH, 80);
        resetBtn->setStyleSheet(bigButtonStyle() +
                                "QPushButton { font-size:28px; }");
        QObject::connect(resetBtn, &QPushButton::clicked,
            [accum, running, startBtn, tick, refresh]() {
                *accum = 0;
                *running = false;
                tick->stop();
                startBtn->setText("▶️  Start");
                refresh();
            });

        col->addSpacing(10);
        col->addWidget(startBtn, 0, Qt::AlignHCenter);
        col->addWidget(resetBtn, 0, Qt::AlignHCenter);

        col->addSpacing(20);
        col->addWidget(makeBackButton([this]() { goHome(); }),
                       0, Qt::AlignCenter);
        col->addStretch();

        scroll->setWidget(inner);
        return scroll;
    }
};

// ------------------------------------------------------
int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    // Same font-size config as osm-settings
    {
        QString cfgPath = QDir::homePath() + "/.config/Alternix/osm-settings.conf";
        QSettings cfg(cfgPath, QSettings::IniFormat);
        QScreen *scr = QGuiApplication::primaryScreen();
        int defaultPt = (scr && scr->size().width() <= 720) ? 18 : 22;
        int pt = cfg.value("UI/SettingsFontSize", defaultPt).toInt();
        pt = qBound(14, pt, 36);
        QFont f = a.font();
        f.setPointSize(pt);
        a.setFont(f);
    }

    ClockHub w;
    w.show();

    return a.exec();
}
