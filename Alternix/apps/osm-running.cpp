#include <QApplication>
#include <QCoreApplication>
#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QGraphicsDropShadowEffect>
#include <QMouseEvent>
#include <QLockFile>
#include <QDir>
#include <QDebug>
#include <QPixmap>
#include <QImage>
#include <QIcon>
#include <QScroller>
#include <QScrollerProperties>
#include <QPropertyAnimation>
#include <QEasingCurve>

#include <functional>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

// ───────────────────────────────────────────── X11 helpers
static Atom getAtom(Display *dpy, const char *name) {
    return XInternAtom(dpy, name, False);
}

static QString getWindowTitle(Display *dpy, Window win) {
    Atom prop = getAtom(dpy, "_NET_WM_NAME");
    Atom utf8 = getAtom(dpy, "UTF8_STRING");

    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *data = nullptr;

    if (XGetWindowProperty(dpy, win, prop, 0, (~0L), False, utf8,
                           &type, &format, &nitems, &after, &data)
        == Success && data)
    {
        QString out = QString::fromUtf8((char*)data);
        XFree(data);
        if (!out.isEmpty()) return out;
    }

    XTextProperty tp;
    if (XGetWMName(dpy, win, &tp) && tp.value) {
        QString out = QString::fromLatin1((char*)tp.value);
        XFree(tp.value);
        return out;
    }

    return "";
}

static QString getWindowClass(Display *dpy, Window win) {
    XClassHint hint;
    if (XGetClassHint(dpy, win, &hint)) {
        QString cls = hint.res_class ? QString::fromLatin1(hint.res_class) : "";
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
        return cls.toLower();
    }
    return "";
}

static QPixmap getNetWmIcon(Display *dpy, Window win, int size = 28) {
    Atom prop = getAtom(dpy, "_NET_WM_ICON");

    Atom type;
    int format;
    unsigned long nitems, after;
    unsigned char *raw = nullptr;

    if (XGetWindowProperty(dpy, win, prop, 0, (~0L), False,
                           AnyPropertyType,
                           &type, &format, &nitems, &after, &raw) != Success || !raw)
    {
        if (raw) XFree(raw);
        return QPixmap();
    }

    unsigned long *data = (unsigned long*)raw;
    unsigned long len = nitems;

    int bestW = 0, bestH = 0;
    unsigned long bestOffset = 0;

    unsigned long i = 0;
    while (i + 1 < len) {
        unsigned long w = data[i];
        unsigned long h = data[i+1];
        unsigned long count = w * h;

        if (w < 1 || h < 1 || i + 2 + count > len)
            break;

        if ((int)w*h > bestW * bestH) {
            bestW = w;
            bestH = h;
            bestOffset = i + 2;
        }

        i += 2 + count;
    }

    QPixmap out;
    if (bestOffset > 0) {
        QImage img(bestW, bestH, QImage::Format_ARGB32);
        unsigned long *pix = data + bestOffset;

        for (int y = 0; y < bestH; ++y)
        for (int x = 0; x < bestW; ++x) {
            unsigned long p = pix[y * bestW + x];
            img.setPixel(x,y, qRgba((p>>16)&0xFF,(p>>8)&0xFF,p&0xFF,(p>>24)&0xFF));
        }

        out = QPixmap::fromImage(img)
                .scaled(size,size,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    }

    XFree(raw);
    return out;
}

// ───────────────────────────────────────────── Structures

struct WindowInfo {
    Window id;
    QString title;
    QString appClass;
};

class SidePanel;
class WindowCard;
class OverlayRoot;   // forward

// ───────────────────────────────────────────── SidePanel

class SidePanel : public QWidget {
public:
    explicit SidePanel(Display *dpy, QWidget *parent=nullptr);

    void refreshWindows();
    void activateWindow(Window w);
    void closeAppWindow(Window w);
    void handleEntryActivateAndClose(Window w);
    void resizeToItems(int count);

    int computeRequiredWidth(const QStringList &titles) {
        int base = 160;
        QFont f; f.setPointSize(32);
        QFontMetrics fm(f);
        int max = 0;
        for (const QString &t : titles)
            max = qMax(max, fm.horizontalAdvance(t));
        return base + max;
    }

    void setCloseCallback(std::function<void()> fn) { onClose = fn; }

public:
    std::function<void()> onClose;

private:
    Display *m_dpy;
    QWidget *m_inner;
    QScrollArea *m_scroll;
    QVBoxLayout *m_list;
    int m_width;
    int m_maxH;
};

// ───────────────────────────────────────────── WindowCard

class WindowCard : public QFrame {
public:
    WindowCard(SidePanel*, Display*, const WindowInfo&, Window active, QWidget *parent=nullptr);

protected:
    void mousePressEvent(QMouseEvent *e) override;

private:
    SidePanel *m_panel;
    Display *m_dpy;
    WindowInfo m_info;
    QLabel *m_titleLabel;
};

// ───────────────────────────────────────────── SidePanel impl

SidePanel::SidePanel(Display *dpy, QWidget *parent)
    : QWidget(parent), m_dpy(dpy)
{
    setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TranslucentBackground);

    QRect g = QGuiApplication::primaryScreen()->geometry();

    m_width = qMin(520, int(g.width()*0.35));
    m_maxH  = int(g.height()*0.9);

    int top = int(g.height()*0.20);
    setGeometry(0, top, m_width, 200);

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 20, 20, 20);  // top/bottom padding

    m_inner = new QWidget(this);
    m_inner->setObjectName("inner");
    m_inner->setStyleSheet(
        "#inner{"
        " background:#80708099;"
        " border-top-left-radius:0px;"
        " border-bottom-left-radius:0px;"
        " border-top-right-radius:26px;"
        " border-bottom-right-radius:26px;"
        "}"
    );

    QVBoxLayout *inner = new QVBoxLayout(m_inner);
    inner->setContentsMargins(16, 16, 16, 16);

    m_scroll = new QScrollArea(m_inner);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_scroll->setStyleSheet("QScrollArea, QScrollArea * { background:#00000099; border-radius:14px; border:none; }");

    QScroller::grabGesture(m_scroll->viewport(), QScroller::TouchGesture);

    QWidget *content = new QWidget;
    content->setStyleSheet("background:#00000099;");
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    m_list = new QVBoxLayout(content);
    m_list->setSpacing(2);
    m_list->setContentsMargins(5, 5, 10, 15);

    m_scroll->setWidget(content);

    inner->addWidget(m_scroll);
    outer->addWidget(m_inner);

    // shadow
    QGraphicsDropShadowEffect *sh = new QGraphicsDropShadowEffect(this);
    sh->setBlurRadius(32);
    sh->setOffset(0, 0);
    sh->setColor(QColor(0, 0, 0, 220));
    m_inner->setGraphicsEffect(sh);

    QTimer *t = new QTimer(this);
    t->setInterval(600);
    connect(t, &QTimer::timeout, [this](){ refreshWindows(); });
    t->start();

    refreshWindows();
}

