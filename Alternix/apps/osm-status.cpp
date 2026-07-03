#include <QApplication>
#include <QCoreApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QGraphicsDropShadowEffect>
#include <QMouseEvent>
#include <QLockFile>
#include <QDir>
#include <QDebug>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QDateTime>
#include <QTextStream>
#include <QPainter>
#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <QStandardPaths>
#include <QProcess>

#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusVariant>
#include <QtDBus/QDBusVirtualObject>
#include <QtDBus/QDBusArgument>
#include <QtDBus/QDBusMetaType>

#include <functional>

// ───────────────────────────────────────────── Structures

struct NotificationInfo {
    QString title;
    QString body;
    QString path;
    QString sound;     // "" = default, "none" = silent, else file path
    QDateTime when;
};

// ───────────────────────────────────────────── Sounds
// Default sound files (wav, ogg, flac, or mp3 – drop your own here):
//   ~/.config/Alternix/sounds/notify.*   – ordinary notifications
//   ~/.config/Alternix/sounds/alarm.*    – alarms / critical

static QString soundDirPath() {
    return QDir::homePath() + "/.config/Alternix/sounds";
}

static void playSoundFile(const QString &file) {
    if (file.isEmpty() || !QFile::exists(file))
        return;

    QString q = "'" + QString(file).replace("'", "'\\''") + "'";
    QString ext = QFileInfo(file).suffix().toLower();

    // pick players by format:
    //  mp3            → mpg123, cvlc fallback
    //  wav/ogg/flac/… → paplay (libsndfile), aplay, cvlc fallback
    QString cmd;
    if (ext == "mp3")
        cmd = QString("mpg123 -q %1 2>/dev/null || "
                      "cvlc --play-and-exit --intf dummy %1 2>/dev/null").arg(q);
    else
        cmd = QString("paplay %1 2>/dev/null || aplay -q %1 2>/dev/null || "
                      "cvlc --play-and-exit --intf dummy %1 2>/dev/null").arg(q);

    QProcess::startDetached("sh", QStringList() << "-c" << cmd);
}

// find notify.* / alarm.* in the sounds folder, any supported format
static QString findDefaultSound(const QString &baseName) {
    static const QStringList exts = {"wav", "ogg", "flac", "mp3"};
    for (const QString &e : exts) {
        QString p = soundDirPath() + "/" + baseName + "." + e;
        if (QFile::exists(p)) return p;
    }
    return QString();
}

// sound: "" → default by kind, "none" → silent, path → that file
static void playNotificationSound(const QString &sound, bool alarmish) {
    if (sound.compare("none", Qt::CaseInsensitive) == 0)
        return;
    if (!sound.isEmpty() && QFile::exists(sound)) {
        playSoundFile(sound);
        return;
    }
    QString def = alarmish ? findDefaultSound("alarm") : QString();
    if (def.isEmpty())
        def = findDefaultSound("notify");   // general default / fallback
    playSoundFile(def);
}

class StatusPanel;
class NotificationCard;
class OverlayRoot;         // forward
class NotificationBadge;   // forward

// ───────────────────────────────────────────── StatusPanel

class StatusPanel : public QWidget {
public:
    explicit StatusPanel(QWidget *parent=nullptr);

    void refreshNotifications();
    void removeNotification(const QString &path);
    void resizeToItems(int count);

    // overlay width from edge of screen
    int computeRequiredWidth(const QStringList &titles) {
        // fixed horizontal overhead around the title text:
        // outer margin 20 + inner 32 + list 15 + card 20 + time 64
        // + close 48 + spacings + text margins ≈ 260
        int base = 280;
        QFont f;
        f.setPixelSize(28);        // matches the card title style
        f.setBold(true);
        QFontMetrics fm(f);
        int max = 0;
        for (const QString &t : titles)
            max = qMax(max, fm.horizontalAdvance(t));
        return base + max;
    }

    void setCloseCallback(std::function<void()> fn) { onClose = fn; }

    int notificationCount() const { return m_notificationCount; }

public:
    std::function<void()>    onClose;
    std::function<void(int)> onCountChanged;

private:
    QWidget     *m_inner;
    QWidget     *m_content;
    QVBoxLayout *m_list;
    int          m_width;
    int          m_maxH;
    QString      m_dirPath;
    int          m_notificationCount;
    QSet<QString> m_seenPaths;
    bool          m_firstScan = true;
};

// ───────────────────────────────────────────── NotificationCard

