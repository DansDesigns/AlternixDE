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
#include <QFileDialog>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QImage>
#include <QPixmap>
#include <QMap>
#include <QPair>
#include <algorithm>

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

// Boot sounds live in sounds/boot/, notification (+ alarm) sounds in
// sounds/notifications/ — osm-status.cpp resolves the same way.
static QString soundsSubDir(const QString &sub)
{
    return soundsDir() + "/" + sub;
}

// -----------------------------------------------------
// Cursor themes
// -----------------------------------------------------
// The same folders libXcursor itself searches, in the same order, so
// what the picker lists is exactly what the system can actually use.
// ~/.icons comes first: a user-installed theme shadows a system one
// of the same name, which is also how libXcursor resolves it.
static QStringList cursorSearchDirs()
{
    return QStringList()
        << QDir::homePath() + "/.icons"
        << "/usr/local/share/icons"
        << "/usr/share/icons";
}

// A folder is a cursor theme if it holds a non-empty cursors/ subfolder.
// index.theme alone is not enough — plain icon themes have one too.
static bool isCursorTheme(const QString &dir)
{
    QDir c(dir + "/cursors");
    return c.exists() &&
           !c.entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot).isEmpty();
}

// Absolute path of an installed theme, or empty if it isn't installed.
static QString cursorThemeDir(const QString &name)
{
    if (name.isEmpty()) return QString();
    for (const QString &base : cursorSearchDirs()) {
        QString full = base + "/" + name;
        if (isCursorTheme(full)) return full;
    }
    return QString();
}

// Human-readable name out of index.theme ("Oreo Blue Cursors"), falling
// back to the folder name. 19 folders called oreo_*_cursors are useless
// to pick between otherwise.
static QString cursorThemeLabel(const QString &dir)
{
    QFile f(dir + "/index.theme");
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!f.atEnd()) {
            QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.startsWith("Name=", Qt::CaseInsensitive))
                return line.mid(5).trimmed();
        }
    }
    return QFileInfo(dir).fileName();
}

