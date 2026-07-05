#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QFrame>
#include <QSlider>
#include <QComboBox>
#include <QScrollArea>
#include <QScroller>
#include <QProcess>
#include <QSettings>
#include <QDir>

// ---------------------------------------------------------
// PulseAudio Helpers (only for Main Volume now)
// ---------------------------------------------------------
static QString runCmd(const QString &cmd) {
    QProcess p;
    p.start("bash", {"-c", cmd});
    p.waitForFinished();
    return p.readAllStandardOutput();
}

static int percentToPa(int percent) {
    if (percent < 0)  percent = 0;
    if (percent > 100) percent = 100;
    return (percent * 65536) / 100;
}

// Set default sink volume (Main Volume)
static void setDefaultSinkVolumePercent(int percent) {
    int paValue = percentToPa(percent);
    QString cmd = QString("pactl set-sink-volume @DEFAULT_SINK@ %1").arg(paValue);
    runCmd(cmd);
}

// ---------------------------------------------------------
// Slider Style (rounded, thin groove, circular handles)
// ---------------------------------------------------------
static QString sliderStyle() {
    return
        // Groove (thin, rounded)
        "QSlider { background:transparent; }"
        "QSlider::groove:horizontal {"
        "   background:#666666;"
        "   height:14px;"
        "   border-radius:7px;"
        "   margin:0px;"
        "}"

        // Active section (blue)
        "QSlider::sub-page:horizontal {"
        "   background:#4aa3ff;"
        "   border-radius:7px;"
        "}"

        // Large circular handle
        "QSlider::handle:horizontal {"
        "   background:white;"
        "   border-radius:16px;"
        "   width:32px;"
        "   height:32px;"
        "   margin:-9px 0;"     /* centers handle on 14px groove */
        "}"

        "QSlider::handle:horizontal:pressed {"
        "   background:#e0e0e0;"
        "}";
}

// ---------------------------------------------------------
// Card Builder
// ---------------------------------------------------------
static QFrame* makeSliderCard(const QString &title, QSlider **outSlider) {
    QFrame *card = new QFrame();
    card->setStyleSheet(
        "QFrame { background:#3a3a3a; border-radius:30px; }"
    );
    card->setFixedHeight(170);

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(20, 20, 20, 20);
    lay->setSpacing(15);

    QLabel *lbl = new QLabel(title, card);
    lbl->setStyleSheet("font-size:30px; color:white; font-weight:bold;");
    lbl->setAlignment(Qt::AlignCenter);

    QSlider *slider = new QSlider(Qt::Horizontal, card);
    slider->setRange(0, 100);
    slider->setValue(50);
    slider->setStyleSheet(sliderStyle());
    slider->setFixedHeight(40);

    *outSlider = slider;

    lay->addWidget(lbl);
    lay->addWidget(slider);

    return card;
}

// ---------------------------------------------------------
// Boot sound picker
// Lists audio files from ~/.config/Alternix/sounds/ in a dropdown.
// Selection is stored as Sound/BootSound (filename); osm-status
// reads it when playing the once-per-boot sound after unlock.
// ---------------------------------------------------------
static QString soundsDir()
{
    return QDir::homePath() + "/.config/Alternix/sounds";
}

static QString comboStyle()
{
    return
        "QComboBox {"
        "   background:#444444;"
        "   color:white;"
        "   border:1px solid #222222;"
        "   border-radius:16px;"
        "   font-size:24px;"
        "   font-family:'DejaVu Sans';"
        "   padding:10px 20px;"
        "}"
        "QComboBox::drop-down { border:none; width:50px; }"
        "QComboBox::down-arrow {"
        "   image:none;"
        "   border-left:12px solid transparent;"
        "   border-right:12px solid transparent;"
        "   border-top:14px solid #bbbbbb;"
        "   margin-right:16px;"
        "}"
        "QComboBox QFrame {"            /* popup container frame */
        "   border:none;"
        "   background:#4a4a4a;"
        "}"
        "QComboBox QAbstractItemView {"
        "   background:#4a4a4a;"          /* lighter than the cards */
        "   color:white;"
        "   font-size:24px;"
        "   font-family:'DejaVu Sans';"
        "   border:none;"                 /* no white frame */
        "   selection-background-color:#5f5f5f;"
        "   outline:none;"
        "   padding:6px;"
        "}";
}