class NotificationCard : public QFrame {
public:
    NotificationCard(StatusPanel *panel, const NotificationInfo &info, QWidget *parent=nullptr);

protected:
    void mousePressEvent(QMouseEvent *e) override;

private:
    StatusPanel      *m_panel;
    NotificationInfo  m_info;
    QLabel           *m_titleLabel;
};

// ───────────────────────────────────────────── NotificationBadge
// Small always-on-top stub + numbered circle. Only shows count.

class NotificationBadge : public QWidget {
public:
    explicit NotificationBadge(OverlayRoot *overlay, QWidget *parent=nullptr);

    void setCount(int c);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;   // body defined later

private:
    OverlayRoot *m_overlay;
    int          m_count;
    QTimer      *m_raiseTimer;
};

// ───────────────────────────────────────────── StatusPanel impl

StatusPanel::StatusPanel(QWidget *parent)
    : QWidget(parent),
      m_inner(nullptr),
      m_content(nullptr),
      m_list(nullptr),
      m_width(0),
      m_maxH(0),
      m_dirPath(),
      m_notificationCount(0)
{
    setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_TranslucentBackground);

    QRect g = QGuiApplication::primaryScreen()->geometry();

    m_width = qMin(520, int(g.width()*0.65));
    m_maxH  = int(g.height()*0.9);

    int top = int(g.height()*0.20);
    // right side mirror of osm-running
    setGeometry(g.width() - m_width, top, m_width, 200);

    QVBoxLayout *outer = new QVBoxLayout(this);
    // mirror margins: osm-running uses (0,20,20,20); we use (20,20,0,20)
    outer->setContentsMargins(20, 20, 0, 20);

    m_inner = new QWidget(this);
    m_inner->setObjectName("inner");
    m_inner->setStyleSheet(
        "#inner{"
        " background:#80708099;"
        " border-top-left-radius:26px;"
        " border-bottom-left-radius:26px;"
        " border-top-right-radius:0px;"
        " border-bottom-right-radius:0px;"
        "}"
    );

    QVBoxLayout *inner = new QVBoxLayout(m_inner);
    inner->setContentsMargins(16, 16, 16, 16);

    m_content = new QWidget;
    m_content->setStyleSheet("background:#00000099; border-radius:14px;");
    m_content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    m_list = new QVBoxLayout(m_content);
    m_list->setSpacing(16);
    // mirror of (5,5,10,15) → (10,5,5,15)
    m_list->setContentsMargins(10, 5, 5, 15);

    inner->addWidget(m_content);
    outer->addWidget(m_inner);

    // shadow (same as osm-running)
    QGraphicsDropShadowEffect *sh = new QGraphicsDropShadowEffect(this);
    sh->setBlurRadius(32);
    sh->setOffset(0, 0);
    sh->setColor(QColor(0, 0, 0, 220));
    m_inner->setGraphicsEffect(sh);

    // notifications folder
    m_dirPath = QDir::homePath() + "/.osm-notify";
    QDir d(m_dirPath);
    if (!d.exists()) d.mkpath(".");

    QTimer *t = new QTimer(this);
    t->setInterval(600);
    connect(t, &QTimer::timeout, [this](){ refreshNotifications(); });
    t->start();

    refreshNotifications();
}

// HEIGHT BASED ON ACTUAL CARD SIZEHINTS (auto height)
void StatusPanel::resizeToItems(int count) {
    Q_UNUSED(count);

    int totalH = 0;

    if (m_list) {
        // real width each card will get:
        // panel − outer(20) − inner(16+16) − list margins(10+5)
        int cardW = m_width - 20 - 32 - 15;

        int itemCount = m_list->count();
        for (int i = 0; i < itemCount; ++i) {
            QLayoutItem *it = m_list->itemAt(i);
            if (!it) continue;
            QWidget *w = it->widget();
            if (!w) continue;

            // word-wrapped labels need height-for-width, not sizeHint
            int hh = w->hasHeightForWidth() ? w->heightForWidth(cardW) : -1;
            if (hh <= 0) hh = w->sizeHint().height();
            totalH += hh;
        }

        if (itemCount > 1)
            totalH += (itemCount - 1) * m_list->spacing();

        QMargins lm = m_list->contentsMargins();
        totalH += lm.top() + lm.bottom();
    }

    // inner + outer margins overhead (approx), similar to osm-running
    int h = totalH + 72;
    h = qBound(120, h, m_maxH);

    QRect screenGeo = QGuiApplication::primaryScreen()->geometry();
    int top = int(screenGeo.height() * 0.15);

    setGeometry(screenGeo.width() - m_width, top, m_width, h);
}

