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
