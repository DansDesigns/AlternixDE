#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QStackedWidget>
#include <QProcess>
#include <QSettings>
#include <QDir>
#include <QComboBox>
#include <QFile>
#include <QApplication>
#include <QFont>
#include <QScreen>
#include <QGuiApplication>

// -----------------------------------------------------
// Config path (shared with other settings modules)
// -----------------------------------------------------
static QString uiCfgPath()
{
    return QDir::homePath() + "/.config/Alternix/osm-settings.conf";
}

// -----------------------------------------------------
// Button styles
// -----------------------------------------------------
static QString uiBtnStyle(const QString &txtColor = "white")
{
    return QString(
        "QPushButton {"
        " background:#444444;"
        " color:%1;"
        " border:1px solid #222222;"
        " border-radius:16px;"
        " font-size:22px;"
        " font-weight:bold;"
        " padding:6px 16px;"
        "}"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
    ).arg(txtColor);
}

static QString uiBtnBright()
{
    return
        "QPushButton {"
        " background:#33aaff;"
        " color:white;"
        " border:0px;"
        " border-radius:16px;"
        " font-size:22px;"
        " font-weight:bold;"
        " padding:6px 18px;"
        "}"
        "QPushButton:hover { background:#55bbff; }"
        "QPushButton:pressed { background:#2299dd; }";
}

