// AlternixOS — osm-keyboard.h
// Reusable Qt5 on-screen keyboard; no Q_OBJECT/moc required.
// Embedded use: OsmKeyboard *kb = new OsmKeyboard(parent); layout->addWidget(kb);
// Optional OS-wide delivery: kb->setKeyHandler(callback); see osm-keyboard.cpp.
#ifndef OSM_KEYBOARD_H
#define OSM_KEYBOARD_H

#include <QApplication>
#include <QColor>
#include <QGridLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <utility>
#include <vector>

#ifndef OSM_KEY_HIDE
#define OSM_KEY_HIDE (-100)
#endif
#define OSM_KEY_CUSTOMISE (-101)

struct OsmKeyDef {
    QString lower;
    QString upper;
    int span = 2;
    int special = 0;
    int rowSpan = 1; // Enter occupies both the home and lower letter rows.
};

struct OsmKeyboardSettings {
    QString theme = QStringLiteral("Dark");
    QString layout = QStringLiteral("uk"); // On-screen arrangement, not host XKB setting.
    QColor accent = QColor(QStringLiteral("#4a90d9"));
    QColor customBackground = QColor(QStringLiteral("#1e1e1e"));
    QColor customKeys = QColor(QStringLiteral("#383838"));
    QColor customText = QColor(QStringLiteral("#ededed"));
    int keyHeight = 48;
    int fontSize = 17;
    int opacity = 100;
    bool keyPopup = true;
    int popupDuration = 125;
    bool autoShow = true;  // OS-wide auto-show requires AT-SPI support.
    bool docked = true;
    bool reserveSpace = true;

    static OsmKeyboardSettings load() {
        QSettings s(QStringLiteral("AlternixOS"), QStringLiteral("osm-keyboard"));
        OsmKeyboardSettings p;
        p.theme = s.value(QStringLiteral("theme"), p.theme).toString();
        p.layout = s.value(QStringLiteral("layout"), p.layout).toString().toLower();
        if (!QStringList{QStringLiteral("uk"), QStringLiteral("us"), QStringLiteral("intl"),
                         QStringLiteral("fr"), QStringLiteral("de"), QStringLiteral("es"),
                         QStringLiteral("it"), QStringLiteral("pt"), QStringLiteral("se")}.contains(p.layout))
            p.layout = QStringLiteral("uk");
        if (p.theme != QLatin1String("Dark") &&
            p.theme != QLatin1String("Light") &&
            p.theme != QLatin1String("Midnight") &&
            p.theme != QLatin1String("Custom")) p.theme = QStringLiteral("Dark");
        p.accent = s.value(QStringLiteral("accent"), p.accent).value<QColor>();
        p.customBackground = s.value(QStringLiteral("customBackground"), p.customBackground).value<QColor>();
        p.customKeys = s.value(QStringLiteral("customKeys"), p.customKeys).value<QColor>();
        p.customText = s.value(QStringLiteral("customText"), p.customText).value<QColor>();
        if (!p.accent.isValid()) p.accent = QColor("#4a90d9");
        if (!p.customBackground.isValid()) p.customBackground = QColor("#1e1e1e");
        if (!p.customKeys.isValid()) p.customKeys = QColor("#383838");
        if (!p.customText.isValid()) p.customText = QColor("#ededed");
        p.keyHeight = qBound(36, s.value(QStringLiteral("keyHeight"), p.keyHeight).toInt(), 85);
        p.fontSize = qBound(12, s.value(QStringLiteral("fontSize"), p.fontSize).toInt(), 28);
        p.opacity = qBound(65, s.value(QStringLiteral("opacity"), p.opacity).toInt(), 100);
        p.keyPopup = s.value(QStringLiteral("keyPopup"), p.keyPopup).toBool();
        p.popupDuration = qBound(0, s.value(QStringLiteral("popupDuration"), p.popupDuration).toInt(), 600);
        p.autoShow = s.value(QStringLiteral("autoShow"), p.autoShow).toBool();
        p.docked = s.value(QStringLiteral("docked"), p.docked).toBool();
        p.reserveSpace = s.value(QStringLiteral("reserveSpace"), p.reserveSpace).toBool();
        return p;
    }

