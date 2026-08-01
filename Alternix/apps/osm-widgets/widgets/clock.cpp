// ─────────────────────────────────────────────────────────────
//  clock.cpp  —  Alternix desktop widget plugin
//
//  Reference implementation of the osm-widgets plugin ABI.
//  Copy this file as the starting point for new widgets.
//
//  Build:
//    g++ -fPIC -shared clock.cpp -o clock.so \
//        $(pkg-config --cflags --libs Qt5Widgets)
// ─────────────────────────────────────────────────────────────

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QDateTime>
#include <QPainter>
#include <QProcess>
#include <QMouseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QSlider>
#include <QPushButton>
#include <QColorDialog>

#include "osm-widget-plugin.h"

static const OsmWidgetInfo kInfo = {
    OSM_WIDGET_ABI,
    "clock",
    "Clock",
    "🕒",
    "Time and date on the desktop",
    300, 140,      // default size
    140,  70       // minimum size
};

// ─────────────────────────────────────────────────────────────
//  Live widget
// ─────────────────────────────────────────────────────────────
class ClockWidget : public QWidget {
public:
    explicit ClockWidget(const QString &instanceId, QWidget *parent)
        : QWidget(parent), m_id(instanceId)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        if (!arrangeMode())
            setCursor(Qt::PointingHandCursor);

        m_timeFmt  = osmWidgetGet(m_id, "TimeFormat", "HH:mm").toString();
        m_dateFmt  = osmWidgetGet(m_id, "DateFormat", "dddd d MMMM").toString();
        m_showDate = osmWidgetGet(m_id, "ShowDate", true).toBool();
        m_colour   = osmWidgetGet(m_id, "Colour", "#ffffff").toString();
        m_bgAlpha  = osmWidgetGet(m_id, "BackgroundAlpha", 120).toInt();
        m_timePt   = osmWidgetGet(m_id, "TimeFontSize", 46).toInt();
        m_datePt   = osmWidgetGet(m_id, "DateFontSize", 18).toInt();

        QVBoxLayout *v = new QVBoxLayout(this);
        v->setContentsMargins(16, 10, 16, 10);
        v->setSpacing(2);
        v->setAlignment(Qt::AlignCenter);

        m_time = new QLabel(this);
        m_time->setAlignment(Qt::AlignCenter);
        m_time->setStyleSheet(QString("color:%1; font-weight:bold; font-size:%2px; background:transparent;")
                              .arg(m_colour).arg(m_timePt));
        v->addWidget(m_time);

        m_date = new QLabel(this);
        m_date->setAlignment(Qt::AlignCenter);
        m_date->setStyleSheet(QString("color:%1; font-size:%2px; background:transparent;")
                              .arg(m_colour).arg(m_datePt));
        m_date->setVisible(m_showDate);
        v->addWidget(m_date);

        tick();

        QTimer *t = new QTimer(this);
        t->setInterval(1000);
        connect(t, &QTimer::timeout, this, [this]() { tick(); });
        t->start();
    }

protected:
    // Tap opens the full clock app. In arrange mode the events are
    // ignored instead, so they propagate up to the WidgetFrame and the
    // widget can still be dragged and resized.
    void mousePressEvent(QMouseEvent *e) override {
        if (arrangeMode() || e->button() != Qt::LeftButton) {
            e->ignore();
            return;
        }
        m_pressed  = true;
        m_pressPos = e->globalPos();
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (!m_pressed) { e->ignore(); return; }
        m_pressed = false;

        // Tap, not a stray drag.
        if ((e->globalPos() - m_pressPos).manhattanLength() < 12)
            QProcess::startDetached("osm-clock", QStringList());

        e->accept();
    }

    // Rounded translucent slab behind the text so it stays readable
    // on top of any wallpaper.
    void paintEvent(QPaintEvent *) override {
        if (m_bgAlpha <= 0) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 40, 40, m_bgAlpha));
        p.drawRoundedRect(rect(), 16, 16);
    }

