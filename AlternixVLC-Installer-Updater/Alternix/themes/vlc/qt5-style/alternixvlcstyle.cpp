/*
 * Alternix VLC native Qt5 style plugin.
 * Styles standard VLC 3.x Qt widgets without replacing its native interface.
 * Window geometry, tiling, video layout and fullscreen remain under VLC/Qtile.
 *
 * SPDX-License-Identifier: MIT
 */
#include <QAbstractButton>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QPalette>
#include <QProxyStyle>
#include <QPushButton>
#include <QStyleFactory>
#include <QStylePlugin>
#include <QTimer>
#include <QToolButton>
#include <QWidget>
#include <QPointer>

namespace {

QPalette alternixPalette()
{
    QPalette p;
    const QColor window("#292B2D"), base("#202225"), alternate("#303336");
    const QColor control("#55595C"), foreground("#E9EEEA");
    const QColor green("#2ADB5B"), dim("#929A95");
    for (const auto group : {QPalette::Active, QPalette::Inactive}) {
        p.setColor(group, QPalette::Window, window);
        p.setColor(group, QPalette::WindowText, foreground);
        p.setColor(group, QPalette::Base, base);
        p.setColor(group, QPalette::AlternateBase, alternate);
        p.setColor(group, QPalette::Text, foreground);
        p.setColor(group, QPalette::Button, control);
        p.setColor(group, QPalette::ButtonText, foreground);
        p.setColor(group, QPalette::Highlight, green);
        p.setColor(group, QPalette::HighlightedText, QColor("#101A12"));
        p.setColor(group, QPalette::ToolTipBase, QColor("#25282A"));
        p.setColor(group, QPalette::ToolTipText, foreground);
        p.setColor(group, QPalette::Light, QColor("#7A7F80"));
        p.setColor(group, QPalette::Midlight, QColor("#626668"));
        p.setColor(group, QPalette::Mid, QColor("#35383A"));
        p.setColor(group, QPalette::Dark, QColor("#17191B"));
        p.setColor(group, QPalette::Shadow, QColor("#101213"));
        p.setColor(group, QPalette::Link, QColor("#42E675"));
        p.setColor(group, QPalette::LinkVisited, QColor("#8DDBAB"));
        p.setColor(group, QPalette::BrightText, QColor("#FFFFFF"));
    }
    p.setColor(QPalette::Disabled, QPalette::WindowText, dim);
    p.setColor(QPalette::Disabled, QPalette::Text, dim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, dim);
    return p;
}

QString alternixStyleSheet()
{
    // No fixed player width or height: native Qt layouts size with the window.
    // Avoid QOpenGLWidget and the native video surface: Qt/VLC own those.
    return QStringLiteral(R"QSS(
QMainWindow, QDialog, QMessageBox { background-color: #292B2D; color: #E9EEEA; }
QMenuBar { background-color: #272A2C; color: #E9EEEA; spacing: 4px; border-bottom: 1px solid #45494B; }
QMenuBar::item { padding: 5px 10px; background: transparent; border-radius: 6px; }
QMenuBar::item:selected { background: #415649; color: #FFFFFF; }
QMenu { background-color: #25282A; color: #E9EEEA; border: 1px solid #494F4C; padding: 5px; }
QMenu::item { padding: 6px 24px 6px 18px; border-radius: 6px; }
QMenu::item:selected { background-color: #356342; color: #FFFFFF; }
QMenu::separator { height: 1px; background: #404446; margin: 4px 10px; }
QToolBar { background: #303234; border: 0px; spacing: 5px; padding: 5px; }
QDockWidget { color: #E9EEEA; }
QDockWidget::title { background: #34383A; padding: 7px; text-align: left; }
QToolButton, QPushButton {
  background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #686C70,stop:1 #44484B);
  color: #F4F6F4; border: 1px solid #777B7D; border-bottom-color: #282B2D;
  border-radius: 9px; padding: 5px 9px; min-height: 23px;
}
QToolButton:hover, QPushButton:hover { background: #69716D; border-color: #9CAC9C; }
QToolButton:pressed, QPushButton:pressed, QToolButton:checked, QPushButton:checked {
  background: #303B34; border-color: #1DB94F;
}
QToolButton:disabled, QPushButton:disabled { background: #3B3E40; color: #89908C; border-color: #4C4F50; }
QToolButton[alternixRole="play"], QPushButton[alternixRole="play"] {
  background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #46EE76,stop:1 #169E3E);
  border-color: #42DA6A; border-bottom-color: #116C2C;
  color: white; border-radius: 10px;
}
QToolButton[alternixRole="play"]:hover, QPushButton[alternixRole="play"]:hover { background: #40EC70; }
QToolButton[alternixRole="play"]:pressed, QPushButton[alternixRole="play"]:pressed { background: #117F35; }
QToolButton[alternixRole="record"], QPushButton[alternixRole="record"] {
  background: qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 #FA383E,stop:1 #A9151C);
  border-color: #DF3438; border-bottom-color: #731018;
  color: #FFFFFF; border-radius: 10px;
}
QToolButton[alternixRole="record"]:hover, QPushButton[alternixRole="record"]:hover { background: #F34B50; }
QToolButton[alternixRole="record"]:pressed, QPushButton[alternixRole="record"]:pressed { background: #8A1019; }
QSlider::groove:horizontal { height: 7px; background: #73797A; border-radius: 3px; }
QSlider::sub-page:horizontal { background: #2CDA5D; border-radius: 3px; }
QSlider::add-page:horizontal { background: #565B5C; border-radius: 3px; }
QSlider::handle:horizontal { background: #30EA62; border: 1px solid #DEFFE8; height: 14px; width: 14px; margin: -5px 0; border-radius: 8px; }
QSlider::groove:vertical { width: 7px; background: #6A7072; border-radius: 3px; }
QSlider::add-page:vertical { background: #2CDA5D; border-radius: 3px; }
QSlider::handle:vertical { background: #30EA62; border: 1px solid #DEFFE8; height: 14px; width: 14px; margin: 0 -5px; border-radius: 8px; }
QProgressBar { background: #3E4442; border: 1px solid #656C68; border-radius: 5px; text-align: center; color: #F2F6F3; }
QProgressBar::chunk { background: #2ADB5B; border-radius: 4px; }
QTabWidget::pane { background: #25282A; border: 1px solid #484F4A; border-radius: 8px; }
QTabBar::tab { background: #35393B; color: #DAE1DC; border: 1px solid #52575A; border-bottom: 0px; border-top-left-radius: 7px; border-top-right-radius: 7px; padding: 7px 12px; }
QTabBar::tab:selected { background: #405749; color: #FFFFFF; border-color: #2EC65E; }
QTreeView, QTableView, QListView, QTreeWidget, QTableWidget, QListWidget {
  background: #202326; alternate-background-color: #2B2F31; color: #E2EAE5;
  border: 1px solid #4B5350; border-radius: 8px; outline: none; selection-background-color: #315D3E; selection-color: #FFFFFF;
}
QHeaderView::section { background: #383E3E; color: #E9EEEA; border: none; border-bottom: 1px solid #505B54; padding: 6px; }
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
  background: #202326; color: #E9EEEA; border: 1px solid #5C6760; border-radius: 7px;
  padding: 5px 7px; selection-background-color: #2FBD5A; selection-color: #101B13;
}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus { border-color: #2ED862; }
QComboBox::drop-down { width: 22px; border-left: 1px solid #47504A; }
QComboBox QAbstractItemView { background: #232629; selection-background-color: #3C6F4A; }
QCheckBox, QRadioButton, QLabel { color: #E9EEEA; }
QScrollBar:vertical { background: #292C2E; width: 10px; border: none; margin: 0; }
QScrollBar::handle:vertical { background: #555F59; min-height: 28px; border-radius: 5px; }
QScrollBar::handle:vertical:hover { background: #358D51; }
QScrollBar:horizontal { background: #292C2E; height: 10px; border: none; margin: 0; }
QScrollBar::handle:horizontal { background: #555F59; min-width: 28px; border-radius: 5px; }
QScrollBar::handle:horizontal:hover { background: #358D51; }
QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { width: 0; height: 0; background: none; border: none; }
QSplitter::handle { background: #39413C; }
QToolTip { background: #25282A; color: #E9EEEA; border: 1px solid #5B695D; border-radius: 6px; padding: 5px; }
)QSS");
}

class ButtonClassifier final : public QObject {
public:
    explicit ButtonClassifier(QObject *parent) : QObject(parent) {}

    bool eventFilter(QObject *o, QEvent *event) override
    {
        // Existing VLC buttons can change between Play and Pause at runtime.
        if (event->type() != QEvent::Show && event->type() != QEvent::ToolTipChange)
            return false;
        auto *button = qobject_cast<QAbstractButton *>(o);
        if (button) tag(button);
        return false;
    }

    static void tag(QAbstractButton *button)
    {
        // Only change visual properties; never replace icons, signals or geometry.
        QString descriptor = button->objectName() + QLatin1Char(' ')
                           + button->toolTip() + QLatin1Char(' ')
                           + button->accessibleName();
        if (auto *tool = qobject_cast<QToolButton *>(button)) {
            if (tool->defaultAction()) {
                descriptor += QLatin1Char(' ') + tool->defaultAction()->text()
                           + QLatin1Char(' ') + tool->defaultAction()->toolTip();
            }
        }
        descriptor = descriptor.toLower();
        const auto hasWord = [&descriptor](const QString &word) {
            int pos = -1;
            while ((pos = descriptor.indexOf(word, pos+1)) >= 0) {
                const auto boundary = [](QChar ch) { return !ch.isLetterOrNumber(); };
                const bool left = pos == 0 || boundary(descriptor.at(pos-1));
                const int end = pos + word.size();
                const bool right = end == descriptor.size() || boundary(descriptor.at(end));
                if (left && right) return true;
            }
            return false;
        };
        QString role;
        if (hasWord(QStringLiteral("record")) || hasWord(QStringLiteral("recordbutton")))
            role = QStringLiteral("record");
        else if (hasWord(QStringLiteral("play")) || hasWord(QStringLiteral("pause"))
              || hasWord(QStringLiteral("playbutton")) || hasWord(QStringLiteral("playpausebutton")))
            role = QStringLiteral("play");
        // For translated tooltips role is optional, standard VLC remains usable.
        if (button->property("alternixRole").toString() == role) return;
        button->setProperty("alternixRole", role);
        // QSS dynamic properties only take effect after repolishing this button.
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    }
};

class AlternixStyle final : public QProxyStyle {
public:
    AlternixStyle() : QProxyStyle(QStringLiteral("Fusion")) {}

    QPalette standardPalette() const override { return alternixPalette(); }

    void polish(QApplication *app) override
    {
        QProxyStyle::polish(app);
        app->setPalette(alternixPalette());
        if (initialized_) return;
        initialized_ = true;
        // Run after VLC has finished building its Qt interface, not during style loading.
        QTimer::singleShot(0, app, [app]() {
            app->setStyleSheet(alternixStyleSheet());
            auto *classifier = new ButtonClassifier(app);
            app->installEventFilter(classifier);
            for (auto *w : app->allWidgets())
                if (auto *button = qobject_cast<QAbstractButton *>(w))
                    ButtonClassifier::tag(button);
        });
    }
private:
    bool initialized_ = false;
};

} // namespace

class AlternixVLCStylePlugin final : public QStylePlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QStyleFactoryInterface" FILE "alternixvlcstyle.json")
public:
    QStyle *create(const QString &key) override
    {
        return key.compare(QLatin1String("AlternixVLC"), Qt::CaseInsensitive) == 0
             ? new AlternixStyle() : nullptr;
    }
};

#include "alternixvlcstyle.moc"