void StatusPanel::refreshNotifications() {
    // clear list
    QLayoutItem *it;
    while((it = m_list->takeAt(0))) {
        if(it->widget()) it->widget()->deleteLater();
        delete it;
    }

    QDir dir(m_dirPath);
    dir.setNameFilters(QStringList() << "*.txt");

    QFileInfoList files = dir.entryInfoList(
        QDir::Files | QDir::Readable,
        QDir::Time | QDir::Reversed    // oldest first
    );

    // newest first
    std::reverse(files.begin(), files.end());

    QStringList titles;
    QSet<QString> newSeen;
    int count = 0;

    for (const QFileInfo &fi : files) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;

        QTextStream in(&f);
        QString contents = in.readAll();

        QStringList lines = contents.split('\n');
        for (QString &ln : lines)
            ln = ln.trimmed();

        QString rawTitle;
        QString body;
        QString soundLine;

        int firstNonEmpty = -1;
        for (int i = 0; i < lines.size(); ++i) {
            if (!lines[i].isEmpty()) {
                firstNonEmpty = i;
                break;
            }
        }

        if (firstNonEmpty >= 0) {
            rawTitle = lines[firstNonEmpty];
            QStringList rest;
            for (int i = firstNonEmpty + 1; i < lines.size(); ++i) {
                // optional "sound:" control line – not shown in the body
                if (lines[i].startsWith("sound:", Qt::CaseInsensitive)) {
                    soundLine = lines[i].mid(6).trimmed();
                    continue;
                }
                rest << lines[i];
            }
            body = rest.join('\n').trimmed();
        } else {
            rawTitle.clear();
            body.clear();
        }

        QString title = rawTitle;
        if (title.isEmpty()) {
            QString base = fi.completeBaseName();
            if (!base.isEmpty())
                title = base;
            else
                title = "(REDACTED)";
            body = contents.trimmed();
        }

        NotificationInfo info;
        info.title = title;
        info.body  = body;
        info.path  = fi.absoluteFilePath();
        info.sound = soundLine;
        info.when  = fi.lastModified();

        titles << title;

        // sound for newly-appeared notifications (skip initial scan)
        newSeen.insert(info.path);
        if (!m_seenPaths.contains(info.path) && !m_firstScan) {
            bool alarmish = title.startsWith("⏰");
            playNotificationSound(info.sound, alarmish);
        }

        NotificationCard *card = new NotificationCard(this, info, m_content);
        m_list->addWidget(card);
        count++;
    }

    m_seenPaths = newSeen;
    m_firstScan = false;

    m_notificationCount = count;

    // compute and apply width (same logic as osm-running)
    int needed = computeRequiredWidth(titles);
    QRect scr = QGuiApplication::primaryScreen()->geometry();
    m_width = qMin(qMin(needed, 1080), scr.width());

    QRect g = geometry();
    int x = QGuiApplication::primaryScreen()->geometry().width() - m_width;
    if (g.width() != m_width || g.x() != x)
        setGeometry(x, g.y(), m_width, g.height());

    resizeToItems(count);

    if (onCountChanged)
        onCountChanged(m_notificationCount);

    // auto-close when empty, same behaviour as SidePanel’s onClose
    if (count == 0 && onClose)
        onClose();
}

void StatusPanel::removeNotification(const QString &path) {
    QFile::remove(path);
    refreshNotifications();
}

// ───────────────────────────────────────────── NotificationCard impl

NotificationCard::NotificationCard(
    StatusPanel *panel,
    const NotificationInfo &info,
    QWidget *parent)
    : QFrame(parent), m_panel(panel), m_info(info)
{
    // auto height: let layout decide; just a small minimum
    setMinimumHeight(60);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    setStyleSheet("background:#282828;border-radius:14px;border:none;");

    QHBoxLayout *h = new QHBoxLayout(this);
    h->setContentsMargins(10, 5, 10, 5);
    h->setSpacing(10);

    // TIME LABEL (left side)
    QLabel *timeLbl = new QLabel(
        m_info.when.toLocalTime().toString("hh:mm"),
        this
    );
    timeLbl->setFixedWidth(64);
    // Center time vertically & horizontally in its column
    timeLbl->setAlignment(Qt::AlignCenter);
    timeLbl->setStyleSheet("color:#BBBBBB;font-size:20px;");

    // TITLE + BODY stacked vertically
    QWidget *textBox = new QWidget(this);
    QVBoxLayout *v = new QVBoxLayout(textBox);
    v->setContentsMargins(10,10,10,25);
    v->setSpacing(10);

    QLabel *title = new QLabel(m_info.title, this);
    title->setStyleSheet("color:white;font-size:28px;font-weight:bold;");
    title->setWordWrap(true);
    title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_titleLabel = title;
    v->addWidget(title);

    // BODY (2nd line, optional)
    if (!m_info.body.isEmpty()) {
        QLabel *body = new QLabel(m_info.body, this);
        body->setStyleSheet("color:#CCCCCC;font-size:22px;");
        body->setWordWrap(true);
        body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        v->addWidget(body);
    }

    // CLOSE BUTTON
    QPushButton *close = new QPushButton(" ❌", this);
    close->setFixedSize(48,48);
    close->setStyleSheet(
        "QPushButton:hover { color:#ff1616; background:#ad1236; border-radius:18px; }"
        "QPushButton:pressed { color:#ffffff; background:#550000; border-radius:18px; }"
    );

    // build layout
    h->addWidget(timeLbl);
    h->addWidget(textBox, 1);   // expands to fit text
    h->addWidget(close);

    connect(close, &QPushButton::clicked, [this]() {
        if (m_panel)
            m_panel->removeNotification(m_info.path);
    });
}

