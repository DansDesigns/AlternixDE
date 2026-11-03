// tzolkin.cpp — AlternixOS Tzolkin / Harmonic Module widget
// Seal glyphs: original vector redraws based on the supplied Law of Time reference.
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

static void drawSealSymbol(QPainter &p, const QRectF &target, int sealIndex,
                           const QColor &unusedColour = QColor(), qreal unusedScale = 1.0) {
    Q_UNUSED(unusedColour)
    Q_UNUSED(unusedScale)
    if (sealIndex < 0 || sealIndex >= 20 || target.width() <= 0 || target.height() <= 0) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal size=qMin(target.width(), target.height());
    p.translate(target.center().x()-size/2, target.center().y()-size/2);
    p.scale(size/100.0, size/100.0);
    const QColor palette[4]={QColor("#ee4649"), QColor("#fafaf6"), QColor("#4babe0"), QColor("#f4da3e")};
    const QColor ink("#101015");

    p.setPen(QPen(ink,4.5));
    p.setBrush(palette[sealIndex%4]);
    p.drawRoundedRect(QRectF(5,5,90,90),12,12);

    QPen pen(ink,3.9,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(cachedSealPaths(false).at(sealIndex));

    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawPath(cachedSealPaths(true).at(sealIndex));
    p.setPen(Qt::NoPen);
    p.setBrush(ink);
    p.drawPath(cachedSealDots(false).at(sealIndex));
    p.setPen(QPen(ink,3.8,Qt::SolidLine,Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawPath(cachedSealDots(true).at(sealIndex));
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
