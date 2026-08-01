// ─────────────────────────────────────────────────────────────
//  osm-widgets-settings  —  control panel for the desktop widget overlay
//
//  Writes ~/.config/Alternix/osm-widgets.conf. The osm-widgets daemon
//  watches that file and rebuilds itself, so there is no IPC here.
//
//  Build:
//    g++ osm-widgets-settings.cpp -o osm-widgets-settings -fPIC -ldl \
//        $(pkg-config --cflags --libs Qt5Widgets)
// ─────────────────────────────────────────────────────────────

#include <QApplication>
#include <QMainWindow>
#include <QStackedWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QScreen>
#include <QScroller>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QLibrary>
#include <QMessageBox>
#include <QLayoutItem>
#include <functional>

#include "osm-widget-plugin.h"

static const char *WIDGET_DIR = "/usr/local/lib/alternix/widgets";

static const int CARD_PADDING      = 22;
static const int ICON_COLUMN_WIDTH = 54;
static const int ICON_TEXT_SPACING = 18;
static const int CARD_WIDTH        = 620;

typedef const OsmWidgetInfo *(*InfoFn)();
typedef QWidget *(*ConfigFn)(const char *, QWidget *);

// ─────────────────────────────────────────────────────────────
class TouchScrollArea : public QScrollArea {
public:
    explicit TouchScrollArea(QWidget *parent = nullptr)
        : QScrollArea(parent)
    {
        setWidgetResizable(true);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setStyleSheet(
            "QScrollArea { background:#282828; font-family:Sans; border:none; }"
            "QWidget { background:#282828; font-family:Sans; }"
            "QLabel { color:white; font-family:Sans; }"
        );
        QScroller::grabGesture(this, QScroller::LeftMouseButtonGesture);
    }
};

// ─────────────────────────────────────────────────────────────
class ClickableCard : public QFrame {
public:
    explicit ClickableCard(QWidget *parent = nullptr) : QFrame(parent) {
        setCursor(Qt::PointingHandCursor);
    }
    std::function<void()> onClick;

protected:
    QPoint pressPos;
    bool   pressed = false;

    void mousePressEvent(QMouseEvent *event) override {
        pressed  = true;
        pressPos = event->globalPos();
        QFrame::mousePressEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (!pressed) return;
        pressed = false;
        if ((event->globalPos() - pressPos).manhattanLength() < 6 && onClick)
            onClick();
        QFrame::mouseReleaseEvent(event);
    }
};

// ─────────────────────────────────────────────────────────────
//  Plugin discovery
// ─────────────────────────────────────────────────────────────
struct PluginEntry {
    QString              path;
    const OsmWidgetInfo *info;
};

static QList<PluginEntry> scanPlugins()
{
    QList<PluginEntry> out;
    QDir dir(WIDGET_DIR);
    if (!dir.exists()) return out;

    const QStringList files = dir.entryList(QStringList() << "*.so", QDir::Files, QDir::Name);
    for (const QString &f : files) {
        QString path = dir.absoluteFilePath(f);

        QLibrary *lib = new QLibrary(path);
        if (!lib->load()) { delete lib; continue; }

        InfoFn fn = (InfoFn)lib->resolve("osm_widget_info");
        if (!fn) { lib->unload(); delete lib; continue; }

        const OsmWidgetInfo *wi = fn();
        if (!wi || wi->abi != OSM_WIDGET_ABI) { lib->unload(); delete lib; continue; }

        // Deliberately left loaded: wi points into the .so, and a
        // config page created later needs the code still mapped.
        PluginEntry e;
        e.path = path;
        e.info = wi;
        out.append(e);
    }
    return out;
}

static const OsmWidgetInfo *infoForPlugin(const QString &id)
{
    static QList<PluginEntry> cache = scanPlugins();
    for (const PluginEntry &e : cache)
        if (QString::fromUtf8(e.info->id) == id) return e.info;
    return nullptr;
}

// ─────────────────────────────────────────────────────────────
class WidgetSettingsHub : public QMainWindow {
public:
    WidgetSettingsHub() {
        QScreen *screen = QGuiApplication::primaryScreen();
        QRect avail = screen->availableGeometry();

        int w = 800;
        int h = 1280;
        if (h > avail.height()) {
            double scale = double(avail.height()) / double(h);
            w = int(w * scale);
            h = avail.height();
        }
        resize(w, h);
        move(avail.center() - rect().center());

        setWindowTitle("Desktop Widgets");
        QApplication::setFont(QFont("Noto Color Emoji"));
        setStyleSheet("background:#282828;");

        stack = new QStackedWidget(this);
        setCentralWidget(stack);

        mainScroll = new TouchScrollArea;
        mainInner  = new QWidget;
        mainCol    = new QVBoxLayout(mainInner);
        mainCol->setContentsMargins(40, 40, 40, 40);
        mainCol->setSpacing(28);
        mainCol->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
        mainScroll->setWidget(mainInner);

        stack->addWidget(mainScroll);
        populateMain();
    }

protected:
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Backspace && stack->currentIndex() != 0)
            showMain();
        QMainWindow::keyPressEvent(e);
    }

