// AlternixOS Welcome - interactive desktop walkthrough (Qt5 + X11).
// Place at Alternix/apps/welcome.cpp
// g++ -std=c++17 -O3 -fPIC apps/welcome.cpp -o welcome \
//    $(pkg-config --cflags --libs Qt5Widgets) -lX11 -lXext -lXi
//
// The PowerPoint wording, Comfortaa typeface, per-run glows, and individual
// slide gradient directions are preserved. Instead of showing screenshots of
// the bars, this guide spotlights the LIVE desktop underneath. Its X11 Shape
// INPUT region contains only the guide's own controls; desktop clicks, swipes
// and on-screen keyboard touches reach the real underlying windows.
//
// IMPORTANT: XShape input passthrough is REQUIRED. If not available, exit
// rather than launching a fullscreen overlay that traps the desktop.

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLockFile>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QSaveFile>
#include <QScreen>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QTouchEvent>
#include <QWidget>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <memory>
#include <unistd.h>

// Include X11 LAST: its names/macro definitions conflict with Qt headers.
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XInput2.h>
#ifdef Status
#  undef Status
#endif
#ifdef None
#  undef None
#endif

namespace {

bool isLiveSystem() {
    if (QFileInfo::exists("/run/live/medium") ||
        QFileInfo::exists("/lib/live/mount/medium") ||
        QFileInfo::exists("/run/live/overlay")) return true;
    QFile f("/proc/cmdline");
    if (f.open(QIODevice::ReadOnly)) {
        for (const QByteArray &option : f.readAll().simplified().split(' '))
            if (option == "boot=live" || option == "live-media" ||
                option.startsWith("live-media=")) return true;
    }
    return false;
}

bool lockscreenActive() {
    // Never put a topmost first-login tour over Alternix's pattern/PIN UI.
    // Only look at processes owned by this desktop user.
    QDirIterator it("/proc",QDir::Dirs|QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString path=it.next();
        const QString pid=QFileInfo(path).fileName();
        if (pid.isEmpty() || !pid.at(0).isDigit() ||
            QFileInfo(path).ownerId()!=static_cast<uint>(getuid())) continue;
        QFile comm(path+"/comm");
        if (comm.open(QIODevice::ReadOnly) &&
            comm.readLine().trimmed()=="osm-lockscreen") return true;
    }
    return false;
}

QString donePath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)
            + "/Alternix/welcome-completed";
}

void markFinished() {
    const QString name = donePath();
    if (!QDir().mkpath(QFileInfo(name).absolutePath())) {
        qWarning("welcome: completion directory cannot be written");
        return;
    }
    QSaveFile file(name);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning("welcome: completion marker cannot be written");
        return;
    }
    file.write("AlternixOS Welcome completed\n");
    file.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
    file.write("\n");
    if (!file.commit()) qWarning("welcome: could not commit completion marker");
}

using Rect = QRectF;
using Line = std::pair<QString,int>; // int selects per-line PPT text glow

enum Glow { Plain = 0, White = 1, SoftWhite = 2, Purple = 3, Green = 4 };

QLinearGradient deckGradient(int page, const QRectF &bounds, int opacity=255) {
    // PPTX theme1: accent4 -> #96DCF8, bg1 -> #F2F2F2,
    // accent2 -> #F6C6AD, with its recorded DrawingML luminance transforms.
    QColor blue(150,220,248,opacity), offWhite(242,242,242,opacity),
           peach(246,198,173,opacity);
    QLinearGradient gradient;
    if (page <= 2) { // 270 degrees: blue top / peach bottom
        gradient = QLinearGradient(bounds.left(),bounds.top(),bounds.left(),bounds.bottom());
        gradient.setColorAt(0,blue); gradient.setColorAt(.5,offWhite);
        gradient.setColorAt(1,peach);
    } else if (page == 3) { // 0 degrees: peach left / blue right
        gradient = QLinearGradient(bounds.left(),bounds.top(),bounds.right(),bounds.top());
        gradient.setColorAt(0,peach); gradient.setColorAt(.5,offWhite);
        gradient.setColorAt(1,blue);
    } else if (page == 4) { // 180 degrees: blue left / peach right
        gradient = QLinearGradient(bounds.left(),bounds.top(),bounds.right(),bounds.top());
        gradient.setColorAt(0,blue); gradient.setColorAt(.5,offWhite);
        gradient.setColorAt(1,peach);
    } else { // 90 degrees: peach top / blue bottom
        gradient = QLinearGradient(bounds.left(),bounds.top(),bounds.left(),bounds.bottom());
        gradient.setColorAt(0,peach); gradient.setColorAt(.5,offWhite);
        gradient.setColorAt(1,blue);
    }
    return gradient;
}

