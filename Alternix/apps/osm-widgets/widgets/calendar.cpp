// calendar.cpp — AlternixOS Gregorian calendar widget (Qt5 / osm-widgets ABI 1)
// Build: g++ -std=c++11 -fPIC -shared calendar.cpp -o calendar.so \
//           $(pkg-config --cflags --libs Qt5Widgets)

#include "osm-widget-plugin.h"
#include <QApplication>
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
    OSM_WIDGET_ABI, "calendar", "Calendar", "📅",
    "Gregorian monthly calendar with date navigation",
    420, 420, 280, 280
};

namespace {
static QColor chosenColour(const QString &value, const char *fallback) {
    QColor c(value);
    return c.isValid() ? c : QColor(fallback);
}
static bool arranging() {
    QSettings s(osmWidgetConfigPath(), QSettings::IniFormat);
    return s.value("General/EditMode", false).toBool();
}
static void setPainterFont(QPainter &p, int pixels, bool bold = false) {
    QFont f = p.font();
    f.setFamily("Sans Serif");
    f.setPixelSize(qMax(8, pixels));
    f.setBold(bold);
    p.setFont(f);
}
static void colourButton(QPushButton *button, const QColor &colour) {
    button->setText(colour.name().toUpper());
    button->setStyleSheet(QString(
        "QPushButton { background:#303030; color:%1; border:2px dashed %1;"
        "border-radius:10px; padding:10px; font-size:20px; min-height:42px; }")
        .arg(colour.name()));
}
static QLabel *settingLabel(const QString &name, QWidget *parent) {
    QLabel *label = new QLabel(name, parent);
    label->setStyleSheet("color:white; background:transparent; font-size:20px;");
    return label;
}
static QString sliderCSS() {
    return "QSlider::groove:horizontal { height:10px; background:#3a3a3a; border-radius:5px; }"
           "QSlider::handle:horizontal { background:#ddd; width:30px; margin:-10px 0; border-radius:15px; }";
}
} // namespace