void SidePanel::activateWindow(Window w) {
    XRaiseWindow(m_dpy, w);
    XSetInputFocus(m_dpy, w, RevertToPointerRoot, CurrentTime);

    Atom act = getAtom(m_dpy,"_NET_ACTIVE_WINDOW");
    Window root = DefaultRootWindow(m_dpy);

    XEvent e; memset(&e,0,sizeof(e));
    e.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = act;
    e.xclient.format = 32;
    e.xclient.data.l[0] = 1;
    e.xclient.data.l[1] = CurrentTime;

    XSendEvent(m_dpy,root,False,
               SubstructureNotifyMask|SubstructureRedirectMask,&e);
    XFlush(m_dpy);
}

void SidePanel::closeAppWindow(Window w) {
    XKillClient(m_dpy,w);
    XFlush(m_dpy);
    refreshWindows();
}

void SidePanel::handleEntryActivateAndClose(Window w) {
    activateWindow(w);
    QTimer::singleShot(80, [this](){
        if (onClose) onClose();
    });
}

// ───────────────────────────────────────────── HEIGHT BASED ON COUNT

void SidePanel::resizeToItems(int count) {
    const int cardH = 120;
    int h = count * cardH + 60;

    h = qBound(120, h, m_maxH);

    QRect screenGeo = QGuiApplication::primaryScreen()->geometry();
    int top = int(screenGeo.height() * 0.15);

    setGeometry(0, top, m_width, h);
}

