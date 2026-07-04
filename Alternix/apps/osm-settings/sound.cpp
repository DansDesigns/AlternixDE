#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QFrame>
#include <QSlider>
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
