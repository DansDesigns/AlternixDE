// AlternixOS — standalone OS-wide osm-keyboard for Qtile/X11.
// Build with AT-SPI (automatic text-field detection):
//   g++ -std=c++17 -O2 -fPIC -DOSM_USE_ATSPI osm-keyboard.cpp -o osm-keyboard \
//       $(pkg-config --cflags --libs Qt5Widgets Qt5Network x11 xtst atspi-2 gobject-2.0)
// Without AT-SPI, omit -DOSM_USE_ATSPI and atspi-2: --toggle/--show still work.
// No root, no moc, no helper scripts.
#include "osm-keyboard.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <QDebug>
#include <QRegularExpression>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

#ifdef OSM_USE_ATSPI
#include <atspi/atspi.h>
#endif

#include <unistd.h>
#include <algorithm>
#include <functional>
#include <memory>

class X11KeySender {
public:
    X11KeySender() : display_(XOpenDisplay(nullptr)) {
        if (!display_) {
            qWarning() << "osm-keyboard: X11 display is unavailable";
            return;
        }
        int eventBase = 0, errorBase = 0, major = 0, minor = 0;
        if (!XTestQueryExtension(display_, &eventBase, &errorBase, &major, &minor)) {
            qWarning() << "osm-keyboard: XTEST extension is unavailable";
            XCloseDisplay(display_);
            display_ = nullptr;
        }
    }
    ~X11KeySender() { if (display_) XCloseDisplay(display_); }
    Display *display() const { return display_; }