private:
    QStackedWidget  *stack;
    TouchScrollArea *mainScroll;
    QWidget         *mainInner;
    QVBoxLayout     *mainCol;

    // ── config access ────────────────────────────────────────
    QStringList instanceIds() {
        QSettings c(osmWidgetConfigPath(), QSettings::IniFormat);
        QStringList out;
        QStringList groups = c.childGroups();
        groups.sort();
        for (const QString &g : groups)
            if (g.startsWith("Instance-")) out.append(g.mid(9));
        return out;
    }

    // ── navigation ───────────────────────────────────────────
    void showMain() {
        while (stack->count() > 1) {
            QWidget *w = stack->widget(1);
            stack->removeWidget(w);
            w->deleteLater();
        }
        populateMain();
        stack->setCurrentIndex(0);
    }

    void showPage(QWidget *p) {
        while (stack->count() > 1) {
            QWidget *w = stack->widget(1);
            stack->removeWidget(w);
            w->deleteLater();
        }
        stack->addWidget(p);
        stack->setCurrentIndex(1);
    }

    // ── main menu ────────────────────────────────────────────
    void clearMain() {
        QLayoutItem *item;
        while ((item = mainCol->takeAt(0)) != nullptr) {
            if (QWidget *w = item->widget()) w->deleteLater();
            delete item;
        }
    }

    void populateMain() {
        clearMain();

        QSettings c(osmWidgetConfigPath(), QSettings::IniFormat);
        bool enabled = c.value("General/Enabled", true).toBool();
        bool edit    = c.value("General/EditMode", false).toBool();

        // Overlay on/off
        {
            ClickableCard *card = makeCard(enabled ? "🖼️" : "🚫",
                                           "Desktop Widgets",
                                           enabled ? "On — widgets are shown"
                                                   : "Off — overlay hidden");
            card->onClick = [this, enabled]() {
                QSettings s(osmWidgetConfigPath(), QSettings::IniFormat);
                s.setValue("General/Enabled", !enabled);
                s.sync();
                populateMain();
            };
            mainCol->addWidget(card, 0, Qt::AlignHCenter);
        }

        // Arrange mode
        {
            ClickableCard *card = makeCard(edit ? "✋" : "🔒",
                                           "Arrange Mode",
                                           edit ? "On — drag to move, corner to resize"
                                                : "Off — widgets are pinned in place");
            card->onClick = [this, edit]() {
                QSettings s(osmWidgetConfigPath(), QSettings::IniFormat);
                s.setValue("General/EditMode", !edit);
                s.sync();
                populateMain();
            };
            mainCol->addWidget(card, 0, Qt::AlignHCenter);
        }

        // Add
        {
            ClickableCard *card = makeCard("➕", "Add Widget",
                                           "Place a new widget on the desktop");
            card->onClick = [this]() { showPage(makePickerPage()); };
            mainCol->addWidget(card, 0, Qt::AlignHCenter);
        }

        // Heading
        QLabel *hdr = new QLabel("On the desktop");
        hdr->setStyleSheet("font-size:24px; color:#bbbbbb; background:transparent;");
        mainCol->addWidget(hdr, 0, Qt::AlignHCenter);

        const QStringList ids = instanceIds();
        if (ids.isEmpty()) {
            QLabel *none = new QLabel("No widgets yet — tap Add Widget above.");
            none->setStyleSheet("font-size:22px; color:#888888; background:transparent;");
            none->setAlignment(Qt::AlignCenter);
            mainCol->addWidget(none, 0, Qt::AlignHCenter);
        }

        for (const QString &id : ids) {
            QString plugin = c.value("Instance-" + id + "/plugin").toString();
            const OsmWidgetInfo *wi = infoForPlugin(plugin);

            QString icon = wi ? QString::fromUtf8(wi->icon) : "❓";
            QString name = wi ? QString::fromUtf8(wi->name) : plugin;
            QString sub  = wi
                ? QString("%1 · %2").arg(id, QString::fromUtf8(wi->description))
                : QString("%1 · plugin missing from %2").arg(id, WIDGET_DIR);

            ClickableCard *card = makeCard(icon, name, sub);
            card->onClick = [this, id, plugin]() {
                showPage(makeInstancePage(id, plugin));
            };
            mainCol->addWidget(card, 0, Qt::AlignHCenter);
        }

        mainCol->addStretch();
    }

    // ── picker ───────────────────────────────────────────────
    QWidget *makePickerPage() {
        TouchScrollArea *scroll = new TouchScrollArea;
        QWidget *inner = new QWidget;
        QVBoxLayout *v = new QVBoxLayout(inner);
        v->setContentsMargins(40, 40, 40, 40);
        v->setSpacing(28);
        v->setAlignment(Qt::AlignTop | Qt::AlignHCenter);

        v->addWidget(makeBackButton(), 0, Qt::AlignLeft);

        QLabel *ttl = new QLabel("Add Widget");
        ttl->setStyleSheet("font-size:32px; color:white; font-weight:bold; background:transparent;");
        v->addWidget(ttl, 0, Qt::AlignHCenter);

        QList<PluginEntry> plugins = scanPlugins();

        if (plugins.isEmpty()) {
            QLabel *none = new QLabel(
                QString("No widget plugins found in\n%1").arg(WIDGET_DIR));
            none->setAlignment(Qt::AlignCenter);
            none->setStyleSheet("font-size:22px; color:#ff8a8a; background:transparent;");
            v->addWidget(none);
        }

        for (const PluginEntry &e : plugins) {
            QString pid = QString::fromUtf8(e.info->id);
            ClickableCard *card = makeCard(QString::fromUtf8(e.info->icon),
                                           QString::fromUtf8(e.info->name),
                                           QString::fromUtf8(e.info->description));
            card->onClick = [this, pid]() {
                addInstance(pid);
                showMain();
            };
            v->addWidget(card, 0, Qt::AlignHCenter);
        }

        v->addStretch();
        scroll->setWidget(inner);
        return scroll;
    }

    void addInstance(const QString &plugin) {
        QSettings s(osmWidgetConfigPath(), QSettings::IniFormat);

        // First free "<plugin>-N".
        int n = 1;
        while (s.childGroups().contains(QString("Instance-%1-%2").arg(plugin).arg(n)))
            ++n;
        QString id = QString("%1-%2").arg(plugin).arg(n);

        const OsmWidgetInfo *wi = infoForPlugin(plugin);
        int dw = wi ? wi->defaultW : 300;
        int dh = wi ? wi->defaultH : 140;

        // Cascade so a second widget does not land exactly on the first.
        int existing = 0;
        for (const QString &g : s.childGroups())
            if (g.startsWith("Instance-")) ++existing;

        QString g = "Instance-" + id + "/";
        s.setValue(g + "plugin", plugin);
        s.setValue(g + "x", 40 + existing * 36);
        s.setValue(g + "y", 40 + existing * 36);
        s.setValue(g + "w", dw);
        s.setValue(g + "h", dh);
        s.setValue(g + "screen", 0);

        // A brand new widget is almost always in the wrong place, so
        // drop straight into arrange mode.
        s.setValue("General/Enabled", true);
        s.setValue("General/EditMode", true);
        s.sync();
    }

    // ── per-instance page ────────────────────────────────────
    QWidget *makeInstancePage(const QString &id, const QString &plugin) {
        TouchScrollArea *scroll = new TouchScrollArea;
        QWidget *inner = new QWidget;
        QVBoxLayout *v = new QVBoxLayout(inner);
        v->setContentsMargins(30, 30, 30, 30);
        v->setSpacing(22);
        v->setAlignment(Qt::AlignTop);

        v->addWidget(makeBackButton(), 0, Qt::AlignLeft);

        const OsmWidgetInfo *wi = infoForPlugin(plugin);

        QLabel *ttl = new QLabel(wi ? QString::fromUtf8(wi->name) : plugin);
        ttl->setAlignment(Qt::AlignCenter);
        ttl->setStyleSheet("font-size:32px; color:white; font-weight:bold; background:transparent;");
        v->addWidget(ttl);

        QLabel *sub = new QLabel(id);
        sub->setAlignment(Qt::AlignCenter);
        sub->setStyleSheet("font-size:20px; color:#888888; background:transparent;");
        v->addWidget(sub);

        // The plugin's own options, if it has any.
        QWidget *page = nullptr;
        QString  path = QString("%1/%2.so").arg(WIDGET_DIR, plugin);
        if (QFile::exists(path)) {
            QLibrary *lib = new QLibrary(path);
            if (lib->load()) {
                ConfigFn fn = (ConfigFn)lib->resolve("osm_widget_config");
                if (fn) page = fn(qPrintable(id), inner);
            }
        }

        if (page) {
            v->addWidget(page);
        } else {
            QLabel *msg = new QLabel("This widget has no options.");
            msg->setAlignment(Qt::AlignCenter);
            msg->setStyleSheet("font-size:22px; color:#bbbbbb; background:transparent;");
            v->addWidget(msg);
        }

        v->addStretch();

        QPushButton *del = new QPushButton("Remove from desktop");
        del->setMinimumHeight(78);
        del->setStyleSheet(
            "QPushButton { background:#7a2233; color:white; font-size:24px;"
            " font-weight:bold; border:none; border-radius:16px; padding:10px 24px; }"
            "QPushButton:hover  { background:#93293e; }"
            "QPushButton:pressed{ background:#551122; }");
        connect(del, &QPushButton::clicked, this, [this, id]() {
            QMessageBox box(this);
            box.setStyleSheet(
                "QMessageBox { background:#282828; }"
                "QLabel { color:white; font-size:22px; }"
                "QPushButton { background:#505050; color:white; font-size:22px;"
                " border:none; border-radius:12px; padding:12px 28px; min-width:120px; }");
            box.setWindowTitle("Remove widget");
            box.setText(QString("Remove %1 from the desktop?").arg(id));
            box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            box.setDefaultButton(QMessageBox::No);
            if (box.exec() != QMessageBox::Yes) return;

            QSettings s(osmWidgetConfigPath(), QSettings::IniFormat);
            s.remove("Instance-" + id);
            s.remove("Widget-" + id);
            s.sync();
            showMain();
        });
        v->addWidget(del, 0, Qt::AlignHCenter);

        scroll->setWidget(inner);
        return scroll;
    }

    // ── shared chrome ────────────────────────────────────────
    QPushButton *makeBackButton() {
        QPushButton *back = new QPushButton("❮");
        back->setFixedSize(200, 70);
        back->setStyleSheet(
            "QPushButton { background:#505050; color:white; font-size:36px;"
            " font-weight:bold; border:none; border-radius:16px; }"
            "QPushButton:hover  { background:#5c5c5c; }"
            "QPushButton:pressed{ background:#666; }");
        connect(back, &QPushButton::clicked, this, [this]() { showMain(); });
        return back;
    }

    ClickableCard *makeCard(const QString &icon,
                            const QString &title,
                            const QString &sub)
    {
        ClickableCard *card = new ClickableCard;
        card->setFixedHeight(130);
        card->setMinimumWidth(CARD_WIDTH);
        card->setMaximumWidth(CARD_WIDTH);

        QHBoxLayout *row = new QHBoxLayout(card);
        row->setContentsMargins(CARD_PADDING, 10, CARD_PADDING, 10);
        row->setSpacing(ICON_TEXT_SPACING);
        row->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        QWidget *iconWrapper = new QWidget;
        iconWrapper->setFixedWidth(ICON_COLUMN_WIDTH);
        QVBoxLayout *iconCol = new QVBoxLayout(iconWrapper);
        iconCol->setContentsMargins(0, 0, 0, 0);
        iconCol->setAlignment(Qt::AlignCenter);

        QLabel *ico = new QLabel(icon);
        ico->setStyleSheet("font-size:48px; color:white;");
        ico->setAlignment(Qt::AlignCenter);
        iconCol->addWidget(ico);

        QWidget *textWrapper = new QWidget;
        QVBoxLayout *textCol = new QVBoxLayout(textWrapper);
        textCol->setContentsMargins(0, 0, 0, 0);
        textCol->setSpacing(0);
        textCol->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

        QLabel *ttl = new QLabel(title);
        ttl->setStyleSheet("font-size:30px; font-weight:bold; color:white;");

        QLabel *subt = new QLabel(sub);
        subt->setWordWrap(true);
        subt->setStyleSheet("font-size:22px; color:#bbbbbb;");

        textCol->addWidget(ttl);
        textCol->addWidget(subt);

        row->addWidget(iconWrapper);
        row->addWidget(textWrapper, 1);

        card->setStyleSheet(
            "ClickableCard {"
            " background:#303030;"
            " border:3px dashed #777;"
            " border-radius:12px;"
            "}"
            "ClickableCard:hover  { background:#3b3b3b; }"
            "ClickableCard:pressed{ background:#505050; }");

        return card;
    }
};

// ─────────────────────────────────────────────────────────────
int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QDir().mkpath(QDir::homePath() + "/.config/Alternix");

    // Match the font scaling osm-settings uses.
    {
        QSettings cfg(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                      QSettings::IniFormat);
        QScreen *scr = QGuiApplication::primaryScreen();
        int defaultPt = (scr && scr->size().width() <= 720) ? 18 : 22;
        int pt = cfg.value("UI/SettingsFontSize", defaultPt).toInt();
        pt = qBound(14, pt, 36);
        QFont f = a.font();
        f.setPointSize(pt);
        a.setFont(f);
    }

    WidgetSettingsHub w;
    w.show();

    return a.exec();
}