static QPushButton* makeBtn(const QString &txt, const QString &color = "white")
{
    QPushButton *b = new QPushButton(txt);
    b->setStyleSheet(uiBtnStyle(color));
    b->setMinimumSize(140, 54);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

// -----------------------------------------------------
// Slider style (matches sound.cpp)
// -----------------------------------------------------
static QString sliderStyle()
{
    return
        "QSlider::groove:horizontal {"
        "   background:#666666;"
        "   height:14px;"
        "   border-radius:7px;"
        "   margin:0px;"
        "}"
        "QSlider::sub-page:horizontal {"
        "   background:#4aa3ff;"
        "   border-radius:7px;"
        "}"
        "QSlider::handle:horizontal {"
        "   background:white;"
        "   border-radius:16px;"
        "   width:32px;"
        "   height:32px;"
        "   margin:-9px 0;"
        "}"
        "QSlider::handle:horizontal:pressed {"
        "   background:#e0e0e0;"
        "}";
}

// -----------------------------------------------------
// Combo box style (for the sound pickers)
// -----------------------------------------------------
static QString comboStyle()
{
    return
        "QComboBox {"
        "   background:#444444;"
        "   color:white;"
        "   border:1px solid #222222;"
        "   border-radius:16px;"
        "   font-size:24px;"
        "   font-family:'DejaVu Sans';"
        "   padding:10px 20px;"
        "}"
        "QComboBox::drop-down { border:none; width:50px; }"
        "QComboBox::down-arrow {"
        "   image:none;"
        "   border-left:12px solid transparent;"
        "   border-right:12px solid transparent;"
        "   border-top:14px solid #bbbbbb;"
        "   margin-right:16px;"
        "}"
        "QComboBox QFrame {"            /* popup container frame */
        "   border:none;"
        "   background:#4a4a4a;"
        "}"
        "QComboBox QAbstractItemView {"
        "   background:#4a4a4a;"          /* lighter than the cards */
        "   color:white;"
        "   font-size:24px;"
        "   font-family:'DejaVu Sans';"
        "   border:none;"                 /* no white frame */
        "   selection-background-color:#5f5f5f;"
        "   outline:none;"
        "   padding:6px;"
        "}";
}

// Same folder osm-status and sound.cpp read sound files from.
static QString soundsDir()
{
    return QDir::homePath() + "/.config/Alternix/sounds";
}

// -----------------------------------------------------
// UIPage
// -----------------------------------------------------
class UIPage : public QWidget
{
public:
    explicit UIPage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), m_stack(stack)
    {
        setStyleSheet("background:#282828; color:white; font-family:Sans;");

        m_settings = new QSettings(uiCfgPath(), QSettings::IniFormat, this);

        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(40, 40, 40, 40);
        root->setSpacing(10);

        QLabel *title = new QLabel("UI");
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:42px; font-weight:bold;");
        root->addWidget(title);

        // Scroll area
        QScrollArea *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

        QWidget *wrap = new QWidget(scroll);
        QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
        wrapLay->setSpacing(10);
        wrapLay->setContentsMargins(0, 0, 0, 0);

        // Outer card
        QFrame *outer = new QFrame(wrap);
        outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
        QVBoxLayout *outerLay = new QVBoxLayout(outer);
        outerLay->setContentsMargins(50, 30, 50, 30);
        outerLay->setSpacing(30);

        // ── Settings font size ──────────────────────────
        outerLay->addWidget(makeSettingsFontCard());

        // ── App launcher font size ──────────────────────
        outerLay->addWidget(makeLauncherFontCard());

        // ── Wallpaper shortcut ──────────────────────────
        outerLay->addWidget(makeWallpaperCard());

        // ── Login sound (was "Boot Sound", moved from Sound page) ──
        outerLay->addWidget(makeLoginSoundCard());

        // ── Notification sound ──────────────────────────
        outerLay->addWidget(makeNotificationSoundCard());

        wrapLay->addWidget(outer);
        wrapLay->addStretch();

        scroll->setWidget(wrap);
        root->addWidget(scroll);

        // Back button
        QPushButton *back = makeBtn("❮");
        back->setFixedSize(140, 60);
        connect(back, &QPushButton::clicked, this, [this]() {
            if (m_stack)
                m_stack->setCurrentIndex(0);
        });
        root->addWidget(back, 0, Qt::AlignCenter);
    }

private:
    QStackedWidget *m_stack   = nullptr;
    QSettings      *m_settings = nullptr;

    QLabel  *m_settingsFontPreview  = nullptr;
    QLabel  *m_launcherFontPreview  = nullptr;
    QSlider *m_settingsFontSlider   = nullptr;
    QSlider *m_launcherFontSlider   = nullptr;

    // Font size range: 14pt–36pt, stored as integer point size
    static constexpr int FONT_MIN = 14;
    static constexpr int FONT_MAX = 36;

    // ── Settings font card ──────────────────────────────
    QWidget *makeSettingsFontCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30, 24, 30, 24);
        v->setSpacing(16);

        // Header row
        QHBoxLayout *hdr = new QHBoxLayout();
        QLabel *lbl = new QLabel("Settings Font Size");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        hdr->addWidget(lbl);
        hdr->addStretch();

        m_settingsFontPreview = new QLabel();
        m_settingsFontPreview->setStyleSheet("font-size:22px; color:#aaaaaa;");
        hdr->addWidget(m_settingsFontPreview);
        v->addLayout(hdr);

        // Slider
        m_settingsFontSlider = new QSlider(Qt::Horizontal, card);
        m_settingsFontSlider->setRange(FONT_MIN, FONT_MAX);
        m_settingsFontSlider->setStyleSheet(sliderStyle());
        m_settingsFontSlider->setFixedHeight(40);

        int saved = m_settings->value("UI/SettingsFontSize", defaultSettingsFont()).toInt();
        saved = qBound(FONT_MIN, saved, FONT_MAX);
        m_settingsFontSlider->setValue(saved);
        updateSettingsFontPreview(saved);

        v->addWidget(m_settingsFontSlider);

        // Endpoint labels
        QHBoxLayout *endRow = new QHBoxLayout();
        QLabel *minLbl = new QLabel(QString("%1pt").arg(FONT_MIN));
        minLbl->setStyleSheet("font-size:18px; color:#888;");
        QLabel *maxLbl = new QLabel(QString("%1pt").arg(FONT_MAX));
        maxLbl->setStyleSheet("font-size:18px; color:#888;");
        endRow->addWidget(minLbl);
        endRow->addStretch();
        endRow->addWidget(maxLbl);
        v->addLayout(endRow);

        // Reset button
        QPushButton *reset = new QPushButton("Reset");
        reset->setStyleSheet(uiBtnStyle());
        reset->setFixedSize(120, 44);
        QHBoxLayout *btnRow = new QHBoxLayout();
        btnRow->addStretch();
        btnRow->addWidget(reset);
        v->addLayout(btnRow);

        // Connections
        connect(m_settingsFontSlider, &QSlider::valueChanged, this, [this](int v) {
            m_settings->setValue("UI/SettingsFontSize", v);
            m_settings->sync();
            updateSettingsFontPreview(v);
            applySettingsFont(v);
        });

        connect(reset, &QPushButton::clicked, this, [this]() {
            int def = defaultSettingsFont();
            m_settingsFontSlider->setValue(def);
        });

        return card;
    }

    // ── Launcher font card ──────────────────────────────
    QWidget *makeLauncherFontCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30, 24, 30, 24);
        v->setSpacing(16);

        QHBoxLayout *hdr = new QHBoxLayout();
        QLabel *lbl = new QLabel("Launcher Font Size");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        hdr->addWidget(lbl);
        hdr->addStretch();

        m_launcherFontPreview = new QLabel();
        m_launcherFontPreview->setStyleSheet("font-size:22px; color:#aaaaaa;");
        hdr->addWidget(m_launcherFontPreview);
        v->addLayout(hdr);

        m_launcherFontSlider = new QSlider(Qt::Horizontal, card);
        m_launcherFontSlider->setRange(FONT_MIN, FONT_MAX);
        m_launcherFontSlider->setStyleSheet(sliderStyle());
        m_launcherFontSlider->setFixedHeight(40);

        int saved = m_settings->value("UI/LauncherFontSize", 15).toInt();
        saved = qBound(FONT_MIN, saved, FONT_MAX);
        m_launcherFontSlider->setValue(saved);
        updateLauncherFontPreview(saved);

        v->addWidget(m_launcherFontSlider);

        QHBoxLayout *endRow = new QHBoxLayout();
        QLabel *minLbl = new QLabel(QString("%1pt").arg(FONT_MIN));
        minLbl->setStyleSheet("font-size:18px; color:#888;");
        QLabel *maxLbl = new QLabel(QString("%1pt").arg(FONT_MAX));
        maxLbl->setStyleSheet("font-size:18px; color:#888;");
        endRow->addWidget(minLbl);
        endRow->addStretch();
        endRow->addWidget(maxLbl);
        v->addLayout(endRow);

        // Info note
        QLabel *note = new QLabel("Takes effect next time the launcher is opened.");
        note->setStyleSheet("font-size:18px; color:#888888;");
        note->setWordWrap(true);
        v->addWidget(note);

        QPushButton *reset = new QPushButton("Reset");
        reset->setStyleSheet(uiBtnStyle());
        reset->setFixedSize(120, 44);
        QHBoxLayout *btnRow = new QHBoxLayout();
        btnRow->addStretch();
        btnRow->addWidget(reset);
        v->addLayout(btnRow);

        connect(m_launcherFontSlider, &QSlider::valueChanged, this, [this](int v) {
            m_settings->setValue("UI/LauncherFontSize", v);
            m_settings->sync();
            updateLauncherFontPreview(v);
            // Written to config; osm-launcher reads it on next launch
        });

        connect(reset, &QPushButton::clicked, this, [this]() {
            m_launcherFontSlider->setValue(15);
        });

        return card;
    }

    // ── Generic sound picker card (used for Login + Notification) ──
    // settingsKey: e.g. "Sound/BootSound" or "Sound/NotificationSound"
    // defaultPrefix: filename prefix osm-status falls back to when no
    //                explicit selection is made, e.g. "boot" or "notify"
    QWidget *makeSoundPickerCard(const QString &title,
                                  const QString &settingsKey,
                                  const QString &defaultPrefix,
                                  const QString &note)
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *lay = new QVBoxLayout(card);
        lay->setContentsMargins(30, 24, 30, 24);
        lay->setSpacing(16);

        QLabel *lbl = new QLabel(title, card);
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lbl->setAlignment(Qt::AlignCenter);
        lay->addWidget(lbl);

        QHBoxLayout *row = new QHBoxLayout();
        row->setSpacing(16);

        QComboBox *combo = new QComboBox(card);
        combo->setStyleSheet(comboStyle());
        combo->setFixedHeight(60);
        combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        // populate from the sounds folder
        QDir d(soundsDir());
        if (!d.exists()) d.mkpath(".");
        QStringList files = d.entryList(
            {"*.wav", "*.ogg", "*.flac", "*.mp3"},
            QDir::Files, QDir::Name);

        combo->addItem(QString("Default (%1.*)").arg(defaultPrefix), QString());
        for (const QString &f : files)
            combo->addItem(f, f);

        // restore saved selection
        QString saved = m_settings->value(settingsKey).toString();
        if (!saved.isEmpty()) {
            int idx = combo->findData(saved);
            if (idx >= 0) combo->setCurrentIndex(idx);
        }

        // ▶ preview button (same playback chain as osm-status)
        QPushButton *play = new QPushButton("▶", card);
        play->setFixedSize(60, 60);
        play->setStyleSheet(
            "QPushButton { background:#3a3a3a; color:#7CFC00;"
            " border:1px solid #222222; border-radius:16px;"
            " font-size:26px; font-weight:bold; }"
            "QPushButton:hover { background:#4a4a4a; }"
            "QPushButton:pressed { background:#2a2a2a; }");

        row->addWidget(combo, 1);
        row->addWidget(play);
        lay->addLayout(row);

        QLabel *noteLbl = new QLabel(note, card);
        noteLbl->setStyleSheet("font-size:18px; color:#aaaaaa;");
        noteLbl->setWordWrap(true);
        lay->addWidget(noteLbl);

        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, combo, settingsKey](int) {
                m_settings->setValue(settingsKey, combo->currentData().toString());
                m_settings->sync();
            });

        connect(play, &QPushButton::clicked, this, [combo, defaultPrefix]() {
            QString f = combo->currentData().toString();
            QString path;
            if (f.isEmpty()) {
                // default: first <prefix>.* in the folder
                QDir d(soundsDir());
                QStringList m = d.entryList(
                    {defaultPrefix + ".wav", defaultPrefix + ".ogg",
                     defaultPrefix + ".flac", defaultPrefix + ".mp3"},
                    QDir::Files, QDir::Name);
                if (!m.isEmpty()) path = d.absoluteFilePath(m.first());
            } else {
                path = soundsDir() + "/" + f;
            }
            if (path.isEmpty() || !QFile::exists(path)) return;

            QString q = "'" + path.replace("'", "'\\''") + "'";
            QString cmd = path.endsWith(".mp3", Qt::CaseInsensitive)
                ? QString("mpg123 -q %1 2>/dev/null || "
                          "cvlc --play-and-exit --intf dummy %1 2>/dev/null").arg(q)
                : QString("paplay %1 2>/dev/null || aplay -q %1 2>/dev/null || "
                          "cvlc --play-and-exit --intf dummy %1 2>/dev/null").arg(q);
            QProcess::startDetached("sh", QStringList() << "-c" << cmd);
        });

        return card;
    }

    // ── Login sound (was "Boot Sound" on the Sound page) ────────
    // Settings key stays Sound/BootSound — osm-status's resolveBootFile()
    // already reads that key when it plays the once-per-boot sound
    // after unlock, so keeping the key name avoids touching that path.
    QWidget *makeLoginSoundCard()
    {
        return makeSoundPickerCard(
            "Login Sound", "Sound/BootSound", "boot",
            "Played once per login, right after unlock. Files are read "
            "from ~/.config/Alternix/sounds/");
    }

    // ── Notification sound ───────────────────────────────────────
    // Settings key Sound/NotificationSound is read by osm-status's
    // playNotificationSound() as the default when a notification doesn't
    // specify its own "sound:" line.
    QWidget *makeNotificationSoundCard()
    {
        return makeSoundPickerCard(
            "Notification Sound", "Sound/NotificationSound", "notify",
            "Used for notifications that don't set their own sound. "
            "Files are read from ~/.config/Alternix/sounds/");
    }

    // ── Wallpaper shortcut card ─────────────────────────
    QWidget *makeWallpaperCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Wallpaper");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);

        lay->addStretch();

        QPushButton *btn = new QPushButton("Change");
        btn->setStyleSheet(uiBtnBright());
        btn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        lay->addWidget(btn);

        connect(btn, &QPushButton::clicked, this, []() {
            QProcess::startDetached("osm-paper", QStringList());
        });

        return card;
    }

    // ── Helpers ─────────────────────────────────────────

    // Pick a sensible default based on screen width
    int defaultSettingsFont() const
    {
        QScreen *s = QGuiApplication::primaryScreen();
        if (!s) return 22;
        return (s->size().width() <= 720) ? 18 : 22;
    }

    void updateSettingsFontPreview(int pt)
    {
        if (m_settingsFontPreview)
            m_settingsFontPreview->setText(QString("%1pt").arg(pt));
    }

    void updateLauncherFontPreview(int pt)
    {
        if (m_launcherFontPreview)
            m_launcherFontPreview->setText(QString("%1pt").arg(pt));
    }

    // Apply settings font live to the running QApplication
    void applySettingsFont(int pt)
    {
        QFont f = QApplication::font();
        f.setPointSize(pt);
        QApplication::setFont(f);
    }
};

// -----------------------------------------------------
// Factory for osm-settings
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new UIPage(stack);
}
