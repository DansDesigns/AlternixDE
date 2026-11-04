// tzolkin.cpp — AlternixOS Tzolkin / Harmonic Module widget
// Seal glyphs: uses the exact 13moon.com tribes symbols provided by the user.
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
#include <QCalendarWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QAbstractItemView>
#include <QByteArray>
#include <QImage>
#include <QDate>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>
#include <QStringList>

static const OsmWidgetInfo kInfo = {
    OSM_WIDGET_ABI,
    "tzolkin",
    "Tzolkin",
    "🌀",
    "Tzolkin / Harmonic Module widget with Galactic Signature support",
    720, 860,
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

    qreal totalH = 0;
    if (dots > 0) totalH += dotRadius * 2;
    if (bars > 0) totalH += bars * barH + (bars - 1) * rowSpacing;
    if (dots > 0 && bars > 0) totalH += rowSpacing;
    qreal y = r.center().y() - totalH / 2.0;

    if (dots > 0) {
        // Center the Mayan number. A single dot must not drift left.
        const qreal dotStep = qMin(dotRadius * 2.75,
                                   r.width() * 0.75 / qMax(1, dots - 1));
        const qreal startX = cx - (dots - 1) * dotStep * 0.5;
        for (int i = 0; i < dots; ++i)
            p.drawEllipse(QPointF(startX + i * dotStep, y + dotRadius), dotRadius, dotRadius);
        y += dotRadius * 2 + (bars > 0 ? rowSpacing : 0);
    }

    for (int i = 0; i < bars; ++i) {
        QRectF bar(cx - barW / 2.0, y, barW, barH);
        p.drawRoundedRect(bar, barH / 2.0, barH / 2.0);
        y += barH + rowSpacing;
    }
    p.restore();
}

// Law of Time-inspired solar seal vector redraws (20 seals, Dragon through Sun).
// All details are Qt painter paths, never low-resolution raster scaling.
// These are original vector redraws based on the supplied Harmonic Module reference.
static const char *const kSealStrokePaths[20] = {
    "M 13 24 C 27 15 72 15 87 24 M 18 29 C 29 37 72 37 82 29 M 23 54 C 20 64 22 74 26 82 M 34 52 C 30 62 34 71 35 81 M 46 52 C 42 61 46 70 47 82 M 58 52 C 56 60 59 69 58 82 M 70 52 C 69 62 73 73 71 82 M 82 53 C 79 63 84 73 83 80",
    "M 15 28 C 27 35 36 25 38 16 C 38 34 45 36 50 30 C 56 36 64 34 64 16 C 66 27 74 34 86 28 M 29 33 C 28 47 38 51 50 45 C 62 51 71 47 70 33 M 40 51 L 40 63 C 40 72 45 78 50 82 C 55 78 60 72 60 63 L 60 51 M 49 55 L 50 74",
    "M 14 28 C 12 16 28 11 36 21 C 43 30 38 41 32 43 C 22 46 17 34 23 30 C 28 26 34 30 32 34 M 86 28 C 88 16 72 11 64 21 C 57 30 62 41 68 43 C 78 46 83 34 77 30 C 72 26 66 30 68 34 M 49 19 L 49 43 M 15 51 C 30 53 35 60 50 59 C 65 60 70 53 85 51 M 18 61 C 30 58 37 64 50 69 C 63 64 70 58 82 61",
    "M 18 46 L 82 46 M 50 46 L 50 84 M 33 47 L 33 78 M 67 47 L 67 78 M 31 79 L 69 79 M 26 36 C 35 31 42 31 50 36 C 58 31 65 31 74 36 M 50 14 L 50 32",
    "M 10 47 C 28 40 36 40 46 49 C 59 61 71 59 87 49 M 14 67 C 28 57 40 60 49 69 C 58 78 72 78 86 71 M 18 81 C 34 75 45 75 56 82 M 59 21 L 89 20 L 89 47 L 63 46 L 57 37 Z M 60 22 L 87 46 M 74 20 L 89 32 M 62 34 L 79 21 M 70 46 L 88 30 M 10 54 L 32 49",
    "M 49 10 L 49 84 M 19 22 C 35 26 39 38 37 48 C 36 59 26 58 20 52 M 81 22 C 65 26 61 38 63 48 C 64 59 74 58 80 52 M 17 79 C 25 76 32 69 37 63 M 83 79 C 75 76 68 69 63 63",
    "M 30 27 C 45 22 55 22 71 27 M 29 28 L 29 62 M 38 25 L 38 57 M 49 25 L 49 57 M 60 25 L 60 56 M 70 28 L 70 61 M 28 61 C 35 73 48 79 59 70 C 65 65 62 59 60 56 M 26 72 C 30 76 34 81 39 83 M 66 68 C 75 63 78 56 77 47",
    "M 50 14 C 58 33 70 44 87 50 C 70 55 58 69 50 86 C 42 69 30 55 13 50 C 30 44 42 33 50 14 Z M 23 24 L 28 26 M 76 24 L 72 26 M 23 76 L 28 74 M 77 76 L 72 74",
    "M 16 82 L 16 52 C 16 27 31 15 50 15 C 69 15 84 27 84 52 L 84 82 M 27 82 L 27 52 C 27 34 36 27 50 27 C 64 27 73 34 73 52 L 73 82 M 37 82 L 37 53 C 37 44 42 39 50 39 C 58 39 63 44 63 53 L 63 82",
    "M 18 27 C 32 20 43 22 48 36 M 54 33 C 67 20 79 17 85 21 C 87 28 83 36 72 41 M 21 41 C 26 36 35 37 41 44 C 36 50 27 51 21 47 Z M 47 48 C 61 48 67 50 78 45 M 22 61 C 39 59 54 61 68 66 L 80 68 M 22 75 C 41 75 60 76 76 80 M 63 48 L 61 56 M 73 45 L 73 56",
    "M 18 34 C 26 40 31 49 32 58 M 82 34 C 74 40 69 49 68 58 M 28 23 L 72 23 M 36 17 L 64 17 M 43 31 L 57 31 M 32 66 C 42 74 58 74 68 66 M 18 78 C 31 68 36 77 41 84 M 82 78 C 69 68 64 77 59 84",
    "M 22 82 L 29 82 M 34 81 C 35 71 40 63 48 55 C 56 47 57 33 63 25 C 66 20 73 17 80 18 M 48 55 C 61 49 68 50 77 44 M 47 54 L 42 77 M 14 50 L 33 50",
    "M 25 16 L 25 47 M 39 16 L 39 47 M 53 16 L 53 47 M 66 16 L 66 47 M 79 16 L 79 47 M 13 53 L 87 53 M 23 55 L 23 84 M 38 55 L 38 84 M 53 55 L 53 84 M 68 55 L 68 84 M 82 55 L 82 84",
    "M 20 20 C 30 25 39 25 50 20 C 61 25 70 25 80 20 M 50 24 L 50 60 M 22 39 C 30 48 34 54 45 53 M 78 39 C 70 48 66 54 55 53 M 19 67 C 30 64 39 64 50 73 C 61 64 70 64 81 67 M 31 80 C 39 75 44 77 50 84 C 56 77 61 75 69 80",
    "M 16 27 C 31 21 44 22 56 30 M 24 40 C 34 29 45 30 52 38 C 47 50 35 51 24 40 Z M 54 42 C 62 41 73 32 83 31 M 54 55 C 62 51 71 49 84 49 M 20 55 L 14 74 M 33 59 L 29 82 M 46 60 L 46 84 M 60 59 L 65 84 M 74 56 L 82 81",
    "M 49 20 C 60 18 70 26 70 36 C 70 45 62 48 55 52 C 48 56 44 61 47 72 M 38 75 C 42 82 56 82 61 75 M 16 35 L 39 35 M 72 35 L 84 35 M 16 51 L 37 51 M 75 51 L 84 51 M 16 66 L 35 66 M 72 66 L 84 66",
    "M 21 37 C 18 25 28 19 38 25 C 47 31 44 42 38 43 C 32 44 29 39 32 35 M 79 37 C 82 25 72 19 62 25 C 53 31 56 42 62 43 C 68 44 71 39 68 35 M 37 47 C 48 55 47 59 42 68 C 39 75 42 82 50 84 M 63 47 C 52 55 53 59 58 68 C 61 75 58 82 50 84 M 50 83 L 50 90",
    "M 17 17 L 17 33 L 34 33 L 34 45 M 83 17 L 83 33 L 66 33 L 66 45 M 17 83 L 17 67 L 34 67 L 34 55 M 83 83 L 83 67 L 66 67 L 66 55 M 35 35 L 44 44 M 65 35 L 56 44 M 35 65 L 44 56 M 65 65 L 56 56",
    "M 17 33 C 29 23 37 24 45 32 M 55 32 C 63 24 71 23 83 33 M 21 50 C 32 47 43 50 50 56 C 57 50 68 47 79 50 M 40 69 C 49 77 58 78 65 70 C 70 64 66 58 61 61 M 25 73 C 30 79 34 82 39 83",
    "M 50 15 L 50 67 M 50 65 C 34 65 31 83 50 86 C 69 83 66 65 50 65 Z M 18 52 L 32 52 M 68 52 L 82 52"
};
static const char *const kSealFillPaths[20] = {
    "M 26 23 C 30 17 70 17 74 23 C 76 29 73 32 69 33 L 30 33 C 26 32 24 27 26 23 Z M 15 17 L 19 17 L 19 22 L 15 22 Z M 81 17 L 85 17 L 85 22 L 81 22 Z",
    "M 46 57 L 54 57 L 52 75 L 49 78 L 46 72 Z",
    "M 13 82 C 26 60 42 65 50 72 C 58 65 74 60 87 82 L 87 87 L 13 87 Z",
    "M 39 21 C 42 17 48 17 50 24 C 52 17 58 17 61 21 C 64 28 59 35 50 36 C 41 35 36 28 39 21 Z",
    "M 59 19 L 88 19 L 88 48 L 59 48 Z",
    "",
    "M 22 73 L 38 72 L 38 86 L 22 86 Z",
    "",
    "M 42 45 C 45 42 54 42 58 45 C 62 50 62 59 58 63 C 54 66 46 66 42 63 C 38 59 38 50 42 45 Z",
    "",
    "M 43 13 L 57 13 L 57 27 L 43 27 Z M 42 50 C 42 41 58 41 58 50 L 58 69 L 42 69 Z M 29 81 C 36 70 41 74 43 85 L 29 85 Z M 57 85 C 60 74 66 70 72 81 L 72 85 Z",
    "",
    "",
    "M 44 15 L 56 15 L 56 22 L 44 22 Z",
    "M 31 37 C 37 30 46 30 50 37 C 46 44 37 44 31 37 Z",
    "",
    "M 45 31 C 48 28 52 28 55 31 C 58 34 56 39 51 42 C 47 43 44 38 45 31 Z",
    "",
    "M 43 35 C 46 32 51 32 55 36 C 56 39 55 43 52 44 C 47 45 44 42 43 35 Z M 20 78 C 25 75 29 78 29 84 C 29 88 21 88 20 83 Z",
    ""
};
static const char *const kSealDotInfo[20] = {
    "15 35 2.2 84 35 2.2 21 42 2 78 42 2",
    "",
    "20 56 2.4 37 58 2 63 58 2 80 56 2.4",
    "",
    "78 26 2.4 66 37 2 82 39 2",
    "",
    "24 78 3",
    "",
    "",
    "33 43 3",
    "24 42 2.2 76 42 2.2",
    "25 38 4.2 68 26 2 74 21 2 79 34 2 82 39 2 62 40 1.8",
    "",
    "26 45 3 74 45 3 50 70 3",
    "39 39 3",
    "52 54 3",
    "31 35 2 69 35 2",
    "",
    "28 37 3 72 37 3",
    "32 34 6 68 34 6"
};
static const char *const kSealRingInfo[20] = {
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "24 26 6.5 76 26 6.5 24 74 6.5 76 74 6.5",
    "",
    "29 45 7",
    "25 44 5 75 44 5",
    "",
    "",
    "",
    "",
    "49 45 10",
    "",
    "",
    "",
    "50 74 9"
};