class CalendarWidget : public QWidget {
public:
    explicit CalendarWidget(const QString &id, QWidget *parent)
        : QWidget(parent), m_id(id), m_month(QDate::currentDate().year(), QDate::currentDate().month(), 1),
          m_selected(QDate::currentDate()) {
        setAttribute(Qt::WA_TranslucentBackground);
        if (!arranging()) setCursor(Qt::PointingHandCursor);
        m_colour = chosenColour(osmWidgetGet(id, "Colour", "#ffffff").toString(), "#ffffff");
        m_accent = chosenColour(osmWidgetGet(id, "AccentColour", "#67c9f5").toString(), "#67c9f5");
        m_bgAlpha = qBound(0, osmWidgetGet(id, "BackgroundAlpha", 170).toInt(), 255);
        m_mondayFirst = osmWidgetGet(id, "MondayFirst", true).toBool();
        m_showAdjacent = osmWidgetGet(id, "ShowAdjacent", true).toBool();
        m_showDetails = osmWidgetGet(id, "ShowDetails", true).toBool();
        QTimer *timer = new QTimer(this);
        timer->setInterval(30000);
        connect(timer, &QTimer::timeout, this, [this]() {
            const QDate today = QDate::currentDate();
            if (today != m_today) {
                if (m_selected == m_today) {
                    m_selected = today;
                    m_month = QDate(today.year(), today.month(), 1);
                }
                m_today = today;
                update();
            }
        });
        m_today = QDate::currentDate();
        timer->start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF all = rect();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 40, 40, m_bgAlpha));
        p.drawRoundedRect(all.adjusted(1, 1, -1, -1), 17, 17);
        const double unit = qBound(0.7, qMin(width() / 420.0, height() / 420.0), 2.6);
        const qreal pad = 15 * unit;
        const qreal top = 14 * unit;
        const qreal headerH = 51 * unit;
        const qreal weekTop = top + headerH + 12 * unit;
        const qreal weekH = 28 * unit;
        const qreal footerH = 66 * unit;
        const qreal gridTop = weekTop + weekH + 3 * unit;
        const qreal gridBottom = height() - footerH - 8 * unit;
        const qreal cellW = (width() - 2 * pad) / 7.0;
        const qreal cellH = qMax(1.0, (gridBottom - gridTop) / 6.0);

        // Header month name, previous / next navigation.
        p.setPen(m_colour);
        setPainterFont(p, int(24 * unit), true);
        const QString title = QLocale::system().monthName(m_month.month(), QLocale::LongFormat)
                            + " " + QString::number(m_month.year());
        p.drawText(QRectF(pad + 46 * unit, top, width() - 2 * pad - 92 * unit, headerH),
                   Qt::AlignCenter, title);
        drawNav(p, prevRect(), QString::fromUtf8("‹"), unit);
        drawNav(p, nextRect(), QString::fromUtf8("›"), unit);

        const QStringList weekdayNames = m_mondayFirst
            ? QStringList({"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"})
            : QStringList({"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"});
        for (int col = 0; col < 7; ++col) {
            const int dow = m_mondayFirst ? col + 1 : (col == 0 ? 7 : col);
            p.setPen(dow >= 6 ? m_accent : QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 180));
            setPainterFont(p, int(14 * unit), true);
            p.drawText(QRectF(pad + col * cellW, weekTop, cellW, weekH),
                       Qt::AlignCenter, weekdayNames.at(col));
        }

        const QDate first = startOfGrid();
        for (int i = 0; i < 42; ++i) {
            const QDate d = first.addDays(i);
            const QRectF cell(pad + (i % 7) * cellW, gridTop + (i / 7) * cellH,
                              cellW, cellH);
            const bool currentMonth = d.month() == m_month.month() && d.year() == m_month.year();
            if (!currentMonth && !m_showAdjacent) continue;
            QRectF inner = cell.adjusted(2 * unit, 2 * unit, -2 * unit, -2 * unit);
            if (d == m_selected) {
                p.setPen(Qt::NoPen);
                p.setBrush(QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 90));
                p.drawRoundedRect(inner, 9 * unit, 9 * unit);
            }
            if (d == m_today) {
                QPen ring(m_accent, qMax(1.4, 2.0 * unit));
                p.setPen(ring);
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(inner.adjusted(1, 1, -1, -1), 9 * unit, 9 * unit);
            }
            p.setPen(!currentMonth
                ? QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 95)
                : (d == m_today ? m_accent : m_colour));
            setPainterFont(p, int(19 * unit), d == m_today || d == m_selected);
            p.drawText(cell, Qt::AlignCenter, QString::number(d.day()));
        }

        const QRectF footer(pad, height() - footerH, width() - 2 * pad, footerH - 6 * unit);
        p.setPen(QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 85));
        p.drawLine(QPointF(pad, footer.top()), QPointF(width() - pad, footer.top()));
        if (m_showDetails) {
            p.setPen(m_colour);
            setPainterFont(p, int(15 * unit), true);
            p.drawText(QRectF(pad + 5 * unit, footer.top() + 5 * unit,
                              footer.width() - 84 * unit, footer.height() - 4 * unit),
                       Qt::AlignVCenter | Qt::AlignLeft,
                       QLocale::system().toString(m_selected, "ddd d MMM yyyy"));
        }
        drawToday(p, unit);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (arranging() || e->button() != Qt::LeftButton) { e->ignore(); return; }
        m_pressed = true;
        m_pressPos = e->pos();
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent *e) override {
        if (arranging() || !m_pressed || e->button() != Qt::LeftButton) {
            m_pressed = false;
            e->ignore();
            return;
        }
        m_pressed = false;
        if ((e->pos() - m_pressPos).manhattanLength() > 12) { e->accept(); return; }
        const QPointF pos = e->pos();
        if (prevRect().contains(pos)) shiftMonth(-1);
        else if (nextRect().contains(pos)) shiftMonth(1);
        else if (todayRect().contains(pos)) {
            m_selected = QDate::currentDate();
            m_month = QDate(m_selected.year(), m_selected.month(), 1);
            update();
        } else {
            const double u = qBound(0.7, qMin(width() / 420.0, height() / 420.0), 2.6);
            const qreal pad = 15 * u;
            const qreal gridTop = (14 + 51 + 12 + 28 + 3) * u;
            const qreal gridBottom = height() - (66 + 8) * u;
            const qreal cellW = (width() - 2 * pad) / 7.0;
            const qreal cellH = (gridBottom - gridTop) / 6.0;
            if (pos.x() >= pad && pos.x() < width() - pad &&
                pos.y() >= gridTop && pos.y() < gridBottom && cellH > 0) {
                const int col = qBound(0, int((pos.x() - pad) / cellW), 6);
                const int row = qBound(0, int((pos.y() - gridTop) / cellH), 5);
                const QDate d = startOfGrid().addDays(row * 7 + col);
                if (m_showAdjacent || d.month() == m_month.month()) {
                    m_selected = d;
                    m_month = QDate(d.year(), d.month(), 1);
                    update();
                }
            }
        }
        e->accept();
    }

private:
    QDate startOfGrid() const {
        const int dow = m_month.dayOfWeek(); // Monday = 1, Sunday = 7
        return m_month.addDays(- (m_mondayFirst ? (dow - 1) : (dow % 7)));
    }
    void shiftMonth(int direction) {
        const QDate shifted = m_month.addMonths(direction);
        if (shifted.isValid()) { m_month = shifted; update(); }
    }
    QRectF prevRect() const {
        const double u = qBound(0.7, qMin(width() / 420.0, height() / 420.0), 2.6);
        return QRectF(15 * u, 14 * u, 46 * u, 51 * u);
    }
    QRectF nextRect() const {
        const double u = qBound(0.7, qMin(width() / 420.0, height() / 420.0), 2.6);
        return QRectF(width() - 61 * u, 14 * u, 46 * u, 51 * u);
    }
    QRectF todayRect() const {
        const double u = qBound(0.7, qMin(width() / 420.0, height() / 420.0), 2.6);
        return QRectF(width() - 95 * u, height() - 61 * u, 80 * u, 43 * u);
    }
    void drawNav(QPainter &p, QRectF r, const QString &glyph, double u) {
        p.setBrush(QColor(m_colour.red(), m_colour.green(), m_colour.blue(), 22));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(r, 11 * u, 11 * u);
        p.setPen(m_colour);
        setPainterFont(p, int(32 * u));
        p.drawText(r, Qt::AlignCenter, glyph);
    }
    void drawToday(QPainter &p, double u) {
        const QRectF r = todayRect();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(m_accent.red(), m_accent.green(), m_accent.blue(), 54));
        p.drawRoundedRect(r, 11 * u, 11 * u);
        p.setPen(m_accent);
        setPainterFont(p, int(16 * u), true);
        p.drawText(r, Qt::AlignCenter, "Today");
    }

    QString m_id;
    QDate m_month, m_selected, m_today;
    QColor m_colour, m_accent;
    int m_bgAlpha = 170;
    bool m_mondayFirst = true, m_showAdjacent = true, m_showDetails = true;
    bool m_pressed = false;
    QPoint m_pressPos;
};