    void save() const {
        QSettings s(QStringLiteral("AlternixOS"), QStringLiteral("osm-keyboard"));
        s.setValue(QStringLiteral("theme"), theme);
        s.setValue(QStringLiteral("layout"), layout);
        s.setValue(QStringLiteral("accent"), accent);
        s.setValue(QStringLiteral("customBackground"), customBackground);
        s.setValue(QStringLiteral("customKeys"), customKeys);
        s.setValue(QStringLiteral("customText"), customText);
        s.setValue(QStringLiteral("keyHeight"), keyHeight);
        s.setValue(QStringLiteral("fontSize"), fontSize);
        s.setValue(QStringLiteral("opacity"), opacity);
        s.setValue(QStringLiteral("keyPopup"), keyPopup);
        s.setValue(QStringLiteral("popupDuration"), popupDuration);
        s.setValue(QStringLiteral("autoShow"), autoShow);
        s.setValue(QStringLiteral("docked"), docked);
        s.setValue(QStringLiteral("reserveSpace"), reserveSpace);
        s.sync();
    }
};

class OsmKeyboard : public QWidget {
public:
    explicit OsmKeyboard(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("osmKeyboard"));
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_ShowWithoutActivating);
        m_grid = new QGridLayout(this);
        m_grid->setContentsMargins(6, 6, 6, 6);
        m_grid->setSpacing(4);
        m_popup = new QLabel(this);
        m_popup->setObjectName(QStringLiteral("osmKeyboardPopup"));
        m_popup->setAlignment(Qt::AlignCenter);
        m_popup->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_popup->hide();
        m_style = OsmKeyboardSettings::load();
        applyStyle();
        buildLayout();
    }

    const OsmKeyboardSettings &settings() const { return m_style; }
    void setSettings(const OsmKeyboardSettings &settings) {
        const bool rebuild = m_style.layout != settings.layout ||
                             m_style.keyHeight != settings.keyHeight;
        m_style = settings;
        applyStyle();
        if (rebuild) buildLayout();
    }

    // External input backend (XTest for system-wide use). If not set, retains
    // the original local Qt-widget key handling.
    void setKeyHandler(std::function<void(int, const QString &)> handler) {
        m_keyHandler = std::move(handler);
    }
    void setOnCustomiseRequested(std::function<void()> handler) {
        m_onCustomise = std::move(handler);
    }
    void setOnVisibilityChanged(std::function<void(bool)> fn) {
        m_onVisibilityChanged = std::move(fn);
    }

    // Embedded Qt use only: QApplication focusChanged cannot observe other apps.
    void attachAutoShow() {
        if (m_embeddedConnected) return;
        m_embeddedConnected = true;
        m_autoShow = true;
        hide();
        QObject::connect(qApp, &QApplication::focusChanged, this,
                         [this](QWidget *, QWidget *now) {
            if (auto *edit = qobject_cast<QLineEdit *>(now)) m_lastLineEdit = edit;
            if (!m_autoShow || (now && isAncestorOf(now)) || m_userHidden) return;
            setVisible(qobject_cast<QLineEdit *>(now) != nullptr);
        });
    }
    void setAutoShow(bool on) { m_autoShow = on; }
    void showFromTaskbar() { m_userHidden = false; show(); }
    void hideByUser() { m_userHidden = true; hide(); }
    bool userHidden() const { return m_userHidden; }
    void clearUserHidden() { m_userHidden = false; }

protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        if (m_onVisibilityChanged) m_onVisibilityChanged(true);
    }
    void hideEvent(QHideEvent *event) override {
        QWidget::hideEvent(event);
        m_popup->hide();
        if (m_onVisibilityChanged) m_onVisibilityChanged(false);
    }