// Path tokens are separated by spaces. Supported: M move, L line,
// Q quadratic, C cubic, Z close. No SVG Qt module is required.
static QPainterPath parseSealPath(const char *data) {
    QPainterPath path;
    const QString raw = QString::fromLatin1(data);
    if (raw.isEmpty()) return path;
    const QStringList t = raw.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    int i = 0;
    while (i < t.size()) {
        const QString cmd = t.at(i++);
        if (cmd == QLatin1String("Z")) { path.closeSubpath(); continue; }
        int n = 0;
        if (cmd == QLatin1String("M") || cmd == QLatin1String("L")) n=2;
        else if (cmd == QLatin1String("Q")) n=4;
        else if (cmd == QLatin1String("C")) n=6;
        else break;
        if (i+n > t.size()) break;
        qreal v[6] = {0,0,0,0,0,0};
        for (int k=0;k<n;++k) v[k] = t.at(i++).toDouble();
        if (cmd == QLatin1String("M")) path.moveTo(v[0],v[1]);
        else if (cmd == QLatin1String("L")) path.lineTo(v[0],v[1]);
        else if (cmd == QLatin1String("Q")) path.quadTo(v[0],v[1],v[2],v[3]);
        else path.cubicTo(v[0],v[1],v[2],v[3],v[4],v[5]);
    }
    return path;
}

static const QVector<QPainterPath> &cachedSealPaths(bool filled) {
    static const QVector<QPainterPath> strokes = []() {
        QVector<QPainterPath> a; a.reserve(20);
        for (int i=0;i<20;++i) a.append(parseSealPath(kSealStrokePaths[i]));
        return a;
    }();
    static const QVector<QPainterPath> fills = []() {
        QVector<QPainterPath> a; a.reserve(20);
        for (int i=0;i<20;++i) a.append(parseSealPath(kSealFillPaths[i]));
        return a;
    }();
    return filled ? fills : strokes;
}

// Cache all dots and rings, rather than re-parsing while painting 260 cells.
static QPainterPath parseSealDots(const char *raw) {
    QPainterPath path;
    if (!raw || !*raw) return path;
    const QStringList nums = QString::fromLatin1(raw).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (int i=0; i+2<nums.size(); i+=3) {
        const qreal x = nums.at(i).toDouble();
        const qreal y = nums.at(i+1).toDouble();
        const qreal radius = nums.at(i+2).toDouble();
        path.addEllipse(QPointF(x,y), radius, radius);
    }
    return path;
}

static const QVector<QPainterPath> &cachedSealDots(bool rings) {
    static const QVector<QPainterPath> dots = []() {
        QVector<QPainterPath> a; a.reserve(20);
        for (int i=0;i<20;++i) a.append(parseSealDots(kSealDotInfo[i]));
        return a;
    }();
    static const QVector<QPainterPath> ringsData = []() {
        QVector<QPainterPath> a; a.reserve(20);
        for (int i=0;i<20;++i) a.append(parseSealDots(kSealRingInfo[i]));
        return a;
    }();
    return rings ? ringsData : dots;
}


