// tzolkin.cpp — AlternixOS Tzolkin / Harmonic Module widget
// Build: g++ -I. -std=c++11 -fPIC -shared widgets/tzolkin.cpp -o tzolkin.so \
//           $(pkg-config --cflags --libs Qt5Widgets)
//
// Grounding used for the daily kin correlation:
// - Law of Time material states that June 23, 1987 = Kin 1 (Red Magnetic Dragon)
// - July 26, 1987 = Kin 34 (White Galactic Wizard)
// - February 29 is treated as 0.0 Hunab Ku and does not advance the spin.

#include "osm-widget-plugin.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDate>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

static const OsmWidgetInfo kInfo = {
    OSM_WIDGET_ABI,
    "tzolkin",
    "Tzolkin",
    "🌀",
    "Tzolkin / Harmonic Module widget with Galactic Signature support",
    620, 760,
    320, 240
};

namespace {

enum ViewMode {
    ViewFull = 0,
    ViewWeek = 1,
    ViewToday = 2
};

static const char *kToneNames[13] = {
    "Magnetic", "Lunar", "Electric", "Self-Existing", "Overtone",
    "Rhythmic", "Resonant", "Galactic", "Solar", "Planetary",
    "Spectral", "Crystal", "Cosmic"
};

static const char *kTonePowers[13] = {
    "Purpose", "Challenge", "Service", "Form", "Radiance",
    "Equality", "Attunement", "Integrity", "Intention", "Manifestation",
    "Liberation", "Cooperation", "Presence"
};

static const char *kSealNames[20] = {
    "Dragon", "Wind", "Night", "Seed", "Serpent",
    "Worldbridger", "Hand", "Star", "Moon", "Dog",
    "Monkey", "Human", "Skywalker", "Wizard", "Eagle",
    "Warrior", "Earth", "Mirror", "Storm", "Sun"
};

static const char *kColorNames[4] = {
    "Red", "White", "Blue", "Yellow"
};

static QColor colourFrom(const QString &s, const char *fallback) {
    QColor c(s);
    return c.isValid() ? c : QColor(fallback);
}

static bool arranging() {
    QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
    return cfg.value("General/EditMode", false).toBool();
}

static int positiveMod(int value, int mod) {
    int r = value % mod;
    return r < 0 ? r + mod : r;
}

static bool isHunabKu(const QDate &d) {
    return d.month() == 2 && d.day() == 29;
}

static QDate kinAnchorDate() {
    return QDate(1987, 6, 23); // Kin 1: Red Magnetic Dragon
}

static QColor sealColour(int sealIndex) {
    switch (sealIndex % 4) {
        case 0: return QColor("#e05a5a"); // red
        case 1: return QColor("#e7e7e7"); // white
        case 2: return QColor("#7ab4f5"); // blue
        default: return QColor("#ebd252"); // yellow
    }
}

static QColor tonedCellColour(int sealIndex, int alpha = 120) {
    QColor c = sealColour(sealIndex);
    c.setAlpha(alpha);
    return c;
}

static QColor readableTextFor(const QColor &bg) {
    return bg.lightness() > 170 ? QColor(30, 30, 30) : QColor(255, 255, 255);
}

static QString kinTitle(int kin) {
    if (kin < 1 || kin > 260) return QString();
    const int seal = (kin - 1) % 20;
    const int tone = (kin - 1) % 13;
    const int colour = seal % 4;
    return QString("Kin %1 · %2 %3 %4")
        .arg(kin)
        .arg(QString::fromLatin1(kColorNames[colour]))
        .arg(QString::fromLatin1(kToneNames[tone]))
        .arg(QString::fromLatin1(kSealNames[seal]));
}

static QString kinSummary(int kin) {
    if (kin < 1 || kin > 260) return QString();
    const int seal = (kin - 1) % 20;
    const int tone = (kin - 1) % 13;
    return QString("%1 tone of %2")
        .arg(QString::fromLatin1(kToneNames[tone]))
        .arg(QString::fromLatin1(kSealNames[seal]));
}

static QString kinDetailLine(int kin) {
    if (kin < 1 || kin > 260) return QString();
    const int tone = (kin - 1) % 13;
    return QString("Tone %1 · %2")
        .arg(tone + 1)
        .arg(QString::fromLatin1(kTonePowers[tone]));
}

static QString signatureShort(int kin) {
    if (kin < 1 || kin > 260) return QString();
    const int seal = (kin - 1) % 20;
    const int tone = (kin - 1) % 13;
    const int colour = seal % 4;
    return QString("%1 %2 %3")
        .arg(QString::fromLatin1(kColorNames[colour]))
        .arg(QString::fromLatin1(kToneNames[tone]))
        .arg(QString::fromLatin1(kSealNames[seal]));
}

static int countHunabKuDays(const QDate &fromInclusive, const QDate &toExclusive) {
    if (!(fromInclusive < toExclusive)) return 0;
    int count = 0;
    for (int y = fromInclusive.year(); y <= toExclusive.year(); ++y) {
        const QDate leap(y, 2, 29);
        if (!leap.isValid()) continue;
        if (leap >= fromInclusive && leap < toExclusive) ++count;
    }
    return count;
}

static int kinForDate(const QDate &d) {
    if (!d.isValid() || isHunabKu(d)) return 0;
    const QDate anchor = kinAnchorDate();
    int offset = 0;
    if (d >= anchor) {
        offset = anchor.daysTo(d) - countHunabKuDays(anchor, d);
    } else {
        offset = -(d.daysTo(anchor) - countHunabKuDays(d, anchor));
    }
    return positiveMod(offset, 260) + 1;
}

static int daysUntilKin(int targetKin, const QDate &fromDate) {
    if (targetKin < 1 || targetKin > 260) return -1;
    if (kinForDate(fromDate) == targetKin) return 0;
    QDate d = fromDate;
    for (int step = 1; step <= 520; ++step) {
        d = d.addDays(1);
        if (kinForDate(d) == targetKin) return step;
    }
    return -1;
}

static void setPainterFont(QPainter &p, int px, bool bold = false) {
    QFont f = p.font();
    f.setFamily("Sans Serif");
    f.setPixelSize(qMax(8, px));
    f.setBold(bold);
    p.setFont(f);
}

static void paintRoundedButton(QPainter &p, const QRectF &r, const QString &text,
                               const QColor &edge, const QColor &fill, int fontPx) {
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(r, 10, 10);
    p.setPen(edge);
    setPainterFont(p, fontPx, true);
    p.drawText(r, Qt::AlignCenter, text);
}

static void drawToneGlyph(QPainter &p, const QRectF &r, int tone, const QColor &c) {
    if (tone < 1 || tone > 13) return;
    const int bars = tone / 5;
    const int dots = tone % 5;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);

