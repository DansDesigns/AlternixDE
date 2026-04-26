#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QStackedWidget>
#include <QProcess>
#include <QMessageBox>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QMap>
#include <QScreen>
#include <QGuiApplication>
#include <QSizePolicy>

// -----------------------------------------------------
// Alternix compact button style (same as Security/Storage)
// -----------------------------------------------------
static QString altBtnStyle(const QString &txtColor)
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

static QPushButton* makeBtn(const QString &txt, const QString &color = "white")
{
    QPushButton *b = new QPushButton(txt);
    b->setStyleSheet(altBtnStyle(color));
    b->setMinimumSize(140, 54);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return b;
}

static QString altBtnBright()
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

// -----------------------------------------------------
// Simple CONFIG helpers (same file as Security)
// -----------------------------------------------------
static QString cfgFile()
{
    return QDir::homePath() + "/.config/Alternix/osm-settings.conf";
}

static QMap<QString,QString> loadCfg()
{
    QMap<QString,QString> map;
    QFile f(cfgFile());
    if (!f.exists()) return map;

    if (f.open(QFile::ReadOnly))
    {
        QTextStream s(&f);
        while (!s.atEnd())
        {
            QString line = s.readLine().trimmed();
            if (line.startsWith("#") || !line.contains("="))
                continue;
            QStringList parts = line.split("=");
            if (parts.size() == 2)
                map[parts[0].trimmed()] = parts[1].trimmed();
        }
    }
    return map;
}

static void saveCfg(const QMap<QString,QString> &map)
{
    QFile f(cfgFile());
    f.open(QFile::WriteOnly | QFile::Truncate);
    QTextStream s(&f);
    for (auto it = map.begin(); it != map.end(); ++it)
        s << it.key() << "=" << it.value() << "\n";
}

// -----------------------------------------------------
// Simple ON/OFF pill toggle (same as Security BoolPillToggle)
// -----------------------------------------------------
class BoolPillToggle : public QPushButton
{
public:
    explicit BoolPillToggle(bool initial = false, QWidget *parent = nullptr)
        : QPushButton(parent), m_state(initial)
    {
        setFixedSize(140, 50);
        updateStyle();
    }

    void toggle()
    {
        m_state = !m_state;
        updateStyle();
    }

    void setState(bool on)
    {
        m_state = on;
        updateStyle();
    }

    bool isOn() const { return m_state; }

private:
    bool m_state;

    void updateStyle()
    {
        if (m_state) {
            setStyleSheet(
                "QPushButton {"
                " background:#2ecc71;"
                " border-radius:25px;"
                " color:white;"
                " font-size:22px;"
                " padding:4px 16px;"
                "}"
            );
            setText("");
        } else {
            setStyleSheet(
                "QPushButton {"
                " background:#666666;"
                " border-radius:25px;"
                " color:white;"
                " font-size:22px;"
                " padding:4px 16px;"
                "}"
            );
            setText("Disabled");
        }
    }
};

// -----------------------------------------------------
// Timeout multi-state pill (6 states in greyscale)
// -----------------------------------------------------
class TimeoutPill : public QPushButton
{
public:
    explicit TimeoutPill(int initial = 0, QWidget *parent = nullptr)
        : QPushButton(parent), m_state(initial)
    {
        setFixedSize(180, 50);   // fixed width as requested
        updateStyle();
    }

    void advance()
    {
        m_state = (m_state + 1) % 6;
        updateStyle();
    }

    void setState(int s)
    {
        if (s < 0 || s > 5) s = 0;
        m_state = s;
        updateStyle();
    }

    int state() const { return m_state; }

    static QString labelForState(int s)
    {
        switch (s) {
        case 0: return "5s";
        case 1: return "10s";
        case 2: return "15s";
        case 3: return "30s";
        case 4: return "1m";
        case 5: return "Never";
        }
        return "5s";
    }

    static int secondsForState(int s)
    {
        switch (s) {
        case 0: return 5;
        case 1: return 10;
        case 2: return 15;
        case 3: return 30;
        case 4: return 60;
        case 5: return 0;   // 0 = never
        }
        return 5;
    }

private:
    int m_state;