    void send(int qtKey, const QString &text,
              Qt::KeyboardModifiers mods = Qt::NoModifier) {
        if (!display_) return;
        KeySym sym = NoSymbol;
        switch (qtKey) {
        case Qt::Key_Backspace: sym = XK_BackSpace; break;
        case Qt::Key_Return: sym = XK_Return; break;
        case Qt::Key_Tab: sym = XK_Tab; break;
        case Qt::Key_Space: sym = XK_space; break;
        case Qt::Key_Left: sym = XK_Left; break;
        case Qt::Key_Up: sym = XK_Up; break;
        case Qt::Key_Down: sym = XK_Down; break;
        case Qt::Key_Right: sym = XK_Right; break;
        default:
            if (text.size() != 1) return;
            // ASCII is supported by the existing keyboard layout. For Latin-1,
            // X11 uses the Unicode code point directly as a keysym.
            if (text.at(0).unicode() <= 0xff)
                sym = static_cast<KeySym>(text.at(0).unicode());
            else
                sym = static_cast<KeySym>(0x01000000u | text.at(0).unicode());
            break;
        }
        if (sym == NoSymbol) return;
        int minKey = 8, maxKey = 255;
        XDisplayKeycodes(display_, &minKey, &maxKey);
        KeyCode code = 0;
        int level = -1;
        // Search the active base layout, not just XKeysymToKeycode; punctuation
        // differs on UK/US keyboards and often requires Shift or AltGr.
        for (int l = 0; l < 4 && !code; ++l) {
            for (int c = minKey; c <= maxKey; ++c) {
                if (XkbKeycodeToKeysym(display_, static_cast<KeyCode>(c), 0, l) == sym) {
                    code = static_cast<KeyCode>(c);
                    level = l;
                    break;
                }
            }
        }
        if (!code) {
            // OS-wide XTest can only press keys exposed by XKB. For the
            // regional virtual presets, use Unicode hex entry as a fallback
            // when a character is not mapped. Supported by GTK and many Qt
            // editors; some applications do not implement this input method.
            if (qtKey == Qt::Key_unknown && !text.isEmpty() && !unicodeInProgress_ &&
                mods == Qt::NoModifier) {
                typeUnicode(text);
            } else {
                qWarning() << "osm-keyboard: unmapped X11 key:" << qtKey << text;
            }
            return;
        }
        const bool shift = (level & 1) != 0;
        const bool altGr = (level & 2) != 0;
        const KeyCode shiftKey = XKeysymToKeycode(display_, XK_Shift_L);
        const KeyCode ctrlKey = XKeysymToKeycode(display_, XK_Control_L);
        const bool withCtrl = (mods & Qt::ControlModifier) != Qt::NoModifier;
        const KeyCode altGrKey = XKeysymToKeycode(display_, XK_ISO_Level3_Shift);
        if (withCtrl && !ctrlKey) {
            qWarning() << "osm-keyboard: Ctrl key is not mapped";
            return;
        }
        if (altGr && !altGrKey) {
            qWarning() << "osm-keyboard: current layout has no AltGr key";
            return;
        }
        if (withCtrl) XTestFakeKeyEvent(display_, ctrlKey, True, CurrentTime);
        if (altGr) XTestFakeKeyEvent(display_, altGrKey, True, CurrentTime);
        if (shift && shiftKey) XTestFakeKeyEvent(display_, shiftKey, True, CurrentTime);
        XTestFakeKeyEvent(display_, code, True, CurrentTime);
        XTestFakeKeyEvent(display_, code, False, CurrentTime);
        if (shift && shiftKey) XTestFakeKeyEvent(display_, shiftKey, False, CurrentTime);
        if (altGr) XTestFakeKeyEvent(display_, altGrKey, False, CurrentTime);
        if (withCtrl) XTestFakeKeyEvent(display_, ctrlKey, False, CurrentTime);
        XFlush(display_);
    }
private:
    void typeUnicode(const QString &value) {
        const KeyCode ctrl = XKeysymToKeycode(display_, XK_Control_L);
        const KeyCode shift = XKeysymToKeycode(display_, XK_Shift_L);
        const KeyCode u = XKeysymToKeycode(display_, XK_u);
        if (!ctrl || !shift || !u) {
            qWarning() << "osm-keyboard: Unicode input method unavailable:" << value;
            return;
        }
        // X11 itself does not insert arbitrary Unicode text using XTest.
        // Ctrl+Shift+U is the standard GTK Unicode composition sequence and
        // works in many, but not every, X11 text editor. Prevent recursive
        // fallback if a hexadecimal key is missing on an unusual XKB map.
        unicodeInProgress_ = true;
        for (const QChar ch : value) {
            XTestFakeKeyEvent(display_, ctrl, True, CurrentTime);
            XTestFakeKeyEvent(display_, shift, True, CurrentTime);
            XTestFakeKeyEvent(display_, u, True, CurrentTime);
            XTestFakeKeyEvent(display_, u, False, CurrentTime);
            XTestFakeKeyEvent(display_, shift, False, CurrentTime);
            XTestFakeKeyEvent(display_, ctrl, False, CurrentTime);
            for (const QChar digit : QString::number(ch.unicode(), 16))
                send(Qt::Key_unknown, QString(digit));
            send(Qt::Key_Return, {});
        }
        unicodeInProgress_ = false;
    }
    Display *display_ = nullptr;
    bool unicodeInProgress_ = false;
};