struct TextRun {
    QString text;
    Glow glow = Plain;
    int prefix = -1; // highlight first N characters (PowerPoint per-run effect)
};

struct Step {
    QString heading;
    QVector<TextRun> lines;
    QString instruction;
};

const QVector<Step> &guide() {
    static const QVector<Step> steps = {
        {"Welcome", {{"Welcome,",White},{"tap or click to continue..",White}},
          "Tap or click anywhere to start the interactive desktop tour."},
        {"App Launcher", {{"Swipe up",Purple},{"=",Plain},{"App Launcher",Plain},
                          {"or",Plain},{"tap the icon",Green,3}},
          "Swipe up from the bottom edge or tap the real launcher icon."},
        {"Taskbar & Keyboard", {{"Application Taskbar",Green},
                                 {"Toggle Keyboard",Green}},
          "The bottom bar is your taskbar. Tap its keyboard icon to open and hide the keyboard."},
        {"Running Apps", {{"Swipe Right",Purple},{"=",Plain},{"Running Apps",Plain}},
          "Swipe right from the left edge to open Running Apps."},
        {"Notifications", {{"Swipe Left",Purple},{"=",Plain},{"Notifications",Plain}},
          "Swipe left from the right edge to open Notifications."},
        {"Quick Settings", {{"Pull down",Green},{"or",Plain},
                              {"Swipe on desktop",Purple},{"=",Plain},
                              {"Quick Settings",SoftWhite}},
          "Pull down from the top, or swipe down on the desktop, to open Quick Settings."},
        {"Top Bar", {{"Desktop Switcher & System Tray",Plain},
                       {"Tap = Clock",Green,3},
                       {"Window & Device Controls",Plain}},
          "Tap the left, centre and right highlighted areas on the real top bar."},
    };
    return steps;
}

class WelcomeOverlay final : public QWidget {
public:
    explicit WelcomeOverlay(QWidget *parent=nullptr) : QWidget(parent) {
        setWindowTitle("Welcome");
        setObjectName("welcome");
        // An override-redirect window is never added to Qtile's taskbar or
        // tiling layouts. Its ShapeInput region exposes only Back/Next/Exit.
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                       Qt::WindowStaysOnTopHint | Qt::X11BypassWindowManagerHint |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_AcceptTouchEvents, true);
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(false);
        interactionClock.start();
        pollTimer.setInterval(16);
        QObject::connect(&pollTimer, &QTimer::timeout, this, [this]{ pollPointer(); });
    }

    ~WelcomeOverlay() override {
        pollTimer.stop();
        socketNotifier.reset();
        if (display) XCloseDisplay(display);
    }

    bool attachX11() {
        display = XOpenDisplay(nullptr);
        if (!display) { qCritical("welcome: X11 display is unavailable"); return false; }
        int shapeEvent=0, shapeError=0;
        if (!XShapeQueryExtension(display,&shapeEvent,&shapeError)) {
            qCritical("welcome: XShape not supported - refusing to block desktop input");
            return false;
        }
        // The same root pointer is used by mouse and touch pointer emulation.
        root = DefaultRootWindow(display);
        if (XQueryExtension(display,"XInputExtension",&xiOpcode,&xiEvent,&xiError)) {
            int major=2, minor=2;
            if (XIQueryVersion(display,&major,&minor)==Success &&
                    (major>2 || (major==2 && minor>=0))) {
                unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {};
                XISetMask(bits,XI_RawButtonPress);
                XISetMask(bits,XI_RawButtonRelease);
                if (major>2 || (major==2 && minor>=2)) {
                    XISetMask(bits,XI_RawTouchBegin);
                    XISetMask(bits,XI_RawTouchUpdate);
                    XISetMask(bits,XI_RawTouchEnd);
                }
                XIEventMask mask;
                mask.deviceid = XIAllDevices;
                mask.mask_len = sizeof(bits);
                mask.mask = bits;
                if (XISelectEvents(display,root,&mask,1)==Success) {
                    xiEnabled=true;
                    socketNotifier.reset(new QSocketNotifier(ConnectionNumber(display),
                                                              QSocketNotifier::Read,this));
                    QObject::connect(socketNotifier.get(), &QSocketNotifier::activated,
                                     this, [this](int){ readXInputEvents(); });
                }
            }
        }
        updateInputShape();
        pollTimer.start();
        return true;
    }

    void fitScreen(const QRect &geometry) {
        setGeometry(geometry);
        updateInputShape();
        update();
    }