void NotificationCard::mousePressEvent(QMouseEvent *e) {
    if(e->button()==Qt::LeftButton) {
        // could later add “tap to open file” etc; for now do nothing
    }
    QFrame::mousePressEvent(e);
}

// ───────────────────────────────────────────── NotificationBadge impl
// (methods that don't need OverlayRoot's full definition)

NotificationBadge::NotificationBadge(OverlayRoot *overlay, QWidget *parent)
    : QWidget(nullptr),        // force top-level window
      m_overlay(overlay),
      m_count(0),
      m_raiseTimer(nullptr)
{
    Q_UNUSED(parent);

    setWindowFlags(Qt::FramelessWindowHint
                   | Qt::WindowStaysOnTopHint
                   | Qt::BypassWindowManagerHint
                   | Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    QRect g = QGuiApplication::primaryScreen()->geometry();
    int stubWidth  = 22;
    int stubHeight = 54;
    int x = g.x() + g.width() - stubWidth - 4; // 4px inset from edge
    int y = g.y() + int(g.height() * 0.15);    // ~15% from top
    setGeometry(x, y, stubWidth + 8, stubHeight + 32); // extra for circle

    hide();

    m_raiseTimer = new QTimer(this);
    m_raiseTimer->setInterval(1500);
    connect(m_raiseTimer, &QTimer::timeout, this, [this]() {
        if (isVisible())
            this->raise();
    });
    m_raiseTimer->start();
}

void NotificationBadge::setCount(int c) {
    m_count = c;
    if (m_count <= 0) {
        hide();
    } else {
        show();
        raise();
    }
    update();
}

void NotificationBadge::paintEvent(QPaintEvent *e) {
    Q_UNUSED(e);
    if (m_count <= 0)
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    int stubWidth  = 24;
    int stubHeight = 54;

    int marginTop = 24;
    int x = width() - stubWidth - 4;
    int y = marginTop;

    // stub: same translucent colour as main overlay (#80708099)
    // ARGB = (0x80, 0x70, 0x80, 0x99) → (128, 112, 128, 153)
    QRectF stubRect(x, y, stubWidth, stubHeight);
    p.setBrush(QColor(112, 128, 153, 128));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(stubRect, 6, 6);

    // blue circle with count – anchored to TOP LEFT of the stub
    int radius = 14;
    QPointF circleCenter(x + radius, y); // top edge aligned, left aligned
    p.setBrush(QColor(0, 160, 220));
    p.setPen(Qt::NoPen);
    p.drawEllipse(circleCenter, radius, radius);

    // notification count text
    p.setPen(Qt::white);
    QFont f = font();
    f.setBold(true);
    f.setPointSize(16);
    p.setFont(f);

    QRectF textRect(circleCenter.x() - radius,
                    circleCenter.y() - radius,
                    radius * 2,
                    radius * 2);
    p.drawText(textRect, Qt::AlignCenter, QString::number(m_count));
}

// ───────────────────────────────────────────── OverlayRoot

class OverlayRoot : public QWidget {
public:
    explicit OverlayRoot()
        : m_panel(nullptr),
          m_badge(nullptr),
          m_panelVisible(false)
    {
        setWindowFlags(Qt::FramelessWindowHint |
                       Qt::Tool |
                       Qt::WindowStaysOnTopHint |
                       Qt::X11BypassWindowManagerHint |
                       Qt::WindowDoesNotAcceptFocus);

        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);

        m_screenGeo = QGuiApplication::primaryScreen()->geometry();
        setGeometry(m_screenGeo);

        m_panel = new StatusPanel(this);

        QRect finalGeo = m_panel->geometry();
        QRect startGeo = finalGeo;
        // start off-screen to the RIGHT
        startGeo.moveLeft(m_screenGeo.width());
        m_panel->setGeometry(startGeo);
        m_panel->hide();

        m_panel->setCloseCallback([this]() {
            hidePanel();
        });

        // badge window (separate always-on-top widget)
        m_badge = new NotificationBadge(this);

        // sync badge count with panel
        m_panel->onCountChanged = [this](int n) {
            if (!m_badge) return;
            if (!this->isVisible() || !m_panel->isVisible()) {
                m_badge->setCount(n);  // overlay closed ⇒ show badge
            } else {
                m_badge->setCount(0);  // overlay open ⇒ hide badge
            }
        };

        hide(); // start hidden
    }

    void showPanel() {
        if (m_panelVisible) return;
        m_panelVisible = true;

        setGeometry(m_screenGeo);
        show();
        raise();

        QRect finalGeo = m_panel->geometry();
        finalGeo = QRect(m_screenGeo.width() - finalGeo.width(),
                         finalGeo.y(), finalGeo.width(), finalGeo.height());
        QRect startGeo = finalGeo;
        startGeo.moveLeft(m_screenGeo.width());
        m_panel->setGeometry(startGeo);
        m_panel->show();

        if (m_badge)
            m_badge->setCount(0); // hide while open

        QPropertyAnimation *anim = new QPropertyAnimation(m_panel,"geometry",this);
        anim->setDuration(220);
        anim->setStartValue(startGeo);
        anim->setEndValue(finalGeo);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }

    void hidePanel() {
        if (!m_panelVisible || !m_panel) return;
        m_panelVisible = false;

        QRect cur = m_panel->geometry();
        QRect endGeo = cur;
        endGeo.moveLeft(m_screenGeo.width());

        QPropertyAnimation *anim = new QPropertyAnimation(m_panel,"geometry",this);
        anim->setDuration(220);
        anim->setStartValue(cur);
        anim->setEndValue(endGeo);
        anim->setEasingCurve(QEasingCurve::InCubic);
        connect(anim,&QPropertyAnimation::finished,[this](){
            if (m_panel) m_panel->hide();
            hide();
            if (m_badge)
                m_badge->setCount(m_panel->notificationCount());
        });
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (m_panel && !m_panel->geometry().contains(e->pos()))
            hidePanel();
        else
            QWidget::mousePressEvent(e);
    }

private:
    StatusPanel       *m_panel;
    NotificationBadge *m_badge;
    bool               m_panelVisible;
    QRect              m_screenGeo;

    friend class NotificationBadge;
};

