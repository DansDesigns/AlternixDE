// ─────────────────────────────────────────────────────────────
//  osm-widgets  —  Alternix desktop widget overlay
//
//  A frameless, override-redirect window per screen that sits
//  permanently at the bottom of the stack, directly on the Qtile
//  desktop. Widgets are dynamically loaded .so plugins from:
//      /usr/local/lib/alternix/widgets/
//
//  Layout and state live in:
//      ~/.config/Alternix/osm-widgets.conf
//
//  The file is watched; osm-widgets-settings just writes the file
//  and this daemon reloads itself. No IPC, no D-Bus.
//
//  Build:
//    g++ -fPIC osm-widgets.cpp -o osm-widgets -ldl \
//        $(pkg-config --cflags --libs Qt5Widgets) -lX11 -lXext
// ─────────────────────────────────────────────────────────────

#include <QApplication>
#include <QWidget>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>
#include <QScreen>
#include <QTimer>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QLibrary>
#include <QLockFile>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QDateTime>
#include <QMap>
#include <QList>
#include <QDebug>

#include "osm-widget-plugin.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>

static const char *WIDGET_DIR = "/usr/local/lib/alternix/widgets";

static const int  GRIP_SIZE   = 34;   // bottom-right resize corner, edit mode
static const int  LOWER_MS    = 2000; // how often we re-assert bottom of stack

typedef const OsmWidgetInfo *(*InfoFn)();
typedef QWidget *(*CreateFn)(const char *, QWidget *);

static Display *g_dpy = nullptr;

// ─────────────────────────────────────────────────────────────
//  X11 helpers
// ─────────────────────────────────────────────────────────────
static void setDesktopWindowType(WId wid)
{
    if (!g_dpy) return;
    Window w = (Window)wid;

    Atom type     = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom desktop  = XInternAtom(g_dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    XChangeProperty(g_dpy, w, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&desktop, 1);

    Atom state    = XInternAtom(g_dpy, "_NET_WM_STATE", False);
    Atom below    = XInternAtom(g_dpy, "_NET_WM_STATE_BELOW", False);
    Atom skipTb   = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom skipPg   = XInternAtom(g_dpy, "_NET_WM_STATE_SKIP_PAGER", False);
    Atom sticky   = XInternAtom(g_dpy, "_NET_WM_STATE_STICKY", False);
    Atom states[4] = { below, skipTb, skipPg, sticky };
    XChangeProperty(g_dpy, w, state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)states, 4);

    // Present on every group / virtual desktop.
    Atom deskProp = XInternAtom(g_dpy, "_NET_WM_DESKTOP", False);
    unsigned long all = 0xFFFFFFFF;
    XChangeProperty(g_dpy, w, deskProp, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&all, 1);

    XLowerWindow(g_dpy, w);
    XFlush(g_dpy);
}

static void lowerWindow(WId wid)
{
    if (!g_dpy) return;
    XLowerWindow(g_dpy, (Window)wid);
    XFlush(g_dpy);
}

// Restrict the clickable region of the overlay to the given rectangles,
// so taps on bare desktop fall straight through to the root window.
// An empty list makes the whole overlay click-through.
static void setInputRects(WId wid, const QList<QRect> &rects)
{
    if (!g_dpy) return;
    Window w = (Window)wid;

    int base = 0, err = 0;
    if (!XShapeQueryExtension(g_dpy, &base, &err))
        return;   // no SHAPE extension — overlay simply eats desktop clicks

    if (rects.isEmpty()) {
        XRectangle empty = { 0, 0, 0, 0 };
        XShapeCombineRectangles(g_dpy, w, ShapeInput, 0, 0,
                                &empty, 1, ShapeSet, Unsorted);
        XFlush(g_dpy);
        return;
    }

    QVector<XRectangle> xr;
    xr.reserve(rects.size());
    for (const QRect &r : rects) {
        XRectangle x;
        x.x      = (short)r.x();
        x.y      = (short)r.y();
        x.width  = (unsigned short)r.width();
        x.height = (unsigned short)r.height();
        xr.append(x);
    }

    XShapeCombineRectangles(g_dpy, w, ShapeInput, 0, 0,
                            xr.data(), xr.size(), ShapeSet, Unsorted);
    XFlush(g_dpy);
}

static void clearInputShape(WId wid)
{
    if (!g_dpy) return;
    int base = 0, err = 0;
    if (!XShapeQueryExtension(g_dpy, &base, &err)) return;
    XShapeCombineMask(g_dpy, (Window)wid, ShapeInput, 0, 0, None, ShapeSet);
    XFlush(g_dpy);
}