static QFrame* makeBootSoundCard(QSettings *settings)
{
    QFrame *card = new QFrame();
    card->setStyleSheet("QFrame { background:#3a3a3a; border-radius:30px; }");

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(20, 20, 20, 20);
    lay->setSpacing(15);

    QLabel *lbl = new QLabel("Boot Sound", card);
    lbl->setStyleSheet("font-size:30px; color:white; font-weight:bold;");
    lbl->setAlignment(Qt::AlignCenter);
    lay->addWidget(lbl);

    QHBoxLayout *row = new QHBoxLayout();
    row->setSpacing(16);

    QComboBox *combo = new QComboBox(card);
    combo->setStyleSheet(comboStyle());
    combo->setFixedHeight(60);
    combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // populate from the sounds folder
    QDir d(soundsDir());
    if (!d.exists()) d.mkpath(".");
    QStringList files = d.entryList(
        {"*.wav", "*.ogg", "*.flac", "*.mp3"},
        QDir::Files, QDir::Name);

    combo->addItem("Default (boot.*)", QString());
    for (const QString &f : files)
        combo->addItem(f, f);

    // restore saved selection
    QString saved = settings->value("Sound/BootSound").toString();
    if (!saved.isEmpty()) {
        int idx = combo->findData(saved);
        if (idx >= 0) combo->setCurrentIndex(idx);
    }

    // ▶ preview button (same playback chain as osm-status)
    QPushButton *play = new QPushButton("▶", card);
    play->setFixedSize(60, 60);
    play->setStyleSheet(
        "QPushButton { background:#444444; color:#7CFC00;"
        " border:1px solid #222222; border-radius:16px;"
        " font-size:26px; font-weight:bold; }"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }");

    row->addWidget(combo, 1);
    row->addWidget(play);
    lay->addLayout(row);

    QLabel *note = new QLabel(
        "Played once per boot after unlocking. Files are read from "
        "~/.config/Alternix/sounds/", card);
    note->setStyleSheet("font-size:18px; color:#888888;");
    note->setWordWrap(true);
    lay->addWidget(note);

    QObject::connect(combo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        [combo, settings](int) {
            settings->setValue("Sound/BootSound",
                               combo->currentData().toString());
            settings->sync();
        });

    QObject::connect(play, &QPushButton::clicked, [combo]() {
        QString f = combo->currentData().toString();
        QString path;
        if (f.isEmpty()) {
            // default: first boot.* in the folder
            QDir d(soundsDir());
            QStringList m = d.entryList(
                {"boot.wav", "boot.ogg", "boot.flac", "boot.mp3"},
                QDir::Files, QDir::Name);
            if (!m.isEmpty()) path = d.absoluteFilePath(m.first());
        } else {
            path = soundsDir() + "/" + f;
        }
        if (path.isEmpty() || !QFile::exists(path)) return;

        QString q = "'" + path.replace("'", "'\\''") + "'";
        QString cmd = path.endsWith(".mp3", Qt::CaseInsensitive)
            ? QString("mpg123 -q %1 2>/dev/null || "
                      "cvlc --play-and-exit --intf dummy %1 2>/dev/null").arg(q)
            : QString("paplay %1 2>/dev/null || aplay -q %1 2>/dev/null || "
                      "cvlc --play-and-exit --intf dummy %1 2>/dev/null").arg(q);
        QProcess::startDetached("sh", QStringList() << "-c" << cmd);
    });

    return card;
}

