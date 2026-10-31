// moon-calendar.cpp — AlternixOS 13 Moon / Dreamspell calendar widget
// Build: g++ -std=c++11 -fPIC -shared moon-calendar.cpp -o moon-calendar.so \
//           $(pkg-config --cflags --libs Qt5Widgets)
// 13 moons x 28 days; July 26 begins the year, July 25 is Day Out of Time.
// Feb 29 is a separate 0.0 day and is not counted among the 364 moon days.

#include "osm-widget-plugin.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QDate>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

static const OsmWidgetInfo kInfo = {
    OSM_WIDGET_ABI, "moon-calendar", "13 Moon Calendar", "🌙",
    "Dreamspell calendar: 13 moons of 28 days",
    455, 445, 300, 300
};

namespace {
static const char *kMoonNames[13] = {
    "Magnetic", "Lunar", "Electric", "Self-Existing", "Overtone", "Rhythmic",
    "Resonant", "Galactic", "Solar", "Planetary", "Spectral", "Crystal", "Cosmic"
};
static const char *kMoonQualities[13] = {
    "Purpose", "Challenge", "Service", "Form", "Radiance", "Equality",
    "Attunement", "Integrity", "Intention", "Manifestation", "Liberation",
    "Cooperation", "Presence"
};
static const char *kPlasmas[7] = {
    "Dali", "Seli", "Gamma", "Kali", "Alpha", "Limi", "Silio"
};

static int moonYearForDate(const QDate &d) {
    return d >= QDate(d.year(), 7, 26) ? d.year() : d.year() - 1;
}
static QDate leapDayInYear(int moonYear) {
    return QDate(moonYear + 1, 2, 29); // invalid in non-leap years
}
static QDate dayOutOfTime(int moonYear) {
    return QDate(moonYear + 1, 7, 25);
}
static QDate dateForMoonDay(int moonYear, int moon, int day) {
    // Stable 28-day calendar: a leap year's Feb 29 does not shift
    // the Moon Day numbers for the dates after February.
    if (moon < 0 || moon > 12 || day < 1 || day > 28) return QDate();
    const QDate first(moonYear, 7, 26);
    int ordinal = moon * 28 + (day - 1);
    const QDate leap = leapDayInYear(moonYear);
    if (leap.isValid() && ordinal >= first.daysTo(leap)) ++ordinal;
    return first.addDays(ordinal);
}
static int ordinalForDate(const QDate &d) {
    const int year = moonYearForDate(d);
    const QDate first(year, 7, 26);
    int ordinal = first.daysTo(d);
    const QDate leap = leapDayInYear(year);
    if (leap.isValid() && d > leap) --ordinal;
    return ordinal;
}
static bool arranging() {
    QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
    return cfg.value("General/EditMode", false).toBool();
}
static QColor colourFrom(const QString &s, const char *fallback) {
    QColor c(s);
    return c.isValid() ? c : QColor(fallback);
}
static void setPainterFont(QPainter &p, int pixels, bool bold = false) {
    QFont f = p.font();
    f.setFamily("Sans Serif");
    f.setPixelSize(qMax(8, pixels));
    f.setBold(bold);
    p.setFont(f);
}
static void paintButton(QPainter &p, const QRectF &r, const QString &text,
                        const QColor &foreground, int fontPx, double radius) {
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(foreground.red(), foreground.green(), foreground.blue(), 46));
    p.drawRoundedRect(r, radius, radius);
    p.setPen(foreground);
    setPainterFont(p, fontPx, true);
    p.drawText(r, Qt::AlignCenter, text);
}
static QLabel *label(const QString &s, QWidget *parent) {
    QLabel *result = new QLabel(s, parent);
    result->setStyleSheet("color:white; background:transparent; font-size:21px;");
    return result;
}
static void colourButton(QPushButton *button, const QColor &c) {
    button->setText(c.name().toUpper());
    button->setStyleSheet(QString("QPushButton { background:#303030; color:%1;"
                                  "border:2px dashed %1; border-radius:10px; padding:10px;"
                                  "font-size:20px; min-height:42px; }").arg(c.name()));
}
static QString sliderStyle() {
    return "QSlider::groove:horizontal { height:10px; background:#3a3a3a; border-radius:5px; }"
           "QSlider::handle:horizontal { background:#ddd; width:30px; margin:-10px 0; border-radius:15px; }";
}
} // namespace