// ─────────────────────────────────────────────────────────────
//  Instance record, straight out of the config file
// ─────────────────────────────────────────────────────────────
struct Instance {
    QString id;       // "clock-1"
    QString plugin;   // "clock"
    QRect   geo;      // relative to the overlay origin
    int     screen;
};

// ─────────────────────────────────────────────────────────────
//  WidgetFrame — one hosted widget, draggable in edit mode
// ─────────────────────────────────────────────────────────────
class WidgetFrame : public QFrame {
public:
    WidgetFrame(const Instance &inst, QWidget *body,
                bool editMode, QWidget *parent)
        : QFrame(parent),
          m_inst(inst),
          m_edit(editMode),
          m_mode(None_),
          m_minW(80),
          m_minH(60)
    {
        setGeometry(inst.geo);
        setAttribute(Qt::WA_TranslucentBackground);

        QVBoxLayout *v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        if (body) {
            body->setParent(this);
            v->addWidget(body);
        }

        if (m_edit)
            setCursor(Qt::OpenHandCursor);
    }

    void setMinSize(int w, int h) { m_minW = w; m_minH = h; }
    const QString &instanceId() const { return m_inst.id; }

protected:
    // Edit-mode chrome: dashed outline plus a corner grip.
    void paintEvent(QPaintEvent *e) override {
        QFrame::paintEvent(e);
        if (!m_edit) return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        QPen pen(QColor("#777"));
        pen.setStyle(Qt::DashLine);
        pen.setWidth(3);
        p.setPen(pen);
        p.setBrush(QColor(40, 40, 40, 90));
        p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 12, 12);

        QRect grip(width() - GRIP_SIZE, height() - GRIP_SIZE,
                   GRIP_SIZE, GRIP_SIZE);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(80, 80, 80, 200));
        p.drawRoundedRect(grip, 10, 10);
        p.setPen(QColor("#dddddd"));
        for (int i = 1; i <= 3; ++i) {
            int off = i * 7;
            p.drawLine(grip.right() - off, grip.bottom() - 4,
                       grip.right() - 4,   grip.bottom() - off);
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (!m_edit || e->button() != Qt::LeftButton) {
            QFrame::mousePressEvent(e);
            return;
        }
        QRect grip(width() - GRIP_SIZE, height() - GRIP_SIZE,
                   GRIP_SIZE, GRIP_SIZE);

        m_pressGlobal = e->globalPos();
        m_startGeo    = geometry();
        m_mode        = grip.contains(e->pos()) ? Resize : Move;
        setCursor(m_mode == Resize ? Qt::SizeFDiagCursor : Qt::ClosedHandCursor);
        raise();
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (m_mode == None_) { QFrame::mouseMoveEvent(e); return; }

        QPoint d = e->globalPos() - m_pressGlobal;

        if (m_mode == Move) {
            QPoint np = m_startGeo.topLeft() + d;
            // keep at least a corner on-screen
            QWidget *pw = parentWidget();
            if (pw) {
                np.setX(qBound(-width() + 60,  np.x(), pw->width()  - 60));
                np.setY(qBound(-height() + 60, np.y(), pw->height() - 60));
            }
            move(np);
        } else {
            int nw = qMax(m_minW, m_startGeo.width()  + d.x());
            int nh = qMax(m_minH, m_startGeo.height() + d.y());
            resize(nw, nh);
        }
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (m_mode == None_) { QFrame::mouseReleaseEvent(e); return; }
        m_mode = None_;
        setCursor(Qt::OpenHandCursor);
        saveGeometry();
        e->accept();
    }

private:
    enum DragMode { None_, Move, Resize };

    void saveGeometry() {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
        QString g = "Instance-" + m_inst.id + "/";
        cfg.setValue(g + "x", x());
        cfg.setValue(g + "y", y());
        cfg.setValue(g + "w", width());
        cfg.setValue(g + "h", height());
        cfg.sync();
    }

    Instance m_inst;
    bool     m_edit;
    DragMode m_mode;
    QPoint   m_pressGlobal;
    QRect    m_startGeo;
    int      m_minW, m_minH;
};

// ─────────────────────────────────────────────────────────────
//  DesktopOverlay — one per QScreen
// ─────────────────────────────────────────────────────────────
class DesktopOverlay : public QWidget {
public:
    explicit DesktopOverlay(QScreen *screen, int index)
        : QWidget(nullptr), m_screen(screen), m_index(index), m_edit(false)
    {
        // Override-redirect: the WM never sees this window, so Qtile
        // cannot tile it, and it cannot steal focus or a bar slot.
        setWindowFlags(Qt::FramelessWindowHint
                       | Qt::X11BypassWindowManagerHint
                       | Qt::WindowDoesNotAcceptFocus);

        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFocusPolicy(Qt::NoFocus);

        applyGeometry();

        m_lowerTimer = new QTimer(this);
        m_lowerTimer->setInterval(LOWER_MS);
        connect(m_lowerTimer, &QTimer::timeout, this, [this]() {
            if (isVisible()) lowerWindow(winId());
        });
    }