void SidePanel::refreshWindows() {
    Atom listA = getAtom(m_dpy,"_NET_CLIENT_LIST");

    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data=nullptr;

    if(XGetWindowProperty(m_dpy,DefaultRootWindow(m_dpy),
                          listA,0,(~0L),False,XA_WINDOW,
                          &type,&format,&n,&after,&data)!=Success || !data)
    {
        if(data) XFree(data);
        return;
    }

    Window *wins=(Window*)data;

    // active window
    Atom actA=getAtom(m_dpy,"_NET_ACTIVE_WINDOW");
    unsigned char *awD=nullptr;
    unsigned long ni,ba;
    Window active=0;
    if(XGetWindowProperty(m_dpy,DefaultRootWindow(m_dpy),
                          actA,0,(~0L),False,AnyPropertyType,
                          &type,&format,&ni,&ba,&awD)==Success && awD)
    {
        active=*(Window*)awD;
        XFree(awD);
    }

    // clear list
    QLayoutItem *it;
    while((it=m_list->takeAt(0))) {
        if(it->widget()) it->widget()->deleteLater();
        delete it;
    }

    QStringList titles;
    int count = 0;

    // collect windows
    for(unsigned long i=0;i<n;i++) {
        Window w=wins[i];
        if(!w) continue;

        QString title=getWindowTitle(m_dpy,w);
        if(title.isEmpty()) continue;

        if(title.toLower().contains("osm-running")) continue;
        if(title.toLower().contains("osm-launcher")) continue;
        if(title.toLower().contains("wosp-shell")) continue;

        QString cls=getWindowClass(m_dpy,w);
        titles << title;

        WindowInfo info{ w,title,cls };
        WindowCard *card=new WindowCard(this,m_dpy,info,active);
        m_list->addWidget(card);
        count++;
    }

    XFree(data);

    // compute and apply width
    int needed = computeRequiredWidth(titles);
    m_width = qMin(needed, 1080);

    QRect g = geometry();
    if (g.width() != m_width)
        setGeometry(g.x(), g.y(), m_width, g.height());

    resizeToItems(count);

    if(count==0 && onClose) onClose();
}

// ───────────────────────────────────────────── WindowCard impl

WindowCard::WindowCard(
    SidePanel *panel, Display *dpy,
    const WindowInfo &info, Window active,
    QWidget *parent)
    : QFrame(parent), m_panel(panel), m_dpy(dpy), m_info(info)
{
    Q_UNUSED(active);

    setMinimumHeight(75);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    setStyleSheet("background:#282828;border-radius:14px;border:none;");

    QHBoxLayout *lay=new QHBoxLayout(this);
    lay->setContentsMargins(10,2,10,2);
    lay->setSpacing(2);

    QLabel *icon=new QLabel(this);
    icon->setFixedSize(32,32);

    QPixmap px=getNetWmIcon(m_dpy,m_info.id,28);
    if(px.isNull()) {
        QIcon themed=QIcon::fromTheme(m_info.appClass);
        if(!themed.isNull()) px=themed.pixmap(64,64);
    }
    if(px.isNull()) {
        px=QPixmap(28,28);
        px.fill(QColor("#333"));
    }

    icon->setPixmap(px);
    icon->setScaledContents(true);

    QLabel *title = new QLabel(m_info.title, this);
    title->setStyleSheet("color:white;font-size:15px;");
    title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_titleLabel = title;

    QPushButton *close=new QPushButton("❌",this);
    close->setFixedSize(48,48);
    close->setStyleSheet(
        "QPushButton{color:#ff4a6a;background:#00000099;border:none;"
        "border-radius:14px;font-size:26px;}"
        "QPushButton:hover { color:#ff1616; background:#ad1236; border-radius:18px; }"
        "QPushButton:pressed { color:#ffffff; background:#550000; border-radius:18px; }"
    );

    lay->addWidget(icon);
    lay->addWidget(title,1);
    lay->addWidget(close);

    connect(close,&QPushButton::clicked,[this](){
        m_panel->closeAppWindow(m_info.id);
    });
}

void WindowCard::mousePressEvent(QMouseEvent *e) {
    if(e->button()==Qt::LeftButton) {
        if (m_titleLabel && m_titleLabel->geometry().contains(e->pos())) {
            m_panel->handleEntryActivateAndClose(m_info.id);
            return;
        }
    }
    QFrame::mousePressEvent(e);
}

// ───────────────────────────────────────────── OverlayRoot

class OverlayRoot : public QWidget {
public:
    explicit OverlayRoot(Display *dpy)
        : m_panel(nullptr),
          m_panelVisible(false),
          m_dpy(dpy)
    {
        setWindowFlags(Qt::FramelessWindowHint |
                       Qt::Tool |
                       Qt::WindowStaysOnTopHint |
                       Qt::X11BypassWindowManagerHint |
                       Qt::WindowDoesNotAcceptFocus);

        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);