class KeyboardWindow : public QWidget {
public:
    KeyboardWindow(X11KeySender &sender)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint |
                            Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus),
          sender_(sender), prefs_(OsmKeyboardSettings::load()) {
        setObjectName(QStringLiteral("osmKeyboardWindow"));
        setWindowTitle(QStringLiteral("osm-keyboard"));
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_QuitOnClose, false);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        // On very narrow/short screens there may not be room for all 60px
        // touch targets at once. Scroll the keyboard instead of shrinking keys
        // or letting the dock cover the entire desktop.
        auto *scroll = new QScrollArea(this);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setFocusPolicy(Qt::NoFocus);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setStyleSheet(QStringLiteral("QScrollArea { border:0; background:transparent; }"));
        keyboard_ = new OsmKeyboard(scroll);
        scroll->setWidget(keyboard_);
        layout->addWidget(scroll);
        keyboard_->setSettings(prefs_);
        keyboard_->setModifiedKeyHandler(
                [this](int key, const QString &value, Qt::KeyboardModifiers mods) {
            sender_.send(key, value, mods);
        });
        keyboard_->setOnCustomiseRequested([this] { openSettings(); });
        keyboard_->setOnVisibilityChanged([this](bool shown) {
            // A Hide press hides only the child. The outer OS-level dock must
            // also disappear so it stops reserving screen space.
            if (!shown && isVisible() && !rebuilding_) hideWindow(true);
        });
        QTimer::singleShot(0, this, [this] { keyboard_->hide(); });
        // Detect physical screen geometry changes (e.g. auto-rotation), NOT
        // Qtile's availableGeometry: the latter includes our own reservation.
        // Never re-publish a strut periodically: Qtile accumulates each update.
        auto *displayTimer = new QTimer(this);
        displayTimer->setInterval(1000);
        connect(displayTimer, &QTimer::timeout, this, [this] {
            if (isVisible() && !remapping_ && targetScreen_ &&
                targetScreen_->geometry() != lastScreenGeometry_) {
                remapForGeometryChange();
            }
        });
        displayTimer->start();
    }

    const OsmKeyboardSettings &preferences() const { return prefs_; }
    bool isSettingsOpen() const { return settingsOpen_; }

    void showWindow(bool explicitRequest = true) {
        if (explicitRequest) {
            manualShown_ = true;
            keyboard_->clearUserHidden();
        } else if (keyboard_->userHidden()) {
            return;
        }
        // Focus events may ask us to show repeatedly; do not re-map or re-send
        // _NET_WM_STRUT_PARTIAL for an already mapped Qtile dock.
        if (isVisible()) {
            if (!keyboard_->isVisible()) keyboard_->show();
            return;
        }
        if (!targetScreen_ || explicitRequest) {
            targetScreen_ = QGuiApplication::screenAt(QCursor::pos());
            if (!targetScreen_) targetScreen_ = QGuiApplication::primaryScreen();
        }
        rebuilding_ = true;
        configureKeyboardLayout();
        if (!keyboard_->isVisible()) keyboard_->show();
        reposition();
        setWindowOpacity(prefs_.opacity / 100.0);
        applyX11WindowHints();
        // Publish the strut BEFORE mapping so Qtile reads it once when managing
        // the window. Never mutate the strut while Qtile is managing this dock.
        prepareStrutBeforeMap();
        show();
        rebuilding_ = false;
    }
    void hideWindow(bool explicitRequest = true) {
        if (explicitRequest) {
            manualShown_ = false;
            rebuilding_ = true;
            keyboard_->hideByUser();
            rebuilding_ = false;
        }
        if (!isVisible()) return;
        rebuilding_ = true;
        // Important: leave strut properties in place until the UnmapNotify.
        // Qtile frees the dock's reserved space when it unmanages the window;
        // deleting the property before unmapping can lose that accounting.
        hide();
        rebuilding_ = false;
    }
    void toggle() { if (isVisible()) hideWindow(true); else showWindow(true); }

    void autoFocusChange(bool editable) {
        ++focusSerial_;
        if (!prefs_.autoShow || manualShown_ || settingsOpen_ || remapping_) return;
        if (editable) {
            // A deliberate Hide must persist until the taskbar reopens it.
            if (keyboard_->userHidden()) return;
            showWindow(false);
        } else if (isVisible()) {
            // Defer hides to avoid flicker when focus quickly changes widgets.
            const int id = focusSerial_;
            QTimer::singleShot(220, this, [this, id] {
                if (id == focusSerial_ && !manualShown_ && !settingsOpen_)
                    hideWindow(false);
            });
        }
    }

    void openSettings() {
        // Modeless: no nested Qt/GLib event loop while the AT-SPI monitor runs.
        // A second invocation raises the same window instead of creating another.
        if (settingsDialog_) {
            settingsDialog_->show();
            settingsDialog_->raise();
            settingsDialog_->activateWindow();
            return;
        }
        settingsOpen_ = true;
        ++focusSerial_;
        auto *dialog = new QDialog(nullptr);
        settingsDialog_ = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Customise osm-keyboard"));
        dialog->setWindowFlag(Qt::WindowStaysOnTopHint, true);
        dialog->resize(490, 520);

        // Persist the editing data until the nonblocking dialog is closed.
        auto editing = std::make_shared<OsmKeyboardSettings>(prefs_);
        auto *outer = new QVBoxLayout(dialog);
        auto *tabs = new QTabWidget(dialog);
        outer->addWidget(tabs);
        auto *appearance = new QWidget(tabs);
        auto *form1 = new QFormLayout(appearance);
        form1->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        auto *theme = new QComboBox(appearance);
        theme->addItems({QStringLiteral("Dark"), QStringLiteral("Light"),
                         QStringLiteral("Midnight"), QStringLiteral("Custom")});
        theme->setCurrentText(editing->theme);
        form1->addRow(QStringLiteral("Theme"), theme);

        auto colorPicker = [&](const QString &name, QColor OsmKeyboardSettings::*member) {
            auto *button = new QPushButton(appearance);
            button->setMinimumHeight(36);
            auto update = [button, editing, member] {
                const QColor current = editing.get()->*member;
                button->setText(current.name());
                button->setStyleSheet(QStringLiteral("background:%1; color:%2; border:1px solid #888;"
                                      "padding:6px; border-radius:5px;")
                                      .arg(current.name(), current.lightness() < 128 ? "white" : "black"));
            };
            update();
            connect(button, &QPushButton::clicked, dialog, [dialog, editing, member, update] {
                // Native portal-based colour pickers can block for seconds on
                // Qtile. The Qt picker uses the existing Qt5 dependency.
                const QColor next = QColorDialog::getColor(
                    editing.get()->*member, dialog, QStringLiteral("Select keyboard colour"),
                    QColorDialog::DontUseNativeDialog);
                if (next.isValid()) { editing.get()->*member = next; update(); }
            });
            form1->addRow(name, button);
            return button;
        };
        colorPicker(QStringLiteral("Accent"), &OsmKeyboardSettings::accent);
        auto *backgroundButton = colorPicker(QStringLiteral("Custom background"),
                                             &OsmKeyboardSettings::customBackground);
        auto *keysButton = colorPicker(QStringLiteral("Custom key colour"),
                                       &OsmKeyboardSettings::customKeys);
        auto *textButton = colorPicker(QStringLiteral("Custom key text"),
                                       &OsmKeyboardSettings::customText);
        auto updateCustomEnabled = [=] {
            const bool custom = theme->currentText() == QLatin1String("Custom");
            backgroundButton->setEnabled(custom);
            keysButton->setEnabled(custom);
            textButton->setEnabled(custom);
        };
        connect(theme, &QComboBox::currentTextChanged, dialog,
                [updateCustomEnabled](const QString &) { updateCustomEnabled(); });
        updateCustomEnabled();
        auto *size = new QSpinBox(appearance);
        size->setRange(60, 100);
        size->setSuffix(QStringLiteral(" px"));
        size->setValue(editing->keyHeight);
        form1->addRow(QStringLiteral("Minimum key height (60 px minimum)"), size);
        auto *font = new QSpinBox(appearance);
        font->setRange(12, 28);
        font->setSuffix(QStringLiteral(" px"));
        font->setValue(editing->fontSize);
        form1->addRow(QStringLiteral("Key label size"), font);
        auto *opacity = new QSpinBox(appearance);
        opacity->setRange(65, 100);
        opacity->setSuffix(QStringLiteral(" %"));
        opacity->setValue(editing->opacity);
        form1->addRow(QStringLiteral("Opacity"), opacity);
        tabs->addTab(appearance, QStringLiteral("Appearance"));

        auto *layoutPage = new QWidget(tabs);
        auto *layoutForm = new QFormLayout(layoutPage);
        auto *regional = new QComboBox(layoutPage);
        regional->addItem(QStringLiteral("United Kingdom (QWERTY)"), "uk");
        regional->addItem(QStringLiteral("United States (QWERTY)"), "us");
        regional->addItem(QStringLiteral("EU International (accents)"), "intl");
        regional->addItem(QStringLiteral("French (AZERTY)"), "fr");
        regional->addItem(QStringLiteral("German (QWERTZ)"), "de");
        regional->addItem(QStringLiteral("Spanish (QWERTY)"), "es");
        regional->addItem(QStringLiteral("Italian (QWERTY)"), "it");
        regional->addItem(QStringLiteral("Portuguese (QWERTY)"), "pt");
        regional->addItem(QStringLiteral("Swedish (QWERTY)"), "se");
        const int currentLayout = regional->findData(editing->layout);
        regional->setCurrentIndex(currentLayout < 0 ? 0 : currentLayout);
        layoutForm->addRow(QStringLiteral("Keyboard layout"), regional);
        auto *mode = new QComboBox(layoutPage);
        mode->addItem(QStringLiteral("Automatic (compact on small displays)"), "auto");
        mode->addItem(QStringLiteral("Full keyboard"), "full");
        mode->addItem(QStringLiteral("Compact keyboard (letters only)"), "compact");
        const int currentMode = mode->findData(editing->mode);
        mode->setCurrentIndex(currentMode < 0 ? 0 : currentMode);
        layoutForm->addRow(QStringLiteral("Keyboard size/layout mode"), mode);
        auto *layoutNote = new QLabel(QStringLiteral(
            "Key labels and Shift characters follow the selected region "
            "(for example UK Shift+7 is &). The physical keyboard and "
            "system XKB layout are not changed. Compact mode keeps letters, "
            "Shift, Settings, Hide, Enter, Backspace and Space with 60px keys. "
            "Unicode input in other applications depends on their input-method support."), layoutPage);
        layoutNote->setWordWrap(true);
        layoutForm->addRow(layoutNote);
        tabs->addTab(layoutPage, QStringLiteral("Layouts"));

        auto *behaviour = new QWidget(tabs);
        auto *form2 = new QFormLayout(behaviour);
        auto *pop = new QCheckBox(QStringLiteral("Show enlarged key while pressed"), behaviour);
        pop->setChecked(editing->keyPopup);
        form2->addRow(pop);
        auto *popDelay = new QSpinBox(behaviour);
        popDelay->setRange(0, 600);
        popDelay->setSuffix(QStringLiteral(" ms"));
        popDelay->setValue(editing->popupDuration);
        popDelay->setEnabled(editing->keyPopup);
        connect(pop, &QCheckBox::toggled, popDelay, &QWidget::setEnabled);
        form2->addRow(QStringLiteral("Key preview hold time"), popDelay);
        auto *autoShow = new QCheckBox(QStringLiteral("Automatically show for editable fields"), behaviour);
        autoShow->setChecked(editing->autoShow);
#ifndef OSM_USE_ATSPI
        autoShow->setChecked(false);
        autoShow->setEnabled(false);
        autoShow->setToolTip(QStringLiteral("Requires build with libatspi2.0 (-DOSM_USE_ATSPI)."));
#endif
        form2->addRow(autoShow);
        auto *dock = new QCheckBox(QStringLiteral("Dock across the bottom of the screen"), behaviour);
        dock->setChecked(editing->docked);
        form2->addRow(dock);
        auto *reserve = new QCheckBox(QStringLiteral("Reserve desktop space when shown"), behaviour);
        reserve->setChecked(editing->reserveSpace);
        reserve->setEnabled(editing->docked);
        connect(dock, &QCheckBox::toggled, reserve, &QWidget::setEnabled);
        form2->addRow(reserve);
        auto *tip = new QLabel(QStringLiteral("Use osm-keyboard --toggle from the taskbar "
            "to show or hide the keyboard. Auto-detection depends on "
            "applications exposing accessibility information."), behaviour);
        tip->setWordWrap(true);
        tip->setStyleSheet(QStringLiteral("color: palette(mid); margin-top: 16px;"));
        form2->addRow(tip);
        tabs->addTab(behaviour, QStringLiteral("Behaviour"));

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
        outer->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        connect(dialog, &QDialog::finished, this,
                [this, editing, theme, regional, mode, size, font, opacity, pop,
                 popDelay, autoShow, dock, reserve](int result) {
            if (result == QDialog::Accepted) {
                editing->theme = theme->currentText();
                editing->layout = regional->currentData().toString();
                editing->mode = mode->currentData().toString();
                editing->keyHeight = size->value();
                editing->fontSize = font->value();
                editing->opacity = opacity->value();
                editing->keyPopup = pop->isChecked();
                editing->popupDuration = popDelay->value();
                editing->autoShow = autoShow->isChecked();
                editing->docked = dock->isChecked();
                editing->reserveSpace = reserve->isChecked();
                // Colour, font, layout and popup changes should NOT remap
                // the Qtile dock or change its strut. Only these three
                // geometry/reservation settings need an unmap/remap cycle.
                const bool needRemap = prefs_.keyHeight != editing->keyHeight ||
                                       prefs_.mode != editing->mode ||
                                       prefs_.docked != editing->docked ||
                                       prefs_.reserveSpace != editing->reserveSpace;
                const bool reopen = needRemap && isVisible();
                if (reopen) hideWindow(false);
                prefs_ = *editing;
                prefs_.save();
                rebuilding_ = true;
                keyboard_->setSettings(prefs_);
                rebuilding_ = false;
                setWindowOpacity(prefs_.opacity / 100.0);
                if (reopen) {
                    remapping_ = true;
                    QTimer::singleShot(180, this, [this] {
                        remapping_ = false;
                        showWindow(false);
                    });
                }
            }
            settingsOpen_ = false;
            settingsDialog_.clear();
        });
        dialog->show();
        dialog->raise();
        dialog->activateWindow();
    }

