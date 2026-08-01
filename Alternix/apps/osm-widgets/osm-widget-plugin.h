// ─────────────────────────────────────────────────────────────
//  osm-widget-plugin.h
//  Shared ABI for Alternix desktop widgets.
//
//  Every widget lives in its own .so installed to:
//      /usr/local/lib/alternix/widgets/<id>.so
//
//  Each .so must export the three C symbols declared below.
//
//  All widget state is stored in the single shared config file:
//      ~/.config/Alternix/osm-widgets.conf      (QSettings INI)
//
//  Per-instance private settings belong in the group:
//      [Widget-<instanceId>]
//
//  The host owns the group:
//      [Instance-<instanceId>]   plugin / x / y / w / h / screen
//
//  A widget must NEVER write outside its own [Widget-<instanceId>] group.
// ─────────────────────────────────────────────────────────────
#ifndef OSM_WIDGET_PLUGIN_H
#define OSM_WIDGET_PLUGIN_H

#include <QWidget>
#include <QString>
#include <QDir>
#include <QSettings>

// Bump this if the struct or function signatures ever change.
// The host refuses to load a .so whose abi does not match.
#define OSM_WIDGET_ABI 1

struct OsmWidgetInfo {
    int         abi;           // must be OSM_WIDGET_ABI
    const char *id;            // "clock"  — must match the .so filename
    const char *name;          // "Clock"  — shown in the picker
    const char *icon;          // emoji glyph, e.g. "🕒"
    const char *description;   // one line shown under the name
    int         defaultW;      // starting size in pixels
    int         defaultH;
    int         minW;          // smallest the user may resize to
    int         minH;
};

extern "C" {

// Describe the widget. Called before anything is instantiated.
// Must return a pointer with static storage duration.
const OsmWidgetInfo *osm_widget_info();

// Build one live instance for the desktop overlay.
// The host owns and reparents the returned QWidget.
QWidget *osm_widget_create(const char *instanceId, QWidget *parent);

// Build the settings page for one instance, shown inside
// osm-widgets-settings. Return nullptr if the widget has no options.
QWidget *osm_widget_config(const char *instanceId, QWidget *parent);

}

// ─────────────────────────────────────────────────────────────
//  Convenience helpers for plugin authors.
// ─────────────────────────────────────────────────────────────
static inline QString osmWidgetConfigPath() {
    return QDir::homePath() + "/.config/Alternix/osm-widgets.conf";
}

// Read one private setting for this instance.
static inline QVariant osmWidgetGet(const QString &instanceId,
                                    const QString &key,
                                    const QVariant &fallback = QVariant())
{
    QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
    return cfg.value("Widget-" + instanceId + "/" + key, fallback);
}

// Write one private setting for this instance.
static inline void osmWidgetSet(const QString &instanceId,
                                const QString &key,
                                const QVariant &value)
{
    QSettings cfg(osmWidgetConfigPath(), QSettings::IniFormat);
    cfg.setValue("Widget-" + instanceId + "/" + key, value);
    cfg.sync();
}

#endif // OSM_WIDGET_PLUGIN_H