// ---------------------------------------------------------
// SoundPage
// ---------------------------------------------------------
class SoundPage : public QWidget
{
public:
    explicit SoundPage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), stackedWidget(stack)
    {
        setStyleSheet("background:#282828;");

        // QSettings using explicit path: ~/.config/Alternix/osm-settings.conf
        QString confPath = QDir::homePath() + "/.config/Alternix/osm-settings.conf";
        settings = new QSettings(confPath, QSettings::IniFormat, this);

        QVBoxLayout *outer = new QVBoxLayout(this);
        outer->setContentsMargins(40, 40, 40, 40);
        outer->setSpacing(20);
        // NOTE: no layout-level alignment here. Setting Qt::AlignTop on the
        // layout makes Qt ignore stretch factors, so the scroll area grew to
        // its full content height and the cards ran off the bottom of the
        // screen. With no alignment, addWidget(scroll, 1) confines the scroll
        // area between the pinned title and the pinned back button.

        QLabel *titleLabel = new QLabel("Sound", this);
        titleLabel->setStyleSheet("font-size:42px; color:white; font-weight:bold;");
        titleLabel->setAlignment(Qt::AlignCenter);
        outer->addWidget(titleLabel);

        // ----------------------------------------------------
        // Scroll Area (touch scroll, NO visible scrollbars)
        // ----------------------------------------------------
        QScrollArea *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

        QWidget *scrollContainer = new QWidget(scroll);
        scrollContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        QVBoxLayout *scrollLayout = new QVBoxLayout(scrollContainer);
        scrollLayout->setContentsMargins(0, 0, 0, 0);
        scrollLayout->setSpacing(30);  // padding between cards

        QSlider *mainSlider, *notifSlider, *mediaSlider, *callSlider, *alarmSlider, *vibSlider;

        // Renamed: "System Sounds" -> "Main Volume"
        scrollLayout->addWidget(makeSliderCard("Main Volume",         &mainSlider));
        scrollLayout->addWidget(makeSliderCard("Notifications",       &notifSlider));
        scrollLayout->addWidget(makeSliderCard("Media",               &mediaSlider));
        scrollLayout->addWidget(makeSliderCard("In-Call",             &callSlider));
        scrollLayout->addWidget(makeSliderCard("Alarms",              &alarmSlider));
        scrollLayout->addWidget(makeSliderCard("Vibration Strength",  &vibSlider));
        scrollLayout->addWidget(makeBootSoundCard(settings));

        scrollLayout->addStretch();
        scroll->setWidget(scrollContainer);
        outer->addWidget(scroll, 1);

        // ----------------------------------------------------
        // Restore slider positions from config
        // ----------------------------------------------------
        int mainVal   = settings->value("Sound/MainVolume",        50).toInt();
        int notifVal  = settings->value("Sound/Notifications",     50).toInt();
        int mediaVal  = settings->value("Sound/Media",             50).toInt();
        int callVal   = settings->value("Sound/InCall",            50).toInt();
        int alarmVal  = settings->value("Sound/Alarms",            50).toInt();
        int vibVal    = settings->value("Sound/VibrationStrength", 50).toInt();

        mainSlider->setValue(mainVal);
        notifSlider->setValue(notifVal);
        mediaSlider->setValue(mediaVal);
        callSlider->setValue(callVal);
        alarmSlider->setValue(alarmVal);
        vibSlider->setValue(vibVal);

        // ----------------------------------------------------
        // Slider connections
        // ----------------------------------------------------
        // Main Volume: controls real PulseAudio volume + saves setting
        connect(mainSlider, &QSlider::valueChanged, this, [=](int v) {
            setDefaultSinkVolumePercent(v);
            settings->setValue("Sound/MainVolume", v);
        });

        // Others: only remember positions (no audio behavior)
        connect(notifSlider, &QSlider::valueChanged, this, [=](int v) {
            settings->setValue("Sound/Notifications", v);
        });

        connect(mediaSlider, &QSlider::valueChanged, this, [=](int v) {
            settings->setValue("Sound/Media", v);
        });

        connect(callSlider, &QSlider::valueChanged, this, [=](int v) {
            settings->setValue("Sound/InCall", v);
        });

        connect(alarmSlider, &QSlider::valueChanged, this, [=](int v) {
            settings->setValue("Sound/Alarms", v);
        });

        connect(vibSlider, &QSlider::valueChanged, this, [=](int v) {
            settings->setValue("Sound/VibrationStrength", v);
            // Hook up your vibration script here later if you like.
        });

        // ----------------------------------------------------
        // Back Button
        // ----------------------------------------------------
        QPushButton *backButton = new QPushButton(QStringLiteral("❮"), this);
        backButton->setFixedSize(140, 60);
        backButton->setStyleSheet(
            "QPushButton { background:#444444; color:white; border:1px solid #222222; "
            "border-radius:16px; font-size:34px; }"
            "QPushButton:hover { background:#555555; }"
            "QPushButton:pressed { background:#333333; }"
        );
        QHBoxLayout *backLay = new QHBoxLayout();
        backLay->addWidget(backButton, 0, Qt::AlignHCenter);
        outer->addLayout(backLay);

        connect(backButton, &QPushButton::clicked, this, [this](){
            if (stackedWidget)
                stackedWidget->setCurrentIndex(0);
        });
    }

private:
    QStackedWidget *stackedWidget = nullptr;
    QSettings *settings = nullptr;
};

// ---------------------------------------------------------
// Factory
// ---------------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new SoundPage(stack);
}