    const qreal dotRadius = qMin(r.width(), r.height()) * 0.065;
    const qreal rowSpacing = dotRadius * 2.4;
    const qreal barH = qMax<qreal>(2.0, dotRadius * 1.6);
    const qreal barW = r.width() * 0.54;
    const qreal cx = r.center().x();

    int rows = (dots > 0 ? 1 : 0) + bars;
    qreal totalH = 0;
    if (dots > 0) totalH += dotRadius * 2;
    if (bars > 0) totalH += bars * barH + (bars - 1) * rowSpacing;
    if (dots > 0 && bars > 0) totalH += rowSpacing;
    qreal y = r.center().y() - totalH / 2.0;

    if (dots > 0) {
        qreal span = qMin(r.width() * 0.70, dots * dotRadius * 2.8);
        qreal startX = cx - span / 2.0;
        qreal step = dots > 1 ? span / (dots - 1) : 0;
        for (int i = 0; i < dots; ++i) {
            p.drawEllipse(QPointF(startX + i * step, y + dotRadius), dotRadius, dotRadius);
        }
        y += dotRadius * 2 + (bars > 0 ? rowSpacing : 0);
    }

    for (int i = 0; i < bars; ++i) {
        QRectF bar(cx - barW / 2.0, y, barW, barH);
        p.drawRoundedRect(bar, barH / 2.0, barH / 2.0);
        y += barH + rowSpacing;
    }
    p.restore();
}

static QLabel *makeLabel(const QString &text, QWidget *parent) {
    QLabel *l = new QLabel(text, parent);
    l->setStyleSheet("QLabel { color:white; font-size:20px; background:transparent; }");
    return l;
}