class MoonCalendarWidget : public QWidget {
public:
    explicit MoonCalendarWidget(const QString &id, QWidget *parent) : QWidget(parent), m_id(id) {
        setAttribute(Qt::WA_TranslucentBackground);
        if (!arranging()) setCursor(Qt::PointingHandCursor);
        m_colour = colourFrom(osmWidgetGet(id, "Colour", "#ffffff").toString(), "#ffffff");
        m_accent = colourFrom(osmWidgetGet(id, "AccentColour", "#d2a6ff").toString(), "#d2a6ff");
        m_bgAlpha = qBound(0, osmWidgetGet(id, "BackgroundAlpha", 170).toInt(), 255);
        m_showGregorian = osmWidgetGet(id, "ShowGregorian", true).toBool();
        m_showDetails = osmWidgetGet(id, "ShowDetails", true).toBool();
        m_today = QDate::currentDate();
        setViewFromDate(m_today);
        QTimer *timer = new QTimer(this);
        timer->setInterval(30000);
        connect(timer, &QTimer::timeout, this, [this]() {
            QDate today = QDate::currentDate();
            if (today != m_today) {
                m_today = today;
                if (!m_browsing) setViewFromDate(today);
                update();
            }
        });
        timer->start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 40, 40, m_bgAlpha));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 17, 17);
        const double u = scaleUnit();
        const qreal pad = 17 * u;
        const bool special = m_moon >= 13;

        paintButton(p, prevRect(), QString::fromUtf8("‹"), m_colour, int(31 * u), 10 * u);
        paintButton(p, nextRect(), QString::fromUtf8("›"), m_colour, int(31 * u), 10 * u);
        p.setPen(m_colour);
        setPainterFont(p, int((special ? 22 : 25) * u), true);
        QString heading = m_moon == 13 ? "Day Out of Time"
                        : m_moon == 14 ? "Leap Day 0.0"
                        : QString::fromLatin1(kMoonNames[m_moon]) + " Moon";
        p.drawText(QRectF(66 * u, 14 * u, width() - 132 * u, 46 * u), Qt::AlignCenter, heading);

        p.setPen(QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 205));
        setPainterFont(p, int(15 * u));
        QString detail = special ? QString("13 Moon year %1–%2").arg(m_year).arg(m_year + 1)
            : QString("Moon %1 of 13  •  %2").arg(m_moon + 1)
                   .arg(QString::fromLatin1(kMoonQualities[m_moon]));
        p.drawText(QRectF(pad, 62 * u, width() - pad * 2, 26 * u), Qt::AlignCenter, detail);

        if (special) paintSpecial(p, u);
        else paintMoonGrid(p, u);

        const QRectF footerLine(pad, height() - 92 * u, width() - 2 * pad, 1);
        p.setPen(QPen(QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 86), 1));
        p.drawLine(footerLine.topLeft(), footerLine.topRight());
        if (m_showDetails) {
            p.setPen(m_colour);
            setPainterFont(p, int(15 * u), true);
            QString selectedText;
            if (m_moon == 13) selectedText = "25 July  •  Day Out of Time";
            else if (m_moon == 14) selectedText = "29 February  •  0.0 Hunab Ku";
            else {
                selectedText = QString("Day %1  ·  %2").arg(m_selectedDay)
                               .arg(QString::fromLatin1(kPlasmas[(m_selectedDay - 1) % 7]));
                if (m_showGregorian)
                    selectedText += "  ·  " + QLocale::system().toString(
                        dateForMoonDay(m_year, m_moon, m_selectedDay), "d MMM yyyy");
            }
            p.drawText(QRectF(pad + 3 * u, height() - 87 * u,
                              width() - 2 * pad - 6 * u, 34 * u),
                       Qt::AlignCenter, selectedText);
        }
        paintButton(p, specialButtonRect(),
                    m_moon == 13 ? "Back to moons" :
                    m_moon == 14 ? "Back to moon" : "Day Out of Time",
                    m_accent, int(14 * u), 9 * u);
        paintButton(p, todayRect(), "Today", m_accent, int(15 * u), 9 * u);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (arranging() || e->button() != Qt::LeftButton) { e->ignore(); return; }
        m_pressed = true;
        m_pressPos = e->pos();
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (!m_pressed || arranging() || e->button() != Qt::LeftButton) {
            m_pressed = false;
            e->ignore();
            return;
        }
        m_pressed = false;
        if ((e->pos() - m_pressPos).manhattanLength() > 12) { e->accept(); return; }
        const QPointF pt(e->pos());
        if (prevRect().contains(pt)) moveView(-1);
        else if (nextRect().contains(pt)) moveView(1);
        else if (todayRect().contains(pt)) {
            m_today = QDate::currentDate();
            m_browsing = false;
            setViewFromDate(m_today);
        } else if (specialButtonRect().contains(pt)) {
            if (m_moon == 13) { m_moon = 12; m_selectedDay = 28; }
            else if (m_moon == 14) { m_moon = 7; m_selectedDay = 22; }
            else { m_moon = 13; m_selectedDay = 1; }
            m_browsing = true;
            update();
        } else if (m_moon < 13) {
            QRectF grid = gridRect();
            if (grid.contains(pt)) {
                const int col = qBound(0, int((pt.x() - grid.x()) / (grid.width() / 7.0)), 6);
                const int row = qBound(0, int((pt.y() - grid.y()) / (grid.height() / 4.0)), 3);
                m_selectedDay = row * 7 + col + 1;
                m_browsing = true;
                update();
            } else if (leapButtonRect().contains(pt) && hasLeapDayInMoon()) {
                m_moon = 14;
                m_browsing = true;
                update();
            }
        }
        e->accept();
    }