    void updateStyle()
    {
        QString color;
        switch (m_state) {
        case 0: color = "#555555"; break; // 5s
        case 1: color = "#666666"; break; // 10s
        case 2: color = "#777777"; break; // 15s
        case 3: color = "#888888"; break; // 30s
        case 4: color = "#AAAAAA"; break; // 1m
        case 5: color = "#CCCCCC"; break; // Never
        default: color = "#555555"; break;
        }

        setStyleSheet(
            QString(
                "QPushButton {"
                " background:%1;"
                " border-radius:25px;"
                " color:white;"
                " font-size:22px;"
                " padding:4px 16px;"
                "}"
            ).arg(color)
        );
        setText(labelForState(m_state));
    }
};

// -----------------------------------------------------
// DisplayPage
// -----------------------------------------------------
class DisplayPage : public QWidget
{
public:
    explicit DisplayPage(QStackedWidget *stack)
        : QWidget(stack), m_stack(stack)
    {
        setStyleSheet("background:#282828; color:white; font-family:Sans;");

        cfg = loadCfg();

        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(40, 40, 40, 40);
        root->setSpacing(10);

        QLabel *title = new QLabel("Display");
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

        QFrame *outer = new QFrame(wrap);
        outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
        QVBoxLayout *outerLay = new QVBoxLayout(outer);
        outerLay->setContentsMargins(50, 30, 50, 30);
        outerLay->setSpacing(30);

        // Nightlight (BoolPill)
        outerLay->addWidget(makeNightlightRow());

        // Sleep Timeout (TimeoutPill)
        outerLay->addWidget(makeSleepTimeoutRow());

        // Adaptive Brightness (BoolPill)
        outerLay->addWidget(makeAdaptiveRow());

        // Boot animation toggle (BoolPill)
        outerLay->addWidget(makeBootRow());

        // Screen Info (mini-cards)
        outerLay->addWidget(makeScreenInfoCard());

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
    QStackedWidget *m_stack = nullptr;
    QMap<QString,QString> cfg;

    BoolPillToggle *m_nightlightPill = nullptr;
    BoolPillToggle *m_adaptivePill   = nullptr;
    BoolPillToggle *m_bootPill       = nullptr;
    TimeoutPill    *m_timeoutPill    = nullptr;

    // ------------- config helpers (local) -------------
    QString readCfg(const QString &k, const QString &def = QString()) const
    {
        return cfg.contains(k) ? cfg.value(k) : def;
    }

    bool readCfgBool(const QString &k, bool def = false) const
    {
        QString v = readCfg(k, def ? "true" : "false");
        return (v == "true");
    }

    void writeCfg(const QString &k, const QString &v)
    {
        cfg[k] = v;
        saveCfg(cfg);
    }

    // ------------- rows -------------------------------
    QWidget *makeNightlightRow()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Nightlight");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);

        lay->addStretch();

        bool on = readCfgBool("display_nightlight", false);
        m_nightlightPill = new BoolPillToggle(on, card);
        lay->addWidget(m_nightlightPill);

        connect(m_nightlightPill, &QPushButton::clicked, this, [this]() {
            m_nightlightPill->toggle();
            bool state = m_nightlightPill->isOn();
            writeCfg("display_nightlight", state ? "true" : "false");
            // Placeholder: actual nightlight overlay can be applied later
        });

        return card;
    }

    QWidget *makeSleepTimeoutRow()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Sleep Timeout");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);

        lay->addStretch();

        int st = readCfg("display_sleep_timeout", "0").toInt();
        if (st < 0 || st > 5) st = 0;

        m_timeoutPill = new TimeoutPill(st, card);
        lay->addWidget(m_timeoutPill);

        applyTimeoutState(st);

        connect(m_timeoutPill, &QPushButton::clicked, this, [this]() {
            m_timeoutPill->advance();
            int s = m_timeoutPill->state();
            writeCfg("display_sleep_timeout", QString::number(s));
            applyTimeoutState(s);
        });