private:
    static bool raised(const OsmKeyDef &def, bool shift, bool caps) {
        // CapsLock affects letters, not punctuation/digits. Shift+Caps = lower.
        if (def.lower.size() == 1 && def.upper.size() == 1 &&
            def.lower.at(0).isLetter() && def.upper.at(0).isLetter())
            return shift != caps;
        return shift;
    }
    QString face(const OsmKeyDef &def) const {
        return raised(def, m_shift, m_caps) ? def.upper : def.lower;
    }

    void emitKey(int qtKey, const QString &text) {
        if (m_keyHandler) { m_keyHandler(qtKey, text); return; }
        QWidget *target = QApplication::focusWidget();
        auto *edit = qobject_cast<QLineEdit *>(target);
        if (!edit && m_lastLineEdit && m_lastLineEdit->isVisible() &&
            m_lastLineEdit->isEnabled()) edit = m_lastLineEdit.data();
        if (edit && edit->isVisible() && !edit->isReadOnly()) {
            if (qtKey == Qt::Key_Backspace) {
                if (!edit->hasSelectedText() && edit->cursorPosition() == 0 &&
                    !edit->text().isEmpty()) edit->setCursorPosition(edit->text().size());
                edit->backspace();
                return;
            }
            if (qtKey == Qt::Key_unknown && !text.isEmpty()) {
                edit->insert(text);
                return;
            }
            target = edit;
        }
        if (!target || isAncestorOf(target)) return;
        QKeyEvent press(QEvent::KeyPress, qtKey, Qt::NoModifier, text);
        QApplication::sendEvent(target, &press);
        QKeyEvent release(QEvent::KeyRelease, qtKey, Qt::NoModifier, text);
        QApplication::sendEvent(target, &release);
    }

    void showKeyPopup(QPushButton *key, const QString &label) {
        if (!m_style.keyPopup || label.isEmpty()) return;
        ++m_popupSerial;
        m_popup->setText(label);
        const int w = qMax(key->width(), 60);
        const int h = qMax(m_style.keyHeight, 44);
        const QPoint topLeft = key->mapTo(this, QPoint(0, 0));
        // Keep the preview inside the keyboard, including the first key row.
        int y = topLeft.y() - h + 5;
        if (y < 0) y = topLeft.y() + key->height() - h - 2;
        const int x = qBound(0, topLeft.x() + (key->width()-w)/2, qMax(0, width()-w));
        m_popup->setGeometry(x, y, w, h);
        m_popup->raise();
        m_popup->show();
    }
    void schedulePopupHide() {
        const int serial = ++m_popupSerial;
        QTimer::singleShot(m_style.popupDuration, this, [this, serial] {
            if (serial == m_popupSerial) m_popup->hide();
        });
    }

    QPushButton *makeKey(const OsmKeyDef &def) {
        auto *button = new QPushButton(this);
        button->setFocusPolicy(Qt::NoFocus);
        button->setMinimumHeight(m_style.keyHeight);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        if (def.special) {
            button->setObjectName(QStringLiteral("modKey"));
            button->setText(def.lower);
            const int special = def.special;
            QObject::connect(button, &QPushButton::clicked, this, [this, special, button] {
                handleSpecial(special, button);
            });
        } else {
            m_characterKeys.push_back({button, def});
            button->setText(face(def));
            // Display the regional Shift symbol as a small second legend.
            // E.g. UK 7/&; German 7/; French è/7. The selected layout
            // determines BOTH what is shown and what is transmitted.
            const bool punctuationOrNumber = def.lower.size() != 1 ||
                def.upper.size() != 1 || !def.lower.at(0).isLetter() ||
                !def.upper.at(0).isLetter();
            if (punctuationOrNumber && !def.upper.isEmpty() && def.upper != def.lower) {
                auto *legend = new QLabel(def.upper, button);
                legend->setObjectName(QStringLiteral("osmShiftLegend"));
                legend->setAttribute(Qt::WA_TransparentForMouseEvents);
                legend->setFocusPolicy(Qt::NoFocus);
                legend->setAlignment(Qt::AlignRight | Qt::AlignTop);
                auto *overlay = new QVBoxLayout(button);
                overlay->setContentsMargins(3, 2, 4, 2);
                overlay->addWidget(legend, 0, Qt::AlignRight | Qt::AlignTop);
                overlay->addStretch();
                button->setToolTip(QStringLiteral("%1  /  Shift+%1: %2")
                                   .arg(def.lower, def.upper));
            }
            QObject::connect(button, &QPushButton::clicked, this, [this, def] {
                emitKey(Qt::Key_unknown, face(def));
                if (m_shift) { m_shift = false; refreshFaces(); }
            });
        }
        QObject::connect(button, &QPushButton::pressed, this, [this, button, def] {
            if (def.special != OSM_KEY_HIDE && def.special != OSM_KEY_CUSTOMISE)
                showKeyPopup(button, def.special ? def.lower : face(def));
        });
        QObject::connect(button, &QPushButton::released, this, [this] {
            schedulePopupHide();
        });
        return button;
    }

    void handleSpecial(int key, QPushButton *button) {
        switch (key) {
        case Qt::Key_Shift:
            m_shift = !m_shift;
            button->setObjectName(m_shift ? QStringLiteral("modKeyOn") : QStringLiteral("modKey"));
            restyle(button);
            refreshFaces();
            break;
        case Qt::Key_CapsLock:
            m_caps = !m_caps;
            button->setObjectName(m_caps ? QStringLiteral("modKeyOn") : QStringLiteral("modKey"));
            restyle(button);
            refreshFaces();
            break;
        case Qt::Key_Mode_switch:
            m_symbols = !m_symbols;
            buildLayout();
            break;
        case Qt::Key_Backspace: emitKey(Qt::Key_Backspace, {}); break;
        case Qt::Key_Return: emitKey(Qt::Key_Return, {}); break;
        case Qt::Key_Tab: emitKey(Qt::Key_Tab, {}); break;
        case Qt::Key_Space: emitKey(Qt::Key_Space, QStringLiteral(" ")); break;
        case Qt::Key_Left: emitKey(Qt::Key_Left, {}); break;
        case Qt::Key_Up: emitKey(Qt::Key_Up, {}); break;
        case Qt::Key_Down: emitKey(Qt::Key_Down, {}); break;
        case Qt::Key_Right: emitKey(Qt::Key_Right, {}); break;
        case OSM_KEY_HIDE: hideByUser(); break;
        case OSM_KEY_CUSTOMISE: if (m_onCustomise) m_onCustomise(); break;
        default: break;
        }
    }
    static void restyle(QWidget *widget) {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
    }
    void refreshFaces() {
        for (auto &pair : m_characterKeys) pair.first->setText(face(pair.second));
        // Switching keyboard modes recreates modifier keys; keep them illuminated.
        for (auto *button : findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (button->text() == QStringLiteral("⇧") ||
                button->text() == QStringLiteral("Caps")) {
                const bool active = button->text() == QStringLiteral("Caps") ? m_caps : m_shift;
                button->setObjectName(active ? QStringLiteral("modKeyOn") : QStringLiteral("modKey"));
                restyle(button);
            }
        }
    }
    void clearGrid() {
        m_characterKeys.clear();
        m_popup->hide();
        QLayoutItem *item;
        while ((item = m_grid->takeAt(0)) != nullptr) {
            if (item->widget()) { item->widget()->hide(); item->widget()->deleteLater(); }
            delete item;
        }
    }
    void buildLayout() {
        clearGrid();
        const auto rows = m_symbols ? symbolRows() : letterRows(m_style.layout);
        // 24 columns = 12 ordinary keys. Enter begins on row 2 and occupies
        // the same final two columns of rows 2 and 3.
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            int column = 0;
            for (const OsmKeyDef &def : rows[row]) {
                m_grid->addWidget(makeKey(def), row, column, def.rowSpan, def.span);
                column += def.span;
            }
        }
        for (int col = 0; col < 24; ++col) m_grid->setColumnStretch(col, 1);
        refreshFaces();
    }
    void applyStyle() {
        QColor bg("#1e1e1e"), key("#383838"), text("#ededed"),
               mod("#292929"), modText("#c4c4c4"), border("#484848");
        if (m_style.theme == QLatin1String("Light")) {
            bg = QColor("#eaeef4"); key = QColor("#ffffff");
            text = QColor("#16202e"); mod = QColor("#cfd8e3");
            modText = QColor("#182437"); border = QColor("#a2b2c7");
        } else if (m_style.theme == QLatin1String("Midnight")) {
            bg = QColor("#0b1220"); key = QColor("#1d2b45");
            text = QColor("#f0f5ff"); mod = QColor("#142039");
            modText = QColor("#b6c8e5"); border = QColor("#324569");
        } else if (m_style.theme == QLatin1String("Custom")) {
            bg = m_style.customBackground; key = m_style.customKeys;
            text = m_style.customText; mod = key.darker(120);
            modText = text; border = key.lighter(130);
        }
        const QString style = QStringLiteral(
            "QWidget#osmKeyboard { background:%1; border:1px solid %6; }"
            "QWidget#osmKeyboard QPushButton { background:%2; color:%3;"
            "border:1px solid %6; border-radius:7px; font-size:%7px; padding:0px; }"
            "QWidget#osmKeyboard QPushButton:pressed { background:%4; color:#ffffff; }"
            "QWidget#osmKeyboard QPushButton#modKey { background:%5; color:%8; font-size:%9px; }"
            "QWidget#osmKeyboard QPushButton#modKeyOn { background:%4; color:#ffffff; font-size:%9px; }"
            "QLabel#osmShiftLegend { background:transparent; color:%8; border:0;"
            "font-size:%11px; padding:0; }"
            "QLabel#osmKeyboardPopup { background:%4; color:#ffffff; border:2px solid %6;"
            "border-radius:9px; font-size:%10px; font-weight:700; }")
            .arg(bg.name()).arg(key.name()).arg(text.name()).arg(m_style.accent.name())
            .arg(mod.name()).arg(border.name()).arg(m_style.fontSize)
            .arg(modText.name()).arg(qMax(12, m_style.fontSize-2))
             .arg(m_style.fontSize+10).arg(qMax(9, m_style.fontSize-6));
        setStyleSheet(style);
    }

    // These presets change the visible virtual keyboard. They NEVER call
    // setxkbmap or overwrite the user's physical keyboard configuration.
    // Unicode letters absent from the current XKB keymap require an input-method
    // fallback in the OS-wide X11 backend (not all applications support it).
    static std::vector<std::vector<OsmKeyDef>> letterRows(const QString &locale) {
        auto k = [](const QString &lo, const QString &up) {
            return OsmKeyDef{lo, up, 2, 0};
        };
        auto s = [](const QString &face, int key, int span = 2, int rows = 1) {
            return OsmKeyDef{face, {}, span, key, rows};
        };
        using Pair = std::pair<QString, QString>;
        // Region-specific number row pairs mirror the standard XKB level-1/
        // Shift level-2 key legends (not generic uppercase transformations).
        // This keeps Shift+7 as '&' for UK/US, '/' for German/Spanish/etc,
        // and '7' for French AZERTY.
        std::vector<Pair> numbers{
            {"1", "!"}, {"2", "\""}, {"3", "£"}, {"4", "$"}, {"5", "%"},
            {"6", "^"}, {"7", "&"}, {"8", "*"}, {"9", "("}, {"0", ")"}
        };
        Pair numberExtra{"-", "_"};  // Replaces the former ` / ¬ key.
        Pair topExtra{"[", "{"};
        Pair homeExtra{"'", "@"};
        std::vector<Pair> punctuation{{",", "<"}, {".", ">"}, {"/", "?"}};
        QString top = QStringLiteral("qwertyuiop");
        QString home = QStringLiteral("asdfghjkl");
        QString lower = QStringLiteral("zxcvbnm");

        if (locale == QLatin1String("us") || locale == QLatin1String("intl")) {
            numbers[1].second = "@";
            numbers[2].second = "#";
            homeExtra = {"'", "\""};
            if (locale == QLatin1String("intl")) {
                topExtra = {"é", "É"};
                homeExtra = {"ñ", "Ñ"};
            }
        } else if (locale == QLatin1String("fr")) {
            numbers = {{"&", "1"}, {"é", "2"}, {"\"", "3"}, {"'", "4"},
                       {"(", "5"}, {"-", "6"}, {"è", "7"}, {"_", "8"},
                       {"ç", "9"}, {"à", "0"}};
            top = QStringLiteral("azertyuiop");
            home = QStringLiteral("qsdfghjkl");
            lower = QStringLiteral("wxcvbnm");
            topExtra = {"^", "¨"};
            homeExtra = {"m", "M"};
            punctuation = {{",", "?"}, {";", "."}, {":", "/"}};
        } else if (locale == QLatin1String("de")) {
            numbers = {{"1", "!"}, {"2", "\""}, {"3", "§"}, {"4", "$"},
                       {"5", "%"}, {"6", "&"}, {"7", "/"}, {"8", "("},
                       {"9", ")"}, {"0", "="}};
            top = QStringLiteral("qwertzuiop");
            lower = QStringLiteral("yxcvbnm");
            topExtra = {"ü", "Ü"};
            homeExtra = {"ä", "Ä"};
            punctuation = {{",", ";"}, {".", ":"}, {"-", "_"}};
        } else if (locale == QLatin1String("es")) {
            numbers = {{"1", "!"}, {"2", "\""}, {"3", "·"}, {"4", "$"},
                       {"5", "%"}, {"6", "&"}, {"7", "/"}, {"8", "("},
                       {"9", ")"}, {"0", "="}};
            topExtra = {"´", "¨"};
            homeExtra = {"ñ", "Ñ"};
            punctuation = {{",", ";"}, {".", ":"}, {"-", "_"}};
        } else if (locale == QLatin1String("it")) {
            numbers = {{"1", "!"}, {"2", "\""}, {"3", "£"}, {"4", "$"},
                       {"5", "%"}, {"6", "&"}, {"7", "/"}, {"8", "("},
                       {"9", ")"}, {"0", "="}};
            topExtra = {"è", "é"};
            homeExtra = {"ò", "ç"};
            punctuation = {{",", ";"}, {".", ":"}, {"-", "_"}};
        } else if (locale == QLatin1String("pt")) {
            numbers = {{"1", "!"}, {"2", "\""}, {"3", "#"}, {"4", "$"},
                       {"5", "%"}, {"6", "&"}, {"7", "/"}, {"8", "("},
                       {"9", ")"}, {"0", "="}};
            topExtra = {"ã", "Ã"};
            homeExtra = {"ç", "Ç"};
            punctuation = {{",", ";"}, {".", ":"}, {"-", "_"}};
        } else if (locale == QLatin1String("se")) {
            numbers = {{"1", "!"}, {"2", "\""}, {"3", "#"}, {"4", "¤"},
                       {"5", "%"}, {"6", "&"}, {"7", "/"}, {"8", "("},
                       {"9", ")"}, {"0", "="}};
            topExtra = {"å", "Å"};
            homeExtra = {"ö", "Ö"};
            punctuation = {{",", ";"}, {".", ":"}, {"-", "_"}};
        }

        std::vector<std::vector<OsmKeyDef>> rows(5);
        for (const auto &pair : numbers) rows[0].push_back(k(pair.first, pair.second));
        rows[0].push_back(k(numberExtra.first, numberExtra.second));
        rows[0].push_back(s(QStringLiteral("⌫"), Qt::Key_Backspace));

        rows[1].push_back(s(QStringLiteral("Tab"), Qt::Key_Tab));
        for (const QChar letter : top) {
            const QString c(letter);
            rows[1].push_back(k(c, c.toUpper()));
        }
        // No '[' key in the alphabetic UK/US row. Other regions retain
        // their useful national character key here.
        if (locale != QLatin1String("uk") && locale != QLatin1String("us"))
            rows[1].push_back(k(topExtra.first, topExtra.second));

        rows[2].push_back(s(QStringLiteral("Caps"), Qt::Key_CapsLock));
        for (const QChar letter : home) {
            const QString c(letter);
            rows[2].push_back(k(c, c.toUpper()));
        }
        rows[2].push_back(k(homeExtra.first, homeExtra.second));
        rows[2].push_back(s(QStringLiteral("↵"), Qt::Key_Return, 2, 2));

        rows[3].push_back(s(QStringLiteral("⇧"), Qt::Key_Shift));
        for (const QChar letter : lower) {
            const QString c(letter);
            rows[3].push_back(k(c, c.toUpper()));
        }
        // Former '/' key: Up arrow now sits at the right of the lower letters.
        // Keep the first two region-specific punctuation keys.
        for (size_t i = 0; i < punctuation.size() && i < 2; ++i)
            rows[3].push_back(k(punctuation[i].first, punctuation[i].second));
        rows[3].push_back(s(QStringLiteral("↑"), Qt::Key_Up));

        // Hide first, then ?123; slash takes the former Left-arrow slot.
        // Left moves into the former Up-arrow slot on the bottom row.
        rows[4] = {s(QStringLiteral("Hide"), OSM_KEY_HIDE),
                   s(QStringLiteral("?123"), Qt::Key_Mode_switch),
                   s(QStringLiteral("⚙"), OSM_KEY_CUSTOMISE),
                   s(QStringLiteral("space"), Qt::Key_Space, 10),
                   k("/", "?"),
                   s(QStringLiteral("←"), Qt::Key_Left),
                   s(QStringLiteral("↓"), Qt::Key_Down),
                   s(QStringLiteral("→"), Qt::Key_Right)};
        return rows;
    }

    static std::vector<std::vector<OsmKeyDef>> symbolRows() {
        auto k = [](const QString &lo, const QString &up) {
            return OsmKeyDef{lo, up, 2, 0};
        };
        auto s = [](const QString &face, int key, int span = 2, int rows = 1) {
            return OsmKeyDef{face, {}, span, key, rows};
        };
        return {
            {k("1","1"), k("2","2"), k("3","3"), k("4","4"), k("5","5"),
             k("6","6"), k("7","7"), k("8","8"), k("9","9"), k("0","0"),
             k("=","+"), s(QStringLiteral("⌫"), Qt::Key_Backspace)},
            {s(QStringLiteral("Tab"), Qt::Key_Tab), k("!","!"), k("@","@"), k("#","#"),
             k("$","$"), k("%","%"), k("^","^"), k("&","&"),
             k("*","*"), k("(","("), k(")",")"), k("[","{")},
            {s(QStringLiteral("Caps"), Qt::Key_CapsLock), k("-","-"), k("_","_"),
             k("=","="), k("+","+"), k("[","["), k("]","]"), k("{","{"),
             k("}","}"), k("\\","\\"), k("'","\""),
             s(QStringLiteral("↵"), Qt::Key_Return, 2, 2)},
            {s(QStringLiteral("⇧"), Qt::Key_Shift), k(";",";"), k(":",":"),
             k("'","'"), k("\"","\""), k(",",","), k(".","."),
             s(QStringLiteral("↑"), Qt::Key_Up), k("?","?"), k("<","<"), k(">",">")},
            {s(QStringLiteral("Hide"), OSM_KEY_HIDE),
             s(QStringLiteral("abc"), Qt::Key_Mode_switch),
             s(QStringLiteral("⚙"), OSM_KEY_CUSTOMISE),
             s(QStringLiteral("space"), Qt::Key_Space, 10),
             k("/", "?"),
             s(QStringLiteral("←"), Qt::Key_Left),
             s(QStringLiteral("↓"), Qt::Key_Down),
             s(QStringLiteral("→"), Qt::Key_Right)}
        };
    }

    QGridLayout *m_grid = nullptr;
    QLabel *m_popup = nullptr;
    QPointer<QLineEdit> m_lastLineEdit;
    std::vector<std::pair<QPushButton *, OsmKeyDef>> m_characterKeys;
    OsmKeyboardSettings m_style;
    int m_popupSerial = 0;
    bool m_shift = false;
    bool m_caps = false;
    bool m_symbols = false;
    bool m_autoShow = false;
    bool m_embeddedConnected = false;
    bool m_userHidden = false;
    std::function<void(int, const QString &)> m_keyHandler;
    std::function<void()> m_onCustomise;
    std::function<void(bool)> m_onVisibilityChanged;
};

#endif // OSM_KEYBOARD_H