private:
    double scaleUnit() const {
        return qBound(0.65, qMin(width() / 455.0, height() / 445.0), 2.6);
    }
    QRectF prevRect() const {
        double u = scaleUnit();
        return QRectF(17 * u, 15 * u, 47 * u, 45 * u);
    }
    QRectF nextRect() const {
        double u = scaleUnit();
        return QRectF(width() - 64 * u, 15 * u, 47 * u, 45 * u);
    }
    QRectF todayRect() const {
        double u = scaleUnit();
        return QRectF(width() - 100 * u, height() - 47 * u, 83 * u, 35 * u);
    }
    QRectF specialButtonRect() const {
        double u = scaleUnit();
        return QRectF(17 * u, height() - 47 * u, qMin(156 * u, width() - 126 * u), 35 * u);
    }
    QRectF gridRect() const {
        double u = scaleUnit();
        return QRectF(17 * u, 174 * u, width() - 34 * u,
                      qMax(1.0, height() - 276 * u));
    }
    QRectF leapButtonRect() const {
        double u = scaleUnit();
        return QRectF(17 * u, 117 * u, width() - 34 * u, 27 * u);
    }
    bool hasLeapDayInMoon() const {
        if (m_moon > 12) return false;
        const QDate leap = leapDayInYear(m_year);
        if (!leap.isValid()) return false;
        return dateForMoonDay(m_year, m_moon, 1) < leap &&
               dateForMoonDay(m_year, m_moon, 28) > leap;
    }
    void setViewFromDate(const QDate &d) {
        m_year = moonYearForDate(d);
        if (d == dayOutOfTime(m_year)) {
            m_moon = 13;
            m_selectedDay = 1;
        } else if (d == leapDayInYear(m_year)) {
            m_moon = 14;
            m_selectedDay = 1;
        } else {
            const int ordinal = qBound(0, ordinalForDate(d), 363);
            m_moon = ordinal / 28;
            m_selectedDay = ordinal % 28 + 1;
        }
        update();
    }
    void moveView(int direction) {
        if (m_moon == 14) {
            m_moon = 7; // Galactic Moon covers Feb 29 in a leap year
            m_selectedDay = direction < 0 ? 22 : 23;
            m_browsing = true;
            update();
            return;
        } else {
            int next = m_moon + direction;
            if (next < 0) { --m_year; next = 13; }
            if (next > 13) { ++m_year; next = 0; }
            m_moon = next;
        }
        m_selectedDay = (m_moon == 12 && direction < 0) ? 28 : 1;
        m_browsing = true;
        update();
    }
    void paintMoonGrid(QPainter &p, double u) {
        const qreal pad = 17 * u;
        const QDate from = dateForMoonDay(m_year, m_moon, 1);
        const QDate to = dateForMoonDay(m_year, m_moon, 28);
        p.setPen(QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 190));
        setPainterFont(p, int(14 * u));
        p.drawText(QRectF(pad, 92 * u, width() - pad * 2, 27 * u), Qt::AlignCenter,
                   QLocale::system().toString(from, "d MMM yyyy") + "  –  " +
                   QLocale::system().toString(to, "d MMM yyyy"));
        if (hasLeapDayInMoon()) {
            p.setPen(m_accent);
            setPainterFont(p, int(12 * u), true);
            p.drawText(leapButtonRect(), Qt::AlignCenter,
                       "29 Feb · 0.0 (outside the moon count) · tap");
        } else {
            p.setPen(QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 110));
            setPainterFont(p, int(12 * u));
            p.drawText(leapButtonRect(), Qt::AlignCenter, "4 heptads  ·  28 days");
        }

        const QRectF grid = gridRect();
        const qreal cellW = grid.width() / 7.0;
        const qreal cellH = grid.height() / 4.0;
        for (int col = 0; col < 7; ++col) {
            p.setPen(m_accent);
            setPainterFont(p, int(13 * u), true);
            p.drawText(QRectF(grid.x() + cellW * col, grid.y() - 27 * u,
                              cellW, 26 * u), Qt::AlignCenter,
                       QString::fromLatin1(kPlasmas[col]));
        }
        for (int day = 1; day <= 28; ++day) {
            const int idx = day - 1;
            const QRectF box(grid.x() + (idx % 7) * cellW,
                             grid.y() + (idx / 7) * cellH, cellW, cellH);
            const QRectF inner = box.adjusted(2 * u, 2 * u, -2 * u, -2 * u);
            const QDate gregorian = dateForMoonDay(m_year, m_moon, day);
            if (day == m_selectedDay) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 86));
                p.drawRoundedRect(inner, 9 * u, 9 * u);
            }
            if (gregorian == m_today) {
                p.setPen(QPen(m_accent, qMax(1.3, 2.0 * u)));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(inner.adjusted(1, 1, -1, -1), 9 * u, 9 * u);
            }
            p.setPen(gregorian == m_today ? m_accent : m_colour);
            setPainterFont(p, int((m_showGregorian ? 18 : 22) * u),
                 gregorian == m_today || day == m_selectedDay);
            const QRectF dayLabel = m_showGregorian
                ? QRectF(box.x(), box.y() + 4 * u, box.width(), box.height() * 0.55)
                : box;
            p.drawText(dayLabel, Qt::AlignCenter, QString::number(day));
            if (m_showGregorian) {
                p.setPen(QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 166));
                setPainterFont(p, int(11 * u));
                p.drawText(QRectF(box.x(), box.y() + box.height() * 0.55,
                                  box.width(), box.height() * 0.38),
                           Qt::AlignCenter, QLocale::system().toString(gregorian, "d MMM"));
            }
        }
    }
    void paintSpecial(QPainter &p, double u) {
        const QDate d = m_moon == 13 ? dayOutOfTime(m_year) : leapDayInYear(m_year);
        const QString mainText = m_moon == 13 ? "✦" : "0.0";
        const QRectF content(18 * u, 105 * u, width() - 36 * u,
                             height() - 208 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 35));
        const QPointF centre(content.center().x(), content.center().y() - 8 * u);
        const double radius = qMin(content.width(), content.height()) * 0.33;
        p.drawEllipse(centre, radius, radius);
        p.setPen(m_accent);
        setPainterFont(p, int((m_moon == 13 ? 82 : 57) * u), true);
        p.drawText(QRectF(centre.x() - 85 * u, centre.y() - 55 * u, 170 * u, 105 * u),
                   Qt::AlignCenter, mainText);
        p.setPen(m_colour);
        setPainterFont(p, int(19 * u), true);
        p.drawText(QRectF(18 * u, height() - 160 * u, width() - 36 * u, 29 * u),
                   Qt::AlignCenter, QLocale::system().toString(d, "dddd d MMMM yyyy"));
        setPainterFont(p, int(14 * u));
        p.setPen(QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 190));
        p.drawText(QRectF(22 * u, height() - 135 * u, width() - 44 * u, 33 * u),
                   Qt::AlignCenter, m_moon == 13
                       ? "A day outside the thirteen 28-day moons"
                       : "Leap day is outside the 28-day moon count");
    }

    QString m_id;
    QColor m_colour, m_accent;
    int m_bgAlpha = 170;
    bool m_showGregorian = true, m_showDetails = true;
    int m_year = 2026;
    int m_moon = 0; // 0..12 moons, 13 July 25, 14 leap day Feb 29
    int m_selectedDay = 1;
    bool m_browsing = false, m_pressed = false;
    QPoint m_pressPos;
    QDate m_today;
};