        return card;
    }

    QWidget *makeAdaptiveRow()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Adaptive Brightness");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);

        lay->addStretch();

        bool on = readCfgBool("display_adaptive_brightness", false);
        m_adaptivePill = new BoolPillToggle(on, card);
        lay->addWidget(m_adaptivePill);

        connect(m_adaptivePill, &QPushButton::clicked, this, [this]() {
            m_adaptivePill->toggle();
            bool state = m_adaptivePill->isOn();
            writeCfg("display_adaptive_brightness", state ? "true" : "false");
            // Placeholder for real adaptive logic
        });

        return card;
    }

    QWidget *makeBootRow()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QHBoxLayout *lay = new QHBoxLayout(card);
        lay->setContentsMargins(30, 20, 30, 20);
        lay->setSpacing(20);

        QLabel *lbl = new QLabel("Boot animation toggle");
        lbl->setStyleSheet("font-size:30px; font-weight:bold;");
        lay->addWidget(lbl);

        lay->addStretch();

        bool on = readCfgBool("display_boot_animation", true);
        m_bootPill = new BoolPillToggle(on, card);
        lay->addWidget(m_bootPill);

        // Apply initial state
        applyBootState(on);

        connect(m_bootPill, &QPushButton::clicked, this, [this]() {
            m_bootPill->toggle();
            bool state = m_bootPill->isOn();
            writeCfg("display_boot_animation", state ? "true" : "false");
            applyBootState(state);
        });

        return card;
    }

    QWidget *makeScreenInfoCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30, 20, 30, 20);
        v->setSpacing(10);

        QLabel *header = new QLabel("Screen Info");
        header->setStyleSheet("font-size:26px; font-weight:bold;");
        v->addWidget(header);

        QList<QScreen*> screens = QGuiApplication::screens();
        if (screens.isEmpty()) {
            QLabel *none = new QLabel("No screens detected");
            none->setStyleSheet("font-size:22px;");
            v->addWidget(none);
        } else {
            for (int i = 0; i < screens.size(); ++i) {
                v->addWidget(makeScreenMiniCard(screens[i], i));
            }
        }

        return card;
    }

    QWidget *makeScreenMiniCard(QScreen *s, int idx)
    {
        QFrame *mini = new QFrame;
        mini->setStyleSheet("QFrame { background:#555555; border-radius:18px; }");

        QVBoxLayout *v = new QVBoxLayout(mini);
        v->setContentsMargins(20, 12, 20, 12);
        v->setSpacing(4);

        QLabel *hdr = new QLabel(QString("Screen %1: %2").arg(idx).arg(s->name()));
        hdr->setStyleSheet("font-size:24px; font-weight:bold;");
        v->addWidget(hdr);

        auto addRow = [&](const QString &name, const QString &val) {
            QHBoxLayout *h = new QHBoxLayout();
            h->setContentsMargins(0,0,0,0);
            h->setSpacing(6);
            QLabel *l = new QLabel(name);
            QLabel *r = new QLabel(val);
            r->setStyleSheet("color:#e0e0e0;");
            h->addWidget(l);
            h->addStretch();
            h->addWidget(r);
            v->addLayout(h);
        };

        addRow("Resolution",
               QString("%1 x %2").arg(s->size().width()).arg(s->size().height()));
        addRow("Refresh",
               QString("%1 Hz").arg((int)s->refreshRate()));
        addRow("DPI",
               QString::number((int)s->logicalDotsPerInch()));
        addRow("Physical",
               QString("%1mm x %2mm")
               .arg(s->physicalSize().width())
               .arg(s->physicalSize().height()));

        return mini;
    }

    // ------------- backends ---------------------------
    void applyTimeoutState(int st)
    {
        int secs = TimeoutPill::secondsForState(st);
        QString cmd = QString("alternix-set-sleep-timeout %1").arg(secs);
        system(cmd.toUtf8().constData());
    }

    void applyBootState(bool on)
    {
        if (on)
            system("alternix-toggle-bootanimation on");
        else
            system("alternix-toggle-bootanimation off");
    }
};

// -----------------------------------------------------
// Factory for osm-settings
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new DisplayPage(stack);
}