static QString boxStyle() {
    return "QComboBox { background:#303030; color:white; font-size:20px;"
           " border:2px dashed #787878; border-radius:10px; padding:8px 12px; min-height:40px; }"
           "QComboBox QAbstractItemView { background:#303030; color:white; selection-background-color:#505050; font-size:18px; }";
}

static QString sliderStyle() {
    return "QSlider::groove:horizontal { height:10px; background:#3a3a3a; border-radius:5px; }"
           "QSlider::handle:horizontal { background:#dddddd; width:28px; margin:-10px 0; border-radius:14px; }";
}

static void styleColourButton(QPushButton *button, const QColor &c) {
    button->setText(c.name().toUpper());
    button->setStyleSheet(QString(
        "QPushButton { background:#303030; color:%1; border:2px dashed %1; border-radius:10px;"
        " padding:8px 10px; min-height:42px; font-size:18px; }").arg(c.name()));
}

} // namespace

class TzolkinWidget : public QWidget {
public:
    explicit TzolkinWidget(const QString &instanceId, QWidget *parent)
        : QWidget(parent), m_id(instanceId) {
        setAttribute(Qt::WA_TranslucentBackground);
        if (!arranging()) setCursor(Qt::PointingHandCursor);

        m_textColour = colourFrom(osmWidgetGet(m_id, "Colour", "#ffffff").toString(), "#ffffff");
        m_accentColour = colourFrom(osmWidgetGet(m_id, "AccentColour", "#8fe18f").toString(), "#8fe18f");
        m_bgAlpha = qBound(0, osmWidgetGet(m_id, "BackgroundAlpha", 185).toInt(), 255);
        m_mode = qBound(0, osmWidgetGet(m_id, "ViewMode", 0).toInt(), 2);

        m_today = QDate::currentDate();
        int defaultKin = kinForDate(m_today);
        if (defaultKin < 1) defaultKin = 1;
        m_myKin = qBound(1, osmWidgetGet(m_id, "MyKin", defaultKin).toInt(), 260);
        m_selectedKin = defaultKin;
        m_focusDate = m_today;

        QTimer *timer = new QTimer(this);
        timer->setInterval(30000);
        connect(timer, &QTimer::timeout, this, [this]() {
            QDate today = QDate::currentDate();
            if (today != m_today) {
                m_today = today;
                if (m_mode == ViewToday) m_focusDate = today;
                update();
            }
        });
        timer->start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 40, 40, m_bgAlpha));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 18, 18);

        drawHeader(p);
        if (m_mode == ViewFull) drawFullView(p);
        else if (m_mode == ViewWeek) drawWeekView(p);
        else drawTodayView(p);
        drawFooter(p);
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
        const QPointF pt = e->pos();
        if (prevRect().contains(pt)) {
            browse(-1);
        } else if (nextRect().contains(pt)) {
            browse(1);
        } else if (todayRect().contains(pt)) {
            resetToToday();
        } else if (m_mode == ViewFull) {
            int kin = hitTestFullKin(pt);
            if (kin >= 1) {
                m_selectedKin = kin;
                update();
            }
        } else if (m_mode == ViewWeek) {
            int idx = hitTestWeekIndex(pt);
            if (idx >= 0) {
                QDate start = m_focusDate.addDays(-3);
                m_focusDate = start.addDays(idx);
                update();
            }
        }
        e->accept();
    }