// ───────────────────────────────────────────── NotificationBadge::mousePressEvent
// (now after OverlayRoot is fully defined)

void NotificationBadge::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && m_overlay) {
        e->accept();
        m_overlay->showPanel();
        return;
    }
    QWidget::mousePressEvent(e);
}

// ───────────────────────────────────────────── ActivationEdgeBar
// Always-on-top, right-side touch bar (mirror of osm-running's left bar)

class ActivationEdgeBar : public QWidget {
public:
    explicit ActivationEdgeBar(OverlayRoot *overlay, QWidget *parent = nullptr)
        : QWidget(parent),
          m_overlay(overlay),
          m_dragging(false)
    {
        setWindowFlags(Qt::FramelessWindowHint
                       | Qt::WindowStaysOnTopHint
                       | Qt::BypassWindowManagerHint
                       | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setMouseTracking(true);

        QRect g = QGuiApplication::primaryScreen()->geometry();
        int barWidth = 10;
        int x = g.x() + g.width() - barWidth;
        setGeometry(x, g.y(), barWidth, g.height());
        setStyleSheet("background: rgba(0,0,0,0);");

        show();
        raise();

        // keep stubbornly on top, like osm-running
        m_raiseTimer = new QTimer(this);
        m_raiseTimer->setInterval(1500);
        connect(m_raiseTimer, &QTimer::timeout, this, [this]() {
            this->raise();
        });
        m_raiseTimer->start();
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) {
            m_dragging = true;
            m_pressPos = e->globalPos();
            raise();  // assert on top when touched
        }
        QWidget::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!m_dragging) {
            QWidget::mouseMoveEvent(e);
            return;
        }

        // swipe left from the right edge
        int dx = m_pressPos.x() - e->globalPos().x();
        if (dx > 12) {
            if (m_overlay) {
                m_overlay->showPanel();
            }
            m_dragging = false;
        }

        QWidget::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        m_dragging = false;
        QWidget::mouseReleaseEvent(e);
    }

    void paintEvent(QPaintEvent *) override {
        // fully invisible
    }

