// AlternixOS Welcome - fullscreen, touch-friendly first-run guide.
// Source destination: Alternix/apps/welcome.cpp
// Build: g++ -std=c++17 -O3 -fPIC apps/welcome.cpp -o welcome \
//        $(pkg-config --cflags --libs Qt5Widgets)
//
// The 7 screens follow onboarding.pptx. Text, arrows, highlight brackets and
// striped indicators are drawn dynamically, while the PowerPoint's desktop
// bar screenshots are shipped as separate assets and displayed on the relevant
// slides so the result still matches the onboarding deck closely.

#include <QApplication>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QLockFile>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QPen>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QTouchEvent>
#include <QWidget>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <unistd.h>

namespace {

// Check the running system, not the system in which the executable was built.
// In particular, this must not consume the first-login guide in a live ISO.
bool runningLiveSession() {
    if (QFileInfo::exists(QStringLiteral("/run/live/medium")) ||
        QFileInfo::exists(QStringLiteral("/lib/live/mount/medium")) ||
        QFileInfo::exists(QStringLiteral("/run/live/overlay")))
        return true;
    QFile cmdline(QStringLiteral("/proc/cmdline"));
    if (cmdline.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> options = cmdline.readAll().simplified().split(' ');
        for (const QByteArray &option : options) {
            if (option == "boot=live" || option == "live-media" ||
                option.startsWith("live-media=")) return true;
        }
    }
    return false;
}

QString completionFile() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
           + QStringLiteral("/Alternix/welcome-completed");
}

void recordCompletion() {
    const QString path = completionFile();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        qWarning("welcome: unable to create the completion-marker directory");
        return;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning("welcome: unable to create the completion marker");
        return;
    }
    f.write("AlternixOS Welcome completed\n");
    f.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
    f.write("\n");
    if (!f.commit()) qWarning("welcome: unable to save the completion marker");
}

using R = QRectF;

QRectF frac(qreal x, qreal y, qreal w, qreal h, const QSize &size) {
    return {x * size.width(), y * size.height(), w * size.width(), h * size.height()};
}

QColor glowPurple() { return QColor(199, 51, 190); }
QColor glowGreen()  { return QColor(92, 213, 52); }


QString assetPath(const QString &fileName) {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QStringLiteral("/usr/share/alternix/welcome/") + fileName,
        QStringLiteral("/usr/local/share/alternix/welcome/") + fileName,
        QDir::currentPath() + QStringLiteral("/assets/welcome/") + fileName,
        appDir + QStringLiteral("/../share/alternix/welcome/") + fileName,
        appDir + QStringLiteral("/assets/welcome/") + fileName
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path) && QFileInfo(path).isFile())
            return QFileInfo(path).canonicalFilePath().isEmpty() ? path : QFileInfo(path).canonicalFilePath();
    }
    return QString();
}


class WelcomeWindow final : public QWidget {
public:
    explicit WelcomeWindow(QWidget *parent = nullptr) : QWidget(parent) {
        setWindowTitle(QStringLiteral("Welcome"));
        setObjectName(QStringLiteral("welcome"));
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_AcceptTouchEvents, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setMouseTracking(false);
        setFocusPolicy(Qt::StrongFocus);
        lastStep.start();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        drawBackground(p);
        drawPowerPointBars(p);
        const bool portrait = width() * 10 < height() * 12;
        switch (page) {
        case 0: welcome(p); break;
        case 1: launcher(p, portrait); break;
        case 2: taskbar(p, portrait); break;
        case 3: runningApps(p, portrait); break;
        case 4: notifications(p, portrait); break;
        case 5: quickSettings(p, portrait); break;
        case 6: topBar(p, portrait); break;
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            mouseStart = e->pos();
            e->accept();
        } else QWidget::mousePressEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            if ((e->pos() - mouseStart).manhattanLength() < qMax(18, width() / 45))
                advance();
            e->accept();
        } else QWidget::mouseReleaseEvent(e);
    }

    bool event(QEvent *e) override {
        if (e->type() == QEvent::TouchBegin) {
            const auto *touch = static_cast<QTouchEvent *>(e);
            if (!touch->touchPoints().isEmpty())
                touchStart = touch->touchPoints().first().pos();
            e->accept();
            return true;
        }
        if (e->type() == QEvent::TouchUpdate) {
            e->accept();
            return true;
        }
        if (e->type() == QEvent::TouchEnd) {
            const auto *touch = static_cast<QTouchEvent *>(e);
            if (!touch->touchPoints().isEmpty() &&
                (touch->touchPoints().first().pos() - touchStart).manhattanLength()
                    < qMax(28, width() / 30)) advance();
            e->accept();
            return true;
        }
        if (e->type() == QEvent::TouchCancel) {
            e->accept();
            return true;
        }
        return QWidget::event(e);
    }

    void keyPressEvent(QKeyEvent *e) override {
        switch (e->key()) {
        case Qt::Key_Right:
        case Qt::Key_Down:
        case Qt::Key_Space:
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_PageDown:
            advance(); break;
        case Qt::Key_Left:
        case Qt::Key_Up:
        case Qt::Key_Backspace:
        case Qt::Key_PageUp:
            if (page > 0) { --page; update(); }
            break;
        case Qt::Key_Escape:
            close(); break; // Intentionally NOT marked complete.
        default: QWidget::keyPressEvent(e); break;
        }
        e->accept();
    }