// Exact 13moon.com tribes graphic provided by the user, embedded for runtime use.
static const char *k13MoonTribesGraphicBase64 =
    "iVBORw0KGgoAAAANSUhEUgAAASEAAADXCAYAAABVoeerAADFmElEQVR4Ae3AA6AkWZbG8f937o3IzKdyS2Oubdu2bdu2bdu2bWmMnpZKr54yMyLu+Xa3anqm"
    "hztr1a/a5qqrrrrqvwEAlefjGc94hn/mZ36Gn/7pn+a/21u/9VvzVm/1VjzoQQ8S/4l+93d/1z/90z/NX//1X/Pf6bVe67V467d+a17qpV5K/Cd6xjOe4Z/5"
    "mZ/hp3/6p/nv9tZv/da81Vu9FQ960IPEf6Lf/d3f9U//9E/z13/91/x3eumXfmne673ei5d6qZcS/4kuXbrk7/me7+Gnf/qn+e/21m/91rzVW70VD3rQg8Sz"
    "ASDb3O/SpUv+nM/5HL7qq76K/2k++7M/m8/6rM8S/8H+5m/+xh/zMR/Db/3Wb/E/yeu8zuvwXd/1XTzoQQ8S/4EuXbrkz/mcz+Grvuqr+J/mYz7mY/isz/os"
    "jh07Jv4D/c3f/I0/5mM+ht/6rd/if5LXeZ3X4au+6qt4qZd6KfEf7HM+53P82Z/92fxP8zEf8zF81md9FseOHRMAgGwDsLe35zd4gzfgT//0T9neDD703bZ5"
    "89de8JKP7vm3+JJvvcQXfvMlfv5br+E1Xn7Ov8XfPmHg+3/2kB/8uUP2D5NXfMVX5Fd/9Vc5duyY+A/wvd/7vX7v93lfnI2X7Xd4w8UpXrbf5oYy44GEABBX"
    "PHVaspcTAH857PHCvGy/A8D1ZcZ1pcc8mzEPdFdb81uri/zU0X3c09acOnWK3/qt3+IlXuIlxH+Avb09v8EbvAF/+qd/yubmJh//8R/PW7zFW/DSL/3S/Hf5"
    "67/+a376p3+ar/qqr+JoueIVXv7l+NVf/VWOHTsm/gN87/d+r9/7fd4XZ4PtU/Ba7wsPfmnYuZZ/k696Wzh2LbzvN/Fvsncv3PrX8DvfiQ53OXniOF/5lV/J"
    "e77ne4r/AHt7e36DN3gD/vRP/5TtzeBd32KTd3/LTV7y0T3/Xf72CQM//9tLvvEH9tk/TF7xFV+RX/3VX+XYsWMCkG0AXumVXsl/+qd/yos/oucXvu0aTuwE"
    "Ekj8m3zhN1/iC7/5Er/07dfyGi8/49/CBhtuvXPiXT/2HH//5IE3fMM35Fd+5VfEv9PP/uzP+q3f5m1xNj5k+2beZfM6CkKAgEAA/O24zx+vL3F7W/G3wwHn"
    "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"
    "ryz+k33nd36nn/CEJ/BHf/RH/Hd5zGMewyu/8ivzdm/3dhw7dkw8GwCyzVVXXXXVfwMAgquuuuqq/x4ABFddddVV/z0ACK666qqr/nsAEFx11VVX/fcAILjq"
    "qquu+u8BQHDVVVdd9d8DgOCqq6666r8HAJV/wd7entfrNXt7e/x3mM1mzGYzTp48SSlF/Cc7PDz00dERe3t7/HeICLa2tjh58iSlFPGf7PDw0EdHR+zt7fHf"
    "ISLY2tri5MmTlFLEf7LDw0MfHR1xcHBAZvJfLSLY2tpiY2ODzc1N8Z/s8PDQR0dHHBwckJn8d9jZ2WFjY4PNzU3xnACQbZ6fs2fP+klPehK/8Au/wK233sof"
    "//Ef89/h5ptv5sYbb+TN3/zNeeVXfmUe9KAHUUoR/8HOnj3rJz3pSfz2b/82T3ziE/n93/99/jscO3aMxzzmMbz5m785L/3SL83DHvYwZrOZ+A+2u7vr22+/"
    "nZ/92Z/liU98In/0h3/If4et7W0e85jH8OZv/ua89Eu/NA960IPY3NwU/8H29vZ8xx138FM/9VM88YlP5O/+7u+4dOkS/9WOHTvGYx7zGF76pV+aN3/zN+dB"
    "D3oQm5ub4j/Y4eGhn/GMZ/BTP/VTPPGJT+Tv/u7vuHTpEv8dXvmVX5mXfumX5s3f/M256aab2NnZEVcAINs8t6c+9an+nM/5HL7v+74P+g2YLaDO+G8zrWH/"
    "PA95yEP4/M//fN7iLd6C7e1t8R/kqU99qj/ncz6H7/u+72N7M9jeFPOZ+Jes1uau+xrbm8GZkwHA2QvJ/mFywzWF+UwAPO32CYCH3lz5l6zWBuDshQRVvuZr"
    "voZ3fMd35NSpU+I/yNOf/nR/2Zd9Gd/0Td/EdWXGThQeXBdsqPBf7VwO3NcG7pzWDDX4/M//fN77vd+ba665RvwHueuuu/z1X//1fPmXfzkA11xzDX3f899l"
    "GAbuu+8+AD7/8z+fd3/3d+eGG24Q/0Huuusuf//3fz+f/umfzmjBYhvqjP820xrWSzqPfPzHfzwf/uEfzg033CAAANnmgf7iL/7Cb/RGb8SFi7v42LXwUm8C"
    "j3gVmC34b3PvU+GvfwnueRLsn+dTPuVT+ORP/mR2dnbEv9Nf/MVf+I3e6I3Yu3SBm68vvNfbbPEGrzpnazP4l/zF3w+8z6ec48PebZsPeudtAL7lh/f5hh/Y"
    "57u+6DQv9+I9AC/5FncB8Lc/dwP/kgjx+KcM/OgvHfEHf7nirvsa7/Ee78HXfu3Xcvz4cfHv9Dd/8zd+m7d5G+679TZeZrbNu2xczyvNjrFQ8N+hIvbd+Omj"
    "+/jF5Tn+ctjjLd7iLfj2b/92rrnmGvHvdNddd/ld3uVd+N3f/V0e8pCH8MZv/Ma88zu/MzfddBP/Xe644w5++Id/mF/+5V/m6U9/Oq/5mq/JD/3QD3HDDTeI"
    "f6e77rrL7/Iu78Lv/u7vwokb4MEvCy/5BrB9mv82l+6Dp/05PO634OJdvOZrvibf9E3fxGMf+1gByDb3e9KTnuRXfdVX5fylfXj4K8PbfiZsHINpAJv/NqVC"
    "7eGvfh5++Wtg/zyf9VmfxWd8xmdQShH/RufPn/ejHvUo9i5d4M1fZ8F3ftEpjh8vTIOx+Rf93p+veL33upfP/LDjfPqHHgPg87/xEp/7Dbv8xvdcy2u8/ByA"
    "/sWeAcDwDw/iRVELaCZ+9beWfNwXXeTvnzzwER/xEXz5l385fd+Lf6Pz58/7lV/plbj7abfyzpvX8YFbN1EkRifmv4+ATgHAp+4+md9cXeBN3vzN+ZEf+REW"
    "i4X4N9rb2/NbvdVb8Tu/+3u8weu/Ht/4jd/Iwx72MP6neOpTn8rbv/3b89d//de89mu/Nj//8z/P5uam+Dc6PDz0m7/5m/Pbv/3bcN0j4O0/G254DAxLsPlv"
    "I0Ht4Z4nwS98JTz9L3jd131dfuInfoLjx48reKbWmj/kQz6E8+fPw0u8IbznV0PpYHUA0wBthDZCG6GN0EZoI7QR2ghthDZCG6GN0EZoI7QR2ghthDZCG6GN"
    "0EZoI7QR2ghthDZCG6GN0EZoI7QRhiUcXYKXeEN45y+BxQ6f8zmfw5//+Z/zb9Va88d8zMdw/vx53vx1FvzgV55m1ovDg2Q9mGE0w2iG0QyjGUYzjGYYzTCa"
    "YTTDyGVTM8NohtFMzQAMIwyjGUZzv2E0w2iG0QyjGUYzjGYYzTCaYTTDaI5W5vBS8gavOufnv/UMD7258nVf93X88i//Mv8en/AJn8BTnvpU3nnzOj5y+xYS"
    "MzgxzyagIAqiIAqiIAqiIAqiIAqiIAqiIAqiIAqiIAqiIAqiIAqiIAqiIAqiIAoiEM0G4AuPP4JXnh3j537u5/ipn/op/j2+6qu+it/+7d/mtV7zNfiVX/kV"
    "Hvawh/E/ycMe9jB+/dd/nZd+6Zfmt3/7t/mO7/gO/j2+4zu+g9/+7d+G6x4B7/ZlcOpBcHQJpgHaCG2ENkIboY3QRmgjtBHaCG2EbOAEJ/8hnNAmWB3AqQfB"
    "e34NPOil+M3f/E0+93M/FwDZBuAv/uIv/PIv//Jw4gb48B8CJ/8j1R7+/Kfh57+MN3zDN+QXf/EXKaWIf6W/+7u/80u+5EtywzWFv/25G1jMhc2/yu/9+Zo3"
    "ef97+dQPPsanfvAxAL7wmy/xhd98iV/69mt5jZefAbD10rcBcPDXt/Cv1VXx4798yHt9wnke9djH8jd/8zeUUsS/0h133OGHPuQWTrnnJ8+8FM9PQYwkuzkx"
    "2Px3OZ8DH3D+cbzYi70Yf/mXf0nf9+JfaX9/3w95yEPY29vjd3/3d3n0ox9Na437bW9v0/c9/9WWyyVHR0fcr+s6Hv/4x/Mar/Ea9H3P2bNnWSwW4l9pGAaf"
    "PHmSw/UIH/gdcPpBkI0XiQIkyAbrIxiXMK1hmsDJsyh4gZw8X10Pi+Ow2IZsIMH+OfjG90TTmv29S1Se6Wu+5mu47NXeDZz8jzUN8JJvBL/znfzqr/4q58+f"
    "55prruFf60d/9EcB+KB33mZzQ7TG/0jjZN7lzTf5om/Z4+//4R943OMex0u8xEvwr/WjP/qjjJN5153rEGCeU0GczYG/Hvb55dU5njIu+e+0qcI//MM/8Md/"
    "/Me85mu+Jv9aP/VTP8X58+e58cYb+dZv/VZWqxW1Vu736Z/+6TzykY/kv9qv/dqv8eM//uMATNNErZVaK33fc3h4yK/92q/xlm/5lvxr/cEf/AGHh4fw6NeE"
    "Mw+BNvIvkkABy31Y7sL5O+Apfwy3/z1cuheWe/y7LXbgZd8CXvkdYHEMbNi5Bl72LfAf/wg/8iM/QuWZ/u7v/o7LHv2aoOA/l8Hm36xUePRrwp/9FH/8x3/M"
    "W77lW/Kv9Ru/8RsAvP0bb9Aa/6Mdrcxbvt6Cv3/ywO///u/zEi/xEvxr/eEf/iEAL9PvYJ6TgLM58MV7T+f3V7tcV2b8T/HHf/zHvOZrvib/Wn/9138NwGq1"
    "4md/9me534WLuzgb7/Ve78UjH/lI/qv98R//Md/3fd+HonDyxHHuN5/POTw85Ld/+7d5y7d8S/61fumXfonLHv5K0Eb+RRK0CQ7Ow1//EvzZT8JyDwD6DTSt"
    "8Ykb4Pj1cPx62L0bdu+G/XPQzaFNMBzxLA95Odg6BbXnOdz9RPiDH4Dzt8PbfiZEgWzwiFeGP/4Rfv/3f5/KM/31X/81ioIVsNzjP5UCZhv8m9lw7DoA/viP"
    "/5i3fMu35F9rNpsB8NCbK63xP94rvuQMgLvuuot/i3vvvReAR9YNGuaB1k4+c/ep/OWwx7tvXs87b17HI+oG/52+5eAOvnLvGRwdHfFv8cd//McA/MiP/Ag3"
    "33wz9/v6r/96vu7rvo5SCv8dZrMZAB/+YR/Kh3/4h3O/P/7jP+a93uu9eMITnsC/xcHBAZeduol/kQTDCp72p/DzXw7LPVjswENeDh716nD+Nnzf0+B9vhHm"
    "25ATAAxL+IGPh/k2XPsweOqfwrlbYbkPu3fDq787PPQVYLYB2bhsXMO3vA884Xfh/O1w5sFcdupmAJ785CdTeQAfuxZ++Wt4oWoP0wC1h2mA2sM0QO1hGqD2"
    "MA1Qe5gGnq9TN8Orvzv/Ljc+GoC+7/n3aI3/EKVwWS0CIAJK4T/MYi4ASin8W9RaAWiY5/b7613+ctjj/bZu5P23buR8jjx5OmKw6SUAtlRYqJAY868nIBD3"
    "S4x5/gw8ptvk32M2mwHweq/3ejzQtddeC0BE8N/p2muv5ZGPfCT3O3v2LACHh4f8W/zd3/0dl2XyL5rW8Ec/DL/73VB7OHEDvPb7wmNeG/buhVv/Ch7xqvDH"
    "PwqPfg2oM5jW8ITfg2seCudvh4e8HLzi28HT/gx++zvh3DPg+z4GXvO94VXeGboZl0nwGu8BP/aZ8Iy/hjMP5rLtMwDUWqk8gC7diw93eR6lQptgOOJfpfbQ"
    "zaFNPIcHvRT/bpn8d4kACVoagMOlueOexnqAe883AO66t/G02yf6TtwvBAhssPlXyeQ/zU8e3cd1ZcYrz47x1fu38VfDHnvZuN/DuwWv2B/jlWfHOBEdO6o0"
    "zItCQCBGkgM37tchFiokxjwnAYPNf4ZpmgAYx5FhGHigUgqlFP6jtNZorfFA0zQBME0TDzSOIwARwb9FrZUX2d/9Ovzud8NiB17hbWF5CW79a3jKnwLAG34Y"
    "nH4Q/Oa3wfd8JDz4ZeHuJ8KjXxPe6CNg/zz88tdw2XwTzjwEXvz14Q9+AH73u2HjGLz8W3OZDRvHAODoEs/iBACg8gC+5qHw1p/O82gj3P538AffD/vnURRc"
    "ZwBoWgPgOoNSYVzBNEC/Aa/wNvDwV4TNk5DJs/Rz/jeSQMDuXnJpP/nzvxsA+M4fP+DnfvOIp90+AdBV8T6fco6uipuvL9zvL/5h4MZrCyeOBfOZyASb/1aJ"
    "+dtxnxnBB5x/HACbKmxH5X5/td7n91e7ALzT5nW8x+b1nImehnlhBIyYvRx5/HjIr63Oc78X77Z45dkxromemQLzX8M2AL/yK7/CM57xDO5Xa+XMmTO8yZu8"
    "Cf9RfvVXf5WzZ88yTRP3+5u/+RsAbPPfYu8++Pkvg9rDm388vNSbcNmvfzPMt+DV3x3GNYwrqD1cvAsu3gUAL/76MCxhsQ3v9AXw+98P9z4V3ukLAODah8GP"
    "fSb88tfAw18Rjt8AAJm8AABUHmixA9c+jOfxBz8Av/5N0G/AiRvwTS8O1z4Mlnv4cb8F0xpufDF4+CvBvU+B2/8eLt0Lf/ADcN/T4J2+EErHszghG/+bRMDR"
    "0jz5GSPf/IMH/ODPHzJOZnsz2N4Uq7V56M2Vxz6sY3sruP3uxl33TazW5oZrCvuH5jXe9R5e9sV6Puidtnm9V51z+kShq2Dz3+bx4yGTzUTjujLj9ecnea35"
    "Ca4vMx5SFzx9WvKU6Yg/Wl/id1YX+ZHDe3jyeMSXnHgEmyq8MCPmT9aX+Iq9Z3BPW7OpwnZU9nPiV5fnAfjYnQfx5oszLBSY/3xd19F1HV/2ZV/Gc3vFV3xF"
    "3viN3xhJ/Ef47M/+bP70T/+U59Z1HV3X8d/ir3+Jy25+CXjUa8Cl+2DvPrjzcVz2oJeGnWugVvj1b+I5/Po3wWNfG2ZbcO9T4al/CgC3/z1sn4aHvBw89OXh"
    "Cb8Lf/Lj8EYfyb8AgMoD5QRt5Hn8+jehKPh1PwBe+R2h9pANfv2b4eJdKAq+9ynwrl8KUSAK/N2vwc9/KTz5j+DOx8ONj+F/qxCcvZD8xK8c8rFfdBGAG64p"
    "vOxje97lzTd51EM7Xu7lZ1zWgAYUoMDqUvKHf7nmj/96zS/97oq/ecLAB33meR56c+VLPv4Eb/DqC2rhv0VivufwLgCuKzO+7MQjecluixHTbC7lxJnouW42"
    "43VmJ3mPzev54r2n8/urXT7p4pP5yhOPpFfw/Aj4k/UlPuHik9hU4f22buS15yd5TLfJ48dD/mE84CeP7uUr957BpZx4j83r6RX8Z3v0ox/NO7/zO/NA9957"
    "L7/6q7/KxsYGkviPsrGxAcAbvuEbcu211/JAj370o/lv8Yy/4rLZJjz1T2H/HDzx9+FNPprLfumr4VGvDtuneb4e99sw34J/+E14i08EgF/6anjQS8O1D4Pt"
    "U1x239N4EQBQeVH0Gxhg+xTs3g11Bl0PwxJFwdlgtgnLSzBNMC5hvg11hqJg/nfbPzTv+ynn+I0/WnHDNYV3fYtNPvkDj3HqmgKjmSZYXUoeyAYMBl7j5ee8"
    "7qvM+dSPPM7P/NIRX/4de/zJ36x5h486y499zRne6DUWRPBf7unTit9f7VIlPu/4w3hMt8mhGw/UMM1mAM5Ez1ccfxQft/tEfn+1yy+vzvOWizM8P3e3gc/e"
    "fSqbKnzBiYfzOrOTrJyMTh5ZN3hMt8kbzk/xwRcex3cc3MlD6oLXm5/E/Od6u7d7O97u7d6OB/qLv/gLfvVXf5X/LF/4hV/Iy73cy/E/yut+ACz3Yb4Nr/yO"
    "MCy57L2+Fp76ZwDwoJeCZ/wNz3LiBnjIy8E0wLt9OUQBgPf6Wnjqn3HZK709/NlP8SICoPKieM33gj/4Afixz4TtU3Dji8GDXwa2TuI6g+EIHvyy8ITfg1v/"
    "Gm79S7h4F/Qb+GGvCDe/OGTjfyMJvuw7LvEbf7TioTdXfvRrzvByLznj8CC5+86JYTTLlXmgWkXfiXkvtjbF1MwwApg3e+0Fb/UmG3zyF13kK79zj3f4qLP8"
    "4Q9fx0s+uue/2h+udwF4yW6bl+l3GJy8MA0j4LOOPYy3Xv81X7d3G288P0Wv4Ln9wvIsh2587M6DePXZCQ7duF/DNJtNFb7yxKN467N/zfce3sXrzU/y32F/"
    "fx8ASfxHkgTA/v4+/2NE5bILd8CjXgOcsDrgWVYHcPOLA8BbfSr8zBfC3lnYOQNv8xmwcw2XOSEbAKwO4OYXBwXc+1Qui8qLAIDKi+I13wtufgn4/e+De54E"
    "T/ljeMLvAqAouPbwxz/CZbWHbg4Peil4iTeEV35HWB3wP1UpvFDDCF/zPft0VfzKd17Lg26o3HbHxJ/+zZof/PlD/uYJA0+7feKBuipe4lEdr/Hyc975zTZ4"
    "0A2VY1uBgfVg1oP54o8/Tmvmy79jjy/7jj1+4CtOI/ECRfAfbj8nAB7bbbJ2Ip6XAPNsBnZUefONM/zI4T38+uoCb7Y4jXlOv7e+SJV4641rGJw8Pw1zJnpe"
    "d36SX12eZ+2kV/DfZRxH7rvvPp7bNddcw7/kvvvu47mN48j/ODe/BDz9L+AvfhYe+goQheeRDQCOXw8f8O0QBbLBsIRsPF/ZIAf4i5/mstO38CIAoPLcsvE8"
    "Vgdw02PhPb4KLt0Ddz4edu+B1QG+58lcdvx6OH4dLLbh5peAax4K0wCrA14oictsnkUCAJv/bM+4s/HC/N0TB8bJvN6rzLnl+srjnzryPp9yjr/8h4GuivlM"
    "vPgjeh7zsA6A5Sr5y8cNPPnWib/8hz2+5nv2eIc33uTTP/QYD7ulcr/DI/PpH3qcL/+OPZ5x18RtdzVeENucu9D4jyTg1rYEYKZAPC8BSydVoiLul5hXmR3j"
    "Rw7v4e/HA950cZoHErCXjZfstukQ5gVLzM1lDsBfj/u8Yn+M/2q2AfiTP/kTXvd1X5dhGOj7nvv9/d//Pf+S133d1wVgGAb6vmcYBu666y4AbPM/xiu8Nfzx"
    "j8KT/wie/hfwkJeDUsHmeWSD1QH/IgnaBE//C/QXP4trDy/9ZrwIAKg80N5ZePxv8y+qPWyfgu1TcN3DuayNPMuwgtUBL5JhBdlgtsGzrA4BYLbBf7ZHv/Gd"
    "vCjWAxytkjd5/3u572zysi/W8xHvvsObvtaCa64v0Hi2Ak984siP/OIh3/NTB/zYLx/yB3+54le/6zoeclPB5rKuctlf/sPAo9/4Tv4rGXiJbpvfX+1yPkcE"
    "mOc0Yr7z4E5efrbDK/bHuJ+Bl+62AbhtWmFAPJuBnSgAmBfOPNumCv8dFosFN954IwD33HMP8/mcCxcucOHiLs7Gi+If/uEfUBROnjjOfD5ntVpx/Phxjh8/"
    "zmKx4H+MjRPwuh8Av/5N8EOfBG/z6fDwV4HZBkhg8yKTwIbVIdzx9/BDn4RrD6/+HnD9IyEb/wIAKg908S74qc/n3+013xte8714kfzFz8Cle+CNPpJn+fVv"
    "gtrBG30k/9ne4Y03eWEuXGr8xh+tAPj1P1hx132NV3+5Ob/6ndfQdWKczOGl5Lk96MbCZ374MT7h/Xd4iw+6j9/4oxXv/NFn+Y3vuZbNDQFgc9kN1xRe7WXn"
    "vDAXLjV+449W/Ee6scwA+OP1JQ7c2FLhfg3zu6uLfP/h3fz66gI/eealeKCZgvuJ52TgmtLz+6tdLuTIqegwz5+Bvxr2qRIv2W3TMP/VXvmVX5k77rgDANsA"
    "SOKN3uiN+NVf/VVeVG/w+q/HL//yLwMgif+RnPDK7wj75+DPfgp+6vPhJd4QXu3dYOsk9AsoHWBeMEEbYVzD3n3wlz8Lf/ZT0G/AK7wNvO4HwLDkRQBA5YFO"
    "3ACv9A48j9UBnL8dnvj7MByhKLjOoFSoHZy8CRbHuOzUzfCwV+RF9uQ/hGf8DbzRR/Isf/XzAPBGH8l/th/9+jO8ML/zxyt+44/uAeCvHj8A8LqvPMfAejAv"
    "SGtweGQk+OlvvIa3/tD7+I0/WvGV373HZ37YMWye5aE3d/zo15/hhfmdP17xG390D/+RXn12nOvKjHvams+59FQ+eechbKjQMP8wHvDpu09hU4UvO/FIBJhn"
    "Wzt5QQS88fw0v7/a5fsO7+ZDtm5ipgDAXCGgYf582OMvhz1efX6cxPx3k8T9hmHgX2MYBiTxP940wBt9BFzzEPjlr4W/+1X4u1+FR7wKvPjrwYNeGkoFFSgd"
    "z5INcoQ2wTP+Gp7yp/B3v8plix14rfeBV30XGJa8iACoPNDx6+GV35HLosDqAM7eCr/3vfCE34V+AxY7+LpHwKNeHW5+CXjQS0E2nsM0QDb+N7jvvsYLs7uX"
    "3O8VX3IGwJ33TrwwEthcZkMEfO1nnOSl3/Juvu+nD/iE99thPhMPdN99jRdmdy/5j9Yr+LzjD+OjLzyR31/t8rHtiTy22+LIjd9cXWBThc8+/jAeWTdomPsJ"
    "+PXVBV6YV58d55HdBj9yeA8no+NtN66hQ3QKGmay+dtxn0+7+BQ2VfigrZsx/zMdHh7yf86whJd6E3j4q8Af/iD85c/B0/8CnvxHKAo+di3snIGTN0PtAODc"
    "bbDcg3ueDICi4MUOPPo14TXeA07eBMOSfwUAKg+UE0wDly0vwe9+D/zZT6Eo+LpHwEu/KbzEG8L2KcgG2WB1wP9mn/m1u7wwd94zcb8Xe0RHV8Uv/e6SYTRd"
    "Fc+tJRweJRuLoBYus+GhN1fe/o03+KGfP+QnfuWId3+rTWwue9rtI5/5tbu8MHfeM/Gf4SW7bb70xCP5vEtP485pzdOmJTOCh9YFH7X9IF5pdozByf0ErJ38"
    "4vIcAC/Tb2NAPKeZgi8+/kg+9MLj+ab92/m7cZ9Xn53gYXXBvW3gz4c9fn55lhnBZx9/GI+sGzTM/0Sf8AmfQK2VaZqotTJNE7VWpmmi1so0TfyvNA0w34I3"
    "+gh41XeFp/wRPP538Z3/AIe7cLgLdz4eZcNRICoALHbgQS+NH/Oa8PBXge1TMA0wDfwrAVB5fi7cAT/0SXDxLjhxA36ld4BXf3eYBmgjDEv+r/jWHz2iOGkK"
    "ipOmoDhpCoqTyeZ+N1xTeKNXn/Pzv73k1/9gxZu81oLn9nt/vuKbf3Cfj3mfHV7ppWbcrzV4t7fc5Id+/pBf/r0l7/aWm9zvrvsa3/qjRxQnTUFx0hQUJ01B"
    "cTLZ/GdomFeaHeNXrnlZ/nY8AGBThYfVBSsng5P7CRgxP3J0L3857HFdmfE2G9cgnpeBG8uMbzz5GL5y/1b+ar3P7692uV+VeGhd8FHbD+KVZscYnPxP9c3f"
    "8q38n+WEYQnzTXjJN4aXeXOIAvc9DZb7APj2v4ObX4LLFttwzUMhG7QJcoJhyb8RAJXndu9T4fs/Fq0P8aNfE978E2DrJKwO+L/oSz9+hxfmKbdNfMsP7wNg"
    "w7u8+SY//9tLvuhbL/H6rzanrwLB1OAZd0689QedZbJ53VeZ80ovNeOB3uBVF3RV/MnfrHmgh95c+ZB32eaFecptE9/yw/v8ZxicDMAj6wb3O3TjgQQcuPGT"
    "R/fxTfu3s6nCx+08iFPRYZ6/hrmxzPiGE4/hT4dLfPSFJ/I2G9dwTel5WN3g1WfHWTkZnPxP9tVf9ZX8Sz7qoz6K/9VsaCO0kcuOXQfHrweAGx8D2bjMhtUB"
    "/0EAqDzQ+gh+/DPR+hC/4tvDm34MjCuYBv5NJLB5DhLY/E/x0e+9wwvzu3+24lt+eJ/7vf0bb/JF37LHX/7DwK//wYrXf7U5rcEf/82aN//A++iq+LB32ubD"
    "332HYTQPNE7mJR7V8Zf/MPBAN1xT+ej33uGF+d0/W/EtP7zPf6aGeX4MHLrxOZeeyu+vdtlU4bOPP4zXnJ2gYV6YhrnkiV9YnuPQjZvqnLffuJZmc+jG/wYf"
    "+ZEfyb/koz7qo/g/xQnmimz8JwGg8kD3PBkAv8Qbwpt+DAxL/s3aANME3QyiAMC4hmww2+B/irMXGi/Mhd3kgcbJfMoH7fDen3yeT/ryi3z8xWP8zRMGvuNH"
    "DtjeDN7zrTf5us84ydHKPD8b8+D5OXuh8cJc2E3+qwlYO3nydMRn7D6Ve9qa68qMbzz5GG4sMxrmXyLg74YDfvroPl623+FtFtcwOLnqKgCAynPbPgVv/nEw"
    "LPl3+btfhz//aXiTj4EbHwMAv/r1cPcT4f2/lf8p3vB97uOFOVolz+3t33iTP/yrNd/704d86Gefp0osNsSXf9IJ3vfttzhamQcSkIY77238zRMGuioe6G+e"
    "MPCG73MfL8zRKvmvtnbys8uzfO3+bQC8+vw4X3z8EXQEDfOiWDs5UzpefX6cj9y+hYb5n6a1xu7uLs9tmib+NaZp4vz58zy348ePU0rhqucBQOW5vdb7Qun5"
    "d9s7C/c8GaY1z3LhdrjnyfxP8oy7Jv61xsl89aed5E1ea8HfPH7g9InCm732gmtOFdaDeSAJhtE84Wkj7/Zx59g/TD7onbcRYK5Yrc0z7pp4YVZD4b/aNx3c"
    "wY8c3sN1ZcZ7b93A229cy+ikYV4UBr7p4A6ePB7x1ScfxYygYf6n+e3f/m1e//Vfn+dHUXhRKAq///u/z+nTp3luv/7rv87rvd7rcdXzAKDy3B7+Svx/svdX"
    "N/PC/M6frnntd7+H57YezOu9ypw3fLUFANNkhtHcT4LWYLkyX/O9e3zZt+8B8OovN+cLPuY45tle8xVm/Pr3XMsL8zt/uua13/0e/iv9yOE9AHzGsYfy0v02"
    "g5MXlYCnTkt+5PAeHtltsHZSJf4nKqUAcOrUKV77tV+bYRgA6PueYRj4l9jmbd/mrRmGAYC+7xmGgT/8wz/k/PnzlFK46vkCoPIAioK3T4PN/xeHR+aFWa7M"
    "C9IatGaen+XK/MU/DHzKV1zkL/9hYHsz+NB32+aLPvY4y7WxeZb1AIdH5oVZrsx/tXffvJ7vP7ybz7v0NN5scZq32biGk9HxohgxP788y6YKn7DzYDZV+J/u"
    "pV7qpfjxH/9x/rUk8eM//uM8t9d7vdfjN3/zN7nqBQKg8gB+0EuDzVX/duNk7j2XfNE3X+J7fvqAropXf7k5X/Rxx3mll5pxtDL/GwxOXn62w/cf3s09bc33"
    "HN7F+Rz5xJ0H86L4zeUF3mPzel5ldoyX7LZpmP+pIgKAzOQ/UmYCEBFc9XwBUHmgnPg3ywZR+P9snMxXfdc+X/Qtlxgnc8M1hU/+oGN8+LttsxrMMJr/LX7k"
    "6F6+af92AF59fpxP3nkIZ6JnwogX7kKOfP7e0zgdPT9z5qVpmP8phmGgtcYDrVYr/jOtViuWyyUPVEqh73v+nwOg8h/hwh1w4Q648TGwOMb/V1/zPft87jfs"
    "0lXxQe+8zRd8zHE2N8TRyvxvImBwcl2ZcU9b81frfX5heY732Lwe8cIZ+OK9pzPZfPj2zTTM/yTf/d3fzXd8x3fwQIeHh/xn+tiP/Vg2Nzd5oPd7v/fjAz/w"
    "A/kvk43LovA/CACV/wh/88vwBz8A7/FV8KCX5v+rr/jOPQC+/jNP8i5vvkkaWuN/HQMfuHUT15cZn3/paaxJFgoEmBdudPLgsoA5vMH8FA3zP8ldd93Fn/7p"
    "n9J1HQ+0ublJ3/f8R+r7ns3NTZ70pCdxv6klzsabvMmb8F/m4Dw846/h4t3woJeEG18MovA/AACV/wil4yp42zfY4Ht++oAP+szzfMuP7PPeb7vFW7/egpPH"
    "C//bPKMt+fxLTwPg7Tau5Z02rqNhXpgLOfK1+7fxHps38Jhuk8HJ/zSSAPiar/kaXuZlXoYH2tnZ4T/SV33VV7G3t8cDfed3fiff9m3fhiT+S9z5ePi+j4Zp"
    "4Fle5s3hTT4aovDfDIDKVf9uNtx7vnHdNQWAKvF3Txz5rT9ecf3pwpu81oL/bf54fYkqMdnc2VY0zAtj4NsP7uRXl+e5ucx5WF3wP9mLvdiL8cqv/Mr8Z3rs"
    "Yx/Lc/uVX/kV/sssL8H3fTRMA8/hr34eHvzS8GKvx38zACr/W632+Z/i75888HYffpa77msAPPrhHb/6Xddw4lhgQyb/YxgQ/7J32LiWk9HxvYd3ATBhKuL5"
    "MfC0acnPL8/yyG6DD9y6iYb597i7rfmP0FrjgWwDkJn8d7JNa4372ebfQxLP476nwzTwfD3lT+HFXo//ZgBUHugZf8P/Gvc+DQBJ/HvYIPEvOnMysEHiebzk"
    "o3q+4GNO8BXfucffP3ng75888Kg3uos//rHreNANlX/J7l5ig8QLdGG3ASCJfwtJANzZVtxU5vxLDLze/CTfe3gXv7/a5fMvPY3PPvYwnh9jZhJfdPwRXFdm"
    "JObf6+625t9DEgDf8R3fwYMf/GDu97SnPY3/CZ72tKfxG7/xG9zvr//6rwE4deoU/xYv/dIvzW/91m/xIhtX/Je5+0kAcO1DeZbduwCQROWZXvd1X5ff/M3f"
    "hN274PgN/I938S4AXud1Xod/i4gA4K8fP/Ayj+15QX77T1cAvMbLz5B4vmx4g1eb8x0/fgBAlfiY997hwTdWbF4gG172xXr+8h8Gnn7HxENvrrwgv/vnawAe"
    "8pCH8G/x4Ac/GIC/HPa5aTHnRdEwD64LnjQeceu05Pkx8M37d/BTR/fxpSceycPrAvPvY+CJ0yEAJ0+e5N/itV7rtfit3/otPuiDPogHqhKKQkTw3yEiUBS+"
    "7/u+j+/7vu/jub3US70U/xbHjh3jsrufBA96aQC47pGw2IHlHs/jMa/J82gD/+Fu/Wv43e+BxQ48+OV4lnufCsBLv/RLU3mml3u5l+M3f/M34a9+EV7n/fkf"
    "bXkJnvj7KAov9VIvxb/FW73VW/Gbv/mbfP/PHvIyj+15fi7sNr7xB/apEm/6Whu8MJ/wpRf5/b9YAfDGrzXnw95tG5sXSoI3f+0N/vIfBr7oWy7xrZ93Conn"
    "kQk/95tHKAqv9Vqvxb/Fa7/2a/Nd3/Vd/OryPG++OE0gXhgDn3vpafzq8jybKnz+8YeTGCHEs93ZVnz/4d1cV2Y8stvA/Pvd2Vb8/moXgNd5ndfh3+JlXuZl"
    "AHjIQx7C+73f+/HcHvSgB/Hf4bVe67X4vM/9HJ7bV33VV3H+/Hle67Vei3+Lt33bt+WzP/uz4cl/CK/8jgAw24A3/kj0M1/EA/lBLw0v/vpg8xy+6I34z6Ao"
    "+M0+FmYbPMsTfh+Al37pl6byTB/2YR/Gl3/FV+I/+yl45XeAxTH+x/rjH4PhiPf/gA/g2LFj4t/grd7qrfjoj/lYfvDnDvmwd9vmoTdXHqglfOhnX2D/MHmv"
    "t97i5usK5gV77Vec8zt/uuK+s8ldZxstDYh/yYe92zZf9d17/NDPH/Jub7nJa7/inAey4eu+b4+77mu84Ru+IQ960IPEv8Fbv/Vbc+rUKf7y/Hl+fnmOt1ic"
    "QTx/An52eZZfXZ4H4NCNdzz7tzyy2+Azjj2Mh9UFAALOtpFNFT5u50FsqvDvlZiv3b8NgFd8xVfkJV7iJcS/wVu+5Vvq1KlTfvrTn84rv/Ir83qv93r8T/Ca"
    "r/mavOZrviYP9Bu/8RucP3+ehz/sYbzma76m+Dd4iZd4CT38YQ/zU576N/D0v4CHvByXvfjr42seBk/8fS47cT28xBtCNp7HQ16O/3DHr8cv+QZwy0uBDRLc"
    "8Tj4u1/l1KlTvOd7vqcqz/SgBz1I7/1e7+nv/p7vxT/+WfBuXwFR+B/n6X8Bf/ADKAqf9mmfxr/Vgx70IH38x32sv+zLvox3/dhz/PJ3XMPxnQDAhh/42UN+"
    "/reXbG8GX/qJJzAv3Lu/1Sbv8VabPOOuiWPbwc5W8KLY3BDf+YWneJePPse7fMw5/uhHr+fBNxYAbPjrxw988bfuAfDlX/7l/Fvt7Ozooz7qo/yZn/mZfN3e"
    "bbxYt8VD6pxAPLfb24q/HPZ4oPfbupF33ryOTRXuZ+Bl+21+89qXJzHm3ycxn3vpafz+aheAL/uyL+Pf4yu/8it5r/d6L97rvd6Lv/mbv+HkyZNI4n8K2/zl"
    "X/4lb/VWb4Wi8BVf+ZX8e3zFV34lb/VWbwU/9pnwXl8L1z4MbDjzYLjmITxLNp6vd/9K/uMZbLC57GgXfumrAPjMz/xMAIIH+Kqv+ipe6iVfAp7xN/ADHwe7"
    "d/E/yh//KPzAxwPw3d/1nTzoQQ8S/w6f9mmfxku/9Evz908eeOP3u49b72zY8Pt/seZDP/s8isKPf90ZNjfEv8SGNNx8fWVnK/jXeJPXWvBub7XJ/mHyzh99"
    "lt29BOBnf+OIN/2A+9g/TD77sz+bl3iJlxD/Dp/xGZ+h13md1+HQjfc493d818FdHLphwIABA48bDgF4ZLfBA22oYMCAAQMJTJgEDBgwYMCAAQMGDBgwYMCA"
    "ueKOtuIjLzyRX12eB+BjPuZjeM3XfE3x7/Ce7/meep/3eR/uvPNOXuEVXoHf/u3fxja2+e9kG9v85m/+Jm/0Rm/E4eEh7/1e78lbvuVbin+Ht3zLt9T7vM/7"
    "wHAE3/OR8Iy/5llssMHmBXKCE5zgBCc4wQlOcIITnOAEJzjBCU5wghOc4AQn2FyWDe59Knznh8I9T+bt3u7t+MiP/EgBVB7g2LFj+u3f/m2/9mu/Nn/9138N"
    "3/oB8KhXh0e/Ohy/Ac48GKLwX+oZfw13Pwn+4mfg4l0AfM/3fA/v+Z7vKf6djh07pt/+7d/2a7/2a/PXf/3XvPib3cm7vPkmP/9bSwC++bOP86ovM8PmP903"
    "fPZJ/uIfBv7+yQNv/H73cXwn+P2/WAHw2Z/92XzWZ32W+A/wUz/1U7zN27wNv/Vbv8V3HNzJDx/ew6vNj3NzmfNAN5c5N5c5L9Pv8COH9/BXwz7feXAn/xn+"
    "atjnL4c97vc+7/M+fOVXfqX4D/Cd3/mdAvxd3/VdvO7rvi5v8RZvwdu//dtz44038rqv+7r8V/vt3/5tnv70p/M93/M9/O7v/i4A7/M+78N3fud3iv8A3/md"
    "3ynA3/Vd3wXf9zHwoJeCl3hDOHEDPOil+S9371Ph7ifCE38fnvxHALzd270d3/7t3w4AgGzz3HZ3d/3VX/3VfP3Xfz3nz5/nf4rXfu3X5rM+67N47dd+bfEf"
    "7LM/+7P99V//9Zw/f57/KR7ykIfwnd/5nbz2a7+2+A/2lV/5lf7CL/xCzp8/z/8UD3nIQ/jqr/5q3vIt31L8B/ve7/1ef/ZnfzZPf/rT+Z/iIQ95CF/91V/N"
    "W77lW4r/YD/7sz/rj/7oj+bpT386/1OcOnWKz/zMz+QjP/IjxRUA/CNaJqcBOndSYQAAAABJRU5ErkJggg==";