private:
    void configureKeyboardLayout() {
        QScreen *screen = targetScreen_;
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const QRect rect = screen->geometry();
        // 12 regular keys across, each >=60px, plus layout gutters.
        const int fullMinWidth = 12 * 60 + 11 * 4 + 12;
        const bool compact = prefs_.mode == QLatin1String("compact") ||
              (prefs_.mode == QLatin1String("auto") &&
               (rect.width() < fullMinWidth || rect.height() < 560));
        const int available = rect.width() - 12;
        const int columns = qBound(4, (available + 4) / 64, 10);
        keyboard_->setCompactLayout(compact, columns);
    }

    void reposition() {
        QScreen *screen = targetScreen_;
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (!screen) return;
        const QRect rect = screen->geometry();
        lastScreenGeometry_ = rect;
        const int margin = 4;
        const int rowCount = keyboard_->rowCount();
        const int preferredHeight = rowCount * prefs_.keyHeight +
                                    qMax(0, rowCount - 1) * 4 + 12 + 2;
        // Leave at least 90px for Qtile's top/bottom bars even on a small
        // landscape screen. Extra rows remain reachable by touch scrolling.
        const int height = qMin(preferredHeight, qMax(80, rect.height() - 90));
        if (prefs_.docked) {
            const QRect proposed(rect.x(), rect.y() + rect.height() - height,
                                 rect.width(), height);
            if (geometry() != proposed) setGeometry(proposed);
        } else {
            const int width = qMin(1060, qMax(400, rect.width() - 2 * margin));
            const QRect proposed(rect.center().x() - width/2,
                                 rect.y() + rect.height() - height - 24,
                                 width, height);
            if (geometry() != proposed) setGeometry(proposed);
        }
    }

    void applyX11WindowHints() {
        Display *d = sender_.display();
        if (!d) return;
        const Window w = static_cast<Window>(winId());
        XWMHints *hints = XGetWMHints(d, w);
        if (!hints) hints = XAllocWMHints();
        if (hints) {
            hints->flags |= InputHint;
            hints->input = False;
            XSetWMHints(d, w, hints);
            XFree(hints);
        }
        const Atom type = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False);
        const Atom dockType = XInternAtom(d, "_NET_WM_WINDOW_TYPE_DOCK", False);
        const Atom utilityType = XInternAtom(d, "_NET_WM_WINDOW_TYPE_UTILITY", False);
        const Atom use = prefs_.docked ? dockType : utilityType;
        XChangeProperty(d, w, type, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&use), 1);
        const Atom skipTaskbar = XInternAtom(d, "_NET_WM_STATE_SKIP_TASKBAR", False);
        const Atom skipPager = XInternAtom(d, "_NET_WM_STATE_SKIP_PAGER", False);
        const Atom state = XInternAtom(d, "_NET_WM_STATE", False);
        Atom states[] = {skipTaskbar, skipPager};
        XChangeProperty(d, w, state, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(states), 2);
        XFlush(d);
    }

    void remapForGeometryChange() {
        if (!isVisible() || remapping_) return;
        remapping_ = true;
        hideWindow(false);
        // Allow Qtile to unmanage the previous dock before mapping again.
        QTimer::singleShot(180, this, [this] {
            remapping_ = false;
            showWindow(false);
        });
    }

    void prepareStrutBeforeMap() {
        Display *d = sender_.display();
        if (!d) return;
        const Window w = static_cast<Window>(winId());
        const Atom strut = XInternAtom(d, "_NET_WM_STRUT", False);
        const Atom partial = XInternAtom(d, "_NET_WM_STRUT_PARTIAL", False);
        const int rootHeight = DisplayHeight(d, DefaultScreen(d));
        // An EWMH bottom strut is root-relative. A secondary/upper screen
        // cannot advertise a full root-bottom reservation.
        if (!prefs_.docked || !prefs_.reserveSpace ||
            geometry().bottom() != rootHeight - 1) {
            XDeleteProperty(d, w, strut);
            XDeleteProperty(d, w, partial);
            XFlush(d);
            return;
        }
        const unsigned long bottom = static_cast<unsigned long>(height());
        unsigned long basic[4] = {0, 0, 0, bottom};
        unsigned long extended[12] = {};
        extended[3] = bottom;
        extended[10] = static_cast<unsigned long>(qMax(0, x()));
        extended[11] = static_cast<unsigned long>(qMax(0, x() + width() - 1));
        XChangeProperty(d, w, strut, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(basic), 4);
        XChangeProperty(d, w, partial, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(extended), 12);
        XFlush(d);
    }

    X11KeySender &sender_;
    OsmKeyboard *keyboard_ = nullptr;
    OsmKeyboardSettings prefs_;
    bool manualShown_ = false;
    bool settingsOpen_ = false;
    QPointer<QDialog> settingsDialog_;
    bool rebuilding_ = false;
    bool remapping_ = false;
    int focusSerial_ = 0;
    QRect lastScreenGeometry_;
    QPointer<QScreen> targetScreen_;
};