private:
    double scaleUnit() const {
        return qBound(0.58, qMin(width() / 620.0, height() / 760.0), 2.5);
    }

    QRectF prevRect() const {
        const double u = scaleUnit();
        return QRectF(18 * u, 16 * u, 48 * u, 42 * u);
    }
    QRectF nextRect() const {
        const double u = scaleUnit();
        return QRectF(width() - 66 * u, 16 * u, 48 * u, 42 * u);
    }
    QRectF todayRect() const {
        const double u = scaleUnit();
        return QRectF(width() - 112 * u, 66 * u, 94 * u, 32 * u);
    }

    QRectF contentRect() const {
        const double u = scaleUnit();
        return QRectF(16 * u, 108 * u, width() - 32 * u, height() - 218 * u);
    }

    QRectF footerRect() const {
        const double u = scaleUnit();
        return QRectF(16 * u, height() - 104 * u, width() - 32 * u, 86 * u);
    }

    void drawHeader(QPainter &p) {
        const double u = scaleUnit();
        p.setPen(m_textColour);
        setPainterFont(p, int(26 * u), true);
        p.drawText(QRectF(80 * u, 14 * u, width() - 160 * u, 34 * u),
                   Qt::AlignCenter, "Tzolkin / Harmonic Module");
        setPainterFont(p, int(14 * u));
        QString sub = (m_mode == ViewFull) ? "Full Tzolkin" : (m_mode == ViewWeek) ? "7-day view" : "Today";
        p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 190));
        p.drawText(QRectF(16 * u, 48 * u, width() - 32 * u, 20 * u), Qt::AlignCenter, sub);

        paintRoundedButton(p, prevRect(), QString::fromUtf8("‹"), m_textColour,
                           QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 32), int(28 * u));
        paintRoundedButton(p, nextRect(), QString::fromUtf8("›"), m_textColour,
                           QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 32), int(28 * u));
        paintRoundedButton(p, todayRect(), "Today", m_accentColour,
                           QColor(m_accentColour.red(), m_accentColour.green(), m_accentColour.blue(), 36), int(14 * u));
    }

    void drawFullView(QPainter &p) {
        const double u = scaleUnit();
        const QRectF area = contentRect();
        const qreal sidebarW = qMax<qreal>(72 * u, area.width() * 0.14);
        const QRectF grid(area.x() + sidebarW, area.y() + 8 * u,
                          area.width() - sidebarW, area.height() - 8 * u);
        const qreal cellW = grid.width() / 13.0;
        const qreal cellH = grid.height() / 20.0;

        // seal sidebar
        for (int row = 0; row < 20; ++row) {
            QRectF r(area.x(), grid.y() + row * cellH + 1, sidebarW - 8 * u, cellH - 2);
            QColor bg = tonedCellColour(row, 140);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(r, 8, 8);
            p.setPen(readableTextFor(bg));
            setPainterFont(p, int(13 * u), true);
            p.drawText(QRectF(r.x() + 6 * u, r.y(), r.width() - 12 * u, r.height() * 0.55),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::number(row + 1));
            setPainterFont(p, int(11 * u));
            p.drawText(QRectF(r.x() + 6 * u, r.y() + r.height() * 0.38, r.width() - 12 * u, r.height() * 0.42),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::fromLatin1(kSealNames[row]));
        }

        for (int col = 0; col < 13; ++col) {
            const int tone = positiveMod(1 + 20 * col - 1, 13) + 1;
            p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 190));
            setPainterFont(p, int(11 * u), true);
            p.drawText(QRectF(grid.x() + col * cellW, area.y() - 2 * u, cellW, 12 * u), Qt::AlignCenter,
                       QString::number(col + 1));
            drawToneGlyph(p, QRectF(grid.x() + col * cellW + cellW * 0.18, area.y() + 10 * u,
                                    cellW * 0.64, 18 * u), tone, m_textColour);
        }

        for (int row = 0; row < 20; ++row) {
            for (int col = 0; col < 13; ++col) {
                const int kin = row + 1 + 20 * col;
                QRectF cell(grid.x() + col * cellW, grid.y() + row * cellH, cellW, cellH);
                QRectF inner = cell.adjusted(1.5, 1.5, -1.5, -1.5);
                QColor bg = tonedCellColour(row, 96);
                p.setPen(QPen(QColor(255, 255, 255, 34), 1));
                p.setBrush(bg);
                p.drawRoundedRect(inner, 5, 5);

                if (kin == m_selectedKin) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(m_accentColour.red(), m_accentColour.green(), m_accentColour.blue(), 78));
                    p.drawRoundedRect(inner.adjusted(2, 2, -2, -2), 5, 5);
                }

                const int todayKin = kinForDate(m_today);
                if (todayKin == kin) {
                    p.setPen(QPen(m_accentColour, qMax(1.4, 2.0 * u)));
                    p.setBrush(Qt::NoBrush);
                    p.drawRoundedRect(inner.adjusted(1, 1, -1, -1), 5, 5);
                }

                if (m_myKin == kin) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(m_accentColour);
                    p.drawEllipse(QRectF(inner.right() - 9 * u, inner.top() + 3 * u, 6 * u, 6 * u));
                }

                QColor text = readableTextFor(bg);
                if (kin == todayKin) text = m_accentColour;
                p.setPen(text);
                setPainterFont(p, int(12 * u), true);
                p.drawText(QRectF(inner.x(), inner.y() + 2 * u, inner.width(), inner.height() * 0.36),
                           Qt::AlignCenter, QString::number(kin));
                drawToneGlyph(p,
                              QRectF(inner.x() + inner.width() * 0.18, inner.y() + inner.height() * 0.40,
                                     inner.width() * 0.64, inner.height() * 0.45),
                              ((kin - 1) % 13) + 1, text);
            }
        }
    }

    void drawWeekView(QPainter &p) {
        const double u = scaleUnit();
        const QRectF area = contentRect();
        const QDate start = m_focusDate.addDays(-3);
        const qreal gap = 7 * u;
        const qreal rowH = (area.height() - gap * 6) / 7.0;
        for (int i = 0; i < 7; ++i) {
            const QDate d = start.addDays(i);
            const QRectF r(area.x(), area.y() + i * (rowH + gap), area.width(), rowH);
            const int kin = kinForDate(d);
            bool isToday = d == m_today;
            bool isMine = kin > 0 && kin == m_myKin;

            QColor frame = isToday ? m_accentColour : QColor(255, 255, 255, 40);
            QColor fill = QColor(255, 255, 255, 18);
            if (kin > 0) fill = tonedCellColour((kin - 1) % 20, 72);
            if (isMine) fill = QColor(m_accentColour.red(), m_accentColour.green(), m_accentColour.blue(), 42);

            p.setPen(QPen(frame, isToday ? qMax(1.5, 2.0 * u) : 1.0));
            p.setBrush(fill);
            p.drawRoundedRect(r, 12, 12);

            p.setPen(m_textColour);
            setPainterFont(p, int(16 * u), true);
            p.drawText(QRectF(r.x() + 16 * u, r.y() + 8 * u, r.width() * 0.42, r.height() * 0.34),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       d.toString("ddd d MMM"));
            setPainterFont(p, int(12 * u));
            p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 200));
            p.drawText(QRectF(r.x() + 16 * u, r.y() + r.height() * 0.46, r.width() * 0.42, r.height() * 0.28),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       isToday ? "Today" : (d == m_focusDate ? "Selected" : ""));

            if (kin == 0) {
                p.setPen(m_accentColour);
                setPainterFont(p, int(18 * u), true);
                p.drawText(QRectF(r.x() + r.width() * 0.34, r.y() + 8 * u, r.width() * 0.58, r.height() * 0.34),
                           Qt::AlignLeft | Qt::AlignVCenter, "0.0 Hunab Ku");
                p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 210));
                setPainterFont(p, int(12 * u));
                p.drawText(QRectF(r.x() + r.width() * 0.34, r.y() + r.height() * 0.47,
                                  r.width() * 0.58, r.height() * 0.28),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           "Leap Day / timeless day");
            } else {
                QColor badge = tonedCellColour((kin - 1) % 20, 220);
                p.setPen(Qt::NoPen);
                p.setBrush(badge);
                QRectF kinBadge(r.right() - 104 * u, r.y() + 10 * u, 84 * u, 26 * u);
                p.drawRoundedRect(kinBadge, 10, 10);
                p.setPen(readableTextFor(badge));
                setPainterFont(p, int(13 * u), true);
                p.drawText(kinBadge, Qt::AlignCenter, QString("Kin %1").arg(kin));

                p.setPen(m_textColour);
                setPainterFont(p, int(16 * u), true);
                p.drawText(QRectF(r.x() + r.width() * 0.34, r.y() + 8 * u, r.width() * 0.48, r.height() * 0.30),
                           Qt::AlignLeft | Qt::AlignVCenter, signatureShort(kin));
                p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 210));
                setPainterFont(p, int(12 * u));
                p.drawText(QRectF(r.x() + r.width() * 0.34, r.y() + r.height() * 0.41, r.width() * 0.48, r.height() * 0.24),
                           Qt::AlignLeft | Qt::AlignVCenter, kinDetailLine(kin));
                p.drawText(QRectF(r.x() + r.width() * 0.34, r.y() + r.height() * 0.65, r.width() * 0.48, r.height() * 0.18),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           isMine ? "✦ Your Galactic Signature" : "");
            }
        }
    }

    void drawTodayView(QPainter &p) {
        const double u = scaleUnit();
        const QRectF area = contentRect();
        const int kin = kinForDate(m_focusDate);

        p.setPen(QPen(QColor(255, 255, 255, 40), 1));
        p.setBrush(QColor(255, 255, 255, 18));
        p.drawRoundedRect(area, 16, 16);

        p.setPen(m_textColour);
        setPainterFont(p, int(18 * u), true);
        p.drawText(QRectF(area.x(), area.y() + 18 * u, area.width(), 28 * u), Qt::AlignCenter,
                   m_focusDate.toString("dddd d MMMM yyyy"));

        if (kin == 0) {
            p.setPen(m_accentColour);
            setPainterFont(p, int(40 * u), true);
            p.drawText(QRectF(area.x(), area.y() + 80 * u, area.width(), 56 * u), Qt::AlignCenter, "0.0 Hunab Ku");
            p.setPen(m_textColour);
            setPainterFont(p, int(16 * u));
            p.drawText(QRectF(area.x() + 20 * u, area.y() + 150 * u, area.width() - 40 * u, 80 * u),
                       Qt::AlignCenter | Qt::TextWordWrap,
                       "Leap Day is treated as a timeless day outside the 260-kin spin.");
            return;
        }

        QColor sig = tonedCellColour((kin - 1) % 20, 170);
        QRectF badge(area.center().x() - 96 * u, area.y() + 68 * u, 192 * u, 48 * u);
        p.setPen(Qt::NoPen);
        p.setBrush(sig);
        p.drawRoundedRect(badge, 16, 16);
        p.setPen(readableTextFor(sig));
        setPainterFont(p, int(24 * u), true);
        p.drawText(badge, Qt::AlignCenter, QString("Kin %1").arg(kin));

        p.setPen(m_textColour);
        setPainterFont(p, int(22 * u), true);
        p.drawText(QRectF(area.x() + 20 * u, area.y() + 140 * u, area.width() - 40 * u, 34 * u),
                   Qt::AlignCenter, signatureShort(kin));
        setPainterFont(p, int(16 * u));
        p.drawText(QRectF(area.x() + 20 * u, area.y() + 186 * u, area.width() - 40 * u, 28 * u),
                   Qt::AlignCenter, kinDetailLine(kin));
        drawToneGlyph(p, QRectF(area.center().x() - 40 * u, area.y() + 238 * u, 80 * u, 44 * u),
                      ((kin - 1) % 13) + 1, m_accentColour);
        p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 225));
        setPainterFont(p, int(14 * u));
        p.drawText(QRectF(area.x() + 24 * u, area.y() + 300 * u, area.width() - 48 * u, 58 * u),
                   Qt::AlignCenter | Qt::TextWordWrap,
                   kin == m_myKin ? "✦ This is your Galactic Signature today." :
                                    QString("Your Galactic Signature: Kin %1 · %2")
                                        .arg(m_myKin).arg(signatureShort(m_myKin)));
    }

    void drawFooter(QPainter &p) {
        const double u = scaleUnit();
        QRectF r = footerRect();
        p.setPen(QPen(QColor(255, 255, 255, 38), 1));
        p.setBrush(QColor(255, 255, 255, 14));
        p.drawRoundedRect(r, 14, 14);

        const int todayKin = kinForDate(m_today);
        QString line1;
        QString line2;
        if (m_mode == ViewFull) {
            line1 = kinTitle(m_selectedKin);
            if (m_selectedKin == todayKin && todayKin > 0) line2 = "Today in the spin";
            else if (m_selectedKin == m_myKin) line2 = "Your Galactic Signature";
            else line2 = kinDetailLine(m_selectedKin);
        } else {
            const int kin = kinForDate(m_focusDate);
            if (kin == 0) {
                line1 = "0.0 Hunab Ku";
                line2 = "Leap day / timeless day outside the normal count";
            } else {
                line1 = kinTitle(kin);
                line2 = kinDetailLine(kin);
            }
        }

        QString signatureState;
        if (todayKin == m_myKin && todayKin > 0) {
            signatureState = "Today is your Galactic Signature.";
        } else {
            int wait = daysUntilKin(m_myKin, m_today);
            if (wait == 1) signatureState = "Your Galactic Signature returns tomorrow.";
            else if (wait > 1) signatureState = QString("Your Galactic Signature returns in %1 days.").arg(wait);
            else signatureState = QString("Your Galactic Signature: Kin %1").arg(m_myKin);
        }

        p.setPen(m_textColour);
        setPainterFont(p, int(15 * u), true);
        p.drawText(QRectF(r.x() + 16 * u, r.y() + 10 * u, r.width() - 32 * u, 22 * u),
                   Qt::AlignLeft | Qt::AlignVCenter, line1);
        p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 210));
        setPainterFont(p, int(12 * u));
        p.drawText(QRectF(r.x() + 16 * u, r.y() + 36 * u, r.width() - 32 * u, 18 * u),
                   Qt::AlignLeft | Qt::AlignVCenter, line2);
        p.setPen(m_accentColour);
        setPainterFont(p, int(12 * u), true);
        p.drawText(QRectF(r.x() + 16 * u, r.y() + 58 * u, r.width() - 32 * u, 18 * u),
                   Qt::AlignLeft | Qt::AlignVCenter, signatureState);
    }

    void browse(int direction) {
        if (m_mode == ViewFull) {
            m_selectedKin += direction;
            if (m_selectedKin < 1) m_selectedKin = 260;
            if (m_selectedKin > 260) m_selectedKin = 1;
        } else if (m_mode == ViewWeek) {
            m_focusDate = m_focusDate.addDays(direction * 7);
        } else {
            m_focusDate = m_focusDate.addDays(direction);
        }
        update();
    }

    void resetToToday() {
        m_today = QDate::currentDate();
        m_focusDate = m_today;
        int todayKin = kinForDate(m_today);
        if (todayKin > 0) m_selectedKin = todayKin;
        update();
    }

    int hitTestFullKin(const QPointF &pt) const {
        const double u = scaleUnit();
        const QRectF area = contentRect();
        const qreal sidebarW = qMax<qreal>(72 * u, area.width() * 0.14);
        const QRectF grid(area.x() + sidebarW, area.y() + 8 * u,
                          area.width() - sidebarW, area.height() - 8 * u);
        if (!grid.contains(pt)) return -1;
        const qreal cellW = grid.width() / 13.0;
        const qreal cellH = grid.height() / 20.0;
        int col = qBound(0, int((pt.x() - grid.x()) / cellW), 12);
        int row = qBound(0, int((pt.y() - grid.y()) / cellH), 19);
        return row + 1 + 20 * col;
    }

    int hitTestWeekIndex(const QPointF &pt) const {
        const double u = scaleUnit();
        const QRectF area = contentRect();
        const qreal gap = 7 * u;
        const qreal rowH = (area.height() - gap * 6) / 7.0;
        for (int i = 0; i < 7; ++i) {
            QRectF r(area.x(), area.y() + i * (rowH + gap), area.width(), rowH);
            if (r.contains(pt)) return i;
        }
        return -1;
    }

    QString m_id;
    QColor m_textColour, m_accentColour;
    int m_bgAlpha = 185;
    int m_mode = ViewFull;
    int m_myKin = 1;
    int m_selectedKin = 1;
    QDate m_today;
    QDate m_focusDate;
    bool m_pressed = false;
    QPoint m_pressPos;
};

