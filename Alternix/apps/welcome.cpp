// AlternixOS Welcome - fullscreen, touch-friendly first-run guide.
// Source destination: Alternix/apps/welcome.cpp
// Build: g++ -std=c++17 -O3 -fPIC apps/welcome.cpp -o welcome \
//        $(pkg-config --cflags --libs Qt5Widgets)
//
// The 7 screens follow onboarding.pptx. PowerPoint's small desktop screenshots
// showing the top and bottom bars are intentionally NOT displayed. All text,
// arrows, highlight brackets and striped indicators are drawn dynamically,
// without shipping or opening the presentation at runtime.

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
        // Non-invasive progress dots, outside the original slide content.
        const qreal r = qBound(2.0, height() * 0.0035, 5.0);
        const qreal gap = r * 3.3;
        const qreal y = height() - qMax(12.0, height() * 0.026);
        const qreal startX = width() / 2.0 - gap * 3.0;
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 7; ++i) {
            p.setBrush(QColor(0, 25, 36, i == page ? 180 : 70));
            p.drawEllipse(QPointF(startX + i * gap, y), r, r);
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
        // Original PPTX uses pastel blue / off-white / peach stops. The final
        // two slides reverse the gradient, exactly as in the presentation.
        const bool reversed = page >= 5;
        QLinearGradient g(0, 0, 0, height());
        if (!reversed) {
            g.setColorAt(0, QColor("#98def7"));
            g.setColorAt(.50, QColor("#f2f1f1"));
            g.setColorAt(1, QColor("#f8c9b3"));
        } else {
            g.setColorAt(0, QColor("#f8c9b3"));
            g.setColorAt(.50, QColor("#f2f1f1"));
            g.setColorAt(1, QColor("#98def7"));
        }
        p.fillRect(rect(), g);
    }

    void paintWords(QPainter &p, const QString &words, const R &area,
                    qreal sizeFactor = .074, const QColor &shadow = QColor(255,255,255),
                    bool bold = false) const {
        if (area.width() < 10 || area.height() < 10 || words.isEmpty()) return;
        const auto flags = int(Qt::AlignCenter | Qt::TextWordWrap);
        QFont font(QStringLiteral("Ubuntu"));
        font.setWeight(bold ? QFont::DemiBold : QFont::Normal);
        font.setStyleStrategy(QFont::PreferAntialias);
        // Binary-search the largest font that FITS the actual text rectangle.
        // The same code works on tablets in portrait mode and after rotation.
        const int ceiling = qMax(12, int(qMin(height() * sizeFactor, area.width() * .18)));
        int lo = 9, hi = qMax(9, ceiling), best = lo;
        while (lo <= hi) {
            const int mid = (lo + hi) / 2;
            font.setPixelSize(mid);
            const QFontMetricsF met(font);
            const QRectF content = met.boundingRect(
                R(0,0,area.width() * .94,area.height() * .94), flags, words);
            if (content.width() <= area.width() * .96 &&
                content.height() <= area.height() * .96) {
                best = mid;
                lo = mid + 1;
            } else hi = mid - 1;
        }
        font.setPixelSize(best);
        p.setFont(font);
        // A soft coloured halo echoes the arrows and original PowerPoint.
        const qreal radius = qBound(1.0, best * .07, 4.5);
        const QColor halo(shadow.red(), shadow.green(), shadow.blue(), 75);
        p.setPen(halo);
        for (int dx = -2; dx <= 2; ++dx)
            for (int dy = -2; dy <= 2; ++dy)
                if (dx*dx + dy*dy <= 5)
                    p.drawText(area.translated(dx * radius*.55, dy * radius*.55), flags, words);
        p.setPen(QColor("#11151a"));
        p.drawText(area, flags, words);
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
        paintWords(p,QStringLiteral("Welcome,"),frac(.11,.13,.78,.22,sz),.125,QColor(255,255,255));
        paintWords(p,QStringLiteral("tap or click to continue.."),
                   frac(.08,.65,.84,.22,sz),.108,QColor(255,255,255));
    }

    void launcher(QPainter &p,bool portrait) const {
        const QSize sz = size();
        if (!portrait) {
            drawArrowAt(p,frac(.055,.17,.15,.47,sz),0,true);
            drawArrowAt(p,frac(.795,.17,.15,.47,sz),0,true);
            paintWords(p,QStringLiteral("Swipe up\n=\nApp Launcher"),
                       frac(.27,.11,.46,.37,sz),.085,glowPurple());
            paintWords(p,QStringLiteral("or\ntap the icon"),
                       frac(.31,.53,.38,.20,sz),.076,glowPurple());
            drawArrowAt(p,frac(.468,.72,.064,.20,sz),2,false,glowGreen());
        } else {
            drawArrowAt(p,frac(.09,.11,.22,.32,sz),0,true);
            drawArrowAt(p,frac(.69,.11,.22,.32,sz),0,true);
            paintWords(p,QStringLiteral("Swipe up\n=\nApp Launcher"),
                       frac(.18,.37,.64,.23,sz),.071,glowPurple());
            paintWords(p,QStringLiteral("or\ntap the icon"),
                       frac(.21,.62,.58,.18,sz),.066,glowPurple());
            drawArrowAt(p,frac(.43,.80,.14,.14,sz),2,false,glowGreen());
        }
    }

    void taskbar(QPainter &p,bool portrait) const {
        const QSize sz=size();
        if (!portrait) {
            paintWords(p,QStringLiteral("Application Taskbar"),
                       frac(.02,.60,.43,.16,sz),.083,glowPurple());
            paintWords(p,QStringLiteral("Toggle Keyboard"),
                       frac(.58,.60,.40,.16,sz),.083,glowPurple());
            drawArrowAt(p,frac(.095,.77,.09,.19,sz),2,false,glowGreen());
            drawArrowAt(p,frac(.815,.77,.09,.19,sz),2,false,glowGreen());
        } else {
            paintWords(p,QStringLiteral("Application\nTaskbar"),
                       frac(.02,.32,.46,.23,sz),.070,glowPurple());
            paintWords(p,QStringLiteral("Toggle\nKeyboard"),
                       frac(.52,.32,.46,.23,sz),.070,glowPurple());
            drawArrowAt(p,frac(.18,.56,.14,.25,sz),2,false,glowGreen());
            drawArrowAt(p,frac(.68,.56,.14,.25,sz),2,false,glowGreen());
        }
    }

    void runningApps(QPainter &p,bool portrait) const {
        const QSize sz=size();
        if (!portrait) {
            drawArrowAt(p,frac(.10,.30,.32,.39,sz),1,true);
            paintWords(p,QStringLiteral("Swipe Right\n=\nRunning Apps"),
                       frac(.57,.32,.40,.40,sz),.083,glowPurple());
        } else {
            drawArrowAt(p,frac(.12,.23,.76,.26,sz),1,true);
            paintWords(p,QStringLiteral("Swipe Right\n=\nRunning Apps"),
                       frac(.11,.52,.78,.35,sz),.081,glowPurple());
        }
    }

    void notifications(QPainter &p,bool portrait) const {
        const QSize sz=size();
        if (!portrait) {
            paintWords(p,QStringLiteral("Swipe Left\n=\nNotifications"),
                       frac(.04,.32,.39,.40,sz),.083,glowPurple());
            drawArrowAt(p,frac(.58,.30,.32,.39,sz),3,true);
        } else {
            drawArrowAt(p,frac(.12,.23,.76,.26,sz),3,true);
            paintWords(p,QStringLiteral("Swipe Left\n=\nNotifications"),
                       frac(.11,.52,.78,.35,sz),.081,glowPurple());
        }
    }

    void quickSettings(QPainter &p,bool portrait) const {
        const QSize sz=size();
        // Keep the PPT's yellow/black top indicators and green brackets, NOT
        // the photos of Qtile's real top/bottom bars.
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
                       frac(.25,.22,.50,.67,sz),.079,glowGreen());
        } else {
            drawArrowAt(p,frac(.05,.59,.16,.28,sz),2,true);
            drawArrowAt(p,frac(.79,.59,.16,.28,sz),2,true);
            paintWords(p,QStringLiteral("Pull down\nor\nSwipe on desktop\n=\nQuick Settings"),
                       frac(.13,.23,.74,.55,sz),.073,glowGreen());
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
                       frac(.025,.46,.27,.45,sz),.082,QColor(255,255,255));
            paintWords(p,QStringLiteral("Tap\n=\nClock"),
                       frac(.377,.47,.246,.36,sz),.085,glowGreen());
            paintWords(p,QStringLiteral("Window\n&\nDevice\nControls"),
                       frac(.72,.45,.26,.47,sz),.082,QColor(255,255,255));
        } else {
            drawArrowAt(p,frac(.095,.20,.14,.25,sz),0,false,glowGreen());
            drawArrowAt(p,frac(.43,.20,.14,.25,sz),0,false,glowGreen());
            drawArrowAt(p,frac(.765,.20,.14,.25,sz),0,false,glowGreen());
            paintWords(p,QStringLiteral("Desktop\nSwitcher\n&\nSystem Tray"),
                       frac(.01,.52,.32,.35,sz),.061,QColor(255,255,255));
            paintWords(p,QStringLiteral("Tap\n=\nClock"),
                       frac(.34,.52,.32,.35,sz),.069,glowGreen());
            paintWords(p,QStringLiteral("Window\n&\nDevice\nControls"),
                       frac(.67,.52,.32,.35,sz),.061,QColor(255,255,255));
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