private:
    int page = 0;
    QPoint mouseStart;
    QPointF touchStart;
    QElapsedTimer lastStep;

    void advance() {
        // Avoid duplicate steps on a touchscreen that reports both touch and
        // synthesized mouse release, without imposing an animation delay.
        if (lastStep.isValid() && lastStep.elapsed() < 120) return;
        lastStep.restart();
        if (page == 6) {
            recordCompletion();
            close();
        } else {
            ++page;
            update();
        }
    }

    void drawBackground(QPainter &p) const {
        // Exact stops and directions from onboarding.pptx slide XML and theme1:
        // accent4 #0F9ED5 with lumMod 40% + lumOff 60% -> #96DCF8
        // bg1 #FFFFFF with lumMod 95%                 -> #F2F2F2
        // accent2 #E97132 with lumMod 40% + lumOff 60% -> #F6C6AD
        // DrawingML luminance adjustments are HSL-based, not RGB arithmetic.
        // PowerPoint gradient angle: 270 deg on slides 1-3, 0 on slide 4,
        // 180 on slide 5, and 90 on slides 6-7. Do not approximate with the
        // same vertical gradient on every page.
        const QColor blue(150,220,248);
        const QColor middle(242,242,242);
        const QColor peach(246,198,173);
        QLinearGradient gradient;
        if (page <= 2) {  // 270 degrees: from bottom peach to top blue
            gradient = QLinearGradient(0, 0, 0, height());
            gradient.setColorAt(0.0, blue);
            gradient.setColorAt(0.5, middle);
            gradient.setColorAt(1.0, peach);
        } else if (page == 3) { // 0 degrees: peach left, blue right
            gradient = QLinearGradient(0, 0, width(), 0);
            gradient.setColorAt(0.0, peach);
            gradient.setColorAt(0.5, middle);
            gradient.setColorAt(1.0, blue);
        } else if (page == 4) { // 180 degrees: blue left, peach right
            gradient = QLinearGradient(0, 0, width(), 0);
            gradient.setColorAt(0.0, blue);
            gradient.setColorAt(0.5, middle);
            gradient.setColorAt(1.0, peach);
        } else {              // 90 degrees: peach top, blue bottom
            gradient = QLinearGradient(0, 0, 0, height());
            gradient.setColorAt(0.0, peach);
            gradient.setColorAt(0.5, middle);
            gradient.setColorAt(1.0, blue);
        }
        p.fillRect(rect(), gradient);
    }

    const QPixmap &topBarPixmap() const {
        static const QPixmap pix(assetPath(QStringLiteral("top-bar.png")));
        return pix;
    }

    const QPixmap &bottomBarPixmap() const {
        static const QPixmap pix(assetPath(QStringLiteral("bottom-bar.png")));
        return pix;
    }

    void drawPowerPointBars(QPainter &p) const {
        if (page == 0) return;

        const QPixmap &top = topBarPixmap();
        const QPixmap &bottom = bottomBarPixmap();

        if (!top.isNull()) {
            const qreal h = qMax(1.0, width() * (qreal(top.height()) / qreal(top.width())));
            p.drawPixmap(QRectF(0, 0, width(), h), top, QRectF(0, 0, top.width(), top.height()));
        } else {
            p.fillRect(QRectF(0, 0, width(), qMax(16.0, height() * .03)), QColor(10, 16, 24, 210));
        }

        if (!bottom.isNull()) {
            const qreal h = qMax(1.0, width() * (qreal(bottom.height()) / qreal(bottom.width())));
            p.drawPixmap(QRectF(0, height() - h, width(), h), bottom, QRectF(0, 0, bottom.width(), bottom.height()));
        } else {
            p.fillRect(QRectF(0, height() - qMax(18.0, height() * .033), width(), qMax(18.0, height() * .033)), QColor(10, 16, 24, 210));
        }
    }

    enum class TextGlow { None, White, WhiteSoft, Purple, Green };
    struct GlowLine {
        TextGlow effect = TextGlow::None;
        int prefixChars = -1; // -1 = whole line; positive = only this prefix
    };
    struct PaintedLine {
        QString text;
        TextGlow effect = TextGlow::None;
        int prefixChars = -1;
        bool blank = false;
    };

    void drawTextGlow(QPainter &p, const QString &text, const QFont &font,
                      const QPointF &baseLine, TextGlow effect) const {
        if (effect == TextGlow::None || text.isEmpty()) return;

        // The deck uses Office theme colours accent5 / accent6 with 175% HSL
        // saturation, and bg1 white. Its text effects use 40% alpha on the
        // colourful glows and 60-67% on the white glow.
        QColor tint;
        qreal alpha = 0.4;
        qreal radiusScale = .275;
        switch (effect) {
        case TextGlow::Purple: tint = QColor(203, 0, 180); radiusScale = .275; break;
        case TextGlow::Green:  tint = QColor(57, 212, 1); radiusScale = .45; break;
        case TextGlow::White:  tint = QColor(255, 255, 255); alpha = .67;
                               radiusScale = .222; break;
        case TextGlow::WhiteSoft: tint = QColor(255, 255, 255); alpha = .60;
                               radiusScale = .20; break;
        default: return;
        }

        // Feather the outline with concentric translucent strokes. Radius is
        // proportional to font size, using the PPT's nominal ratios: purple
        // 11pt / 40pt, green 18pt / 40pt, white 12pt / 54pt or 8pt / 40pt.
        // The unmodified black letter fill is drawn on top of the glow.
        QPainterPath glyphs;
        glyphs.addText(baseLine, font, text);
        const qreal radius = qMax(1.3, font.pixelSize() * radiusScale);
        p.save();
        p.setBrush(Qt::NoBrush);
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int step = 9; step >= 1; --step) {
            QColor layer = tint;
            layer.setAlphaF(alpha * (step < 4 ? .12 : .065));
            const qreal stroke = radius * (2.0 * step / 9.0);
            p.strokePath(glyphs, QPen(layer, stroke,
                                     Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        }
        p.restore();
    }

    // Paint source wording at a responsive font size, preserving the separate
    // text-run effects from the PowerPoint. Each explicit PPT paragraph keeps
    // its own line, but long phrases can word-wrap on narrow tablet screens.
    void paintWords(QPainter &p, const QString &words, const R &area,
                    qreal sizeFactor = .074,
                    const QVector<GlowLine> &runEffects = {}) const {
        if (area.width() < 10 || area.height() < 10 || words.isEmpty()) return;
        const QStringList paragraphs = words.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        const qreal maxW = area.width() * .95;
        QFont font(QStringLiteral("Comfortaa")); // Exact PowerPoint typeface
        font.setWeight(QFont::Normal);
        font.setStyleStrategy(QFont::PreferAntialias);
        const int ceiling = qMax(9, int(qMin(height() * sizeFactor, area.width() * .18)));

        auto layoutLines = [&](const QFont &f) -> QVector<PaintedLine> {
            const QFontMetricsF fm(f);
            QVector<PaintedLine> lines;
            for (int para = 0; para < paragraphs.size(); ++para) {
                const QString original = paragraphs.at(para);
                const GlowLine style = para < runEffects.size() ? runEffects.at(para)
                                                                   : GlowLine{};
                if (original.isEmpty()) {
                    lines.append(PaintedLine{QString(), TextGlow::None, -1, true});
                    continue;
                }
                QString line;
                int prefixRemaining = style.prefixChars;
                // Explicit line breaks stay where the presentation puts them.
                // Add wraps only where needed to avoid clipping smaller displays.
                const QStringList wordsHere = original.split(' ', Qt::SkipEmptyParts);
                auto appendLine = [&](const QString &portion) {
                    const int prefix = prefixRemaining < 0 ? -1
                                        : qMin(prefixRemaining, portion.size());
                    lines.append(PaintedLine{portion, style.effect, prefix, false});
                    if (prefixRemaining >= 0)
                        prefixRemaining = qMax(0, prefixRemaining - portion.size() - 1);
                };
                for (const QString &token : wordsHere) {
                    QString trial = line.isEmpty() ? token : line + QLatin1Char(' ') + token;
                    if (fm.horizontalAdvance(trial) > maxW && !line.isEmpty()) {
                        appendLine(line);
                        line = token;
                    } else {
                        line = trial;
                    }
                    // Fallback for very small portrait widths / giant words.
                    while (fm.horizontalAdvance(line) > maxW && line.size() > 1) {
                        int split = 1;
                        while (split < line.size() &&
                               fm.horizontalAdvance(line.left(split + 1)) <= maxW)
                            ++split;
                        appendLine(line.left(split));
                        line = line.mid(split);
                    }
                }
                if (!line.isEmpty()) appendLine(line);
            }
            return lines;
        };

        auto linesHeight = [&](const QVector<PaintedLine> &lines, const QFont &f) {
            const qreal base = QFontMetricsF(f).lineSpacing() * 1.06;
            qreal total = 0;
            for (const PaintedLine &line : lines)
                total += base * (line.blank ? .65 : 1.0);
            return total;
        };

        int low = 8, high = ceiling, best = low;
        while (low <= high) {
            const int mid = (low + high) / 2;
            font.setPixelSize(mid);
            const auto lines = layoutLines(font);
            if (linesHeight(lines, font) <= area.height() * .96) {
                best = mid;
                low = mid + 1;
            } else high = mid - 1;
        }
        font.setPixelSize(best);
        const QVector<PaintedLine> lines = layoutLines(font);
        const QFontMetricsF fm(font);
        const qreal rowH = fm.lineSpacing() * 1.06;
        const qreal totalHeight = linesHeight(lines, font);
        qreal y = area.top() + (area.height() - totalHeight) * .5;

        p.save();
        p.setClipRect(area.adjusted(-4,-4,4,4));
        p.setFont(font);
        for (const PaintedLine &line : lines) {
            if (line.blank) {
                y += rowH * .65;
                continue;
            }
            const qreal x = area.center().x() - fm.horizontalAdvance(line.text) * .5;
            const QPointF origin(x, y + fm.ascent());
            if (line.effect != TextGlow::None) {
                if (line.prefixChars < 0)
                    drawTextGlow(p, line.text, font, origin, line.effect);
                else if (line.prefixChars > 0)
                    drawTextGlow(p, line.text.left(line.prefixChars), font,
                                 origin, line.effect);
            }
            p.setPen(QColor(0, 0, 0));
            p.drawText(origin, line.text);
            y += rowH;
        }
        p.restore();
    }

    // Arrow paths are reconstructed as scalable vectors from the PowerPoint
    // shapes, rather than screenshots of the desktop bar.
    void arrow(QPainter &p, qreal cx, qreal cy, qreal w, qreal h,
               int direction, bool striped, const QColor &glow) const {
        // direction: 0 up, 1 right, 2 down, 3 left
        p.save();
        p.translate(cx, cy);
        p.rotate(direction * 90.0);
        // For horizontal arrows, use the supplied arrow width along its axis
        // while the caller chooses the arrow's screen-space bounding box.
        const qreal W = w, H = h;
        QPainterPath path;
        path.moveTo(0,-H*.5);
        path.lineTo(W*.5,-H*.12);
        path.lineTo(W*.22,-H*.12);
        path.lineTo(W*.22,H*.43);
        path.lineTo(-W*.22,H*.43);
        path.lineTo(-W*.22,-H*.12);
        path.lineTo(-W*.5,-H*.12);
        path.closeSubpath();
        const qreal line = qBound(1.15, qMin(width(), height()) * .003, 3.6);
        if (glow.alpha() != 0) {
            for (int i = 3; i >= 1; --i) {
                QColor c = glow;
                c.setAlpha(i == 3 ? 24 : i == 2 ? 45 : 65);
                p.setPen(QPen(c, line + i * qMax(3.0, H * .043), Qt::SolidLine,
                              Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(path);
            }
        }
        p.setBrush(Qt::white);
        p.setPen(QPen(QColor("#072635"), line, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.drawPath(path);
        if (striped) {
            const qreal shaft = W*.22;
            p.setPen(QPen(QColor("#072635"), line*.7));
            p.setBrush(Qt::white);
            const qreal y = H*.31;
            p.drawRect(R(-shaft,y,shaft*2,H*.05));
            p.drawRect(R(-shaft,y+H*.085,shaft*2,H*.05));
        }
        p.restore();
    }

    void drawArrowAt(QPainter &p, const R &target, int direction,
                     bool striped = false, const QColor &glow = glowPurple()) const {
        // Arrow source is vertical; a quarter-turn swaps its screen footprint.
        const bool horiz = direction == 1 || direction == 3;
        const qreal w = horiz ? target.height() : target.width();
        const qreal h = horiz ? target.width() : target.height();
        arrow(p,target.center().x(),target.center().y(),w,h,direction,striped,glow);
    }

    void stripes(QPainter &p, const R &area) const {
        p.save();
        p.setClipRect(area);
        p.fillRect(area, QColor("#ffce00"));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#071011"));
        const qreal step = qMax(18.0, height() * .047);
        for (qreal x = area.left() - area.height()*1.7 - step;
             x < area.right() + step; x += step) {
            QPolygonF diagonal;
            diagonal << QPointF(x, area.bottom())
                     << QPointF(x + step*.42,area.bottom())
                     << QPointF(x + step*.42 + area.height(),area.top())
                     << QPointF(x + area.height(),area.top());
            p.drawPolygon(diagonal);
        }
        p.restore();
    }

    void greenBracket(QPainter &p, const R &area) const {
        p.save();
        const qreal penWidth = qMax(2.2, height()*.006);
        QPainterPath shape;
        shape.moveTo(area.left(),0);
        shape.lineTo(area.left(),area.bottom());
        shape.lineTo(area.right(),area.bottom());
        shape.lineTo(area.right(),0);
        p.setPen(QPen(QColor(61,194,33,60),penWidth*3.2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(shape);
        p.setPen(QPen(QColor(77,183,30),penWidth,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        p.drawPath(shape);
        p.restore();
    }

    void welcome(QPainter &p) const {
        const QSize sz = size();
        paintWords(p,QStringLiteral("Welcome,"),frac(.11,.13,.78,.22,sz),.125,{{TextGlow::White}});
        paintWords(p,QStringLiteral("tap or click to continue.."),
                   frac(.08,.65,.84,.22,sz),.108,{{TextGlow::White}});
    }

    void launcher(QPainter &p,bool portrait) const {
        const QSize sz = size();
        if (!portrait) {
            drawArrowAt(p,frac(.055,.17,.15,.47,sz),0,true);
            drawArrowAt(p,frac(.795,.17,.15,.47,sz),0,true);
            paintWords(p,QStringLiteral("Swipe up\n=\nApp Launcher"),
                       frac(.27,.11,.46,.37,sz),.085,{{TextGlow::Purple}});
            paintWords(p,QStringLiteral("or\ntap the icon"),
                       frac(.31,.53,.38,.20,sz),.076,{{TextGlow::None}, {TextGlow::Green, 3}});
            drawArrowAt(p,frac(.468,.72,.064,.20,sz),2,false,glowGreen());
        } else {
            drawArrowAt(p,frac(.09,.11,.22,.32,sz),0,true);
            drawArrowAt(p,frac(.69,.11,.22,.32,sz),0,true);
            paintWords(p,QStringLiteral("Swipe up\n=\nApp Launcher"),
                       frac(.18,.37,.64,.23,sz),.071,{{TextGlow::Purple}});
            paintWords(p,QStringLiteral("or\ntap the icon"),
                       frac(.21,.62,.58,.18,sz),.066,{{TextGlow::None}, {TextGlow::Green, 3}});
            drawArrowAt(p,frac(.43,.80,.14,.14,sz),2,false,glowGreen());
        }
    }

    void taskbar(QPainter &p,bool portrait) const {
        const QSize sz=size();
        if (!portrait) {
            paintWords(p,QStringLiteral("Application Taskbar"),
                       frac(.02,.60,.43,.16,sz),.083,{{TextGlow::Green}});
            paintWords(p,QStringLiteral("Toggle Keyboard"),
                       frac(.58,.60,.40,.16,sz),.083,{{TextGlow::Green}});
            drawArrowAt(p,frac(.095,.77,.09,.19,sz),2,false,glowGreen());
            drawArrowAt(p,frac(.815,.77,.09,.19,sz),2,false,glowGreen());
        } else {
            paintWords(p,QStringLiteral("Application\nTaskbar"),
                       frac(.02,.32,.46,.23,sz),.070,{{TextGlow::Green}, {TextGlow::Green}});
            paintWords(p,QStringLiteral("Toggle\nKeyboard"),
                       frac(.52,.32,.46,.23,sz),.070,{{TextGlow::Green}, {TextGlow::Green}});
            drawArrowAt(p,frac(.18,.56,.14,.25,sz),2,false,glowGreen());
            drawArrowAt(p,frac(.68,.56,.14,.25,sz),2,false,glowGreen());
        }
    }

    void runningApps(QPainter &p,bool portrait) const {
        const QSize sz=size();
        if (!portrait) {
            drawArrowAt(p,frac(.10,.30,.32,.39,sz),1,true);
            paintWords(p,QStringLiteral("Swipe Right\n=\nRunning Apps"),
                       frac(.57,.32,.40,.40,sz),.083,{{TextGlow::Purple}});
        } else {
            drawArrowAt(p,frac(.12,.23,.76,.26,sz),1,true);
            paintWords(p,QStringLiteral("Swipe Right\n=\nRunning Apps"),
                       frac(.11,.52,.78,.35,sz),.081,{{TextGlow::Purple}});
        }
    }

    void notifications(QPainter &p,bool portrait) const {
        const QSize sz=size();
        if (!portrait) {
            paintWords(p,QStringLiteral("Swipe Left\n=\nNotifications"),
                       frac(.04,.32,.39,.40,sz),.083,{{TextGlow::Purple}});
            drawArrowAt(p,frac(.58,.30,.32,.39,sz),3,true);
        } else {
            drawArrowAt(p,frac(.12,.23,.76,.26,sz),3,true);
            paintWords(p,QStringLiteral("Swipe Left\n=\nNotifications"),
                       frac(.11,.52,.78,.35,sz),.081,{{TextGlow::Purple}});
        }
    }

    void quickSettings(QPainter &p,bool portrait) const {
        const QSize sz=size();
        // Add the PowerPoint bar image first, then overlay the same striped
        // highlight markers and green brackets shown in the presentation.
        stripes(p,frac(0,0,.11,.031,sz));
        stripes(p,frac(.45,0,.10,.031,sz));
        stripes(p,frac(.90,0,.10,.031,sz));
        greenBracket(p,frac(.11,0,.33,.072,sz));
        greenBracket(p,frac(.56,0,.33,.072,sz));
        drawArrowAt(p,frac(.26,.09,.07,.12,sz),2,false,QColor(0,0,0,0));
        drawArrowAt(p,frac(.67,.09,.07,.12,sz),2,false,QColor(0,0,0,0));
        if (!portrait) {
            drawArrowAt(p,frac(.047,.27,.13,.58,sz),2,true);
            drawArrowAt(p,frac(.823,.27,.13,.58,sz),2,true);
            paintWords(p,QStringLiteral("Pull down\nor\nSwipe on desktop\n\n=\n\nQuick Settings"),
                       frac(.25,.22,.50,.67,sz),.079,{{TextGlow::Green}, {}, {TextGlow::Purple}, {}, {}, {}, {TextGlow::WhiteSoft}});
        } else {
            drawArrowAt(p,frac(.05,.59,.16,.28,sz),2,true);
            drawArrowAt(p,frac(.79,.59,.16,.28,sz),2,true);
            paintWords(p,QStringLiteral("Pull down\nor\nSwipe on desktop\n=\nQuick Settings"),
                       frac(.13,.23,.74,.55,sz),.073,{{TextGlow::Green}, {}, {TextGlow::Purple}, {}, {TextGlow::WhiteSoft}});
        }
    }

    void topBar(QPainter &p,bool portrait) const {
        const QSize sz=size();
        stripes(p,frac(.12,0,.32,.031,sz));
        stripes(p,frac(.56,0,.32,.031,sz));
        greenBracket(p,frac(.002,0,.114,.075,sz));
        greenBracket(p,frac(.445,0,.110,.075,sz));
        greenBracket(p,frac(.885,0,.113,.075,sz));
        if (!portrait) {
            drawArrowAt(p,frac(.015,.12,.09,.32,sz),0,false,glowGreen());
            drawArrowAt(p,frac(.455,.12,.09,.32,sz),0,false,glowGreen());
            drawArrowAt(p,frac(.895,.12,.09,.32,sz),0,false,glowGreen());
            paintWords(p,QStringLiteral("Desktop Switcher\n&\nSystem Tray"),
                       frac(.025,.46,.27,.45,sz),.082);
            paintWords(p,QStringLiteral("Tap\n=\nClock"),
                       frac(.377,.47,.246,.36,sz),.085,{{TextGlow::Green}});
            paintWords(p,QStringLiteral("Window\n&\nDevice\nControls"),
                       frac(.72,.45,.26,.47,sz),.082);
        } else {
            drawArrowAt(p,frac(.095,.20,.14,.25,sz),0,false,glowGreen());
            drawArrowAt(p,frac(.43,.20,.14,.25,sz),0,false,glowGreen());
            drawArrowAt(p,frac(.765,.20,.14,.25,sz),0,false,glowGreen());
            paintWords(p,QStringLiteral("Desktop\nSwitcher\n&\nSystem Tray"),
                       frac(.01,.52,.32,.35,sz),.061);
            paintWords(p,QStringLiteral("Tap\n=\nClock"),
                       frac(.34,.52,.32,.35,sz),.069,{{TextGlow::Green}});
            paintWords(p,QStringLiteral("Window\n&\nDevice\nControls"),
                       frac(.67,.52,.32,.35,sz),.061);
        }
    }
};

} // namespace

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("welcome"));
    QCoreApplication::setOrganizationName(QStringLiteral("AlternixOS"));

    const QStringList args = app.arguments();
    const bool once = args.contains(QStringLiteral("--once"));
    if (once && (runningLiveSession() || QFileInfo::exists(completionFile())))
        return 0;

    // Concurrent first-login startup / manual launch must not create two
    // fullscreen welcome windows. The lock is automatically released on exit.
    const QString lockPath = QDir::tempPath() + QStringLiteral("/alternix-welcome-")
                             + QString::number(getuid()) + QStringLiteral(".lock");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) return 0;

    WelcomeWindow window;
    int startupDelayMs = 0;
    if (args.contains(QStringLiteral("--startup-delay"))) startupDelayMs = 3200;
    if (once && startupDelayMs == 0) startupDelayMs = 3200;

    const auto show = [&window]() {
        window.showFullScreen();
        window.raise();
        window.activateWindow();
        window.setFocus(Qt::ActiveWindowFocusReason);
    };
    if (startupDelayMs != 0)
        QTimer::singleShot(startupDelayMs, &window, show);
    else show();
    return app.exec();
}