static const QImage &tribesGraphicAtlas() {
    static const QImage atlas = QImage::fromData(
        QByteArray::fromBase64(QByteArray(k13MoonTribesGraphicBase64)), "PNG");
    return atlas;
}

// Maps standard seal order (Dragon, Wind, Night, ... Sun) to the matching tile
// positions in the supplied 13moon.com tribes graphic.
static int sealSpriteIndex(int sealIndex) {
    static const int map[20] = {
        1,  // Dragon
        6,  // Wind
        19, // Night
        0,  // Seed
        5,  // Serpent
        10, // Worldbridger
        7,  // Hand
        8,  // Star
        9,  // Moon
        2,  // Dog
        3,  // Monkey
        16, // Human
        13, // Skywalker
        14, // Wizard
        11, // Eagle
        17, // Warrior
        12, // Earth
        18, // Mirror
        15, // Storm
        4   // Sun
    };
    return (sealIndex >= 0 && sealIndex < 20) ? map[sealIndex] : 0;
}

static void drawSealSymbol(QPainter &p, const QRectF &target, int sealIndex,
                           const QColor &unusedColour = QColor(), qreal unusedScale = 1.0) {
    Q_UNUSED(unusedColour)
    if (sealIndex < 0 || sealIndex >= 20 || target.width() <= 0 || target.height() <= 0) return;
    const QImage &atlas = tribesGraphicAtlas();
    if (atlas.isNull()) return;

    const int sprite = sealSpriteIndex(sealIndex);
    const int cols = 5;
    const int rows = 4;
    const qreal cellW = atlas.width() / qreal(cols);
    const qreal cellH = atlas.height() / qreal(rows);
    const int col = sprite % cols;
    const int row = sprite / cols;

    // Tight crop around each rounded tile from the supplied sprite sheet.
    QRectF srcRect(col * cellW + cellW * 0.07,
                   row * cellH + cellH * 0.03,
                   cellW * 0.86,
                   cellH * 0.92);

    qreal size = qMin(target.width(), target.height()) * qMax<qreal>(0.70, qMin<qreal>(1.25, unusedScale));
    QRectF dest(target.center().x() - size / 2.0,
                target.center().y() - size / 2.0,
                size,
                size * (srcRect.height() / srcRect.width()));
    if (dest.height() > target.height()) {
        const qreal scale = target.height() / dest.height();
        dest.setWidth(dest.width() * scale);
        dest.setHeight(target.height());
        dest.moveCenter(target.center());
    }

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(dest, atlas, srcRect);
    p.restore();
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
    const QRectF symbolRect(r.center().x() - sealSize / 2,
                            r.top() + toneH + pad, sealSize, sealSize);
    drawSealSymbol(p, symbolRect, sealIndex, toneColour);
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

// High-contrast self-contained emblem used by the picker and settings.
class KinPreview : public QWidget {
public:
    explicit KinPreview(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(152, 190);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    void setKin(int kin) { m_kin = kin; update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#373737"));
        p.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2), 13, 13);
        if (m_kin > 0) {
            // Large glyph at a guaranteed width, never a tiny full-width icon.
            const qreal emblemWidth = qMin<qreal>(142, width() - 16);
            const QRectF emblem(rect().center().x() - emblemWidth * 0.5,
                                6, emblemWidth, qMin<qreal>(height() - 12, 174));
            drawKinEmblem(p, emblem, m_kin, QColor("#ffffff"), true);
        } else {
            p.setPen(QColor("#bababa"));
            setPainterFont(p, 16);
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, "Choose your birth date");
        }
    }