private:
    OverlayRoot *m_overlay;
    bool         m_dragging;
    QPoint       m_pressPos;
    QTimer      *m_raiseTimer;
};

// ───────────────────────────────────────────── Notification helpers

static QString stripMarkup(QString s) {
    // The fdo spec allows a small HTML subset in the body – flatten it.
    s.replace(QRegularExpression("<br\\s*/?>", QRegularExpression::CaseInsensitiveOption), "\n");
    s.remove(QRegularExpression("<[^>]*>"));
    s.replace("&lt;", "<").replace("&gt;", ">")
     .replace("&quot;", "\"").replace("&apos;", "'")
     .replace("&amp;", "&");
    return s.trimmed();
}

static QString notifyDirPath() {
    return QDir::homePath() + "/.osm-notify";
}

// Write one notification file. Returns the file path (empty on failure).
// File format matches what StatusPanel::refreshNotifications() expects:
//   first non-empty line = title, rest = body.
static QString writeNotificationFile(const QString &title,
                                     const QString &body,
                                     quint32 id,
                                     const QString &sound = QString())
{
    QDir d(notifyDirPath());
    if (!d.exists()) d.mkpath(".");

    // sortable, unique name: epoch-ms + dbus id
    QString name = QString("%1-%2.txt")
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(id);

    QFile f(d.absoluteFilePath(name));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();

    QTextStream out(&f);
    QString t = title.trimmed();
    if (t.isEmpty()) t = "Notification";
    // keep title single-line so the panel parser stays happy
    t.replace('\n', ' ');
    out << t << "\n";
    QString b = body.trimmed();
    if (!b.isEmpty())
        out << b << "\n";
    if (!sound.trimmed().isEmpty())
        out << "sound:" << sound.trimmed() << "\n";
    f.close();
    return f.fileName();
}

// ───────────────────────────────────────────── NotificationServer
// Implements org.freedesktop.Notifications so ordinary apps
// (Firefox, mail, chat, notify-send, …) land in ~/.osm-notify
// and therefore in the StatusPanel.

// Implemented as a QDBusVirtualObject (raw message handling) instead of a
// Q_OBJECT slots class, so NO moc step is needed — plain g++ build.

class NotificationServer : public QDBusVirtualObject {
public:
    explicit NotificationServer(OverlayRoot *overlay, QObject *parent = nullptr)
        : QDBusVirtualObject(parent), m_overlay(overlay), m_nextId(1) {}

    bool registerOnBus() {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            qWarning() << "osm-status: no D-Bus session bus – app notifications disabled";
            return false;
        }
        if (!bus.registerService("org.freedesktop.Notifications")) {
            qWarning() << "osm-status: org.freedesktop.Notifications already owned"
                       << "(another notification daemon running?)";
            return false;
        }
        return bus.registerVirtualObject("/org/freedesktop/Notifications", this);
    }

    // ---- QDBusVirtualObject interface -------------------------------

    QString introspect(const QString &path) const override {
        Q_UNUSED(path);
        return QStringLiteral(
            "<interface name=\"org.freedesktop.Notifications\">"
            " <method name=\"Notify\">"
            "  <arg direction=\"in\"  type=\"s\"/>"
            "  <arg direction=\"in\"  type=\"u\"/>"
            "  <arg direction=\"in\"  type=\"s\"/>"
            "  <arg direction=\"in\"  type=\"s\"/>"
            "  <arg direction=\"in\"  type=\"s\"/>"
            "  <arg direction=\"in\"  type=\"as\"/>"
            "  <arg direction=\"in\"  type=\"a{sv}\"/>"
            "  <arg direction=\"in\"  type=\"i\"/>"
            "  <arg direction=\"out\" type=\"u\"/>"
            " </method>"
            " <method name=\"CloseNotification\">"
            "  <arg direction=\"in\" type=\"u\"/>"
            " </method>"
            " <method name=\"GetCapabilities\">"
            "  <arg direction=\"out\" type=\"as\"/>"
            " </method>"
            " <method name=\"GetServerInformation\">"
            "  <arg direction=\"out\" type=\"s\"/>"
            "  <arg direction=\"out\" type=\"s\"/>"
            "  <arg direction=\"out\" type=\"s\"/>"
            "  <arg direction=\"out\" type=\"s\"/>"
            " </method>"
            " <signal name=\"NotificationClosed\">"
            "  <arg type=\"u\"/><arg type=\"u\"/>"
            " </signal>"
            " <signal name=\"ActionInvoked\">"
            "  <arg type=\"u\"/><arg type=\"s\"/>"
            " </signal>"
            "</interface>");
    }

    bool handleMessage(const QDBusMessage &msg,
                       const QDBusConnection &connection) override
    {
        const QString member = msg.member();

        if (member == "Notify") {
            const QList<QVariant> a = msg.arguments();
            if (a.size() < 8) return false;

            QString     app_name    = a.at(0).toString();
            uint        replaces_id = a.at(1).toUInt();
            QString     summary     = a.at(3).toString();
            QString     body        = a.at(4).toString();

            // hints arrive as a marshalled a{sv}
            QVariantMap hints;
            if (a.at(6).canConvert<QDBusArgument>())
                hints = qdbus_cast<QVariantMap>(
                    a.at(6).value<QDBusArgument>());
            else
                hints = a.at(6).toMap();

            uint id = doNotify(app_name, replaces_id, summary, body, hints);
            QDBusConnection(connection).send(msg.createReply(QVariant(id)));
            return true;
        }

        if (member == "CloseNotification") {
            uint id = msg.arguments().value(0).toUInt();
            if (m_files.contains(id))
                QFile::remove(m_files.take(id));

            QDBusConnection bus(connection);
            bus.send(msg.createReply());

            QDBusMessage sig = QDBusMessage::createSignal(
                "/org/freedesktop/Notifications",
                "org.freedesktop.Notifications",
                "NotificationClosed");
            sig << id << uint(3) /* closed by call */;
            bus.send(sig);
            return true;
        }

        if (member == "GetCapabilities") {
            QDBusConnection(connection).send(msg.createReply(
                QVariant(QStringList()
                         << "body" << "body-markup" << "persistence")));
            return true;
        }

        if (member == "GetServerInformation") {
            QDBusConnection(connection).send(msg.createReply(
                QVariantList() << "osm-status"      // name
                               << "AlterniTech"     // vendor
                               << "1.0"             // version
                               << "1.2"));          // spec version
            return true;
        }

        return false;   // unknown member → let Qt reply with an error
    }