class TzolkinConfig : public QWidget {
public:
    explicit TzolkinConfig(const QString &instanceId, QWidget *parent)
        : QWidget(parent), m_id(instanceId) {
        setStyleSheet("background:#282828;");

        QVBoxLayout *v = new QVBoxLayout(this);
        v->setContentsMargins(24, 24, 24, 24);
        v->setSpacing(18);
        v->setAlignment(Qt::AlignTop);

        QLabel *intro = makeLabel("Tzolkin / Harmonic Module with Galactic Signature support", this);
        intro->setWordWrap(true);
        v->addWidget(intro);

        v->addWidget(makeLabel("View mode", this));
        m_mode = new QComboBox(this);
        m_mode->setStyleSheet(boxStyle());
        m_mode->addItem("Full Tzolkin", ViewFull);
        m_mode->addItem("Week view", ViewWeek);
        m_mode->addItem("Today only", ViewToday);
        selectByData(m_mode, osmWidgetGet(m_id, "ViewMode", 0).toInt());
        v->addWidget(m_mode);

        v->addWidget(makeLabel("My Galactic Signature", this));
        m_myKin = new QComboBox(this);
        m_myKin->setStyleSheet(boxStyle());
        m_myKin->setMaxVisibleItems(18);
        for (int kin = 1; kin <= 260; ++kin)
            m_myKin->addItem(kinTitle(kin), kin);
        int defaultKin = kinForDate(QDate::currentDate());
        if (defaultKin < 1) defaultKin = 1;
        selectByData(m_myKin, osmWidgetGet(m_id, "MyKin", defaultKin).toInt());
        v->addWidget(m_myKin);

        m_preview = makeLabel(QString(), this);
        m_preview->setWordWrap(true);
        m_preview->setStyleSheet("QLabel { color:#d8ffd8; font-size:18px; background:transparent; }");
        v->addWidget(m_preview);
        refreshPreview();

        v->addWidget(makeLabel("Background opacity", this));
        m_opacity = new QSlider(Qt::Horizontal, this);
        m_opacity->setStyleSheet(sliderStyle());
        m_opacity->setRange(0, 255);
        m_opacity->setValue(osmWidgetGet(m_id, "BackgroundAlpha", 185).toInt());
        v->addWidget(m_opacity);

        m_textColour = colourFrom(osmWidgetGet(m_id, "Colour", "#ffffff").toString(), "#ffffff");
        m_accentColour = colourFrom(osmWidgetGet(m_id, "AccentColour", "#8fe18f").toString(), "#8fe18f");
        addColourRow(v, "Text colour", m_textColour, m_textButton);
        addColourRow(v, "Highlight colour", m_accentColour, m_accentButton);

        QLabel *note = makeLabel("Leap day (29 February) is treated as 0.0 Hunab Ku and stays outside the 260-kin spin.", this);
        note->setWordWrap(true);
        v->addWidget(note);

        v->addStretch();

        connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { apply(); });
        connect(m_myKin, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            refreshPreview();
            apply();
        });
        connect(m_opacity, &QSlider::sliderReleased, this, [this]() { apply(); });
        connect(m_textButton, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(m_textColour, this, "Tzolkin text colour");
            if (c.isValid()) { m_textColour = c; styleColourButton(m_textButton, c); apply(); }
        });
        connect(m_accentButton, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(m_accentColour, this, "Tzolkin highlight colour");
            if (c.isValid()) { m_accentColour = c; styleColourButton(m_accentButton, c); apply(); }
        });
    }