    void applyGeometry() {
        if (!m_screen) return;
        // availableGeometry() honours Qtile's top and bottom bar struts,
        // so widgets never end up parked underneath a bar.
        setGeometry(m_screen->availableGeometry());
    }

    void startLowering() {
        setDesktopWindowType(winId());
        lowerWindow(winId());
        m_lowerTimer->start();
    }

    void setEditMode(bool on) { m_edit = on; }

    // Recompute the click-through mask from current child frames.
    void refreshInputShape() {
        if (m_edit) {                 // editing: whole overlay must take taps
            clearInputShape(winId());
            return;
        }
        QList<QRect> rects;
        for (WidgetFrame *f : m_frames)
            if (f->isVisible()) rects.append(f->geometry());
        setInputRects(winId(), rects);
    }

    void addFrame(WidgetFrame *f) { m_frames.append(f); }
    void clearFrames() {
        for (WidgetFrame *f : m_frames) f->deleteLater();
        m_frames.clear();
    }

    int index() const { return m_index; }

private:
    QScreen           *m_screen;
    int                m_index;
    bool               m_edit;
    QTimer            *m_lowerTimer;
    QList<WidgetFrame*> m_frames;
};

// ─────────────────────────────────────────────────────────────
//  WidgetHost — config, plugins, overlays
// ─────────────────────────────────────────────────────────────
class WidgetHost : public QObject {
public:
    WidgetHost() : m_watcher(nullptr), m_enabled(true), m_edit(false) {}

    void start() {
        buildOverlays();
        reload();

        m_watcher = new QFileSystemWatcher(this);
        watchConfig();
        connect(m_watcher, &QFileSystemWatcher::fileChanged,
                this, &WidgetHost::onConfigTouched);
        connect(m_watcher, &QFileSystemWatcher::directoryChanged,
                this, &WidgetHost::onConfigTouched);

        // Debounce: QSettings rewrites atomically, which fires the
        // watcher two or three times for a single save.
        m_debounce = new QTimer(this);
        m_debounce->setSingleShot(true);
        m_debounce->setInterval(350);
        connect(m_debounce, &QTimer::timeout, this, [this]() {
            watchConfig();
            reload();
        });

        // Rotation / hotplug.
        connect(qApp, &QGuiApplication::screenAdded,
                this, [this](QScreen *) { rebuildAll(); });
        connect(qApp, &QGuiApplication::screenRemoved,
                this, [this](QScreen *) { rebuildAll(); });
        for (QScreen *s : QGuiApplication::screens()) {
            connect(s, &QScreen::geometryChanged,
                    this, [this](const QRect &) { rebuildAll(); });
            connect(s, &QScreen::availableGeometryChanged,
                    this, [this](const QRect &) { rebuildAll(); });
        }
    }

private:
    // ── config ────────────────────────────────────────────────
    void watchConfig() {
        if (!m_watcher) return;
        QString path = osmWidgetConfigPath();
        QFileInfo fi(path);
        if (!m_watcher->files().contains(path) && fi.exists())
            m_watcher->addPath(path);
        if (!m_watcher->directories().contains(fi.absolutePath()))
            m_watcher->addPath(fi.absolutePath());
    }

    void onConfigTouched(const QString &) { m_debounce->start(); }

