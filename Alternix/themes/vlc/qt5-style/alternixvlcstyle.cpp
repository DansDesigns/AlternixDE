/*
 * Alternix VLC — native VLC 3.x Qt5 interface theme.
 *
 * Runs IN the standard VLC Qt interface via the Qt style plugin mechanism.
 * No skins2 window, no alternative player executable, no VLC private ABI.
 * Its layout enhancement relocates VLC's OWN embedded video widget inside
 * a padded rounded frame; buttons retain VLC's original signals/actions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QPalette>
#include <QPointer>
#include <QProxyStyle>
#include <QPushButton>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QStylePlugin>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace {

static const QString iconRoot = QStringLiteral("/usr/share/alternix/vlc/icons/");

QPalette palette()
{
    QPalette p;
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive}) {
        p.setColor(group, QPalette::Window, QColor("#292B2D"));
        p.setColor(group, QPalette::WindowText, QColor("#EDF3EF"));
        p.setColor(group, QPalette::Base, QColor("#202326"));
        p.setColor(group, QPalette::AlternateBase, QColor("#313638"));
        p.setColor(group, QPalette::Text, QColor("#E7EEE8"));
        p.setColor(group, QPalette::Button, QColor("#54595B"));
        p.setColor(group, QPalette::ButtonText, QColor("#EDF3EF"));
        p.setColor(group, QPalette::Highlight, QColor("#2BDB60"));
        p.setColor(group, QPalette::HighlightedText, QColor("#0C1A11"));
        p.setColor(group, QPalette::ToolTipBase, QColor("#25282A"));
        p.setColor(group, QPalette::ToolTipText, QColor("#EDF3EF"));
        p.setColor(group, QPalette::Link, QColor("#35E971"));
    }
    p.setColor(QPalette::Disabled, QPalette::Text, QColor("#8B9690"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#8B9690"));
    return p;
}

QString sheet()
{
    return QStringLiteral(R"QSS(
QMainWindow { background: #292B2D; color: #EEF2EF; }
QMainWindow QMenuBar { background: #222527; color: #DDE5DF; padding: 2px 4px; }
QMenuBar::item { padding: 4px 8px; border-radius: 6px; }
QMenuBar::item:selected { background: #3C5948; }
QMenu { background: #25292B; color: #E5EEE8; border: 1px solid #464F49; }
QMenu::item { padding: 5px 24px; }
QMenu::item:selected { background: #3D634A; }
QFrame#alternixTitle { background: #2A2D2F; border: 1px solid #4A5051; border-radius: 9px; }
QLabel#alternixBrand { color: #F2F6F3; font-size: 12px; font-weight: 700; letter-spacing: 1px; }
QFrame#alternixVideoFrame { background: #171A1C; border: 2px solid #090B0C; border-radius: 22px; }
QFrame#alternixTrackPanel { background: #515558; border: 2px solid #0C0F10; border-radius: 16px; }
QLabel#alternixTrackHeading { color: #D2DCDA; font-size: 10px; font-weight: 700; }
QLabel#alternixTrackText { color: #F2F6F3; font-size: 13px; }
QWidget#alternixControls, QWidget#alternixSeekControls { background: #292B2D; }
QToolButton, QPushButton {
 background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #73787A,stop:1 #454A4D);
 border: 1px solid #737A7B; border-bottom-color: #1B1F21; border-radius: 8px;
 color: #EEF3EE; padding: 4px 7px; min-height: 22px;
}
QToolButton:hover, QPushButton:hover { background: #68766B; border-color: #9FCBA9; }
QToolButton:pressed, QPushButton:pressed, QToolButton:checked { background: #395140; border-color: #2BDB60; }
QAbstractButton[alternixRole="play"] {
 background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #4BEC7B,stop:1 #159A3A);
 border: 1px solid #48E676; border-bottom-color: #0A6228; border-radius: 10px;
}
QAbstractButton[alternixRole="record"] {
 background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #F34147,stop:1 #AA181E);
 border: 1px solid #F1474C; border-bottom-color: #751419; border-radius: 10px;
}
QSlider::groove:horizontal { height: 7px; background: #6B7571; border-radius: 3px; }
QSlider::sub-page:horizontal { background: #29D35C; border-radius: 3px; }
QSlider::handle:horizontal { background: #2CEA65; border: 1px solid #DAFFE5; width: 14px; margin: -5px 0; border-radius: 7px; }
QSlider::groove:vertical { width: 7px; background: #6B7571; border-radius: 3px; }
QSlider::handle:vertical { background: #2CEA65; border: 1px solid #DAFFE5; height: 14px; margin: 0 -5px; border-radius: 7px; }
QProgressBar { border: 1px solid #5C6860; background: #303735; border-radius: 5px; text-align: center; }
QProgressBar::chunk { background: #2CEA65; border-radius: 4px; }
QTreeView, QListView, QTableView, QTextEdit, QPlainTextEdit {
 color: #E7EEE8; background: #222628; alternate-background-color: #2A3031;
 border: 1px solid #48544C; selection-background-color: #3B744A;
}
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
 color: #E7EEE8; background: #232729; border: 1px solid #536359;
 border-radius: 6px; padding: 4px;
}
QTabBar::tab { color: #DFE8E2; background: #393F3C; padding: 5px 12px; border-radius: 6px; }
QTabBar::tab:selected { background: #42654A; }
QToolTip { background: #282D2B; color: #E9F1EA; border: 1px solid #5B7161; }
QScrollBar:vertical { background: #262A2B; width: 10px; }
QScrollBar::handle:vertical { background: #57675B; border-radius: 5px; min-height: 20px; }
)QSS");
}

QString descriptor(QAbstractButton *b)
{
    QString d = b->objectName() + QLatin1Char(' ') + b->toolTip() +
                QLatin1Char(' ') + b->accessibleName() + QLatin1Char(' ') + b->text();
    if (auto *t = qobject_cast<QToolButton *>(b)) {
        if (QAction *a = t->defaultAction())
            d += QLatin1Char(' ') + a->text() + QLatin1Char(' ') + a->toolTip();
    }
    return d.toLower();
}

bool word(const QString &haystack, const QString &needle)
{
    int pos = -1;
    while ((pos = haystack.indexOf(needle, pos + 1)) >= 0) {
        const int end = pos + needle.length();
        if ((pos == 0 || !haystack[pos - 1].isLetterOrNumber()) &&
            (end == haystack.size() || !haystack[end].isLetterOrNumber()))
            return true;
    }
    return false;
}

void polishButton(QAbstractButton *b)
{
    const QString d = descriptor(b);
    QString role, iconName;
    if (word(d, QStringLiteral("record"))) {
        role = QStringLiteral("record"); iconName = QStringLiteral("record_up.png");
    } else if (word(d, QStringLiteral("play")) || word(d, QStringLiteral("pause"))) {
        role = QStringLiteral("play");
        iconName = word(d, QStringLiteral("pause")) ? QStringLiteral("pause_up.png") : QStringLiteral("play_up.png");
    } else if (word(d, QStringLiteral("stop"))) iconName = QStringLiteral("stop_up.png");
    else if (word(d, QStringLiteral("previous"))) iconName = QStringLiteral("prev_up.png");
    else if (word(d, QStringLiteral("next"))) iconName = QStringLiteral("next_up.png");
    else if (word(d, QStringLiteral("open"))) iconName = QStringLiteral("open_up.png");
    else if (word(d, QStringLiteral("playlist"))) iconName = QStringLiteral("playlist_up.png");
    else if (word(d, QStringLiteral("equalizer")) || word(d, QStringLiteral("effects")) || word(d, QStringLiteral("extended"))) iconName = QStringLiteral("eq_up.png");
    else if (word(d, QStringLiteral("random")) || word(d, QStringLiteral("shuffle"))) iconName = QStringLiteral("shuffle_up.png");
    else if (word(d, QStringLiteral("repeat")) || word(d, QStringLiteral("loop"))) iconName = QStringLiteral("repeat_up.png");
    else if (word(d, QStringLiteral("fullscreen"))) iconName = QStringLiteral("fullscreen_up.png");

    if (b->property("alternixRole").toString() != role) {
        b->setProperty("alternixRole", role);
        // Qt stylesheet dynamic properties require a repolish.
        if (b->style()) { b->style()->unpolish(b); b->style()->polish(b); }
        b->update();
    }
    // Actual existing VLC buttons are retained; only their pixmap is swapped.
    if (auto *t = qobject_cast<QToolButton *>(b)) {
        if (!iconName.isEmpty() && QFileInfo::exists(iconRoot + iconName)) {
            if (t->property("alternixIcon").toString() != iconName) {
                t->setProperty("alternixIcon", iconName);
                t->setIcon(QIcon(iconRoot + iconName));
                t->setIconSize(role == QLatin1String("play") ? QSize(32, 28) : QSize(23, 23));
                t->setMinimumSize(role == QLatin1String("play") ? QSize(60, 42) : QSize(40, 34));
            }
        }
    }
}

QWidget *childNamed(QWidget *root, const char *className)
{
    for (QWidget *w : root->findChildren<QWidget *>()) {
        if (QString::fromLatin1(w->metaObject()->className()) == QLatin1String(className))
            return w;
    }
    return nullptr;
}

void applyPlayerLayout(QMainWindow *win)
{
    if (!win || win->property("alternixNativeApplied").toBool()) return;
    QWidget *central = win->centralWidget();
    if (!central) return;
    auto *mainLayout = qobject_cast<QVBoxLayout *>(central->layout());
    if (!mainLayout) return;
    QWidget *stack = childNamed(central, "QVLCStackedWidget");
    QWidget *controls = childNamed(central, "ControlsWidget");
    QWidget *seek = childNamed(central, "InputControlsWidget");
    if (!stack || !controls || !seek) return; // Leave native VLC untouched on incompatible versions.

    // VLC's own video widget is resized by its Qt parent; never create a
    // standalone player/top-level window or set any display-size geometry.
    win->setProperty("alternixNativeApplied", true);
    central->setObjectName(QStringLiteral("alternixMain"));
    mainLayout->setSpacing(6);
    mainLayout->setContentsMargins(12, 8, 12, 10);

    auto *header = new QFrame(central);
    header->setObjectName(QStringLiteral("alternixTitle"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(13, 4, 13, 4);
    auto *brand = new QLabel(QStringLiteral("●  ALTERNIX   VLC"), header);
    brand->setObjectName(QStringLiteral("alternixBrand"));
    headerLayout->addWidget(brand);
    headerLayout->addStretch(1);
    header->setMaximumHeight(35);
    mainLayout->insertWidget(0, header);

    // Insets prevent the rectangular video surface from obscuring the
    // rounded charcoal frame. The QFrame itself stretches with Qtile's tile.
    auto *videoFrame = new QFrame(central);
    videoFrame->setObjectName(QStringLiteral("alternixVideoFrame"));
    videoFrame->setAttribute(Qt::WA_StyledBackground, true);
    videoFrame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto *videoLayout = new QVBoxLayout(videoFrame);
    videoLayout->setContentsMargins(10, 10, 10, 10);
    videoLayout->setSpacing(0);
    mainLayout->removeWidget(stack);
    stack->setParent(videoFrame);
    stack->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    videoLayout->addWidget(stack, 1);
    mainLayout->insertWidget(1, videoFrame, 1);

    auto *info = new QFrame(central);
    info->setObjectName(QStringLiteral("alternixTrackPanel"));
    auto *infoLayout = new QVBoxLayout(info);
    infoLayout->setContentsMargins(13, 7, 13, 8);
    infoLayout->setSpacing(3);
    auto *label = new QLabel(QStringLiteral("TRACK INFO"), info);
    label->setObjectName(QStringLiteral("alternixTrackHeading"));
    auto *track = new QLabel(info);
    track->setObjectName(QStringLiteral("alternixTrackText"));
    track->setTextInteractionFlags(Qt::TextSelectableByMouse);
    track->setTextFormat(Qt::PlainText);
    track->setText(QStringLiteral("No media selected"));
    track->setWordWrap(false);
    infoLayout->addWidget(label);
    infoLayout->addWidget(track);
    info->setMaximumHeight(75);

    // Follow VLC's own main-window title updates; no private InputManager API.
    QObject::connect(win, &QWidget::windowTitleChanged, track,
        [track](const QString &title) {
            QString name = title;
            name.remove(QStringLiteral(" - VLC media player"), Qt::CaseInsensitive);
            name.remove(QStringLiteral(" - Alternix VLC"), Qt::CaseInsensitive);
            if (name.isEmpty() || name.compare(QStringLiteral("VLC media player"), Qt::CaseInsensitive) == 0)
                name = QStringLiteral("No media selected");
            track->setText(name);
        });
    mainLayout->removeWidget(seek);
    mainLayout->insertWidget(2, seek);
    mainLayout->insertWidget(3, info);
    mainLayout->removeWidget(controls);
    mainLayout->addWidget(controls);
    controls->setObjectName(QStringLiteral("alternixControls"));
    seek->setObjectName(QStringLiteral("alternixSeekControls"));

    for (auto *b : controls->findChildren<QAbstractButton *>()) polishButton(b);
    win->updateGeometry();
}

class AlternixFilter final : public QObject {
public:
    explicit AlternixFilter(QObject *parent) : QObject(parent) {}

    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Show) {
            auto *w = qobject_cast<QMainWindow *>(o);
            if (w && QString::fromLatin1(w->metaObject()->className()) == QLatin1String("MainInterface")) {
                QPointer<QMainWindow> safe(w);
                QTimer::singleShot(0, w, [safe]() { if (safe) applyPlayerLayout(safe.data()); });
            }
        }
        if (e->type() == QEvent::Show || e->type() == QEvent::ToolTipChange) {
            if (auto *b = qobject_cast<QAbstractButton *>(o)) polishButton(b);
        }
        return false;
    }
};

class AlternixStyle final : public QProxyStyle {
public:
    AlternixStyle() : QProxyStyle(QStyleFactory::create(QStringLiteral("kvantum"))) {
        if (!baseStyle()) setBaseStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    }
    QPalette standardPalette() const override { return palette(); }
    void polish(QApplication *app) override
    {
        QProxyStyle::polish(app);
        if (installed) return;
        installed = true;
        app->setPalette(palette());
        QTimer::singleShot(0, app, [app]() {
            app->setStyleSheet(sheet());
            auto *filter = new AlternixFilter(app);
            app->installEventFilter(filter);
            for (auto *w : app->allWidgets()) {
                if (auto *b = qobject_cast<QAbstractButton *>(w)) polishButton(b);
                if (auto *m = qobject_cast<QMainWindow *>(w))
                    if (QString::fromLatin1(m->metaObject()->className()) == QLatin1String("MainInterface"))
                        applyPlayerLayout(m);
            }
        });
    }
private:
    bool installed = false;
};

} // namespace

class AlternixVLCStylePlugin final : public QStylePlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QStyleFactoryInterface" FILE "alternixvlcstyle.json")
public:
    QStyle *create(const QString &key) override {
        if (key.compare(QLatin1String("AlternixVLC"), Qt::CaseInsensitive) != 0) return nullptr;
        return new AlternixStyle;
    }
};

#include "alternixvlcstyle.moc"