        m_screenGeo = QGuiApplication::primaryScreen()->geometry();
        setGeometry(m_screenGeo);

        m_panel = new SidePanel(dpy,this);

        QRect finalGeo = m_panel->geometry();
        QRect startGeo = finalGeo;
        startGeo.moveLeft(-finalGeo.width());
        m_panel->setGeometry(startGeo);
        m_panel->hide();

        m_panel->setCloseCallback([this]() {
            hidePanel();
        });

        hide(); // start hidden
    }

    void showPanel() {
        if (m_panelVisible) return;
        m_panelVisible = true;

        setGeometry(m_screenGeo);
        show();
        raise();

        QRect finalGeo = m_panel->geometry();
        finalGeo = QRect(0, finalGeo.y(), finalGeo.width(), finalGeo.height());
        QRect startGeo = finalGeo;
        startGeo.moveLeft(-finalGeo.width());
        m_panel->setGeometry(startGeo);
        m_panel->show();

        QPropertyAnimation *anim = new QPropertyAnimation(m_panel,"geometry",this);
        anim->setDuration(220);
        anim->setStartValue(startGeo);
        anim->setEndValue(finalGeo);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }

    void hidePanel() {
        if (!m_panelVisible || !m_panel) return;
        m_panelVisible = false;

        QRect cur = m_panel->geometry();
        QRect endGeo = cur;
        endGeo.moveLeft(-cur.width());

        QPropertyAnimation *anim = new QPropertyAnimation(m_panel,"geometry",this);
        anim->setDuration(220);
        anim->setStartValue(cur);
        anim->setEndValue(endGeo);
        anim->setEasingCurve(QEasingCurve::InCubic);
        connect(anim,&QPropertyAnimation::finished,[this](){
            if (m_panel) m_panel->hide();
            hide();
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (m_panel && !m_panel->geometry().contains(e->pos()))
            hidePanel();
        else
            QWidget::mousePressEvent(e);
    }

private:
    SidePanel *m_panel;
    bool m_panelVisible;
    QRect m_screenGeo;
    Display *m_dpy;
};

// ───────────────────────────────────────────── ActivationEdgeBar
// Always-on-top, left-side touch bar. Periodically re-raises itself.

class ActivationEdgeBar : public QWidget {
public:
    explicit ActivationEdgeBar(OverlayRoot *overlay, QWidget *parent = nullptr)
        : QWidget(parent),
          m_overlay(overlay),
          m_dragging(false)
    {
        setWindowFlags(Qt::FramelessWindowHint
                       | Qt::WindowStaysOnTopHint
                       | Qt::BypassWindowManagerHint
                       | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setMouseTracking(true);

        QRect g = QGuiApplication::primaryScreen()->geometry();
        setGeometry(g.x(), g.y(), 25, g.height());
        setStyleSheet("background: rgba(0,0,0,0);");

        show();
        raise();

        // Keep stubbornly on top, like a real gesture edge.
        m_raiseTimer = new QTimer(this);
        m_raiseTimer->setInterval(1500);   // 1.5s is usually enough
        connect(m_raiseTimer, &QTimer::timeout, this, [this]() {
            this->raise();
        });
        m_raiseTimer->start();
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            m_dragging = true;
            m_pressPos = e->globalPos();
            raise();  // assert on top when touched
        }
        QWidget::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!m_dragging) {
            QWidget::mouseMoveEvent(e);
            return;
        }

        int dx = e->globalX() - m_pressPos.x();
        if (dx > 12) {  // slide-right threshold
            if (m_overlay) {
                m_overlay->showPanel();
            }
            m_dragging = false;
        }

        QWidget::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        m_dragging = false;
        QWidget::mouseReleaseEvent(e);
    }

    void paintEvent(QPaintEvent *) override {
        // fully invisible
    }

private:
    OverlayRoot *m_overlay;
    bool m_dragging;
    QPoint m_pressPos;
    QTimer *m_raiseTimer;
};

// ───────────────────────────────────────────── main

int main(int argc,char**argv) {
    QApplication app(argc,argv);

    QLockFile lock(QDir::temp().absoluteFilePath("osm-running.lock"));
    lock.setStaleLockTime(0);
    if(!lock.tryLock(20))
        return 0;

    Display *dpy=XOpenDisplay(nullptr);
    if(!dpy) return 1;

    OverlayRoot root(dpy);          // overlay window
    ActivationEdgeBar bar(&root);   // always-on-top gesture edge

    int r=app.exec();
    XCloseDisplay(dpy);
    return r;
}