protected:
    void resizeEvent(QResizeEvent *e) override {
        QWidget::resizeEvent(e);
        updateInputShape();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                         QPainter::SmoothPixmapTransform);
        if (page==0) {
            // The introductory slide keeps its *entire* original PPT gradient.
            p.fillRect(rect(),deckGradient(0,rect()));
            const QRectF center(width()*.08,height()*.22,width()*.84,height()*.52);
            drawLines(p,center,guide().at(0).lines,
                      qBound(25.0,height()*.085,86.0));
            drawFooter(p);
            return;
        }

        // Semi-transparent PowerPoint gradient over the LIVE desktop, with
        // completely clear holes around the real targets. The gradient is not
        // rotated or recoloured: direction follows each slide's XML definition.
        p.fillRect(rect(),deckGradient(page,rect(),85));
        p.fillRect(rect(),QColor(9,14,24,52));
        const auto holes = targetRegions();
        p.save();
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        for (const QRectF &hole : holes) {
            QPainterPath path;
            path.addRoundedRect(hole, qMin(18.0,hole.height()*.25),
                                qMin(18.0,hole.height()*.25));
            p.fillPath(path,Qt::transparent);
        }
        p.restore();
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        const QColor edge = (page==3 || page==4) ? QColor(203,0,180) : QColor(57,212,1);
        for (const QRectF &hole : holes) {
            p.setPen(QPen(QColor(edge.red(),edge.green(),edge.blue(),60),11));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(hole,17,17);
            p.setPen(QPen(QColor(edge.red(),edge.green(),edge.blue(),220),3));
            p.drawRoundedRect(hole,17,17);
        }
        drawInstructionCard(p);
        drawFooter(p);
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button()==Qt::LeftButton) mousePress=e->pos();
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button()==Qt::LeftButton &&
            (e->pos()-mousePress).manhattanLength()<=14)
            activateGuideControl(e->localPos());
        e->accept();
    }

    bool event(QEvent *e) override {
        // Native touchscreen activation of the guide's own Back/Next/Exit.
        // Everywhere else, XShape sends the touch to the real desktop.
        if (e->type()==QEvent::TouchBegin) {
            const auto *touch=static_cast<QTouchEvent*>(e);
            if (!touch->touchPoints().isEmpty())
                touchPress=touch->touchPoints().first().pos();
            e->accept();return true;
        }
        if (e->type()==QEvent::TouchUpdate) {e->accept();return true;}
        if (e->type()==QEvent::TouchEnd) {
            const auto *touch=static_cast<QTouchEvent*>(e);
            if (!touch->touchPoints().isEmpty()) {
                const QPointF pos=touch->touchPoints().first().pos();
                if ((pos-touchPress).manhattanLength()<=24)
                    activateGuideControl(pos);
            }
            e->accept();return true;
        }
        if (e->type()==QEvent::TouchCancel) {e->accept();return true;}
        return QWidget::event(e);
    }