private:
    uint doNotify(const QString &app_name,
                  uint replaces_id,
                  const QString &summary,
                  const QString &body,
                  const QVariantMap &hints)
    {
        uint id = replaces_id != 0 ? replaces_id : m_nextId++;

        // Replacing an existing notification? Drop the old file first.
        if (replaces_id != 0 && m_files.contains(replaces_id))
            QFile::remove(m_files.take(replaces_id));

        QString title = summary.trimmed();
        if (title.isEmpty()) title = app_name.trimmed();

        QString cleanBody = stripMarkup(body);

        // Prefix the app name when it adds information
        if (!app_name.trimmed().isEmpty() &&
            app_name.trimmed().compare(title, Qt::CaseInsensitive) != 0)
        {
            cleanBody = cleanBody.isEmpty()
                ? QString("(%1)").arg(app_name.trimmed())
                : QString("%1\n(%2)").arg(cleanBody, app_name.trimmed());
        }

        QString path = writeNotificationFile(title, cleanBody, id);
        if (!path.isEmpty())
            m_files.insert(id, path);

        // urgency: 0 low, 1 normal, 2 critical.  Alarms/critical pop the panel.
        int urgency = 1;
        if (hints.contains("urgency"))
            urgency = hints.value("urgency").toInt();
        QString category = hints.value("category").toString();

        bool isAlarm = category.startsWith("alarm") || urgency >= 2;
        if (isAlarm && m_overlay)
            m_overlay->showPanel();

        return id;
    }

    OverlayRoot        *m_overlay;
    uint                m_nextId;
    QMap<uint, QString> m_files;
};

// ───────────────────────────────────────────── AlarmWatcher
// Reads ~/.osm-alarms, one alarm per line:
//   HH:MM|Title|Body                  → one-shot today (removed after firing)
//   yyyy-MM-dd HH:MM|Title|Body       → one-shot at date (removed after firing)
//   daily HH:MM|Title|Body            → repeats every day
// An optional 4th field is a per-alarm sound file:
//   HH:MM|Title|Body|/path/to/sound.wav
// Fires as a critical notification, so the panel slides out.

class AlarmWatcher : public QObject {
public:
    explicit AlarmWatcher(OverlayRoot *overlay, QObject *parent = nullptr)
        : QObject(parent), m_overlay(overlay)
    {
        m_path = QDir::homePath() + "/.osm-alarms";

        // ensure the file exists so users can just edit it
        if (!QFile::exists(m_path)) {
            QFile f(m_path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&f);
                out << "# osm-status alarms\n"
                       "# HH:MM|Title|Body            one-shot today\n"
                       "# yyyy-MM-dd HH:MM|Title|Body one-shot on date\n"
                       "# daily HH:MM|Title|Body      repeats every day\n";
            }
        }