    // ── overlays ──────────────────────────────────────────────
    void buildOverlays() {
        QList<QScreen *> screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            DesktopOverlay *o = new DesktopOverlay(screens.at(i), i);
            o->show();
            o->startLowering();
            m_overlays.append(o);
        }
    }

    void rebuildAll() {
        for (DesktopOverlay *o : m_overlays) {
            o->clearFrames();
            o->hide();
            o->deleteLater();
        }
        m_overlays.clear();
        buildOverlays();
        reload();
    }

    // ── plugins ───────────────────────────────────────────────
    // Loaded libraries are kept open for the lifetime of the daemon;
    // unloading one while its QWidget still lives would crash us.
    QLibrary *pluginLib(const QString &id) {
        if (m_libs.contains(id)) return m_libs.value(id);

        QString path = QString("%1/%2.so").arg(WIDGET_DIR, id);
        if (!QFile::exists(path)) {
            qWarning("osm-widgets: plugin missing: %s", qPrintable(path));
            return nullptr;
        }

        QLibrary *lib = new QLibrary(path, this);
        if (!lib->load()) {
            qWarning("osm-widgets: failed to load %s: %s",
                     qPrintable(path), qPrintable(lib->errorString()));
            delete lib;
            return nullptr;
        }

        InfoFn info = (InfoFn)lib->resolve("osm_widget_info");
        if (!info) {
            qWarning("osm-widgets: %s has no osm_widget_info()", qPrintable(path));
            lib->unload();
            delete lib;
            return nullptr;
        }

        const OsmWidgetInfo *wi = info();
        if (!wi || wi->abi != OSM_WIDGET_ABI) {
            qWarning("osm-widgets: %s ABI mismatch (got %d, need %d)",
                     qPrintable(path), wi ? wi->abi : -1, OSM_WIDGET_ABI);
            lib->unload();
            delete lib;
            return nullptr;
        }

        m_libs.insert(id, lib);
        return lib;
    }

    // ── build ─────────────────────────────────────────────────
    void reload() {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);

        m_enabled = cfg.value("General/Enabled", true).toBool();
        m_edit    = cfg.value("General/EditMode", false).toBool();

        for (DesktopOverlay *o : m_overlays) {
            o->clearFrames();
            o->setEditMode(m_edit);
        }

        if (!m_enabled) {
            for (DesktopOverlay *o : m_overlays) o->hide();
            return;
        }

        for (DesktopOverlay *o : m_overlays) {
            if (!o->isVisible()) {
                o->show();
                o->startLowering();
            }
        }

        // Instances are discovered by group name, so there is no
        // separate index list to fall out of sync.
        QStringList groups = cfg.childGroups();
        groups.sort();

        for (const QString &g : groups) {
            if (!g.startsWith("Instance-")) continue;

            Instance inst;
            inst.id     = g.mid(9);
            inst.plugin = cfg.value(g + "/plugin").toString();
            inst.screen = cfg.value(g + "/screen", 0).toInt();
            if (inst.id.isEmpty() || inst.plugin.isEmpty()) continue;

            QLibrary *lib = pluginLib(inst.plugin);
            if (!lib) continue;

            InfoFn   infoFn   = (InfoFn)lib->resolve("osm_widget_info");
            CreateFn createFn = (CreateFn)lib->resolve("osm_widget_create");
            if (!infoFn || !createFn) continue;
            const OsmWidgetInfo *wi = infoFn();

            inst.geo = QRect(
                cfg.value(g + "/x", 40).toInt(),
                cfg.value(g + "/y", 40).toInt(),
                cfg.value(g + "/w", wi->defaultW).toInt(),
                cfg.value(g + "/h", wi->defaultH).toInt());

            DesktopOverlay *o = overlayFor(inst.screen);
            if (!o) continue;

            QWidget *body = createFn(qPrintable(inst.id), o);
            if (!body) {
                qWarning("osm-widgets: %s refused to create instance %s",
                         qPrintable(inst.plugin), qPrintable(inst.id));
                continue;
            }

            WidgetFrame *f = new WidgetFrame(inst, body, m_edit, o);
            f->setMinSize(wi->minW, wi->minH);
            f->show();
            o->addFrame(f);
        }

        for (DesktopOverlay *o : m_overlays) {
            o->refreshInputShape();
            lowerWindow(o->winId());
        }
    }

    DesktopOverlay *overlayFor(int screen) {
        for (DesktopOverlay *o : m_overlays)
            if (o->index() == screen) return o;
        return m_overlays.isEmpty() ? nullptr : m_overlays.first();
    }

    QList<DesktopOverlay *>  m_overlays;
    QMap<QString, QLibrary*> m_libs;
    QFileSystemWatcher      *m_watcher;
    QTimer                  *m_debounce;
    bool                     m_enabled;
    bool                     m_edit;
};

// ─────────────────────────────────────────────────────────────
int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QLockFile lock(QDir::temp().absoluteFilePath("osm-widgets.lock"));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(20))
        return 0;

    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) {
        qWarning("osm-widgets: cannot open X display");
        return 1;
    }

    QDir().mkpath(QDir::homePath() + "/.config/Alternix");

    // First run: create the file so the watcher has something to watch
    // and osm-widgets-settings has somewhere to write.
    if (!QFile::exists(osmWidgetConfigPath())) {
        QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
        cfg.setValue("General/Enabled", true);
        cfg.setValue("General/EditMode", false);
        cfg.sync();
    }

    WidgetHost host;
    host.start();

    int r = app.exec();
    XCloseDisplay(g_dpy);
    return r;
}