class MoonCalendarConfig : public QWidget {
public:
    explicit MoonCalendarConfig(const QString &id, QWidget *parent) : QWidget(parent), m_id(id) {
        setStyleSheet("background:#282828;");
        QVBoxLayout *v = new QVBoxLayout(this);
        v->setContentsMargins(24, 24, 24, 24);
        v->setSpacing(19);
        v->setAlignment(Qt::AlignTop);
        QLabel *note = label("13 moons × 28 days · year starts 26 July", this);
        note->setWordWrap(true);
        v->addWidget(note);
        m_gregorian = new QCheckBox("Show Gregorian dates in each cell", this);
        m_details = new QCheckBox("Show selected moon day", this);
        for (QCheckBox *c : {m_gregorian, m_details}) {
            c->setStyleSheet("QCheckBox { color:white; font-size:20px; spacing:12px; }"
                             "QCheckBox::indicator { width:30px; height:30px; }");
            v->addWidget(c);
        }
        m_gregorian->setChecked(osmWidgetGet(id, "ShowGregorian", true).toBool());
        m_details->setChecked(osmWidgetGet(id, "ShowDetails", true).toBool());
        v->addWidget(label("Background opacity", this));
        m_opacity = new QSlider(Qt::Horizontal, this);
        m_opacity->setRange(0, 255);
        m_opacity->setValue(osmWidgetGet(id, "BackgroundAlpha", 170).toInt());
        m_opacity->setStyleSheet(sliderStyle());
        v->addWidget(m_opacity);
        m_textColour = colourFrom(osmWidgetGet(id, "Colour", "#ffffff").toString(), "#ffffff");
        m_accentColour = colourFrom(osmWidgetGet(id, "AccentColour", "#d2a6ff").toString(), "#d2a6ff");
        addColour(v, "Text colour", m_textColour, m_textButton);
        addColour(v, "Highlight colour", m_accentColour, m_accentButton);
        QLabel *foot = label("25 July is Day Out of Time. Feb 29 is treated as 0.0 in leap years.", this);
        foot->setWordWrap(true);
        v->addWidget(foot);
        connect(m_textButton, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(m_textColour, this, "13 Moon text colour");
            if (c.isValid()) { m_textColour = c; colourButton(m_textButton, c); apply(); }
        });
        connect(m_accentButton, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(m_accentColour, this, "13 Moon highlight colour");
            if (c.isValid()) { m_accentColour = c; colourButton(m_accentButton, c); apply(); }
        });
        for (QCheckBox *c : {m_gregorian, m_details})
            connect(c, &QCheckBox::toggled, this, [this](bool) { apply(); });
        connect(m_opacity, &QSlider::sliderReleased, this, [this]() { apply(); });
    }