        QTimer *t = new QTimer(this);
        t->setInterval(15 * 1000);   // check ~4× per minute
        connect(t, &QTimer::timeout, [this]() { check(); });
        t->start();
        check();
    }

private:
    void check() {
        QFile f(m_path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return;

        QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
        f.close();

        QDateTime now = QDateTime::currentDateTime();
        QString nowHM = now.toString("HH:mm");
        QString today = now.date().toString("yyyy-MM-dd");

        bool changed = false;
        QStringList keep;

        for (const QString &raw : lines) {
            QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith('#')) {
                keep << raw;
                continue;
            }

            QStringList parts = line.split('|');
            QString when = parts.value(0).trimmed();
            QString title = parts.value(1, "Alarm").trimmed();
            QString body  = parts.value(2).trimmed();
            QString sound = parts.value(3).trimmed();   // optional per-alarm sound file
            if (title.isEmpty()) title = "Alarm";

            bool fire = false;
            bool oneShot = true;

            if (when.startsWith("daily ", Qt::CaseInsensitive)) {
                oneShot = false;
                fire = (when.mid(6).trimmed() == nowHM);
            } else if (when.contains(' ')) {
                // dated: yyyy-MM-dd HH:mm  – also fire if overdue (missed while off)
                QDateTime dt = QDateTime::fromString(when, "yyyy-MM-dd HH:mm");
                fire = dt.isValid() && dt <= now;
            } else {
                fire = (when == nowHM);
            }

            // debounce: don't refire the same daily alarm within the same minute
            QString fireKey = when + "|" + title + "|" + today + " " + nowHM;
            if (fire && m_fired.contains(fireKey))
                fire = false;

            if (fire) {
                m_fired.insert(fireKey);
                if (m_fired.size() > 200) m_fired.clear();

                QString b = body.isEmpty()
                    ? now.toString("HH:mm")
                    : body;
                writeNotificationFile("⏰ " + title, b,
                                      QDateTime::currentMSecsSinceEpoch() % 100000,
                                      sound);
                if (m_overlay)
                    m_overlay->showPanel();

                if (oneShot) { changed = true; continue; }  // drop the line
            }

            keep << raw;
        }

        if (changed) {
            QFile w(m_path);
            if (w.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&w);
                out << keep.join('\n');
            }
        }
    }

    OverlayRoot  *m_overlay;
    QString       m_path;
    QSet<QString> m_fired;
};

// ───────────────────────────────────────────── main

int main(int argc,char**argv) {
    QApplication app(argc,argv);

    QLockFile lock(QDir::temp().absoluteFilePath("osm-status.lock"));
    lock.setStaleLockTime(0);
    if(!lock.tryLock(20))
        return 0;

    OverlayRoot root;          // overlay window
    ActivationEdgeBar bar(&root);   // always-on-top gesture edge

    // ensure the default sound folder exists
    QDir().mkpath(soundDirPath());

    // ── Boot sound: played once per boot, after the first unlock ──
    // osm-lockd touches /tmp/osm_boot_unlocked on successful unlock.
    // /tmp is cleared on reboot, so the "played" marker naturally
    // resets each boot but survives re-locks, wakes, and restarts
    // of osm-status within the same boot.
    {
        const QString unlockedMarker = "/tmp/osm_boot_unlocked";
        const QString playedMarker   = "/tmp/osm_boot_sound_played";

        QTimer *bootPoll = new QTimer(&app);
        bootPoll->setInterval(500);
        QObject::connect(bootPoll, &QTimer::timeout,
            [bootPoll, unlockedMarker, playedMarker]() {
                if (QFile::exists(playedMarker)) {
                    bootPoll->stop();       // already done this boot
                    return;
                }
                if (!QFile::exists(unlockedMarker))
                    return;                 // still locked

                // mark first, then play – guarantees once per boot
                QFile m(playedMarker);
                if (m.open(QIODevice::WriteOnly)) m.close();

                playSoundFile(findDefaultSound("boot"));
                bootPoll->stop();
            });
        bootPoll->start();

        // no lockscreen in use? stop polling quietly after 10 minutes
        QTimer::singleShot(10 * 60 * 1000, bootPoll, &QTimer::stop);
    }

    // become the desktop notification daemon (app push notifications)
    NotificationServer server(&root);
    server.registerOnBus();

    // alarms → notifications
    AlarmWatcher alarms(&root);

    return app.exec();
}