private:
    // Read live rather than cached: the daemon rebuilds this widget on
    // config change, but the tap must never fight the drag handler.
    static bool arrangeMode() {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
        return cfg.value("General/EditMode", false).toBool();
    }

    void tick() {
        QDateTime now = QDateTime::currentDateTime();
        m_time->setText(now.toString(m_timeFmt));
        if (m_showDate) m_date->setText(now.toString(m_dateFmt));
    }

    QString m_id, m_timeFmt, m_dateFmt, m_colour;
    bool    m_showDate;
    int     m_bgAlpha, m_timePt, m_datePt;
    QLabel *m_time;
    QLabel *m_date;
    bool    m_pressed = false;
    QPoint  m_pressPos;
};

// ─────────────────────────────────────────────────────────────
//  Settings page (shown inside osm-widgets-settings)
// ─────────────────────────────────────────────────────────────
static const char *kRowStyle =
    "QLabel { color:white; font-size:22px; background:transparent; }";

static const char *kBoxStyle =
    "QComboBox { background:#303030; color:white; font-size:22px;"
    " border:3px dashed #777; border-radius:12px; padding:10px 14px;"
    " min-height:44px; }"
    "QComboBox QAbstractItemView { background:#303030; color:white;"
    " selection-background-color:#505050; font-size:22px; }";

static const char *kCheckStyle =
    "QCheckBox { color:white; font-size:22px; spacing:14px; }"
    "QCheckBox::indicator { width:34px; height:34px; }";

static const char *kSliderStyle =
    "QSlider::groove:horizontal { height:12px; background:#303030;"
    " border-radius:6px; }"
    "QSlider::handle:horizontal { background:#bbbbbb; width:38px;"
    " margin:-14px 0; border-radius:19px; }";

class ClockConfig : public QWidget {
public:
    explicit ClockConfig(const QString &instanceId, QWidget *parent)
        : QWidget(parent), m_id(instanceId)
    {
        setStyleSheet("background:#282828;");

        QVBoxLayout *v = new QVBoxLayout(this);
        v->setContentsMargins(24, 24, 24, 24);
        v->setSpacing(22);
        v->setAlignment(Qt::AlignTop);

        // Time format
        v->addWidget(label("Time format"));
        m_timeBox = new QComboBox(this);
        m_timeBox->setStyleSheet(kBoxStyle);
        m_timeBox->addItem("24 hour  ·  14:05",        "HH:mm");
        m_timeBox->addItem("24 hour + seconds",        "HH:mm:ss");
        m_timeBox->addItem("12 hour  ·  2:05 pm",      "h:mm ap");
        m_timeBox->addItem("12 hour + seconds",        "h:mm:ss ap");
        selectData(m_timeBox, osmWidgetGet(m_id, "TimeFormat", "HH:mm").toString());
        v->addWidget(m_timeBox);

        // Date format
        v->addWidget(label("Date format"));
        m_dateBox = new QComboBox(this);
        m_dateBox->setStyleSheet(kBoxStyle);
        m_dateBox->addItem("Saturday 1 August",  "dddd d MMMM");
        m_dateBox->addItem("Sat 1 Aug",          "ddd d MMM");
        m_dateBox->addItem("01/08/2026",         "dd/MM/yyyy");
        m_dateBox->addItem("2026-08-01",         "yyyy-MM-dd");
        selectData(m_dateBox, osmWidgetGet(m_id, "DateFormat", "dddd d MMMM").toString());
        v->addWidget(m_dateBox);

        m_showDate = new QCheckBox("Show the date", this);
        m_showDate->setStyleSheet(kCheckStyle);
        m_showDate->setChecked(osmWidgetGet(m_id, "ShowDate", true).toBool());
        v->addWidget(m_showDate);

        // Time font size
        v->addWidget(label("Time size"));
        m_timePt = new QSlider(Qt::Horizontal, this);
        m_timePt->setStyleSheet(kSliderStyle);
        m_timePt->setRange(20, 110);
        m_timePt->setValue(osmWidgetGet(m_id, "TimeFontSize", 46).toInt());
        v->addWidget(m_timePt);

        // Background opacity
        v->addWidget(label("Background opacity"));
        m_bgAlpha = new QSlider(Qt::Horizontal, this);
        m_bgAlpha->setStyleSheet(kSliderStyle);
        m_bgAlpha->setRange(0, 255);
        m_bgAlpha->setValue(osmWidgetGet(m_id, "BackgroundAlpha", 120).toInt());
        v->addWidget(m_bgAlpha);

        // Text colour
        QHBoxLayout *colRow = new QHBoxLayout;
        colRow->setSpacing(16);
        colRow->addWidget(label("Text colour"));
        m_colour = osmWidgetGet(m_id, "Colour", "#ffffff").toString();
        m_colourBtn = new QPushButton(m_colour, this);
        m_colourBtn->setMinimumHeight(64);
        restyleColourButton();
        connect(m_colourBtn, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(QColor(m_colour), this, "Text colour");
            if (c.isValid()) {
                m_colour = c.name();
                m_colourBtn->setText(m_colour);
                restyleColourButton();
                apply();
            }
        });
        colRow->addWidget(m_colourBtn, 1);
        v->addLayout(colRow);

