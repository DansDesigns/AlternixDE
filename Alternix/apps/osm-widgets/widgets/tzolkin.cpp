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
#include <QDateEdit>
#include <QImage>
#include <QByteArray>
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

// Foundation for the Law of Time: Feb 29 birth before noon uses Feb 28;
// after noon uses Mar 1. The selected actual birth date is never altered.
static int kinForBirthDate(const QDate &birthDate, bool leapAfterNoon) {
    if (!birthDate.isValid()) return 0;
    QDate effective = birthDate;
    if (isHunabKu(birthDate))
        effective = birthDate.addDays(leapAfterNoon ? 1 : -1);
    return kinForDate(effective);
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

// Self-contained atlas of the 20 solar-seal symbols, in seal order (Dragon–Sun).
// Cropped from the Tzolkin reference image supplied with this request.
// No separate icon files are required at installation or runtime.
static const char kSealAtlasPngBase64[] =
    "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";

static const QImage &sealAtlas() {
    static const QImage atlas = QImage::fromData(
        QByteArray::fromBase64(QByteArray(kSealAtlasPngBase64)), "PNG");
    return atlas;
}

// Places the Maya dot-and-bar tone ABOVE the corresponding solar-seal symbol.
// Both are determined by the Kin; neither is guessed from the date's month.
static void drawKinEmblem(QPainter &p, const QRectF &r, int kin,
                          const QColor &toneColour, bool showKinNumber = false) {
    if (kin < 1 || kin > 260 || r.width() <= 0 || r.height() <= 0) return;
    const int sealIndex = (kin - 1) % 20;
    const int tone = (kin - 1) % 13 + 1;
    const qreal pad = qMax<qreal>(2, qMin(r.width(), r.height()) * 0.035);
    const qreal toneH = r.height() * 0.24;
    const qreal labelH = showKinNumber ? r.height() * 0.13 : 0;
    const qreal sealH = qMax<qreal>(8, r.height() - toneH - labelH - 3 * pad);
    const qreal sealSize = qMin(r.width() - 2 * pad, sealH);
    const QRectF toneRect(r.center().x() - r.width() * 0.28,
                          r.top() + pad, r.width() * 0.56, toneH - pad);
    drawToneGlyph(p, toneRect, tone, toneColour);
    const QRectF imageRect(r.center().x() - sealSize / 2,
                            r.top() + toneH + pad, sealSize, sealSize);
    const QImage &atlas = sealAtlas();
    if (!atlas.isNull()) {
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(imageRect, atlas, QRectF(0, sealIndex * 28, 28, 28));
        p.restore();
    }
    if (showKinNumber) {
        p.setPen(toneColour);
        setPainterFont(p, int(qBound<qreal>(10, labelH * 0.75, 22)), true);
        p.drawText(QRectF(r.x(), r.bottom() - labelH - pad,
                          r.width(), labelH), Qt::AlignCenter,
                   QString("Kin %1").arg(kin));
    }
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

// Drawn directly in settings: automatically follows the selected birth date.
class KinPreview : public QWidget {
public:
    explicit KinPreview(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(162);
    }
    void setKin(int kin) { m_kin = kin; update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#323232"));
        p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 13, 13);
        if (m_kin > 0)
            drawKinEmblem(p, QRectF(rect()).adjusted(12, 5, -12, -5), m_kin,
                          QColor("#e7e7e7"), true);
        else {
            p.setPen(QColor("#bababa"));
            setPainterFont(p, 15);
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                       "Choose your birth date");
        }
    }
private:
    int m_kin = 0;
};

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
        const QDate savedBirthDate = QDate::fromString(
            osmWidgetGet(m_id, "BirthDate", "").toString(), Qt::ISODate);
        const bool leapAfterNoon = osmWidgetGet(m_id, "BirthLeapAfterNoon", false).toBool();
        // Existing manual selections still work when no birth date is configured.
        m_myKin = savedBirthDate.isValid()
            ? kinForBirthDate(savedBirthDate, leapAfterNoon)
            : qBound(0, osmWidgetGet(m_id, "MyKin", 0).toInt(), 260);
        m_selectedKin = kinForDate(m_today);
        if (m_selectedKin < 1) m_selectedKin = (m_myKin > 0 ? m_myKin : 1);
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

                if (m_myKin > 0 && m_myKin == kin) {
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
                drawKinEmblem(p, QRectF(r.right() - 72 * u, r.y() + 3 * u,
                                        62 * u, r.height() - 6 * u),
                              kin, m_textColour, false);
                p.setPen(m_textColour);
                setPainterFont(p, int(16 * u), true);
                p.drawText(QRectF(r.x() + r.width() * 0.32, r.y() + 8 * u, r.width() * 0.48, r.height() * 0.30),
                           Qt::AlignLeft | Qt::AlignVCenter, signatureShort(kin));
                p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 210));
                setPainterFont(p, int(12 * u));
                p.drawText(QRectF(r.x() + r.width() * 0.32, r.y() + r.height() * 0.41, r.width() * 0.48, r.height() * 0.24),
                           Qt::AlignLeft | Qt::AlignVCenter, kinDetailLine(kin));
                p.drawText(QRectF(r.x() + r.width() * 0.32, r.y() + r.height() * 0.65, r.width() * 0.48, r.height() * 0.18),
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
        drawKinEmblem(p, QRectF(area.center().x() - 77 * u, area.y() + 234 * u,
                                154 * u, 161 * u), kin, m_accentColour, false);
        p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 225));
        setPainterFont(p, int(14 * u));
        p.drawText(QRectF(area.x() + 24 * u, area.y() + 404 * u, area.width() - 48 * u, 66 * u),
                   Qt::AlignCenter | Qt::TextWordWrap,
                   m_myKin == 0 ? "Set your Gregorian birth date in widget settings." :
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
            else if (m_myKin > 0 && m_selectedKin == m_myKin) line2 = "Your Galactic Signature";
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
        if (m_myKin == 0) {
            signatureState = "Set your birth date in Tzolkin widget settings.";
        } else if (todayKin == m_myKin && todayKin > 0) {
            signatureState = "Today is your Galactic Signature.";
        } else {
            int wait = daysUntilKin(m_myKin, m_today);
            if (wait == 1) signatureState = "Your Galactic Signature returns tomorrow.";
            else if (wait > 1) signatureState = QString("Your Galactic Signature returns in %1 days.").arg(wait);
            else signatureState = QString("Your Galactic Signature: Kin %1").arg(m_myKin);
        }

        const bool signatureHere = m_myKin > 0 &&
            (kinForDate(m_today) == m_myKin ||
             (m_mode == ViewFull && m_selectedKin == m_myKin) ||
             (m_mode != ViewFull && kinForDate(m_focusDate) == m_myKin));
        const qreal emblemW = signatureHere ? qMin<qreal>(64 * u, r.height() * 0.8) : 0;
        if (signatureHere)
            drawKinEmblem(p, QRectF(r.right() - emblemW - 10 * u, r.y() + 5 * u,
                                    emblemW, r.height() - 10 * u), m_myKin,
                          m_accentColour);
        const qreal textWidth = r.width() - 32 * u - emblemW;
        p.setPen(m_textColour);
        setPainterFont(p, int(15 * u), true);
        p.drawText(QRectF(r.x() + 16 * u, r.y() + 10 * u, textWidth, 22 * u),
                   Qt::AlignLeft | Qt::AlignVCenter, line1);
        p.setPen(QColor(m_textColour.red(), m_textColour.green(), m_textColour.blue(), 210));
        setPainterFont(p, int(12 * u));
        p.drawText(QRectF(r.x() + 16 * u, r.y() + 36 * u, textWidth, 18 * u),
                   Qt::AlignLeft | Qt::AlignVCenter, line2);
        p.setPen(m_accentColour);
        setPainterFont(p, int(12 * u), true);
        p.drawText(QRectF(r.x() + 16 * u, r.y() + 58 * u, textWidth, 18 * u),
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
    int m_myKin = 0;
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
        v->setSpacing(17);
        v->setAlignment(Qt::AlignTop);

        QLabel *intro = makeLabel("Tzolkin / Harmonic Module", this);
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

        v->addWidget(makeLabel("My Galactic Signature – Gregorian birth date", this));
        const QDate savedBirthDate = QDate::fromString(
            osmWidgetGet(m_id, "BirthDate", "").toString(), Qt::ISODate);
        m_savedBirthDate = savedBirthDate;
        m_birthDate = new QDateEdit(this);
        m_birthDate->setDisplayFormat("dd/MM/yyyy");
        m_birthDate->setCalendarPopup(true);
        m_birthDate->setMinimumDate(QDate(1700, 1, 1));
        m_birthDate->setMaximumDate(QDate::currentDate());
        m_birthDate->setDate(savedBirthDate.isValid() ? savedBirthDate : QDate(2000, 1, 1));
        m_birthDate->setStyleSheet(
            "QDateEdit { background:#303030; color:white; font-size:22px;"
            " border:2px dashed #787878; border-radius:10px; padding:10px; min-height:42px; }"
            "QCalendarWidget { background:#282828; color:white; }"
            "QCalendarWidget QWidget { background:#303030; color:white; }");
        v->addWidget(m_birthDate);

        m_leapChoice = new QComboBox(this);
        m_leapChoice->setStyleSheet(boxStyle());
        m_leapChoice->addItem("Born before noon → use 28 February", false);
        m_leapChoice->addItem("Born after noon → use 1 March", true);
        selectByData(m_leapChoice, osmWidgetGet(m_id, "BirthLeapAfterNoon", false).toBool());
        m_leapChoice->setVisible(isHunabKu(m_birthDate->date()));
        v->addWidget(m_leapChoice);

        QHBoxLayout *birthButtons = new QHBoxLayout;
        m_saveBirth = new QPushButton("Use this birth date", this);
        m_clearBirth = new QPushButton("Clear birth date", this);
        const char *buttonStyle =
            "QPushButton { background:#3a3a3a; color:white; font-size:19px;"
            " border:2px dashed #777; border-radius:12px; padding:10px; min-height:46px; }"
            "QPushButton:hover { background:#505050; }";
        m_saveBirth->setStyleSheet(buttonStyle);
        m_clearBirth->setStyleSheet(buttonStyle);
        birthButtons->addWidget(m_saveBirth, 2);
        birthButtons->addWidget(m_clearBirth, 1);
        v->addLayout(birthButtons);

        m_birthStatus = makeLabel(QString(), this);
        m_birthStatus->setWordWrap(true);
        v->addWidget(m_birthStatus);

        m_kinArt = new KinPreview(this);
        v->addWidget(m_kinArt);
        m_preview = makeLabel(QString(), this);
        m_preview->setWordWrap(true);
        m_preview->setStyleSheet(
            "QLabel { color:#d8ffd8; font-size:19px; background:transparent; }");
        v->addWidget(m_preview);

        v->addWidget(makeLabel("Manual Kin selection (when birth date is cleared)", this));
        m_myKin = new QComboBox(this);
        m_myKin->setStyleSheet(boxStyle());
        m_myKin->setMaxVisibleItems(18);
        m_myKin->addItem("Not set", 0);
        for (int kin = 1; kin <= 260; ++kin)
            m_myKin->addItem(kinTitle(kin), kin);
        int initialKin = savedBirthDate.isValid()
            ? kinForBirthDate(savedBirthDate,
                 osmWidgetGet(m_id, "BirthLeapAfterNoon", false).toBool())
            : qBound(0, osmWidgetGet(m_id, "MyKin", 0).toInt(), 260);
        selectByData(m_myKin, initialKin);
        m_myKin->setEnabled(!savedBirthDate.isValid());
        v->addWidget(m_myKin);

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

        QLabel *note = makeLabel(
            "Conversion uses the Dreamspell/Law of Time count. 29 February "
            "is 0.0 Hunab Ku: a leap-day birth requires the before/after-noon choice.", this);
        note->setWordWrap(true);
        v->addWidget(note);
        v->addStretch();

        connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { applyAppearance(); });
        connect(m_myKin, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
                    if (!m_savedBirthDate.isValid()) {
                        m_birthDateEdited = false;
                        writeKinConfig(QDate(), false, m_myKin->currentData().toInt());
                        refreshPreview();
                    }
                });
        connect(m_birthDate, &QDateTimeEdit::dateChanged,
                this, [this](const QDate &d) {
                    m_birthDateEdited = true;
                    m_leapChoice->setVisible(isHunabKu(d));
                    refreshPreview();
                });
        connect(m_leapChoice, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { m_birthDateEdited = true; refreshPreview(); });
        connect(m_saveBirth, &QPushButton::clicked, this, [this]() {
            const QDate selectedDate = m_birthDate->date();
            if (!selectedDate.isValid()) return;
            const bool afterNoon = m_leapChoice->currentData().toBool();
            const int derivedKin = kinForBirthDate(selectedDate, afterNoon);
            m_savedBirthDate = selectedDate;
            m_birthDateEdited = false;
            m_myKin->blockSignals(true);
            selectByData(m_myKin, derivedKin);
            m_myKin->setEnabled(false);
            m_myKin->blockSignals(false);
            writeKinConfig(selectedDate, afterNoon, derivedKin);
            refreshPreview();
        });
        connect(m_clearBirth, &QPushButton::clicked, this, [this]() {
            m_savedBirthDate = QDate();
            m_birthDateEdited = false;
            m_myKin->setEnabled(true);
            writeKinConfig(QDate(), false, m_myKin->currentData().toInt());
            refreshPreview();
        });
        connect(m_opacity, &QSlider::sliderReleased, this,
                [this]() { applyAppearance(); });
        connect(m_textButton, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(m_textColour, this, "Tzolkin text colour");
            if (c.isValid()) { m_textColour = c; styleColourButton(m_textButton, c); applyAppearance(); }
        });
        connect(m_accentButton, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(m_accentColour, this, "Tzolkin highlight colour");
            if (c.isValid()) { m_accentColour = c; styleColourButton(m_accentButton, c); applyAppearance(); }
        });
        refreshPreview();
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

    void writeKinConfig(const QDate &birthDate, bool afterNoon, int kin) {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
        const QString group = "Widget-" + m_id + "/";
        if (birthDate.isValid()) cfg.setValue(group + "BirthDate", birthDate.toString(Qt::ISODate));
        else cfg.remove(group + "BirthDate");
        cfg.setValue(group + "BirthLeapAfterNoon", afterNoon);
        cfg.setValue(group + "MyKin", kin);
        cfg.sync();
    }

    void refreshPreview() {
        // Preview the chosen date before saving. A saved date or an edited
        // field takes priority over any manual Kin selection.
        const QDate chosenDate = m_birthDate->date();
        const bool afterNoon = m_leapChoice->currentData().toBool();
        const bool showBirth = m_savedBirthDate.isValid() || m_birthDateEdited ||
                               m_myKin->currentData().toInt() == 0;
        const int previewKin = showBirth
            ? kinForBirthDate(chosenDate, afterNoon)
            : m_myKin->currentData().toInt();
        m_kinArt->setKin(previewKin);
        m_preview->setText(previewKin > 0
            ? kinTitle(previewKin) + "\n" + kinDetailLine(previewKin)
            : "Choose your Gregorian birth date or select a Kin manually.");
        QString status;
        if (m_savedBirthDate.isValid()) {
            status = QString("Saved birth date: %1").arg(m_savedBirthDate.toString("dd/MM/yyyy"));
            if (m_birthDateEdited)
                status += " · Preview only — select 'Use this birth date' to update.";
        } else if (m_birthDateEdited) {
            status = "Preview only — select 'Use this birth date' to save the conversion.";
        } else {
            status = "No birth date saved. Choose a date and select 'Use this birth date'.";
        }
        m_birthStatus->setText(status);
    }

    void applyAppearance() {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
        const QString group = "Widget-" + m_id + "/";
        cfg.setValue(group + "ViewMode", m_mode->currentData().toInt());
        cfg.setValue(group + "BackgroundAlpha", m_opacity->value());
        cfg.setValue(group + "Colour", m_textColour.name());
        cfg.setValue(group + "AccentColour", m_accentColour.name());
        cfg.sync();
    }

    QString m_id;
    QDate m_savedBirthDate;
    bool m_birthDateEdited = false;
    QDateEdit *m_birthDate = nullptr;
    QComboBox *m_leapChoice = nullptr;
    QPushButton *m_saveBirth = nullptr;
    QPushButton *m_clearBirth = nullptr;
    QLabel *m_birthStatus = nullptr;
    QComboBox *m_mode = nullptr;
    QComboBox *m_myKin = nullptr;
    QLabel *m_preview = nullptr;
    KinPreview *m_kinArt = nullptr;
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