static inline quint32 xcursorU32(const QByteArray &b, int off)
{
    if (off < 0 || off + 4 > b.size()) return 0;
    const uchar *p = reinterpret_cast<const uchar *>(b.constData()) + off;
    return quint32(p[0])        | (quint32(p[1]) << 8)
         | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

// Minimal Xcursor reader — just enough to pull the largest still frame
// out of a theme's left_ptr for the preview swatch.
//
// Xcursor layout: "Xcur" magic, then a table of chunks; image chunks
// (type 0xfffd0002) carry a 36-byte header followed by raw
// premultiplied ARGB32 little-endian pixels — which is bit-for-bit
// QImage::Format_ARGB32_Premultiplied, so no conversion is needed.
static QPixmap cursorThemePreview(const QString &themeDir, int px)
{
    static const char *candidates[] = { "left_ptr", "default", "arrow", nullptr };

    QString path;
    for (int i = 0; candidates[i]; ++i) {
        QString p = themeDir + "/cursors/" + QString::fromLatin1(candidates[i]);
        if (QFile::exists(p)) { path = p; break; }
    }
    if (path.isEmpty()) return QPixmap();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QPixmap();
    QByteArray b = f.read(8 * 1024 * 1024);     // cursors are ~50 KB; cap anyway
    f.close();

    if (b.size() < 16 || xcursorU32(b, 0) != 0x72756358u) return QPixmap();  // "Xcur"
    quint32 ntoc = xcursorU32(b, 12);
    if (ntoc == 0 || ntoc > 4096) return QPixmap();

    // Pick the biggest nominal size available — sharpest swatch.
    quint32 bestPos = 0, bestSize = 0;
    for (quint32 i = 0; i < ntoc; ++i) {
        int e = 16 + int(i) * 12;
        if (e + 12 > b.size()) return QPixmap();
        if (xcursorU32(b, e) != 0xfffd0002u) continue;      // not an image chunk
        quint32 nominal = xcursorU32(b, e + 4);
        if (nominal >= bestSize) { bestSize = nominal; bestPos = xcursorU32(b, e + 8); }
    }
    if (bestPos == 0 || int(bestPos) + 36 > b.size()) return QPixmap();

    quint32 w = xcursorU32(b, int(bestPos) + 16);
    quint32 h = xcursorU32(b, int(bestPos) + 20);
    if (w == 0 || h == 0 || w > 512 || h > 512) return QPixmap();

    int need = int(w) * int(h) * 4;
    // mid() hands back a fresh, correctly aligned buffer — QImage needs
    // 32-bit alignment and the chunk offset gives no such guarantee.
    QByteArray pix = b.mid(int(bestPos) + 36, need);
    if (pix.size() != need) return QPixmap();

    QImage img(reinterpret_cast<const uchar *>(pix.constData()),
               int(w), int(h), int(w) * 4,
               QImage::Format_ARGB32_Premultiplied);
    if (img.isNull()) return QPixmap();

    // copy() before pix goes out of scope — QImage does not own the data.
    return QPixmap::fromImage(img.copy())
               .scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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

        // ── Mouse cursor ────────────────────────────────
        outerLay->addWidget(makeCursorCard());

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

    QComboBox *m_cursorCombo    = nullptr;
    QLabel    *m_cursorPreview  = nullptr;
    QLabel    *m_cursorStatus   = nullptr;
    bool       m_cursorLoading  = false;   // suppress apply while populating

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
    // subDir: "boot" or "notifications" — matches osm-status.cpp's
    //         resolution of the same settingsKey
    // defaultPrefix: filename prefix osm-status falls back to when no
    //                explicit selection is made, e.g. "boot" or "notify"
    QWidget *makeSoundPickerCard(const QString &title,
                                  const QString &settingsKey,
                                  const QString &subDir,
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

        // populate from this card's subfolder (boot/ or notifications/)
        QDir d(soundsSubDir(subDir));
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

        connect(play, &QPushButton::clicked, this, [combo, subDir, defaultPrefix]() {
            QString f = combo->currentData().toString();
            QString path;
            if (f.isEmpty()) {
                // default: first <prefix>.* in this card's subfolder
                QDir d(soundsSubDir(subDir));
                QStringList m = d.entryList(
                    {defaultPrefix + ".wav", defaultPrefix + ".ogg",
                     defaultPrefix + ".flac", defaultPrefix + ".mp3"},
                    QDir::Files, QDir::Name);
                if (!m.isEmpty()) path = d.absoluteFilePath(m.first());
            } else {
                path = soundsSubDir(subDir) + "/" + f;
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
            "Login Sound", "Sound/BootSound", "boot", "boot",
            "Played once per login, right after unlock. Files are read "
            "from ~/.config/Alternix/sounds/boot/");
    }

    // ── Notification sound ───────────────────────────────────────
    // Settings key Sound/NotificationSound is read by osm-status's
    // playNotificationSound() as the default when a notification doesn't
    // specify its own "sound:" line.
    QWidget *makeNotificationSoundCard()
    {
        return makeSoundPickerCard(
            "Notification Sound", "Sound/NotificationSound", "notifications", "notify",
            "Used for notifications that don't set their own sound. "
            "Files are read from ~/.config/Alternix/sounds/notifications/");
    }

    // ── Mouse cursor card ───────────────────────────────
    // Lists "Default" plus every cursor theme actually installed
    // (the Oreo set the installer unpacks into /usr/share/icons, and
    // anything the user imports into ~/.icons), and can import more
    // from a .tar.*/.zip archive.
    QWidget *makeCursorCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *lay = new QVBoxLayout(card);
        lay->setContentsMargins(30, 24, 30, 24);
        lay->setSpacing(16);

        QLabel *lbl = new QLabel("Mouse Cursor", card);
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lbl->setAlignment(Qt::AlignCenter);
        lay->addWidget(lbl);

        QHBoxLayout *row = new QHBoxLayout();
        row->setSpacing(16);

        m_cursorPreview = new QLabel(card);
        m_cursorPreview->setFixedSize(60, 60);
        m_cursorPreview->setAlignment(Qt::AlignCenter);
        m_cursorPreview->setStyleSheet(
            "background:#3a3a3a; color:#888888; font-size:22px;"
            " border:1px solid #222222; border-radius:16px;");

        m_cursorCombo = new QComboBox(card);
        m_cursorCombo->setStyleSheet(comboStyle());
        m_cursorCombo->setFixedHeight(60);
        m_cursorCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        row->addWidget(m_cursorPreview);
        row->addWidget(m_cursorCombo, 1);
        lay->addLayout(row);

        populateCursorCombo();

        QPushButton *importBtn = new QPushButton("Install from Archive…", card);
        importBtn->setStyleSheet(uiBtnBright());
        importBtn->setMinimumHeight(54);
        lay->addWidget(importBtn);

        QLabel *note = new QLabel(
            "Themes are read from ~/.icons and /usr/share/icons. Imported "
            "archives are installed to ~/.icons. Apps that are already open "
            "keep their old cursor until they are restarted.", card);
        note->setStyleSheet("font-size:18px; color:#aaaaaa;");
        note->setWordWrap(true);
        lay->addWidget(note);

        m_cursorStatus = new QLabel(QString(), card);
        m_cursorStatus->setStyleSheet("font-size:18px; color:#ff6b6b;");
        m_cursorStatus->setWordWrap(true);
        m_cursorStatus->setVisible(false);
        lay->addWidget(m_cursorStatus);

        connect(m_cursorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (m_cursorLoading) return;
                applyCursorTheme(m_cursorCombo->currentData().toString());
            });

        connect(importBtn, &QPushButton::clicked, this, [this]() {
            importCursorArchive();
        });

        return card;
    }

    // Errors stay on screen once shown — a cursor that silently failed
    // to apply is exactly the kind of thing that must not be cleared.
    void cursorError(const QString &msg)
    {
        if (!m_cursorStatus) return;
        QString existing = m_cursorStatus->text();
        m_cursorStatus->setText(existing.isEmpty() ? msg : existing + "\n" + msg);
        m_cursorStatus->setVisible(true);
    }

    void populateCursorCombo()
    {
        if (!m_cursorCombo) return;

        m_cursorLoading = true;
        m_cursorCombo->clear();
        m_cursorCombo->addItem("Default (system cursor)", QString());

        QMap<QString, QString> found;   // folder name -> absolute path
        for (const QString &base : cursorSearchDirs()) {
            QDir d(base);
            if (!d.exists()) continue;
            const QStringList subs =
                d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QString &sub : subs) {
                // "default" is our own redirect stub, not a real theme
                if (sub == "default") continue;
                if (found.contains(sub)) continue;      // earlier dir wins
                QString full = d.absoluteFilePath(sub);
                if (isCursorTheme(full)) found.insert(sub, full);
            }
        }

        // Sort on the display label so the Oreo colours group together
        QList<QPair<QString, QString> > items;   // label, folder name
        for (QMap<QString, QString>::const_iterator it = found.constBegin();
             it != found.constEnd(); ++it)
            items.append(qMakePair(cursorThemeLabel(it.value()), it.key()));

        std::sort(items.begin(), items.end(),
                  [](const QPair<QString, QString> &a,
                     const QPair<QString, QString> &b) {
                      return a.first.localeAwareCompare(b.first) < 0;
                  });

        for (int i = 0; i < items.size(); ++i)
            m_cursorCombo->addItem(items.at(i).first, items.at(i).second);

        QString saved = m_settings->value("UI/CursorTheme").toString();
        if (!saved.isEmpty()) {
            int idx = m_cursorCombo->findData(saved);
            if (idx >= 0) m_cursorCombo->setCurrentIndex(idx);
        }

        m_cursorLoading = false;
        updateCursorPreview();
    }

    void updateCursorPreview()
    {
        if (!m_cursorPreview || !m_cursorCombo) return;

        QString dir = cursorThemeDir(m_cursorCombo->currentData().toString());
        QPixmap pm  = dir.isEmpty() ? QPixmap() : cursorThemePreview(dir, 40);

        if (pm.isNull()) {
            m_cursorPreview->setPixmap(QPixmap());
            m_cursorPreview->setText("—");
        } else {
            m_cursorPreview->setText(QString());
            m_cursorPreview->setPixmap(pm);
        }
    }

    void applyCursorTheme(const QString &name)
    {
        m_settings->setValue("UI/CursorTheme", name);
        m_settings->sync();

        // ~/.icons/default/index.theme is the file libXcursor actually
        // reads. Every toolkit resolves the theme named "default"
        // through it, so this one file covers Qt, GTK and plain X11
        // apps with no environment variable and no session restart.
        QString defDir = QDir::homePath() + "/.icons/default";
        QString idx    = defDir + "/index.theme";

        if (name.isEmpty()) {
            // Back to whatever the system would have used on its own
            QFile::remove(idx);
            QDir().rmdir(defDir);          // only succeeds if now empty
            updateCursorPreview();
            return;
        }

        if (!QDir().mkpath(defDir)) {
            cursorError("Could not create " + defDir + " — cursor not changed.");
            return;
        }

        QFile f(idx);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            cursorError("Could not write " + idx + " — " + f.errorString());
            return;
        }
        f.write(QString("[Icon Theme]\n"
                        "Name=Default\n"
                        "Comment=Alternix cursor selection\n"
                        "Inherits=%1\n").arg(name).toUtf8());
        f.close();

        // Repaint the root window immediately so the change is visible
        // straight away rather than only after the next login.
        QString dir = cursorThemeDir(name);
        if (!dir.isEmpty()) {
            QString ptr = dir + "/cursors/left_ptr";
            if (QFile::exists(ptr)) {
                QString q = "'" + ptr.replace("'", "'\\''") + "'";
                QProcess::startDetached("sh", QStringList() << "-c"
                    << QString("xsetroot -xcf %1 24 2>/dev/null").arg(q));
            }
        }

        updateCursorPreview();
    }

    // ── Import a theme from .tar.gz / .tar.xz / .tar.bz2 / .tar / .zip ──
    // Everything lands in ~/.icons, so this needs no root and no
    // polkit prompt.
    void importCursorArchive()
    {
        QString file = QFileDialog::getOpenFileName(
            this, "Select a cursor theme archive", QDir::homePath(),
            "Cursor themes (*.tar.gz *.tgz *.tar.xz *.tar.bz2 *.tar *.zip)"
            ";;All files (*)");
        if (file.isEmpty()) return;

        QTemporaryDir tmp;
        if (!tmp.isValid()) {
            cursorError("Could not create a temporary folder to unpack into.");
            return;
        }

        QString qf = "'" + QString(file).replace("'", "'\\''") + "'";
        QString qd = "'" + QString(tmp.path()).replace("'", "'\\''") + "'";

        // Unpack into the temp folder first: nothing touches ~/.icons
        // until we have confirmed the archive really holds a cursor theme.
        QString cmd = file.endsWith(".zip", Qt::CaseInsensitive)
            ? QString("unzip -q -o %1 -d %2").arg(qf, qd)
            : QString("tar -xf %1 -C %2").arg(qf, qd);

        QProcess p;
        p.start("sh", QStringList() << "-c" << cmd);
        if (!p.waitForFinished(180000) || p.exitStatus() != QProcess::NormalExit
            || p.exitCode() != 0) {
            QString detail = QString::fromUtf8(p.readAllStandardError()).trimmed();
            cursorError("Could not unpack " + QFileInfo(file).fileName() +
                        (detail.isEmpty() ? QString() : " — " + detail));
            return;
        }

        QStringList themes;
        if (isCursorTheme(tmp.path())) {
            // Archive had cursors/ at the top level with no theme folder
            // around it — name it after the archive.
            themes << tmp.path();
        } else {
            collectCursorThemes(tmp.path(), themes, 0);
        }

        if (themes.isEmpty()) {
            cursorError("No cursor theme in " + QFileInfo(file).fileName() +
                        " — the archive must contain a folder with a "
                        "cursors/ subfolder inside it.");
            return;
        }

        QString dest = QDir::homePath() + "/.icons";
        if (!QDir().mkpath(dest)) {
            cursorError("Could not create " + dest);
            return;
        }

        QString firstInstalled;
        int installed = 0;
        for (int i = 0; i < themes.size(); ++i) {
            QString src  = themes.at(i);
            QString name = (src == tmp.path())
                ? QFileInfo(file).completeBaseName()
                : QFileInfo(src).fileName();

            // Paranoia before an rm -rf: fileName()/completeBaseName()
            // already strip any path, but an empty name here would
            // expand to "rm -rf ~/.icons/".
            if (name.isEmpty() || name == "." || name == "..") {
                cursorError("Skipped a theme with an unusable folder name.");
                continue;
            }

            QString qsrc = "'" + QString(src).replace("'", "'\\''") + "'";
            QString qdst = "'" + QString(dest + "/" + name).replace("'", "'\\''") + "'";

            // cp -a, not a Qt copy loop: cursor themes are largely
            // symlinks (87 of the 148 entries in an Oreo theme) and
            // QFile::copy would dereference every one of them.
            QProcess cp;
            cp.start("sh", QStringList() << "-c"
                << QString("rm -rf %1 && mkdir -p %1 && cp -a %2/. %1/").arg(qdst, qsrc));
            if (cp.waitForFinished(180000) && cp.exitCode() == 0) {
                if (firstInstalled.isEmpty()) firstInstalled = name;
                ++installed;
            } else {
                cursorError("Could not install theme '" + name + "' into " + dest);
            }
        }

        if (installed == 0) return;

        populateCursorCombo();
        int idx = m_cursorCombo->findData(firstInstalled);
        if (idx >= 0) m_cursorCombo->setCurrentIndex(idx);   // applies it
    }

    // Themes usually sit one level down (archive/<theme>/cursors), but
    // some archives nest deeper. Bounded depth so a pathological archive
    // cannot spin here.
    static void collectCursorThemes(const QString &dir, QStringList &out, int depth)
    {
        if (depth > 3) return;
        QDir d(dir);
        const QStringList subs =
            d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &sub : subs) {
            QString full = d.absoluteFilePath(sub);
            if (isCursorTheme(full)) out << full;
            else collectCursorThemes(full, out, depth + 1);
        }
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