        v->addStretch();

        // Everything writes straight through — the daemon is watching
        // the config file and rebuilds itself on change.
        connect(m_timeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { apply(); });
        connect(m_dateBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { apply(); });
        connect(m_showDate, &QCheckBox::toggled, this, [this](bool) { apply(); });
        connect(m_timePt,  &QSlider::sliderReleased, this, [this]() { apply(); });
        connect(m_bgAlpha, &QSlider::sliderReleased, this, [this]() { apply(); });
    }

private:
    QLabel *label(const QString &t) {
        QLabel *l = new QLabel(t, this);
        l->setStyleSheet(kRowStyle);
        return l;
    }

    void restyleColourButton() {
        m_colourBtn->setStyleSheet(QString(
            "QPushButton { background:#303030; color:%1; font-size:22px;"
            " border:3px dashed %1; border-radius:12px; padding:10px; }")
            .arg(m_colour));
    }

    static void selectData(QComboBox *box, const QString &data) {
        int i = box->findData(data);
        if (i >= 0) box->setCurrentIndex(i);
    }

    void apply() {
        osmWidgetSet(m_id, "TimeFormat",      m_timeBox->currentData().toString());
        osmWidgetSet(m_id, "DateFormat",      m_dateBox->currentData().toString());
        osmWidgetSet(m_id, "ShowDate",        m_showDate->isChecked());
        osmWidgetSet(m_id, "TimeFontSize",    m_timePt->value());
        osmWidgetSet(m_id, "BackgroundAlpha", m_bgAlpha->value());
        osmWidgetSet(m_id, "Colour",          m_colour);
    }

    QString    m_id, m_colour;
    QComboBox *m_timeBox;
    QComboBox *m_dateBox;
    QCheckBox *m_showDate;
    QSlider   *m_timePt;
    QSlider   *m_bgAlpha;
    QPushButton *m_colourBtn;
};

// ─────────────────────────────────────────────────────────────
//  Exported ABI
// ─────────────────────────────────────────────────────────────
extern "C" {

const OsmWidgetInfo *osm_widget_info() { return &kInfo; }

QWidget *osm_widget_create(const char *instanceId, QWidget *parent) {
    return new ClockWidget(QString::fromUtf8(instanceId), parent);
}

QWidget *osm_widget_config(const char *instanceId, QWidget *parent) {
    return new ClockConfig(QString::fromUtf8(instanceId), parent);
}

}