private:
    int m_kin = 0;
};

// One obvious birth-date picker, accessible from the widget itself AND settings.
// Changes are only committed after Save to avoid the host rebuilding while the
// user is still typing a year or browsing the calendar.
class GalacticSignaturePicker : public QDialog {
public:
    GalacticSignaturePicker(const QDate &savedDate, bool afterNoon, QWidget *parent = nullptr)
        : QDialog(parent) {
        setWindowTitle("My Galactic Signature · Tzolkin");
        setModal(true);
        setWindowFlags(Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
        setAttribute(Qt::WA_ShowWithoutActivating, false);
        resize(560, 690);
        setMinimumWidth(340);
        setStyleSheet("QDialog { background:#282828; }"
                      "QLabel { color:#f4f4f4; background:transparent; }"
                      "QPushButton { background:#414141; color:white; border:1px solid #777;"
                      " border-radius:10px; padding:9px 15px; font-size:18px; }"
                      "QPushButton:hover { background:#555; }");
        QVBoxLayout *layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 16, 20, 16);
        layout->setSpacing(12);

        QLabel *heading = makeLabel("Choose your Gregorian date of birth", this);
        heading->setWordWrap(true);
        heading->setStyleSheet("color:white; font-size:23px; font-weight:bold;");
        layout->addWidget(heading);

        m_dateEdit = new QDateEdit(this);
        m_dateEdit->setDisplayFormat("dd/MM/yyyy");
        m_dateEdit->setCalendarPopup(true);
        m_dateEdit->setMinimumDate(QDate(1700, 1, 1));
        m_dateEdit->setMaximumDate(QDate::currentDate());
        m_dateEdit->setDate(savedDate.isValid() ? savedDate : QDate(2000, 1, 1));
        m_dateEdit->setStyleSheet(
            "QDateEdit { background:#404040; color:white; border:2px solid #888;"
            " border-radius:10px; padding:9px; min-height:40px; font-size:22px; }"
            "QDateEdit::drop-down { width:48px; border:none; }"
            "QDateEdit::down-arrow { image:none; }");
        layout->addWidget(m_dateEdit);

        m_calendar = new QCalendarWidget(this);
        m_calendar->setFirstDayOfWeek(Qt::Monday);
        m_calendar->setGridVisible(false);
        m_calendar->setVerticalHeaderFormat(QCalendarWidget::NoVerticalHeader);
        m_calendar->setMinimumDate(QDate(1700, 1, 1));
        m_calendar->setMaximumDate(QDate::currentDate());
        m_calendar->setSelectedDate(m_dateEdit->date());
        m_calendar->setMinimumHeight(225);
        m_calendar->setStyleSheet(
            "QCalendarWidget { background:#343434; color:#fff; }"
            "QCalendarWidget QWidget { alternate-background-color:#383838; }"
            "QCalendarWidget QAbstractItemView { background:#353535; color:#fff;"
            " selection-background-color:#278052; selection-color:#fff; font-size:17px; }"
            "QCalendarWidget QToolButton { background:#414141; color:#fff;"
            " font-size:18px; border:none; padding:6px; }"
            "QCalendarWidget QSpinBox { background:#414141; color:#fff; font-size:18px; }");
        layout->addWidget(m_calendar, 1);

        m_leapChoice = new QComboBox(this);
        m_leapChoice->setStyleSheet(boxStyle());
        m_leapChoice->addItem("29 February: born before noon (use 28 Feb)", false);
        m_leapChoice->addItem("29 February: born after noon (use 1 Mar)", true);
        int choice = m_leapChoice->findData(afterNoon);
        m_leapChoice->setCurrentIndex(choice < 0 ? 0 : choice);
        m_leapChoice->setVisible(isHunabKu(m_dateEdit->date()));
        layout->addWidget(m_leapChoice);

        QHBoxLayout *previewRow = new QHBoxLayout;
        m_art = new KinPreview(this);
        m_art->setFixedSize(152, 190);
        previewRow->addWidget(m_art);
        m_name = makeLabel(QString(), this);
        m_name->setWordWrap(true);
        m_name->setStyleSheet("QLabel { color:#ffffff; font-size:19px; font-weight:bold; }");
        previewRow->addWidget(m_name, 1);
        layout->addLayout(previewRow);

        QDialogButtonBox *buttons = new QDialogButtonBox(
            QDialogButtonBox::Save | QDialogButtonBox::Cancel, Qt::Horizontal, this);
        if (QPushButton *save = buttons->button(QDialogButtonBox::Save)) {
            save->setText("Save my Galactic Signature");
            save->setStyleSheet("QPushButton { background:#256b4f; color:white; border:none;"
                                " border-radius:10px; font-size:18px; padding:12px; }");
        }
        layout->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(m_calendar, &QCalendarWidget::selectionChanged, this, [this]() {
            if (m_dateEdit->date() != m_calendar->selectedDate())
                m_dateEdit->setDate(m_calendar->selectedDate());
        });
        connect(m_dateEdit, &QDateTimeEdit::dateChanged, this, [this](const QDate &d) {
            if (m_calendar->selectedDate() != d) m_calendar->setSelectedDate(d);
            m_leapChoice->setVisible(isHunabKu(d));
            refresh();
        });
        connect(m_leapChoice, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { refresh(); });
        refresh();
    }