private:
    int page=0;
    unsigned seen=0;
    QPoint mousePress;
    QPointF touchPress;
    QElapsedTimer interactionClock;
    QTimer pollTimer;
    Display *display=nullptr;
    Window root=0;
    int xiOpcode=0,xiEvent=0,xiError=0;
    bool xiEnabled=false;
    std::unique_ptr<QSocketNotifier> socketNotifier;
    bool physicalDown=false;
    bool gestureActive=false;
    QPointF gestureStart, gestureLast;
    QPointF pointerLast;

    Rect frac(qreal x,qreal y,qreal w,qreal h) const {
        return Rect(x*width(),y*height(),w*width(),h*height());
    }

    // Cards move away from the live areas they describe. No historical bar
    // screenshots are drawn over the actual desktop controls.
    Rect cardRect() const {
        qreal w=qMin(width()*.82,qreal(700));
        w=qMax(qMin(width()-20.0,w),qreal(230));
        qreal h=qBound(qreal(155),height()*.43,qreal(340));
        if (height()<520) h=height()*.56;
        qreal x=(width()-w)*.5;
        qreal y=height()*.28;
        if (page==1) y=height()*.15;
        if (page==2) y=height()*.27;
        if (page==3) x=qMin(width()-w-8.0,width()*.53);
        if (page==4) x=qMax(8.0,width()*.04);
        if (page==5) y=height()*.36;
        if (page==6) y=height()*.37;
        y=qMin(y,height()-h-12.0);
        return Rect(x,y,w,h);
    }

    Rect buttonBar() const {
        const Rect c=cardRect();
        qreal margin=qBound(6.0,c.width()*.03,18.0);
        return Rect(c.left()+margin,c.bottom()-qMin(64.0,c.height()*.22)-margin,
                    c.width()-margin*2,qMin(64.0,c.height()*.22));
    }
    Rect backRect() const {
        const Rect b=buttonBar();
        return Rect(b.left(),b.top(),b.width()*.24,b.height());
    }
    Rect exitRect() const {
        const Rect b=buttonBar();
        return Rect(b.left()+b.width()*.27,b.top(),b.width()*.22,b.height());
    }
    Rect nextRect() const {
        const Rect b=buttonBar();
        return Rect(b.right()-b.width()*.45,b.top(),b.width()*.45,b.height());
    }

    QVector<Rect> targetRegions() const {
        switch (page) {
        case 1: return {frac(.36,.915,.28,.08),frac(.02,.925,.14,.07),frac(.84,.925,.14,.07)};
        case 2: return {frac(.00,.92,.40,.08),frac(.84,.91,.16,.09)};
        case 3: return {frac(.00,.12,.09,.74)};
        case 4: return {frac(.91,.12,.09,.74)};
        case 5: return {frac(.00,.00,1.0,.085),frac(.34,.16,.32,.16)};
        case 6: return {frac(.0,.0,.145,.085),frac(.43,.0,.14,.085),
                        frac(.85,.0,.15,.085)};
        default: return {};
        }
    }

    void updateInputShape() {
        if (!display || !winId() || width()<1 || height()<1) return;
        const qreal dpi=devicePixelRatioF();
        QVector<XRectangle> shapes;
        auto append=[&](const QRectF &r) {
            const QRect physical=QRect(qRound(r.left()*dpi),qRound(r.top()*dpi),
                                       qMax(1,qRound(r.width()*dpi)),
                                       qMax(1,qRound(r.height()*dpi)));
            XRectangle item;
            item.x=short(physical.x()); item.y=short(physical.y());
            item.width=static_cast<unsigned short>(qBound(1,physical.width(),65535));
            item.height=static_cast<unsigned short>(qBound(1,physical.height(),65535));
            shapes.append(item);
        };
        if (page==0) append(rect()); // Intro: tap anywhere to start.
        else { append(backRect()); append(exitRect()); append(nextRect()); }
        XShapeCombineRectangles(display,static_cast<Window>(winId()),ShapeInput,
                                0,0,shapes.data(),shapes.size(),ShapeSet,Unsorted);
        XFlush(display);
    }

    void activateGuideControl(const QPointF &at) {
        // Some touch drivers also synthesize a mouse release. Never let one
        // physical tap unintentionally activate two consecutive tour steps.
        if (interactionClock.isValid() && interactionClock.elapsed()<180) return;
        interactionClock.restart();
        if (page==0) {nextPage();return;}
        if (backRect().contains(at)) previousPage();
        else if (nextRect().contains(at)) nextPage();
        else if (exitRect().contains(at)) close(); // Incomplete -- first run retries
    }

    void nextPage() {
        if (page==guide().size()-1) {
            markFinished();
            close();
            return;
        }
        ++page;
        seen=0;
        gestureActive=false;
        physicalDown=false;
        updateInputShape();
        update();
    }

    void previousPage() {
        if (page<=0) return;
        --page;
        seen=0;
        gestureActive=false;
        physicalDown=false;
        updateInputShape();
        update();
    }

    void mark(unsigned bit) {
        if (!(seen & bit)) { seen|=bit; update(); }
    }

    QPointF queryPointer() const {
        if (!display) return {};
        Window child=0, rootWindow=0;
        int rx=0,ry=0,wx=0,wy=0;
        unsigned int mask=0;
        if (!XQueryPointer(display,root,&rootWindow,&child,&rx,&ry,&wx,&wy,&mask))
            return pointerLast;
        // Qt logical units: XQueryPointer returns physical root pixel units.
        const qreal factor=qMax(qreal(1.0),devicePixelRatioF());
        return QPointF(rx/factor-geometry().x(),ry/factor-geometry().y());
    }

    bool insideGuideControl(const QPointF &pt) const {
        return page==0 || backRect().contains(pt) || exitRect().contains(pt) ||
               nextRect().contains(pt);
    }

    void beginGesture(const QPointF &pt) {
        if (insideGuideControl(pt) || page==0 || gestureActive) return;
        gestureStart=gestureLast=pt;
        gestureActive=true;
    }

    void moveGesture(const QPointF &pt) {
        if (gestureActive) gestureLast=pt;
    }

    void finishGesture(const QPointF &pt) {
        if (!gestureActive || page==0) { gestureActive=false; return; }
        gestureActive=false;
        const QPointF diff=pt-gestureStart;
        const qreal dx=diff.x(),dy=diff.y();
        const bool click=std::hypot(dx,dy)<qMax(24.0,width()*.025);
        if (page==1) {
            if ((gestureStart.y()>height()*.86 && dy< -height()*.12) ||
                (click && gestureStart.y()>height()*.89 &&
                 std::abs(gestureStart.x()-width()*.5)<width()*.15)) mark(1);
        } else if (page==2 && click && gestureStart.y()>height()*.89) {
            if (gestureStart.x()>width()*.82) mark(2);
            else if (gestureStart.x()<width()*.43) mark(1);
        } else if (page==3) {
            if (gestureStart.x()<width()*.13 && dx>width()*.15 &&
                std::abs(dx)>std::abs(dy)*1.2) mark(1);
        } else if (page==4) {
            if (gestureStart.x()>width()*.87 && dx< -width()*.15 &&
                std::abs(dx)>std::abs(dy)*1.2) mark(1);
        } else if (page==5) {
            if (dy>height()*.13 && std::abs(dy)>std::abs(dx)*1.1) mark(1);
        } else if (page==6 && click && gestureStart.y()<height()*.12) {
            if (gestureStart.x()<width()*.16) mark(1);
            else if (gestureStart.x()>width()*.42 && gestureStart.x()<width()*.58) mark(2);
            else if (gestureStart.x()>width()*.83) mark(4);
        }
    }

    void pollPointer() {
        if (!display || page==0) return;
        Window rootOut=0,child=0;
        int rx=0,ry=0,wx=0,wy=0;
        unsigned int mask=0;
        if (!XQueryPointer(display,root,&rootOut,&child,&rx,&ry,&wx,&wy,&mask)) return;
        const qreal factor=qMax(qreal(1.0),devicePixelRatioF());
        const QPointF pt(rx/factor-geometry().x(),ry/factor-geometry().y());
        pointerLast=pt;
        const bool down=(mask & Button1Mask)!=0;
        if (down && !physicalDown) beginGesture(pt);
        if (down) moveGesture(pt);
        if (!down && physicalDown) finishGesture(pt);
        physicalDown=down;
    }

    void readXInputEvents() {
        if (!display || !xiEnabled) return;
        while (XPending(display)>0) {
            XEvent e;
            XNextEvent(display,&e);
            if (e.type!=GenericEvent || e.xcookie.extension!=xiOpcode ||
                !XGetEventData(display,&e.xcookie)) continue;
            const int kind=e.xcookie.evtype;
            if (kind==XI_RawButtonPress || kind==XI_RawTouchBegin) {
                beginGesture(queryPointer());
            } else if (kind==XI_RawTouchUpdate) {
                moveGesture(queryPointer());
            } else if (kind==XI_RawButtonRelease || kind==XI_RawTouchEnd) {
                finishGesture(queryPointer());
            }
            XFreeEventData(display,&e.xcookie);
        }
    }

    void drawGlow(QPainter &p,const QString &text,const QFont &font,
                  const QPointF &baseline,Glow effect) const {
        if (effect==Plain || text.isEmpty()) return;
        QColor tint;
        qreal alpha=.4,scale=.275;
        switch (effect) {
        case Purple: tint=QColor(203,0,180); break;
        case Green: tint=QColor(57,212,1); scale=.45; break;
        case White: tint=Qt::white; alpha=.67;scale=.222; break;
        case SoftWhite: tint=Qt::white; alpha=.60;scale=.20;break;
        default:return;
        }
        QPainterPath glyphs;
        glyphs.addText(baseline,font,text);
        const qreal radius=qMax(1.3,font.pixelSize()*scale);
        p.save(); p.setBrush(Qt::NoBrush);
        for (int i=9;i>=1;--i) {
            QColor layer=tint;
            layer.setAlphaF(alpha*(i<4 ? .12:.065));
            p.strokePath(glyphs,QPen(layer,radius*(2.0*i/9.0),
                                     Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        }
        p.restore();
    }

    void drawLines(QPainter &p,const QRectF &bounds,const QVector<TextRun> &lines,
                   qreal preferredSize) const {
        if (bounds.isEmpty() || lines.isEmpty()) return;
        QFont font("Comfortaa");
        font.setWeight(QFont::Normal);
        font.setStyleStrategy(QFont::PreferAntialias);
        int lo=8,hi=qMax(lo,qRound(preferredSize)),size=lo;
        while (lo<=hi) {
            const int mid=(lo+hi)/2;
            font.setPixelSize(mid);
            QFontMetricsF fm(font);
            qreal longest=0;
            for (const TextRun &line:lines)
                longest=qMax(longest,fm.horizontalAdvance(line.text));
            if (longest<=bounds.width()*.95 &&
                fm.lineSpacing()*lines.size()*1.04<=bounds.height()*.95) {
                size=mid;lo=mid+1;
            } else hi=mid-1;
        }
        font.setPixelSize(size);
        QFontMetricsF fm(font);
        const qreal lineSpace=fm.lineSpacing()*1.04;
        qreal cursorY=bounds.center().y()-lineSpace*lines.size()*.5;
        p.save();p.setClipRect(bounds.adjusted(-6,-6,6,6));p.setFont(font);
        for (const TextRun &line:lines) {
            const qreal x=bounds.center().x()-fm.horizontalAdvance(line.text)*.5;
            const QPointF baseline(x,cursorY+fm.ascent());
            if (line.glow!=Plain) drawGlow(p,line.prefix<0 ? line.text :
                                             line.text.left(line.prefix),font,baseline,line.glow);
            p.setPen(QColor("#070a0d"));
            p.drawText(baseline,line.text);
            cursorY+=lineSpace;
        }
        p.restore();
    }

    void drawButton(QPainter &p,const Rect &area,const QString &label,bool primary) const {
        p.save();
        QPainterPath shape;
        shape.addRoundedRect(area,qMin(14.0,area.height()*.32),
                             qMin(14.0,area.height()*.32));
        p.fillPath(shape, primary ? QColor(18,27,37,238) : QColor(255,255,255,200));
        p.setPen(QPen(primary ? QColor("#39d401") : QColor("#5c7586"),1.7));
        p.drawPath(shape);
        QFont font("Comfortaa");font.setWeight(QFont::DemiBold);
        font.setPixelSize(qBound(11,qRound(area.height()*.36),20));
        p.setFont(font);
        p.setPen(primary ? QColor(Qt::white) : QColor("#142027"));
        p.drawText(area,Qt::AlignCenter,label);
        p.restore();
    }

    QString statusLine() const {
        if (!seen) return "Try the highlighted control, then choose Next.";
        if (page==2 && (seen&2))
            return "Keyboard icon selected. You can hide it and continue.";
        if (page==6) {
            const int n=((seen&1)!=0)+((seen&2)!=0)+((seen&4)!=0);
            return QString("Top-bar areas tried: %1 of 3. Keep exploring or continue.").arg(n);
        }
        return "Interaction detected. Explore the desktop or choose Next.";
    }

    void drawInstructionCard(QPainter &p) const {
        const Rect c=cardRect();
        p.save();
        p.setPen(Qt::NoPen);p.setBrush(QColor(0,0,0,80));
        p.drawRoundedRect(c.translated(4,7),20,20);
        QPainterPath path;
        path.addRoundedRect(c,20,20);
        p.fillPath(path,deckGradient(page,c,247));
        p.setPen(QPen(QColor(40,61,68,135),1.4));
        p.drawPath(path);
        p.setClipPath(path);
        const qreal margin=qBound(10.0,c.width()*.038,25.0);
        const qreal barHeight=buttonBar().height();
        const qreal infoHeight=qMin(qreal(72),c.height()*.24);
        const qreal textHeight=c.height()-barHeight-infoHeight-margin*2;
        drawLines(p,Rect(c.left()+margin,c.top()+margin,
                         c.width()-margin*2,textHeight),guide().at(page).lines,
                  qMin(height()*.071,qreal(48)));
        QFont font("Comfortaa");font.setPixelSize(qBound(10,qRound(height()*.018),17));
        p.setFont(font);p.setPen(QColor("#182530"));
        const Rect details(c.left()+margin,c.top()+margin+textHeight,
                           c.width()-margin*2,infoHeight*.55);
        p.drawText(details,Qt::AlignCenter|Qt::TextWordWrap,guide().at(page).instruction);
        font.setPixelSize(qBound(9,qRound(height()*.014),13));p.setFont(font);
        p.setPen(QColor("#27525b"));
        p.drawText(Rect(c.left()+margin,details.bottom(),c.width()-margin*2,infoHeight*.40),
                   Qt::AlignCenter|Qt::TextWordWrap,statusLine());
        p.restore();
        drawButton(p,backRect(),"Back",false);
        drawButton(p,exitRect(),"Exit",false);
        drawButton(p,nextRect(),page==6 ? "Finish" : "Next",true);
    }

    void drawFooter(QPainter &p) const {
        p.save();
        const qreal r=qBound(qreal(2),height()*.004,qreal(5));
        const qreal gap=r*3.7;
        const qreal start=width()*.5-gap*3;
        const qreal y=height()-qMax(10.0,height()*.017);
        p.setPen(Qt::NoPen);
        for (int i=0;i<guide().size();++i) {
            p.setBrush(i==page ? QColor(10,38,54,230) : QColor(10,38,54,85));
            p.drawEllipse(QPointF(start+gap*i,y),r,r);
        }
        p.restore();
    }
};

} // namespace