private:
    void addColourRow(QVBoxLayout *v, const QString &title, QColor &c, QPushButton *&button) {
        QHBoxLayout *row = new QHBoxLayout;
        row->addWidget(makeLabel(title, this));
        button = new QPushButton(this);
        styleColourButton(button, c);
        row->addWidget(button, 1);
        v->addLayout(row);
    }

    static void selectByData(QComboBox *box, int data) {
        int idx = box->findData(data);
        if (idx >= 0) box->setCurrentIndex(idx);
    }

    void refreshPreview() {
        int kin = m_myKin->currentData().toInt();
        m_preview->setText(QString("Current selection: %1\n%2")
                           .arg(signatureShort(kin))
                           .arg(kinDetailLine(kin)));
    }

    void apply() {
        osmWidgetSet(m_id, "ViewMode", m_mode->currentData().toInt());
        osmWidgetSet(m_id, "MyKin", m_myKin->currentData().toInt());
        osmWidgetSet(m_id, "BackgroundAlpha", m_opacity->value());
        osmWidgetSet(m_id, "Colour", m_textColour.name());
        osmWidgetSet(m_id, "AccentColour", m_accentColour.name());
    }

    QString m_id;
    QComboBox *m_mode = nullptr;
    QComboBox *m_myKin = nullptr;
    QLabel *m_preview = nullptr;
    QSlider *m_opacity = nullptr;
    QPushButton *m_textButton = nullptr;
    QPushButton *m_accentButton = nullptr;
    QColor m_textColour;
    QColor m_accentColour;
};

extern "C" {
const OsmWidgetInfo *osm_widget_info() { return &kInfo; }
QWidget *osm_widget_create(const char *instanceId, QWidget *parent) {
    return new TzolkinWidget(QString::fromUtf8(instanceId), parent);
}
QWidget *osm_widget_config(const char *instanceId, QWidget *parent) {
    return new TzolkinConfig(QString::fromUtf8(instanceId), parent);
}
}