    QDate birthDate() const { return m_dateEdit->date(); }
    bool leapAfterNoon() const { return m_leapChoice->currentData().toBool(); }
    int selectedKin() const { return kinForBirthDate(birthDate(), leapAfterNoon()); }
private:
    void refresh() {
        const int kin = selectedKin();
        m_art->setKin(kin);
        m_name->setText(kinTitle(kin) + "\n" + kinDetailLine(kin) +
                        "\n\nThe tone is shown ABOVE the solar-seal glyph.");
    }
    QDateEdit *m_dateEdit = nullptr;
    QCalendarWidget *m_calendar = nullptr;
    QComboBox *m_leapChoice = nullptr;
    KinPreview *m_art = nullptr;
    QLabel *m_name = nullptr;
};

static void saveBirthSignature(const QString &id, const QDate &date, bool afterNoon) {
    QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
    const QString prefix = "Widget-" + id + "/";
    cfg.setValue(prefix + "BirthDate", date.toString(Qt::ISODate));
    cfg.setValue(prefix + "BirthLeapAfterNoon", afterNoon);
    cfg.setValue(prefix + "MyKin", kinForBirthDate(date, afterNoon));
    cfg.sync();
}

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

        // Today is a deliberately compact standalone layout.  Do not draw
        // the full-calendar header, navigation, or footer over it: at small
        // sizes those elements previously crowded out readable information.
        if (m_mode == ViewToday) {
            drawTodayView(p);
        } else {
            drawHeader(p);
            if (m_mode == ViewFull) drawFullView(p);
            else drawWeekView(p);
            drawFooter(p);
        }
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
        // Today contains no tiny or invisible navigation controls.  Use the
        // normal widget settings to change the saved Galactic Signature.
        if (m_mode == ViewToday) { e->accept(); return; }
        if (signatureButtonRect().contains(pt)) {
            openSignaturePicker();
        } else if (prevRect().contains(pt)) {
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

    QRectF signatureButtonRect() const {
        const double u = scaleUnit();
        return QRectF(18 * u, 66 * u, qMax<qreal>(120, width() - 148 * u), 32 * u);
    }

    void openSignaturePicker() {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
        const QDate saved = QDate::fromString(
            cfg.value("Widget-" + m_id + "/BirthDate", "").toString(), Qt::ISODate);
        const bool afterNoon = cfg.value("Widget-" + m_id + "/BirthLeapAfterNoon", false).toBool();
        // A normal WM-managed dialog must NOT inherit the desktop overlay's
        // override-redirect / no-focus window behaviour.
        GalacticSignaturePicker picker(saved, afterNoon, nullptr);
        picker.setWindowModality(Qt::ApplicationModal);
        if (picker.exec() != QDialog::Accepted) return;
        m_myKin = picker.selectedKin();
        update();
        // One atomic config write; the host will reload this widget automatically.
        saveBirthSignature(m_id, picker.birthDate(), picker.leapAfterNoon());
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
        const QString sigButton = m_myKin > 0
            ? QString("✦ My Kin %1 · Change birthday").arg(m_myKin)
            : "✦ Set my Galactic Signature";
        paintRoundedButton(p, signatureButtonRect(), sigButton, m_accentColour,
                           QColor(m_accentColour.red(), m_accentColour.green(), m_accentColour.blue(), 34), int(12 * u));
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
            p.drawText(QRectF(r.x() + 28 * u, r.y() + r.height() * 0.38, r.width() - 30 * u,
                              r.height() * 0.42),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::fromLatin1(kSealNames[row]));
            const qreal sz = qMin<qreal>(r.height() - 4, 24 * u);
            drawSealSymbol(p, QRectF(r.x() + 2 * u, r.center().y() - sz * 0.5, sz, sz),
                           row, readableTextFor(bg), 0.95);
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

                // Mayan tone above the crisp vector solar-seal glyph, Kin below.
                const QColor text = kin == todayKin ? m_accentColour : m_textColour;
                const qreal toneH = inner.height() * 0.26;
                const qreal kinH = inner.height() * 0.23;
                const qreal glyphH = qMax<qreal>(5.0, inner.height() - toneH - kinH - 2);
                drawToneGlyph(p, QRectF(inner.x() + inner.width() * 0.16,
                                       inner.y() + 1, inner.width() * 0.68, toneH),
                              ((kin - 1) % 13) + 1, text);
                const qreal glyphW = qMin(inner.width() - 3, glyphH);
                const QRectF dst(inner.center().x() - glyphW * 0.5,
                                 inner.y() + toneH + 1, glyphW, glyphW);
                drawSealSymbol(p, dst, (kin - 1) % 20, text, 0.92);
                p.setPen(text);
                setPainterFont(p, int(qBound<qreal>(7, kinH * 0.78, 13)), true);
                p.drawText(QRectF(inner.x(), inner.bottom() - kinH,
                                  inner.width(), kinH),
                           Qt::AlignCenter, QString::number(kin));
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

    // Compact, resizable Today view: only the Gregorian date, the daily Kin
    // (number + coloured solar seal), and its Galactic tone (Mayan dots/bars).
    // Font sizes are calculated independently of the large full-calendar
    // scaleUnit(), with a readable 16px floor at 320x240 and above.
    void drawTodayView(QPainter &p) {
        const QRectF bounds = QRectF(rect()).adjusted(1, 1, -1, -1);
        const qreal w = bounds.width();
        const qreal h = bounds.height();
        if (w <= 0 || h <= 0) return;

        const int kin = kinForDate(m_focusDate);
        const qreal margin = qBound<qreal>(9, qMin(w, h) * 0.045, 26);
        const QRectF inner = bounds.adjusted(margin, margin, -margin, -margin);
        const bool portrait = inner.height() > inner.width() * 1.10;
        const qreal k = qBound<qreal>(1.0, qMin(w / 340.0, h / 270.0), 2.15);
        const int dateSize = qBound(17, int(19 * k), 34);
        const int kinSize = qBound(30, int(35 * k), 65);
        const int nameSize = qBound(16, int(18 * k), 32);
        const int toneSize = qBound(16, int(18 * k), 33);
        const QColor text = m_textColour;
        const QColor accent = kin > 0 ? sealColour((kin - 1) % 20) : m_accentColour;

        // Keep date and Kin text readable even when horizontal space is tight.
        auto fitSingleLine = [&](const QString &value, QRectF box, int desired,
                                 int floorSize, const QColor &colour, bool bold) {
            QFont font = p.font();
            font.setFamily("Sans Serif");
            font.setBold(bold);
            int px = desired;
            while (px > floorSize) {
                font.setPixelSize(px);
                if (QFontMetrics(font).horizontalAdvance(value) <= box.width() - 4) break;
                --px;
            }
            font.setPixelSize(px);
            p.setPen(colour);
            p.setFont(font);
            p.drawText(box, Qt::AlignCenter | Qt::TextSingleLine, value);
        };

        const QString dateLabel = m_focusDate.toString("ddd d MMM yyyy");
        if (kin == 0) { // 29 February, 0.0 Hunab Ku
            fitSingleLine(dateLabel, QRectF(inner.left(), inner.top(), inner.width(), 36 * k),
                          dateSize, 16, text, true);
            fitSingleLine("0.0 Hunab Ku", QRectF(inner.left(), inner.center().y() - 42 * k,
                                                inner.width(), 84 * k),
                          kinSize, 24, m_accentColour, true);
            return;
        }

        const int tone = ((kin - 1) % 13) + 1;
        const int sealIndex = (kin - 1) % 20;
        const QString title = QString("Kin %1").arg(kin);
        const QString name = signatureShort(kin);
        const QString toneLabel = QString("Tone %1 · %2")
                                      .arg(tone)
                                      .arg(QString::fromLatin1(kToneNames[tone - 1]));

        // Portrait: date, Kin, coloured seal and Mayan tone, tone name.
        if (portrait) {
            const qreal dateH = 36 * k;
            const qreal kinH = 51 * k;
            const qreal nameH = 47 * k;
            const qreal toneLabelH = 32 * k;
            fitSingleLine(dateLabel, QRectF(inner.left(), inner.top(), inner.width(), dateH),
                          dateSize, 16, text, true);
            const qreal ky = inner.top() + dateH + 4 * k;
            fitSingleLine(title, QRectF(inner.left(), ky, inner.width(), kinH),
                          kinSize, 27, accent, true);

            // The longer signatures wrap over two lines; they never shrink
            // down to illegible 9px text to fit a single line.
            p.setPen(text);
            setPainterFont(p, nameSize, true);
            p.drawText(QRectF(inner.left() + 4, ky + kinH, inner.width() - 8, nameH),
                       Qt::AlignCenter | Qt::TextWordWrap, name);

            const qreal artTop = ky + kinH + nameH + 6 * k;
            const qreal artBottom = inner.bottom() - toneLabelH - 7 * k;
            const qreal artH = qMax<qreal>(14, artBottom - artTop);
            const qreal artW = qMin(inner.width() * 0.77, artH * 0.89);
            const QRectF art(inner.center().x() - artW / 2, artTop, artW, artH);
            const qreal markH = art.height() * 0.27;
            drawToneGlyph(p, QRectF(art.center().x() - art.width() * 0.32,
                                    art.top(), art.width() * 0.64, markH),
                          tone, text);
            const qreal sealSize = qMin(art.width(), art.height() - markH - 3);
            drawSealSymbol(p, QRectF(art.center().x() - sealSize / 2,
                                     art.top() + markH + 3, sealSize, sealSize),
                           sealIndex);
            fitSingleLine(toneLabel, QRectF(inner.left(), inner.bottom() - toneLabelH,
                                           inner.width(), toneLabelH),
                          toneSize, 16, text, true);
            return;
        }

        // Compact / landscape: date across the top, with the solar seal and
        // Mayan number on the left and large Kin / tone text on the right.
        const qreal dateH = qBound<qreal>(30, 34 * k, 52);
        fitSingleLine(dateLabel, QRectF(inner.left(), inner.top(), inner.width(), dateH),
                      dateSize, 16, text, true);
        const qreal gap = qBound<qreal>(6, 10 * k, 20);
        const QRectF body(inner.left(), inner.top() + dateH + gap,
                          inner.width(), qMax<qreal>(0, inner.height() - dateH - gap));
        const qreal leftW = body.width() * 0.43;
        const QRectF emblemArea(body.left(), body.top(), leftW, body.height());
        const QRectF words(body.left() + leftW + 6, body.top(),
                           qMax<qreal>(0, body.width() - leftW - 6), body.height());
        const qreal markH = qBound<qreal>(25, emblemArea.height() * 0.25, 56);
        drawToneGlyph(p, QRectF(emblemArea.center().x() - leftW * 0.32,
                                emblemArea.top(), leftW * 0.64, markH), tone, text);
        const qreal sealSize = qMax<qreal>(0,
            qMin(leftW - 6, emblemArea.height() - markH - 4));
        drawSealSymbol(p, QRectF(emblemArea.center().x() - sealSize / 2,
                                 emblemArea.top() + markH + 4,
                                 sealSize, sealSize), sealIndex);

        const qreal kinH = qBound<qreal>(38, words.height() * 0.30, 66);
        const qreal toneH = words.width() < 225
            ? qBound<qreal>(42, words.height() * 0.27, 60)
            : qBound<qreal>(30, words.height() * 0.21, 54);
        const qreal nameH = qMax<qreal>(0, words.height() - kinH - toneH - 4);
        fitSingleLine(title, QRectF(words.left(), words.top(), words.width(), kinH),
                      kinSize, 26, accent, true);
        p.setPen(text);
        setPainterFont(p, words.width() < 210 ? 16 : nameSize, true);
        p.drawText(QRectF(words.left(), words.top() + kinH,
                          words.width(), nameH),
                   Qt::AlignCenter | Qt::TextWordWrap, name);
        const QRectF labelBox(words.left(), words.bottom() - toneH,
                              words.width(), toneH);
        QFont toneFont = p.font();
        toneFont.setPixelSize(words.width() < 225 ? 16 : toneSize);
        toneFont.setBold(true);
        p.setFont(toneFont);
        p.setPen(text);
        p.drawText(labelBox, Qt::AlignCenter | Qt::TextWordWrap, toneLabel);
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
        v->setContentsMargins(22, 18, 22, 22);
        v->setSpacing(15);
        v->setAlignment(Qt::AlignTop);

        QLabel *title = makeLabel("My Galactic Signature", this);
        title->setStyleSheet("color:white; background:transparent; font-size:25px; font-weight:bold;");
        v->addWidget(title);

        m_savedDate = QDate::fromString(
            osmWidgetGet(m_id, "BirthDate", "").toString(), Qt::ISODate);
        m_afterNoon = osmWidgetGet(m_id, "BirthLeapAfterNoon", false).toBool();
        m_currentKin = m_savedDate.isValid()
            ? kinForBirthDate(m_savedDate, m_afterNoon)
            : qBound(0, osmWidgetGet(m_id, "MyKin", 0).toInt(), 260);

        // The main action is immediately visible at the top of settings.
        m_birthButton = new QPushButton("Choose my birthday  ·  Open Gregorian calendar", this);
        m_birthButton->setMinimumHeight(64);
        m_birthButton->setStyleSheet(
            "QPushButton { color:white; background:#256b4f; border:2px solid #67ac8c;"
            " border-radius:13px; font-size:19px; font-weight:bold; padding:11px; }"
            "QPushButton:hover { background:#317e5e; }");
        v->addWidget(m_birthButton);

        m_birthStatus = makeLabel(QString(), this);
        m_birthStatus->setStyleSheet("color:#d0d0d0; background:transparent; font-size:18px;");
        m_birthStatus->setWordWrap(true);
        v->addWidget(m_birthStatus);

        QHBoxLayout *resultRow = new QHBoxLayout;
        resultRow->setSpacing(15);
        m_kinArt = new KinPreview(this);
        m_kinArt->setFixedSize(160, 190);
        resultRow->addWidget(m_kinArt);
        m_preview = makeLabel(QString(), this);
        m_preview->setStyleSheet("color:white; font-size:18px; font-weight:bold; background:transparent;");
        m_preview->setWordWrap(true);
        resultRow->addWidget(m_preview, 1);
        v->addLayout(resultRow);

        // Manual selection is optional; existing users can keep using it.
        v->addWidget(makeLabel("Manual Kin selection (optional)", this));
        m_myKin = new QComboBox(this);
        m_myKin->setStyleSheet(boxStyle());
        m_myKin->setMaxVisibleItems(14);
        m_myKin->addItem("Not set", 0);
        for (int kin = 1; kin <= 260; ++kin)
            m_myKin->addItem(kinTitle(kin), kin);
        selectByData(m_myKin, m_currentKin);
        m_myKin->setEnabled(!m_savedDate.isValid());
        v->addWidget(m_myKin);

        m_clearBirth = new QPushButton("Clear saved birth date / enable manual selection", this);
        m_clearBirth->setStyleSheet(
            "QPushButton { color:white; background:#404040; border:1px dashed #888;"
            " border-radius:10px; font-size:17px; padding:10px; min-height:42px; }"
            "QPushButton:hover { background:#505050; }");
        v->addWidget(m_clearBirth);

        v->addWidget(makeLabel("Widget display", this));
        m_mode = new QComboBox(this);
        m_mode->setStyleSheet(boxStyle());
        m_mode->addItem("Full Tzolkin · 260 Kin", ViewFull);
        m_mode->addItem("Week view · 7 days", ViewWeek);
        m_mode->addItem("Today only", ViewToday);
        selectByData(m_mode, osmWidgetGet(m_id, "ViewMode", 0).toInt());
        v->addWidget(m_mode);

        v->addWidget(makeLabel("Background opacity", this));
        m_opacity = new QSlider(Qt::Horizontal, this);
        m_opacity->setRange(0, 255);
        m_opacity->setStyleSheet(sliderStyle());
        m_opacity->setValue(osmWidgetGet(m_id, "BackgroundAlpha", 185).toInt());
        v->addWidget(m_opacity);

        m_textColour = colourFrom(osmWidgetGet(m_id, "Colour", "#ffffff").toString(), "#ffffff");
        m_accentColour = colourFrom(osmWidgetGet(m_id, "AccentColour", "#8fe18f").toString(), "#8fe18f");
        addColourRow(v, "Text colour", m_textColour, m_textButton);
        addColourRow(v, "Highlight colour", m_accentColour, m_accentButton);
        v->addStretch();

        connect(m_birthButton, &QPushButton::clicked, this, [this]() {
            GalacticSignaturePicker picker(m_savedDate, m_afterNoon, this);
            if (picker.exec() != QDialog::Accepted) return;
            m_savedDate = picker.birthDate();
            m_afterNoon = picker.leapAfterNoon();
            m_currentKin = picker.selectedKin();
            m_myKin->blockSignals(true);
            selectByData(m_myKin, m_currentKin);
            m_myKin->setEnabled(false);
            m_myKin->blockSignals(false);
            refreshPreview();
            saveBirthSignature(m_id, m_savedDate, m_afterNoon);
        });
        connect(m_clearBirth, &QPushButton::clicked, this, [this]() {
            m_savedDate = QDate();
            m_afterNoon = false;
            m_myKin->setEnabled(true);
            m_currentKin = m_myKin->currentData().toInt();
            refreshPreview();
            QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
            const QString prefix = "Widget-" + m_id + "/";
            cfg.remove(prefix + "BirthDate");
            cfg.remove(prefix + "BirthLeapAfterNoon");
            cfg.setValue(prefix + "MyKin", m_currentKin);
            cfg.sync();
        });
        connect(m_myKin, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) {
                    if (m_savedDate.isValid()) return;
                    m_currentKin = m_myKin->currentData().toInt();
                    refreshPreview();
                    osmWidgetSet(m_id, "MyKin", m_currentKin);
                });
        connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { applyAppearance(); });
        connect(m_opacity, &QSlider::sliderReleased, this, [this]() { applyAppearance(); });
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
    static void selectByData(QComboBox *box, int data) {
        const int i = box->findData(data);
        if (i >= 0) box->setCurrentIndex(i);
    }
    void addColourRow(QVBoxLayout *v, const QString &title, QColor &c, QPushButton *&button) {
        QHBoxLayout *row = new QHBoxLayout;
        row->addWidget(makeLabel(title, this));
        button = new QPushButton(this);
        styleColourButton(button, c);
        row->addWidget(button, 1);
        v->addLayout(row);
    }
    void refreshPreview() {
        m_kinArt->setKin(m_currentKin);
        if (m_savedDate.isValid()) {
            m_birthStatus->setText("Born " + m_savedDate.toString("dd MMMM yyyy") +
                                   "  ·  Automatically converted to Kin " +
                                   QString::number(m_currentKin));
        } else {
            m_birthStatus->setText("No birthday saved. Tap the green button above and choose your birth date.");
        }
        m_preview->setText(m_currentKin > 0
            ? kinTitle(m_currentKin) + "\n" + kinDetailLine(m_currentKin) +
              "\n\nMayan tone above the coloured solar-seal symbol"
            : "Enter a Gregorian birthday to discover your Galactic Signature.");
    }
    void applyAppearance() {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
        const QString prefix = "Widget-" + m_id + "/";
        cfg.setValue(prefix + "ViewMode", m_mode->currentData().toInt());
        cfg.setValue(prefix + "BackgroundAlpha", m_opacity->value());
        cfg.setValue(prefix + "Colour", m_textColour.name());
        cfg.setValue(prefix + "AccentColour", m_accentColour.name());
        cfg.sync();
    }

    QString m_id;
    QDate m_savedDate;
    bool m_afterNoon = false;
    int m_currentKin = 0;
    QPushButton *m_birthButton = nullptr;
    QPushButton *m_clearBirth = nullptr;
    QLabel *m_birthStatus = nullptr;
    KinPreview *m_kinArt = nullptr;
    QLabel *m_preview = nullptr;
    QComboBox *m_myKin = nullptr;
    QComboBox *m_mode = nullptr;
    QSlider *m_opacity = nullptr;
    QPushButton *m_textButton = nullptr;
    QPushButton *m_accentButton = nullptr;
    QColor m_textColour, m_accentColour;
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