int main(int argc,char **argv) {
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc,argv);
    QCoreApplication::setOrganizationName("AlternixOS");
    QCoreApplication::setApplicationName("welcome");

    const QStringList args=app.arguments();
    const bool firstRun=args.contains("--once");
    if (firstRun && (isLiveSystem() || QFileInfo::exists(donePath()))) return 0;

    QLockFile lock(QDir::tempPath()+"/alternix-welcome-"+QString::number(getuid())+".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) return 0;

    WelcomeOverlay overlay;
    const int delay=(firstRun || args.contains("--startup-delay")) ? 3200 : 0;
    auto open=[&overlay]() {
        QScreen *screen=QGuiApplication::screenAt(QCursor::pos());
        if (!screen) screen=QGuiApplication::primaryScreen();
        if (!screen) { qCritical("welcome: no desktop screen"); return; }
        // An X11 override-redirect covers the real screen without asking Qtile
        // to create a managed fullscreen app and without hiding its panels.
        overlay.fitScreen(screen->geometry());
        overlay.show();
        if (!overlay.attachX11()) {overlay.close();return;}
        overlay.raise();
        QObject::connect(screen,&QScreen::geometryChanged,&overlay,
                         [&overlay](const QRect &geometry){overlay.fitScreen(geometry);});
    };
    auto *startWatcher=new QTimer(&overlay);
    startWatcher->setInterval(600);
    QObject::connect(startWatcher,&QTimer::timeout,&overlay,[&overlay,startWatcher,open]() {
        if (lockscreenActive()) return;
        startWatcher->stop();
        open();
    });
    QTimer::singleShot(delay,&overlay,[startWatcher](){startWatcher->start();});
    return app.exec();
}