#ifdef OSM_USE_ATSPI
class AccessibleFocusMonitor {
public:
    explicit AccessibleFocusMonitor(KeyboardWindow *window) : window_(window) {
        if (atspi_init() != 0) {
            qWarning() << "osm-keyboard: AT-SPI unavailable (manual toggle still works)";
            return;
        }
        listener_ = atspi_event_listener_new(&AccessibleFocusMonitor::onEvent, this, nullptr);
        if (!listener_ || !atspi_event_listener_register(
                listener_, "object:state-changed:focused", nullptr)) {
            qWarning() << "osm-keyboard: could not subscribe to accessibility focus";
            if (listener_) { g_object_unref(listener_); listener_ = nullptr; }
            return;
        }
        auto *pump = new QTimer(window_);
        pump->setInterval(100);
        QObject::connect(pump, &QTimer::timeout, window_, [] {
            // Bound accessibility pumping to avoid stealing the UI thread
            // during focus storms (especially when Customise is open).
            for (int n = 0; n < 3 && g_main_context_pending(nullptr); ++n)
                g_main_context_iteration(nullptr, FALSE);
        });
        pump->start();
    }
    ~AccessibleFocusMonitor() {
        if (listener_) {
            atspi_event_listener_deregister(listener_, "object:state-changed:focused", nullptr);
            g_object_unref(listener_);
        }
    }
private:
    static void onEvent(AtspiEvent *e, void *ptr) {
        auto *self = static_cast<AccessibleFocusMonitor *>(ptr);
        if (!self || !self->window_ || !e || !e->source) return;
        // Never query accessible roles/states for our own settings window:
        // these can cause synchronous cross-process DBus roundtrips on every
        // focus event and make the Customise UI feel frozen.
        if (self->window_->isSettingsOpen()) return;
        if (e->detail1 == 0) {
            QTimer::singleShot(0, self->window_, [window = self->window_] {
                if (window) window->autoFocusChange(false);
            });
            return;
        }
        const AtspiRole role = atspi_accessible_get_role(e->source, nullptr);
        bool editable = role == ATSPI_ROLE_ENTRY || role == ATSPI_ROLE_PASSWORD_TEXT ||
                        role == ATSPI_ROLE_TERMINAL || role == ATSPI_ROLE_EDITBAR;
        AtspiStateSet *states = atspi_accessible_get_state_set(e->source);
        if (states) {
            // Plain text views should open a keyboard only when editable.
            editable = (editable && atspi_state_set_contains(states, ATSPI_STATE_EDITABLE)) ||
                       (role == ATSPI_ROLE_TEXT && atspi_state_set_contains(states, ATSPI_STATE_EDITABLE));
            g_object_unref(states);
        }
        QTimer::singleShot(0, self->window_, [window = self->window_, editable] {
            if (window) window->autoFocusChange(editable);
        });
    }
    QPointer<KeyboardWindow> window_;
    AtspiEventListener *listener_ = nullptr;
};
#endif

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("osm-keyboard"));
    app.setOrganizationName(QStringLiteral("AlternixOS"));
    app.setQuitOnLastWindowClosed(false);

    const QStringList args = app.arguments();
    QString command = QStringLiteral("show");
    if (args.contains(QStringLiteral("--daemon"))) command = QStringLiteral("daemon");
    if (args.contains(QStringLiteral("--show"))) command = QStringLiteral("show");
    if (args.contains(QStringLiteral("--hide"))) command = QStringLiteral("hide");
    if (args.contains(QStringLiteral("--toggle"))) command = QStringLiteral("toggle");
    if (args.contains(QStringLiteral("--settings"))) command = QStringLiteral("settings");
    if (args.contains(QStringLiteral("--quit"))) command = QStringLiteral("quit");

    QString socketName = QStringLiteral("osm-keyboard-%1-%2")
        .arg(static_cast<qulonglong>(getuid()))
        .arg(qEnvironmentVariable("DISPLAY", QStringLiteral("default")));
    socketName.replace(QRegularExpression(QStringLiteral("[^a-zA-Z0-9_-]")), QStringLiteral("_"));
    QLocalSocket client;
    client.connectToServer(socketName);
    if (client.waitForConnected(180)) {
        client.write(command.toUtf8() + '\n');
        client.waitForBytesWritten(250);
        client.disconnectFromServer();
        return 0;
    }
    if (command == QLatin1String("hide") || command == QLatin1String("quit")) return 0;
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    // If a previous instance crashed, the socket path may remain.
    QLocalServer::removeServer(socketName);
    if (!server.listen(socketName)) {
        qCritical() << "osm-keyboard: cannot create local control socket:" << server.errorString();
        return 2;
    }

    X11KeySender sender;
    if (!sender.display()) return 3;
    KeyboardWindow window(sender);
#ifdef OSM_USE_ATSPI
    AccessibleFocusMonitor monitor(&window);
#endif
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            QLocalSocket *peer = server.nextPendingConnection();
            if (!peer) break;
            QObject::connect(peer, &QLocalSocket::readyRead, peer, [&, peer] {
                const QString cmd = QString::fromUtf8(peer->readAll()).trimmed();
                if (cmd == QLatin1String("show")) window.showWindow(true);
                else if (cmd == QLatin1String("hide")) window.hideWindow(true);
                else if (cmd == QLatin1String("toggle")) window.toggle();
                else if (cmd == QLatin1String("settings")) window.openSettings();
                else if (cmd == QLatin1String("quit")) app.quit();
                peer->disconnectFromServer();
            });
            QObject::connect(peer, &QLocalSocket::disconnected, peer, &QObject::deleteLater);
        }
    });
    if (command == QLatin1String("settings")) {
        QTimer::singleShot(0, &window, [&] { window.openSettings(); });
    } else if (command != QLatin1String("daemon")) {
        QTimer::singleShot(0, &window, [&] { window.showWindow(true); });
    }
    return app.exec();
}