private:
    void addColour(QVBoxLayout *v, const QString &title, QColor &c, QPushButton *&button) {
        QHBoxLayout *row = new QHBoxLayout;
        row->addWidget(label(title, this));
        button = new QPushButton(this);
        colourButton(button, c);
        row->addWidget(button, 1);
        v->addLayout(row);
    }
    void apply() {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
        const QString g = "Widget-" + m_id + "/";
        cfg.setValue(g + "ShowGregorian", m_gregorian->isChecked());
        cfg.setValue(g + "ShowDetails", m_details->isChecked());
        cfg.setValue(g + "BackgroundAlpha", m_opacity->value());
        cfg.setValue(g + "Colour", m_textColour.name());
        cfg.setValue(g + "AccentColour", m_accentColour.name());
        cfg.sync();
    }
    QString m_id;
    QCheckBox *m_gregorian, *m_details;
    QSlider *m_opacity;
    QPushButton *m_textButton, *m_accentButton;
    QColor m_textColour, m_accentColour;
};

extern "C" {
const OsmWidgetInfo *osm_widget_info() { return &kInfo; }
QWidget *osm_widget_create(const char *instanceId, QWidget *parent) {
    return new MoonCalendarWidget(QString::fromUtf8(instanceId), parent);
}
QWidget *osm_widget_config(const char *instanceId, QWidget *parent) {
    return new MoonCalendarConfig(QString::fromUtf8(instanceId), parent);
}
}