class CalendarConfig : public QWidget {
public:
    explicit CalendarConfig(const QString &id, QWidget *parent) : QWidget(parent), m_id(id) {
        setStyleSheet("background:#282828;");
        QVBoxLayout *v = new QVBoxLayout(this);
        v->setContentsMargins(24, 24, 24, 24);
        v->setSpacing(19);
        v->setAlignment(Qt::AlignTop);
        m_monday = new QCheckBox("Week starts on Monday", this);
        m_adjacent = new QCheckBox("Show adjacent-month days", this);
        m_details = new QCheckBox("Show selected date", this);
        for (QCheckBox *c : {m_monday, m_adjacent, m_details}) {
            c->setStyleSheet("QCheckBox { color:white; font-size:21px; spacing:12px; }"
                             "QCheckBox::indicator { width:30px; height:30px; }");
            v->addWidget(c);
        }
        m_monday->setChecked(osmWidgetGet(id, "MondayFirst", true).toBool());
        m_adjacent->setChecked(osmWidgetGet(id, "ShowAdjacent", true).toBool());
        m_details->setChecked(osmWidgetGet(id, "ShowDetails", true).toBool());
        v->addWidget(settingLabel("Background opacity", this));
        m_opacity = new QSlider(Qt::Horizontal, this);
        m_opacity->setRange(0, 255);
        m_opacity->setValue(osmWidgetGet(id, "BackgroundAlpha", 170).toInt());
        m_opacity->setStyleSheet(sliderCSS());
        v->addWidget(m_opacity);
        m_textColour = chosenColour(osmWidgetGet(id, "Colour", "#ffffff").toString(), "#ffffff");
        m_accentColour = chosenColour(osmWidgetGet(id, "AccentColour", "#67c9f5").toString(), "#67c9f5");
        addColour(v, "Text colour", m_textColour, m_textButton);
        addColour(v, "Today / selection colour", m_accentColour, m_accentButton);
        connect(m_textButton, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(m_textColour, this, "Calendar text colour");
            if (c.isValid()) { m_textColour = c; colourButton(m_textButton, c); apply(); }
        });
        connect(m_accentButton, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(m_accentColour, this, "Calendar highlight colour");
            if (c.isValid()) { m_accentColour = c; colourButton(m_accentButton, c); apply(); }
        });
        for (QCheckBox *c : {m_monday, m_adjacent, m_details})
            connect(c, &QCheckBox::toggled, this, [this](bool) { apply(); });
        connect(m_opacity, &QSlider::sliderReleased, this, [this]() { apply(); });
    }
private:
    void addColour(QVBoxLayout *v, const QString &title, QColor &colour, QPushButton *&button) {
        QHBoxLayout *row = new QHBoxLayout;
        row->addWidget(settingLabel(title, this));
        button = new QPushButton(this);
        colourButton(button, colour);
        row->addWidget(button, 1);
        v->addLayout(row);
    }
    void apply() {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
        const QString g = "Widget-" + m_id + "/";
        cfg.setValue(g + "MondayFirst", m_monday->isChecked());
        cfg.setValue(g + "ShowAdjacent", m_adjacent->isChecked());
        cfg.setValue(g + "ShowDetails", m_details->isChecked());
        cfg.setValue(g + "BackgroundAlpha", m_opacity->value());
        cfg.setValue(g + "Colour", m_textColour.name());
        cfg.setValue(g + "AccentColour", m_accentColour.name());
        cfg.sync();
    }
    QString m_id;
    QCheckBox *m_monday, *m_adjacent, *m_details;
    QSlider *m_opacity;
    QPushButton *m_textButton, *m_accentButton;
    QColor m_textColour, m_accentColour;
};

extern "C" {
const OsmWidgetInfo *osm_widget_info() { return &kInfo; }
QWidget *osm_widget_create(const char *instanceId, QWidget *parent) {
    return new CalendarWidget(QString::fromUtf8(instanceId), parent);
}
QWidget *osm_widget_config(const char *instanceId, QWidget *parent) {
    return new CalendarConfig(QString::fromUtf8(instanceId), parent);
}
}
