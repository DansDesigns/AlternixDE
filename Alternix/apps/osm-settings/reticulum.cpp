#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QStackedWidget>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QTextStream>
#include <QMessageBox>
#include <QInputDialog>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QStringList>
#include <QVector>
#include <QMap>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QtMath>
#include <unistd.h>

// =====================================================================
// Alternix compact button style (same as Emulation / Storage pages)
// =====================================================================
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
        "QPushButton:disabled { background:#3a3a3a; color:#777777; }"
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

// Simple helper to grab stdout (ignore errors/exit code)
static QString runCmd(const QString &cmd, int timeoutMs = 5000)
{
    QProcess p;
    p.start("/bin/sh", {"-c", cmd});
    if (!p.waitForFinished(timeoutMs))
        p.kill();
    return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
}

// -----------------------------------------------------
// Root privileges for the setup actions.
// NOPASSWD fast path first, osm-sudo askpass fallback — same pattern as
// wifi.cpp and emulation.cpp. Prepend this to any script that needs root and
// prefix the privileged commands with $SUDOX. Deliberately NOT nested inside a
// second `sh -c` so scripts can use quotes and heredocs normally.
// -----------------------------------------------------
static const char *SUDO_PREAMBLE =
    "if sudo -n true 2>/dev/null; then SUDOX=\"sudo -n\"; "
    "else export SUDO_ASKPASS=$(command -v osm-sudo); SUDOX=\"sudo -A\"; fi\n";

// -----------------------------------------------------
// Config — ~/.config/Alternix/osm-settings.conf (QSettings INI)
//   Reticulum/LastMapRefresh  = unused placeholder for future map caching
// The Reticulum stack itself is NOT configured through QSettings: its own
// config file uses configobj syntax with nested [[sub-sections]], which
// QSettings cannot represent. See the rnsCfg* helpers below.
// -----------------------------------------------------
static QSettings* cfg()
{
    static QSettings s(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                       QSettings::IniFormat);
    return &s;
}

// The list of console scripts shipped by the `rns` package. Only the ones that
// actually exist in the venv get symlinked, so this can stay ahead of upstream.
static const char *RNS_BINARIES =
    "rnsd rnstatus rnpath rnprobe rnid rncp rnx rnsh rngit rnodeconf";

static QString rnsUser()
{
    QString u = qEnvironmentVariable("USER");
    if (u.isEmpty()) u = qEnvironmentVariable("LOGNAME");
    if (u.isEmpty()) u = runCmd("id -un");
    return u.trimmed();
}

static bool rnsInstalled()
{
    return runCmd("command -v rnsd >/dev/null 2>&1 && echo yes || echo no") == "yes";
}

// The pidfile written by /etc/init.d/rnsd is authoritative. A bare
// `pgrep -f rnsd` is not, even with the usual bracket trick: this page runs
// its own commands through /bin/sh -c and several of those command strings
// contain the literal "/etc/init.d/rnsd", so pgrep matches the very shell
// that is running the check and reports "running" when nothing is. The
// second clause catches an rnsd started by hand outside the service, and
// matches on the venv path, which none of this page's commands contain.
static bool rnsRunning()
{
    return runCmd(
        "if [ -f /run/rnsd.pid ] && kill -0 \"$(cat /run/rnsd.pid 2>/dev/null)\" 2>/dev/null; then "
        "echo yes; "
        "elif pgrep -f '/opt/reticulum/venv/bin/[r]nsd' >/dev/null 2>&1; then echo yes; "
        "else echo no; fi") == "yes";
}

static bool rnsBootEnabled()
{
    return runCmd(
        "if rc-update show default 2>/dev/null | grep -qw rnsd; then echo yes; "
        "elif ls /etc/rc2.d/S*rnsd >/dev/null 2>&1; then echo yes; "
        "else echo no; fi") == "yes";
}

// =====================================================================
// Reticulum config file (~/.reticulum/config)
//
// This is configobj format, not plain INI: interfaces live in doubly-bracketed
// sub-sections under [interfaces]. Everything here is deliberately
// line-oriented rather than parse-and-regenerate, so comments, ordering and
// hand-written options survive edits made from this page untouched.
// =====================================================================
static QString rnsDir()        { return QDir::homePath() + "/.reticulum"; }
static QString rnsConfigPath() { return rnsDir() + "/config"; }

static QStringList rnsCfgRead()
{
    QFile f(rnsConfigPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringList();
    const QString all = QString::fromUtf8(f.readAll());
    f.close();
    return all.split('\n');
}

static bool rnsCfgWrite(const QStringList &lines)
{
    QDir().mkpath(rnsDir());
    QFile f(rnsConfigPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    ts << lines.join("\n");
    f.close();
    return true;
}

// 0 = not a section header, 1 = [name], 2 = [[name]]
static int rnsHeaderLevel(const QString &line, QString *nameOut)
{
    const QString t = line.trimmed();
    if (t.startsWith("[[") && t.endsWith("]]") && t.length() > 4) {
        if (nameOut) *nameOut = t.mid(2, t.length() - 4).trimmed();
        return 2;
    }
    if (t.startsWith("[") && t.endsWith("]") && t.length() > 2 && !t.startsWith("[[")) {
        if (nameOut) *nameOut = t.mid(1, t.length() - 2).trimmed();
        return 1;
    }
    return 0;
}

// Half-open span [start, end) of a top-level section, header line included.
static bool rnsFindSection(const QStringList &lines, const QString &name,
                           int *start, int *end)
{
    QString n;
    for (int i = 0; i < lines.size(); ++i) {
        if (rnsHeaderLevel(lines[i], &n) == 1 && n.compare(name, Qt::CaseInsensitive) == 0) {
            int j = i + 1;
            while (j < lines.size() && rnsHeaderLevel(lines[j], nullptr) != 1)
                ++j;
            if (start) *start = i;
            if (end)   *end   = j;
            return true;
        }
    }
    return false;
}

struct RIface {
    QString name;
    QString type;
    bool    enabled = true;
    int     start   = -1;   // index of the [[name]] line
    int     end     = -1;   // one past the last line of the block
    QMap<QString, QString> kv;
};

static QVector<RIface> rnsCfgInterfaces(const QStringList &lines)
{
    QVector<RIface> out;
    int s = 0, e = 0;
    if (!rnsFindSection(lines, "interfaces", &s, &e))
        return out;

    QString n;
    for (int i = s + 1; i < e; ++i) {
        if (rnsHeaderLevel(lines[i], &n) != 2)
            continue;

        RIface f;
        f.name  = n;
        f.start = i;

        int j = i + 1;
        while (j < e && rnsHeaderLevel(lines[j], nullptr) != 2)
            ++j;
        f.end = j;

        for (int k = i + 1; k < j; ++k) {
            const QString t = lines[k].trimmed();
            if (t.isEmpty() || t.startsWith('#')) continue;
            const int eq = t.indexOf('=');
            if (eq < 0) continue;
            f.kv.insert(t.left(eq).trimmed().toLower(), t.mid(eq + 1).trimmed());
        }

        f.type = f.kv.value("type", "Unknown");
        const QString en = f.kv.value("interface_enabled",
                                      f.kv.value("enabled", "true")).toLower();
        f.enabled = !(en == "false" || en == "no" || en == "0" || en == "off");

        out.append(f);
        i = j - 1;
    }
    return out;
}

// Set (or insert) a key inside one interface block. Indentation of an existing
// key is preserved; new keys get the four-space style Reticulum itself writes.
static bool rnsCfgSetIfaceKey(QStringList &lines, const QString &ifname,
                              const QString &key, const QString &value)
{
    const QVector<RIface> ifs = rnsCfgInterfaces(lines);
    for (const RIface &f : ifs) {
        if (f.name != ifname) continue;

        for (int k = f.start + 1; k < f.end; ++k) {
            const QString t = lines[k].trimmed();
            if (t.isEmpty() || t.startsWith('#')) continue;
            const int eq = t.indexOf('=');
            if (eq < 0) continue;
            if (t.left(eq).trimmed().compare(key, Qt::CaseInsensitive) != 0) continue;

            const int indent = lines[k].length() - lines[k].midRef(0).trimmed().length();
            lines[k] = QString(indent > 0 ? indent : 4, ' ') + key + " = " + value;
            return true;
        }

        lines.insert(f.start + 1, QString(4, ' ') + key + " = " + value);
        return true;
    }
    return false;
}

static bool rnsCfgRemoveIface(QStringList &lines, const QString &ifname)
{
    const QVector<RIface> ifs = rnsCfgInterfaces(lines);
    for (const RIface &f : ifs) {
        if (f.name != ifname) continue;
        for (int k = f.end - 1; k >= f.start; --k)
            lines.removeAt(k);
        return true;
    }
    return false;
}

// Appends a new [[name]] block at the end of the [interfaces] section,
// creating that section if the config does not have one yet.
static bool rnsCfgAddIface(QStringList &lines, const QString &name,
                           const QStringList &optionLines)
{
    QStringList block;
    block << QString();
    block << QString(2, ' ') + "[[" + name + "]]";
    for (const QString &o : optionLines)
        block << QString(4, ' ') + o;

    int s = 0, e = 0;
    if (!rnsFindSection(lines, "interfaces", &s, &e)) {
        if (!lines.isEmpty() && !lines.last().trimmed().isEmpty())
            lines << QString();
        lines << "[interfaces]";
        lines << block;
        return true;
    }

    for (int i = 0; i < block.size(); ++i)
        lines.insert(e + i, block.at(i));
    return true;
}

// Same idea one level up, for keys in [reticulum] such as enable_transport.
static bool rnsCfgSetTopKey(QStringList &lines, const QString &section,
                            const QString &key, const QString &value)
{
    int s = 0, e = 0;
    if (!rnsFindSection(lines, section, &s, &e)) {
        if (!lines.isEmpty() && !lines.last().trimmed().isEmpty())
            lines << QString();
        lines << "[" + section + "]";
        lines << QString(2, ' ') + key + " = " + value;
        return true;
    }

    for (int k = s + 1; k < e; ++k) {
        const QString t = lines[k].trimmed();
        if (t.isEmpty() || t.startsWith('#')) continue;
        if (rnsHeaderLevel(lines[k], nullptr) == 2) break;   // into the sub-sections
        const int eq = t.indexOf('=');
        if (eq < 0) continue;
        if (t.left(eq).trimmed().compare(key, Qt::CaseInsensitive) != 0) continue;

        const int indent = lines[k].length() - lines[k].midRef(0).trimmed().length();
        lines[k] = QString(indent > 0 ? indent : 2, ' ') + key + " = " + value;
        return true;
    }

    lines.insert(s + 1, QString(2, ' ') + key + " = " + value);
    return true;
}

static QString rnsCfgGetTopKey(const QStringList &lines, const QString &section,
                               const QString &key, const QString &fallback)
{
    int s = 0, e = 0;
    if (!rnsFindSection(lines, section, &s, &e))
        return fallback;

    for (int k = s + 1; k < e; ++k) {
        const QString t = lines[k].trimmed();
        if (t.isEmpty() || t.startsWith('#')) continue;
        if (rnsHeaderLevel(lines[k], nullptr) == 2) break;
        const int eq = t.indexOf('=');
        if (eq < 0) continue;
        if (t.left(eq).trimmed().compare(key, Qt::CaseInsensitive) == 0)
            return t.mid(eq + 1).trimmed();
    }
    return fallback;
}

// Written only when the user has no config at all. Matches the shape of the
// stock Reticulum default so the file stays familiar to anyone who edits it by
// hand later. Transport is off: routing for other peers costs battery, and
// these are tablets.
static bool rnsCfgEnsure()
{
    if (QFile::exists(rnsConfigPath()))
        return true;

    QStringList d;
    d << "# Reticulum configuration, managed by Alternix Settings."
      << "# Hand edits are preserved: the settings page edits lines in place."
      << ""
      << "[reticulum]"
      << "  enable_transport = False"
      << "  share_instance = Yes"
      << "  shared_instance_port = 37428"
      << "  instance_control_port = 37429"
      << "  panic_on_interface_error = No"
      << ""
      << "[logging]"
      << "  loglevel = 4"
      << ""
      << "[interfaces]"
      << ""
      << "  [[Default Interface]]"
      << "    type = AutoInterface"
      << "    interface_enabled = True"
      << "";

    return rnsCfgWrite(d);
}

// =====================================================================
// rnstatus / rnpath parsing (for the info cards and the network map)
// =====================================================================
struct RnsIfaceStatus {
    QString type;      // AutoInterface, TCPClientInterface, "Shared Instance", ...
    QString name;      // text inside the brackets
    QString status;
    QString mode;
    QString rate;
    QString peers;
    QString traffic;
};

struct RnsPath {
    QString dest;
    QString via;
    QString iface;
    int     hops = 0;
};

// rnstatus prints one "Type[Name]" header per interface at column 0, followed
// by indented "Key : Value" lines.
static QVector<RnsIfaceStatus> rnsParseStatus(const QString &out, QString *transportHash)
{
    QVector<RnsIfaceStatus> res;
    QRegularExpression hdr("^([A-Za-z0-9_. ]+)\\[(.*)\\]\\s*$");
    QRegularExpression tr("Transport Instance\\s+<?([0-9a-fA-F]{4,})>?");

    if (transportHash) {
        const QRegularExpressionMatch m = tr.match(out);
        if (m.hasMatch()) *transportHash = m.captured(1);
    }

    const QStringList lines = out.split('\n');
    for (const QString &raw : lines) {
        if (raw.trimmed().isEmpty()) continue;

        const bool indented = raw.startsWith(' ') || raw.startsWith('\t');
        if (!indented) {
            const QRegularExpressionMatch m = hdr.match(raw.trimmed());
            if (m.hasMatch()) {
                RnsIfaceStatus s;
                s.type = m.captured(1).trimmed();
                s.name = m.captured(2).trimmed();
                res.append(s);
            }
            continue;
        }

        if (res.isEmpty()) continue;
        const QString t = raw.trimmed();
        const int colon = t.indexOf(':');
        if (colon < 0) {
            // Traffic spans two lines — the second has no key of its own.
            if (!res.last().traffic.isEmpty())
                res.last().traffic += "  " + t;
            continue;
        }

        const QString k = t.left(colon).trimmed().toLower();
        const QString v = t.mid(colon + 1).trimmed();
        RnsIfaceStatus &s = res.last();
        if      (k == "status")  s.status  = v;
        else if (k == "mode")    s.mode    = v;
        else if (k == "rate")    s.rate    = v;
        else if (k == "peers")   s.peers   = v;
        else if (k == "traffic") s.traffic = v;
    }
    return res;
}

// rnpath -t prints one row per known path:
//   <dest> is N hops away via <via> on Type[Name] expires <timestamp>
// The trailing " expires ..." clause has to be excluded explicitly, or it ends
// up glued onto the interface name and no path ever matches the interface it
// arrived on. The clause is optional because a single-destination lookup
// (rnpath <hash>) prints the same sentence without it. Note also that upstream
// emits two spaces in "1 hop  away" — hence \s+ rather than a literal space.
static QVector<RnsPath> rnsParsePaths(const QString &out)
{
    QVector<RnsPath> res;
    QRegularExpression re(
        "<([0-9a-fA-F]+)>\\s+is\\s+(\\d+)\\s+hop[s]?\\s+away\\s+via\\s+<([0-9a-fA-F]+)>"
        "\\s+on\\s+(.+?)(?:\\s+expires\\s+.*)?$");

    const QStringList lines = out.split('\n');
    for (const QString &raw : lines) {
        const QRegularExpressionMatch m = re.match(raw.trimmed());
        if (!m.hasMatch()) continue;
        RnsPath p;
        p.dest  = m.captured(1);
        p.hops  = m.captured(2).toInt();
        p.via   = m.captured(3);
        p.iface = m.captured(4).trimmed();
        res.append(p);
    }
    return res;
}

static QString shortHash(const QString &h, int n = 8)
{
    return h.length() > n ? h.left(n) : h;
}

// Strip the "Type[" ... "]" wrapper rnpath puts around interface names.
static QString ifaceDisplayName(const QString &full)
{
    const int lb = full.indexOf('[');
    const int rb = full.lastIndexOf(']');
    if (lb >= 0 && rb > lb)
        return full.mid(lb + 1, rb - lb - 1);
    return full;
}

// =====================================================================
// Network map overlay
//
// A child of the settings window rather than a frameless top-level, so it
// covers the page reliably under a tiling WM (Qtile) without needing any
// floating rules.
// =====================================================================
class NetworkMapOverlay : public QWidget
{
public:
    explicit NetworkMapOverlay(QWidget *parent);
    ~NetworkMapOverlay() override;

    void reload();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    QVector<RnsIfaceStatus> m_ifaces;
    QVector<RnsPath>        m_paths;
    QString                 m_transport;
    QString                 m_error;

    QPushButton *m_close   = nullptr;
    QPushButton *m_refresh = nullptr;
    QWidget     *m_watched = nullptr;

    void layoutButtons();
};

NetworkMapOverlay::NetworkMapOverlay(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_DeleteOnClose, true);
    setAutoFillBackground(false);
    setStyleSheet("color:white; font-family:Sans;");

    m_close = new QPushButton("Close", this);
    m_close->setStyleSheet(altBtnStyle("white"));
    m_close->setFixedSize(180, 56);
    connect(m_close, &QPushButton::clicked, this, [this]() { close(); });

    m_refresh = new QPushButton("Refresh", this);
    m_refresh->setStyleSheet(altBtnStyle("white"));
    m_refresh->setFixedSize(180, 56);
    connect(m_refresh, &QPushButton::clicked, this, [this]() { reload(); });

    if (parent) {
        m_watched = parent;
        m_watched->installEventFilter(this);
        setGeometry(parent->rect());
    }
}

NetworkMapOverlay::~NetworkMapOverlay()
{
    if (m_watched)
        m_watched->removeEventFilter(this);
}

bool NetworkMapOverlay::eventFilter(QObject *o, QEvent *e)
{
    if (o == m_watched && e->type() == QEvent::Resize && m_watched)
        setGeometry(m_watched->rect());
    return QWidget::eventFilter(o, e);
}

void NetworkMapOverlay::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    layoutButtons();
}

void NetworkMapOverlay::layoutButtons()
{
    const int y = height() - 78;
    if (m_refresh) m_refresh->move(width() / 2 - 190, y);
    if (m_close)   m_close->move(width() / 2 + 10, y);
}

void NetworkMapOverlay::reload()
{
    m_ifaces.clear();
    m_paths.clear();
    m_transport.clear();
    m_error.clear();

    if (!rnsInstalled()) {
        m_error = "Reticulum is not installed.\nUse Install / setup on the Reticulum page.";
        update();
        return;
    }
    if (!rnsRunning()) {
        m_error = "Reticulum is not running.\nStart it on the Reticulum page, then refresh.";
        update();
        return;
    }

    const QString status = runCmd("rnstatus 2>&1", 12000);
    m_ifaces = rnsParseStatus(status, &m_transport);
    if (m_ifaces.isEmpty()) {
        m_error = "rnstatus returned nothing usable:\n\n" + status.left(400);
        update();
        return;
    }

    m_paths = rnsParsePaths(runCmd("rnpath -t 2>&1", 12000));
    update();
}

void NetworkMapOverlay::paintEvent(QPaintEvent *e)
{
    Q_UNUSED(e);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor("#1e1e1e"));

    QFont title("DejaVu Sans");
    title.setPointSize(20);
    title.setBold(true);
    p.setFont(title);
    p.setPen(QColor("#ffffff"));
    p.drawText(QRectF(0, 18, width(), 44), Qt::AlignCenter, "Reticulum network");

    QFont body("DejaVu Sans");
    body.setPointSize(12);

    if (!m_error.isEmpty()) {
        p.setFont(body);
        p.setPen(QColor("#FFAA00"));
        p.drawText(QRectF(40, 0, width() - 80, height()),
                   Qt::AlignCenter | Qt::TextWordWrap, m_error);
        return;
    }

    // Real interfaces only — the shared-instance socket is not a link.
    QVector<RnsIfaceStatus> links;
    for (const RnsIfaceStatus &s : m_ifaces) {
        if (s.type.compare("Shared Instance", Qt::CaseInsensitive) == 0) continue;
        links.append(s);
    }

    // A tall, narrow tablet window suits a top-down tree far better than a
    // radial layout: three interface nodes wide enough to hold a real
    // interface name cannot clear a centre node inside 800px.
    const int topBar = 84;
    const int botBar = 112;
    const QRectF area(0, topBar, width(), qMax(200, height() - topBar - botBar));

    // Group paths by the interface they arrive on.
    QVector<QVector<int>> byIface(links.size());
    QVector<int> orphans;
    for (int i = 0; i < m_paths.size(); ++i) {
        const QString disp = ifaceDisplayName(m_paths[i].iface);
        int hit = -1;
        for (int k = 0; k < links.size(); ++k) {
            if (links[k].name == disp || m_paths[i].iface.contains("[" + links[k].name + "]")) {
                hit = k;
                break;
            }
        }
        if (hit >= 0) byIface[hit].append(i);
        else          orphans.append(i);
    }

    QFont bold = body;
    bold.setBold(true);

    p.save();
    p.setClipRect(area);

    const double leftPad = 40;
    const double spineX  = leftPad + 32;

    // ---- this device ----
    QRectF dev(width() / 2.0 - 170, area.top() + 8, 340, 78);
    p.setPen(QPen(QColor("#FFAA00"), 3));
    p.setBrush(QColor("#444444"));
    p.drawRoundedRect(dev, 18, 18);

    p.setFont(bold);
    p.setPen(QColor("#ffffff"));
    p.drawText(QRectF(dev.x(), dev.y() + 10, dev.width(), 24), Qt::AlignCenter, "This device");
    p.setFont(body);
    p.setPen(QColor("#FFAA00"));
    p.drawText(QRectF(dev.x(), dev.y() + 40, dev.width(), 24), Qt::AlignCenter,
               m_transport.isEmpty() ? "no identity yet" : shortHash(m_transport, 20));

    // Elbow down from the device card, then left onto the spine.
    const double elbowY = dev.bottom() + 18;
    p.setPen(QPen(QColor("#555555"), 2));
    p.drawLine(QPointF(dev.center().x(), dev.bottom()), QPointF(dev.center().x(), elbowY));
    p.drawLine(QPointF(dev.center().x(), elbowY), QPointF(spineX, elbowY));

    double y = elbowY;
    double spineBottom = elbowY;
    bool truncated = false;

    const int n = links.size();
    const int rows = n + (orphans.isEmpty() ? 0 : 1);

    if (rows == 0) {
        p.setFont(body);
        p.setPen(QColor("#FFAA00"));
        p.drawText(QRectF(40, dev.bottom() + 50, width() - 80, 60),
                   Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                   "No interfaces are configured. Add one from the Interfaces page.");
    }

    for (int r = 0; r < rows; ++r) {
        const bool isOrphan = (r >= n);
        const QString rname = isOrphan ? QStringLiteral("Other links") : links[r].name;
        const QString rtype = isOrphan ? QStringLiteral("interface not reported by rnstatus")
                                       : links[r].type;
        const QString rstat = isOrphan
                                ? QStringLiteral("unmatched")
                                : (links[r].peers.isEmpty() ? links[r].status : links[r].peers);
        const bool up = isOrphan ? false : links[r].status.startsWith("Up", Qt::CaseInsensitive);
        const QColor col = isOrphan ? QColor("#888888") : QColor(up ? "#7CFC00" : "#CC6666");
        const QVector<int> &kids = isOrphan ? orphans : byIface[r];

        y += 30;
        const QRectF box(spineX + 46, y, width() - (spineX + 46) - leftPad, 64);

        p.setPen(QPen(QColor("#555555"), 2));
        p.drawLine(QPointF(spineX, box.center().y()), QPointF(box.left(), box.center().y()));
        spineBottom = box.center().y();

        p.setPen(QPen(col, 2));
        p.setBrush(QColor("#3a3a3a"));
        p.drawRoundedRect(box, 16, 16);

        const double textW = box.width() - 230;
        p.setFont(bold);
        p.setPen(QColor("#ffffff"));
        p.drawText(QRectF(box.x() + 20, box.y() + 8, textW, 24),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(bold).elidedText(rname, Qt::ElideRight, int(textW)));

        p.setFont(body);
        p.setPen(QColor("#999999"));
        p.drawText(QRectF(box.x() + 20, box.y() + 34, textW, 22),
                   Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(body).elidedText(rtype, Qt::ElideRight, int(textW)));

        p.setPen(col);
        p.drawText(QRectF(box.right() - 200, box.y(), 180, box.height()),
                   Qt::AlignVCenter | Qt::AlignRight, rstat);

        y = box.bottom();

        // Destinations reachable through this interface, as a wrapped row of
        // chips underneath it.
        if (!kids.isEmpty()) {
            const int chipW  = 172;
            const int chipH  = 34;
            const int perRow = qMax(1, int((box.width() - 48) / chipW));
            const int show   = qMin(kids.size(), 12);
            if (show < kids.size()) truncated = true;

            for (int j = 0; j < show; ++j) {
                const RnsPath &path = m_paths[kids[j]];
                const double cx = box.x() + 26 + (j % perRow) * chipW;
                const double cy = y + 24 + (j / perRow) * chipH;

                p.setPen(Qt::NoPen);
                p.setBrush(isOrphan ? QColor("#888888") : QColor("#66AAFF"));
                p.drawEllipse(QPointF(cx, cy), 6, 6);

                p.setFont(body);
                p.setPen(QColor("#cccccc"));
                p.drawText(QRectF(cx + 14, cy - 12, chipW - 22, 24),
                           Qt::AlignVCenter | Qt::AlignLeft,
                           shortHash(path.dest) + "  " + QString::number(path.hops) + "h");
            }
            y += 12 + ((show + perRow - 1) / perRow) * chipH;
        }
    }

    p.setPen(QPen(QColor("#555555"), 2));
    p.drawLine(QPointF(spineX, elbowY), QPointF(spineX, spineBottom));
    p.restore();

    // ---- legend ----
    p.setFont(body);
    p.setPen(QColor("#bbbbbb"));
    QString legend = QString("%1 interface%2 · %3 known destination%4   —   Nh = hops away")
                       .arg(n).arg(n == 1 ? "" : "s")
                       .arg(m_paths.size()).arg(m_paths.size() == 1 ? "" : "s");
    if (truncated)
        legend += "  (list shortened)";
    p.drawText(QRectF(20, height() - 132, width() - 40, 26), Qt::AlignCenter, legend);
}

// =====================================================================
// ReticulumPage
// =====================================================================
class ReticulumPage : public QWidget
{
public:
    explicit ReticulumPage(QStackedWidget *stack);

private:
    QStackedWidget *m_stack = nullptr;   // osm-settings page stack
    QStackedWidget *m_inner = nullptr;   // main / setup / interfaces
    QLabel *m_title = nullptr;

    // main view
    QLabel *m_mainInfo = nullptr;
    QLabel *m_netInfo  = nullptr;

    // setup view
    QLabel      *m_setupDetail  = nullptr;
    QLabel      *m_setupStatus  = nullptr;
    QPushButton *m_btnInstall   = nullptr;
    QPushButton *m_btnRemove    = nullptr;
    QPushButton *m_btnBootOn    = nullptr;
    QPushButton *m_btnBootOff   = nullptr;

    // interfaces view
    QListWidget *m_ifList       = nullptr;
    QLabel      *m_ifStatus     = nullptr;
    QPushButton *m_btnTransport = nullptr;

    // construction
    QWidget *buildMainPage();
    QWidget *buildSetupPage();
    QWidget *buildInterfacesPage();
    void gotoPage(int idx);

    // helpers
    bool runTask(QLabel *status, const QString &busyText, const QString &cmd,
                 QString &output, int timeoutMs);
    void reportTask(QLabel *status, const QString &what, bool ok, const QString &output);

    QString buildMainInfoHtml();
    QString buildNetInfoHtml();
    QString buildSetupInfoHtml();

    void refreshMain();
    void refreshSetup();
    void refreshInterfaces();

    // actions
    void startRns();
    void stopRns();
    void restartRns();
    void showMap();

    void installReticulum();
    void removeReticulum();
    void setBootEnabled(bool on);

    void addAutoInterface();
    void addTcpInterface();
    void addRNodeInterface();
    void toggleSelectedInterface();
    void removeSelectedInterface();
    void toggleTransport();

    QString selectedInterfaceName() const;
};

// -----------------------------------------------------
// Constructor
// -----------------------------------------------------
ReticulumPage::ReticulumPage(QStackedWidget *stack)
    : QWidget(stack), m_stack(stack)
{
    setStyleSheet("background:#282828; color:white; font-family:Sans;");

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(40, 40, 40, 40);
    root->setSpacing(10);

    m_title = new QLabel("Reticulum");
    m_title->setAlignment(Qt::AlignCenter);
    m_title->setStyleSheet("font-size:42px; font-weight:bold;");
    root->addWidget(m_title);

    m_inner = new QStackedWidget(this);
    m_inner->addWidget(buildMainPage());        // 0
    m_inner->addWidget(buildSetupPage());       // 1
    m_inner->addWidget(buildInterfacesPage());  // 2
    root->addWidget(m_inner);

    // Back leaves a sub-view first, then the page
    QPushButton *back = makeBtn("❮");
    back->setFixedSize(140, 60);
    connect(back, &QPushButton::clicked, this, [this]() {
        if (m_inner && m_inner->currentIndex() != 0) {
            gotoPage(0);
            return;
        }
        if (m_stack)
            m_stack->setCurrentIndex(0);
    });
    root->addWidget(back, 0, Qt::AlignCenter);

    refreshMain();
}

// -----------------------------------------------------
// Main view
// -----------------------------------------------------
QWidget* ReticulumPage::buildMainPage()
{
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

    // ---- Stack card ----
    QFrame *cardStack = new QFrame(outer);
    cardStack->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *sLay = new QVBoxLayout(cardStack);
    sLay->setContentsMargins(30, 30, 30, 30);
    sLay->setSpacing(16);

    QLabel *sLabel = new QLabel("Mesh network (Reticulum)");
    sLabel->setAlignment(Qt::AlignCenter);
    sLabel->setStyleSheet("font-size:28px; font-weight:bold;");
    sLay->addWidget(sLabel);

    m_mainInfo = new QLabel("Loading...");
    m_mainInfo->setWordWrap(true);
    m_mainInfo->setAlignment(Qt::AlignCenter);
    m_mainInfo->setStyleSheet(
        "QLabel { background:#3a3a3a; border-radius:20px; font-size:24px; padding:20px; }");
    sLay->addWidget(m_mainInfo);

    QHBoxLayout *sRow1 = new QHBoxLayout();
    sRow1->setSpacing(16);
    QPushButton *btnStart = makeBtn("Start");
    QPushButton *btnStop  = makeBtn("Stop", "#CC6666");
    sRow1->addWidget(btnStart);
    sRow1->addWidget(btnStop);
    sLay->addLayout(sRow1);

    QHBoxLayout *sRow2 = new QHBoxLayout();
    sRow2->setSpacing(16);
    QPushButton *btnSetup   = makeBtn("Install / setup");
    QPushButton *btnRefresh = makeBtn("Refresh");
    sRow2->addWidget(btnSetup);
    sRow2->addWidget(btnRefresh);
    sLay->addLayout(sRow2);

    outerLay->addWidget(cardStack);

    // ---- Network card ----
    QFrame *cardNet = new QFrame(outer);
    cardNet->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *nLay = new QVBoxLayout(cardNet);
    nLay->setContentsMargins(30, 30, 30, 30);
    nLay->setSpacing(16);

    QLabel *nLabel = new QLabel("Network");
    nLabel->setAlignment(Qt::AlignCenter);
    nLabel->setStyleSheet("font-size:28px; font-weight:bold;");
    nLay->addWidget(nLabel);

    m_netInfo = new QLabel("Loading...");
    m_netInfo->setWordWrap(true);
    m_netInfo->setAlignment(Qt::AlignCenter);
    m_netInfo->setStyleSheet(
        "QLabel { background:#3a3a3a; border-radius:20px; font-size:24px; padding:20px; }");
    nLay->addWidget(m_netInfo);

    QHBoxLayout *nRow = new QHBoxLayout();
    nRow->setSpacing(16);
    QPushButton *btnMap = makeBtn("Network map");
    QPushButton *btnIfs = makeBtn("Interfaces");
    nRow->addWidget(btnMap);
    nRow->addWidget(btnIfs);
    nLay->addLayout(nRow);

    outerLay->addWidget(cardNet);

    wrapLay->addWidget(outer);
    wrapLay->addStretch();
    scroll->setWidget(wrap);

    connect(btnStart,   &QPushButton::clicked, this, [this]() { startRns(); refreshMain(); });
    connect(btnStop,    &QPushButton::clicked, this, [this]() { stopRns();  refreshMain(); });
    connect(btnRefresh, &QPushButton::clicked, this, [this]() { refreshMain(); });
    connect(btnSetup,   &QPushButton::clicked, this, [this]() { gotoPage(1); });
    connect(btnIfs,     &QPushButton::clicked, this, [this]() { gotoPage(2); });
    connect(btnMap,     &QPushButton::clicked, this, [this]() { showMap(); });

    return scroll;
}

// -----------------------------------------------------
// Setup view
// -----------------------------------------------------
QWidget* ReticulumPage::buildSetupPage()
{
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

    // ---- Status card ----
    QFrame *cardStatus = new QFrame(outer);
    cardStatus->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *stLay = new QVBoxLayout(cardStatus);
    stLay->setContentsMargins(30, 30, 30, 30);
    stLay->setSpacing(16);

    QLabel *stTitle = new QLabel("Status");
    stTitle->setAlignment(Qt::AlignCenter);
    stTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    stLay->addWidget(stTitle);

    m_setupDetail = new QLabel("Loading...");
    m_setupDetail->setWordWrap(true);
    m_setupDetail->setAlignment(Qt::AlignCenter);
    m_setupDetail->setStyleSheet(
        "QLabel { background:#3a3a3a; border-radius:20px; font-size:24px; padding:20px; }");
    stLay->addWidget(m_setupDetail);

    m_setupStatus = new QLabel("");
    m_setupStatus->setWordWrap(true);
    m_setupStatus->setAlignment(Qt::AlignCenter);
    m_setupStatus->setStyleSheet("QLabel { font-size:22px; }");
    m_setupStatus->setVisible(false);
    stLay->addWidget(m_setupStatus);

    outerLay->addWidget(cardStatus);

    // ---- Install card ----
    QFrame *cardInstall = new QFrame(outer);
    cardInstall->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *iLay = new QVBoxLayout(cardInstall);
    iLay->setContentsMargins(30, 30, 30, 30);
    iLay->setSpacing(16);

    QLabel *iTitle = new QLabel("Install");
    iTitle->setAlignment(Qt::AlignCenter);
    iTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    iLay->addWidget(iTitle);

    QLabel *iHelp = new QLabel(
        "Reticulum is an encrypted mesh network that runs on top of Wi-Fi, "
        "Ethernet, the internet or LoRa radio. It is optional — install it only "
        "if you want mesh networking. This adds Python packages and about 40 MB.");
    iHelp->setWordWrap(true);
    iHelp->setAlignment(Qt::AlignCenter);
    iHelp->setStyleSheet(
        "QLabel { background:#3a3a3a; border-radius:20px; font-size:22px; padding:16px; }");
    iLay->addWidget(iHelp);

    QHBoxLayout *iRow = new QHBoxLayout();
    iRow->setSpacing(16);
    m_btnInstall = makeBtn("Install Reticulum");
    m_btnRemove  = makeBtn("Remove Reticulum", "#CC6666");
    iRow->addWidget(m_btnInstall);
    iRow->addWidget(m_btnRemove);
    iLay->addLayout(iRow);

    outerLay->addWidget(cardInstall);

    // ---- Boot card ----
    QFrame *cardBoot = new QFrame(outer);
    cardBoot->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *bLay = new QVBoxLayout(cardBoot);
    bLay->setContentsMargins(30, 30, 30, 30);
    bLay->setSpacing(16);

    QLabel *bTitle = new QLabel("Start at boot");
    bTitle->setAlignment(Qt::AlignCenter);
    bTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    bLay->addWidget(bTitle);

    QLabel *bHelp = new QLabel(
        "Runs the Reticulum daemon in the background from boot, as your user, so "
        "mesh apps can reach the network without opening this page. Leaving it "
        "off saves a little battery.");
    bHelp->setWordWrap(true);
    bHelp->setAlignment(Qt::AlignCenter);
    bHelp->setStyleSheet(
        "QLabel { background:#3a3a3a; border-radius:20px; font-size:22px; padding:16px; }");
    bLay->addWidget(bHelp);

    QHBoxLayout *bRow = new QHBoxLayout();
    bRow->setSpacing(16);
    m_btnBootOn  = makeBtn("Enable at boot");
    m_btnBootOff = makeBtn("Disable at boot", "#CC6666");
    bRow->addWidget(m_btnBootOn);
    bRow->addWidget(m_btnBootOff);
    bLay->addLayout(bRow);

    outerLay->addWidget(cardBoot);

    wrapLay->addWidget(outer);
    wrapLay->addStretch();
    scroll->setWidget(wrap);

    connect(m_btnInstall, &QPushButton::clicked, this, [this]() { installReticulum(); });
    connect(m_btnRemove,  &QPushButton::clicked, this, [this]() { removeReticulum();  });
    connect(m_btnBootOn,  &QPushButton::clicked, this, [this]() { setBootEnabled(true);  });
    connect(m_btnBootOff, &QPushButton::clicked, this, [this]() { setBootEnabled(false); });

    return scroll;
}

// -----------------------------------------------------
// Interfaces view
// -----------------------------------------------------
QWidget* ReticulumPage::buildInterfacesPage()
{
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

    // ---- Interface list card ----
    QFrame *cardList = new QFrame(outer);
    cardList->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *lLay = new QVBoxLayout(cardList);
    lLay->setContentsMargins(30, 30, 30, 30);
    lLay->setSpacing(16);

    QLabel *lTitle = new QLabel("Configured interfaces");
    lTitle->setAlignment(Qt::AlignCenter);
    lTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    lLay->addWidget(lTitle);

    m_ifList = new QListWidget();
    m_ifList->setWordWrap(true);
    m_ifList->setFixedHeight(330);
    // Same treatment as every other scrollable surface in Settings: no visible
    // bars, drag to scroll. A by-id serial path is long enough to raise a
    // horizontal bar otherwise.
    m_ifList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_ifList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QScroller::grabGesture(m_ifList->viewport(), QScroller::LeftMouseButtonGesture);
    m_ifList->setStyleSheet(
        "QListWidget {"
        " background:#3a3a3a;"
        " color:white;"
        " border-radius:20px;"
        " font-size:24px;"
        " padding-left:14px;"
        " padding-right:14px;"
        "}"
        "QListWidget::item { padding:14px; border-radius:16px; }"
        "QListWidget::item:selected { background:#555555; border-radius:16px; }"
    );
    lLay->addWidget(m_ifList);

    m_ifStatus = new QLabel("");
    m_ifStatus->setWordWrap(true);
    m_ifStatus->setAlignment(Qt::AlignCenter);
    m_ifStatus->setStyleSheet("QLabel { font-size:22px; }");
    m_ifStatus->setVisible(false);
    lLay->addWidget(m_ifStatus);

    QHBoxLayout *lRow1 = new QHBoxLayout();
    lRow1->setSpacing(16);
    QPushButton *btnToggle = makeBtn("Enable / disable");
    QPushButton *btnDelete = makeBtn("Remove", "#CC6666");
    lRow1->addWidget(btnToggle);
    lRow1->addWidget(btnDelete);
    lLay->addLayout(lRow1);

    outerLay->addWidget(cardList);

    // ---- Add card ----
    QFrame *cardAdd = new QFrame(outer);
    cardAdd->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *aLay = new QVBoxLayout(cardAdd);
    aLay->setContentsMargins(30, 30, 30, 30);
    aLay->setSpacing(16);

    QLabel *aTitle = new QLabel("Add an interface");
    aTitle->setAlignment(Qt::AlignCenter);
    aTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    aLay->addWidget(aTitle);

    QLabel *aHelp = new QLabel(
        "Auto finds other Reticulum devices on the same Wi-Fi or Ethernet with no "
        "configuration. TCP connects over the internet to a hub you know. RNode "
        "uses a LoRa radio plugged into USB, which works with no infrastructure "
        "at all.");
    aHelp->setWordWrap(true);
    aHelp->setAlignment(Qt::AlignCenter);
    aHelp->setStyleSheet(
        "QLabel { background:#3a3a3a; border-radius:20px; font-size:22px; padding:16px; }");
    aLay->addWidget(aHelp);

    QHBoxLayout *aRow1 = new QHBoxLayout();
    aRow1->setSpacing(16);
    QPushButton *btnAuto = makeBtn("Add Auto (local)");
    QPushButton *btnTcp  = makeBtn("Add TCP (internet)");
    aRow1->addWidget(btnAuto);
    aRow1->addWidget(btnTcp);
    aLay->addLayout(aRow1);

    QHBoxLayout *aRow2 = new QHBoxLayout();
    aRow2->setSpacing(16);
    QPushButton *btnRnode = makeBtn("Add RNode (LoRa)");
    QPushButton *btnApply = makeBtn("Apply / restart");
    aRow2->addWidget(btnRnode);
    aRow2->addWidget(btnApply);
    aLay->addLayout(aRow2);

    outerLay->addWidget(cardAdd);

    // ---- Transport card ----
    QFrame *cardTr = new QFrame(outer);
    cardTr->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");
    QVBoxLayout *tLay = new QVBoxLayout(cardTr);
    tLay->setContentsMargins(30, 30, 30, 30);
    tLay->setSpacing(16);

    QLabel *tTitle = new QLabel("Transport node");
    tTitle->setAlignment(Qt::AlignCenter);
    tTitle->setStyleSheet("font-size:28px; font-weight:bold;");
    tLay->addWidget(tTitle);

    QLabel *tHelp = new QLabel(
        "A transport node relays traffic for other people on the mesh and answers "
        "path requests, which extends everyone's range. It uses more battery and "
        "more bandwidth, so leave it off on a tablet that runs on battery.");
    tHelp->setWordWrap(true);
    tHelp->setAlignment(Qt::AlignCenter);
    tHelp->setStyleSheet(
        "QLabel { background:#3a3a3a; border-radius:20px; font-size:22px; padding:16px; }");
    tLay->addWidget(tHelp);

    m_btnTransport = makeBtn("Transport: off");
    tLay->addWidget(m_btnTransport);

    outerLay->addWidget(cardTr);

    wrapLay->addWidget(outer);
    wrapLay->addStretch();
    scroll->setWidget(wrap);

    connect(btnToggle, &QPushButton::clicked, this, [this]() { toggleSelectedInterface(); });
    connect(btnDelete, &QPushButton::clicked, this, [this]() { removeSelectedInterface(); });
    connect(btnAuto,   &QPushButton::clicked, this, [this]() { addAutoInterface();  });
    connect(btnTcp,    &QPushButton::clicked, this, [this]() { addTcpInterface();   });
    connect(btnRnode,  &QPushButton::clicked, this, [this]() { addRNodeInterface(); });
    connect(btnApply,  &QPushButton::clicked, this, [this]() { restartRns(); refreshInterfaces(); });
    connect(m_btnTransport, &QPushButton::clicked, this, [this]() { toggleTransport(); });

    return scroll;
}

void ReticulumPage::gotoPage(int idx)
{
    if (!m_inner) return;
    m_inner->setCurrentIndex(idx);

    if (m_title) {
        if (idx == 1)      m_title->setText("Reticulum setup");
        else if (idx == 2) m_title->setText("Interfaces");
        else               m_title->setText("Reticulum");
    }

    if (idx == 1)      refreshSetup();
    else if (idx == 2) refreshInterfaces();
    else               refreshMain();
}

// -----------------------------------------------------
// Task helpers (same polling approach as emulation.cpp)
// -----------------------------------------------------
bool ReticulumPage::runTask(QLabel *status, const QString &busyText, const QString &cmd,
                            QString &output, int timeoutMs)
{
    QProcess p;
    p.start("/bin/sh", {"-c", cmd});
    if (!p.waitForStarted(5000)) {
        output = "Could not start /bin/sh.";
        return false;
    }

    setEnabled(false);
    if (status) {
        status->setVisible(true);
        status->setText(QString("<span style='color:#FFAA00;'>%1</span>")
                        .arg(busyText.toHtmlEscaped()));
    }

    QElapsedTimer timer;
    timer.start();
    qint64 lastTick = -1;
    bool timedOut = false;

    while (p.state() != QProcess::NotRunning) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        p.waitForFinished(50);

        const qint64 tick = timer.elapsed() / 500;
        if (status && tick != lastTick) {
            lastTick = tick;
            status->setText(QString("<span style='color:#FFAA00;'>%1 (%2s)</span>")
                            .arg(busyText.toHtmlEscaped())
                            .arg(timer.elapsed() / 1000));
        }

        if (timeoutMs > 0 && timer.elapsed() > timeoutMs) {
            p.kill();
            p.waitForFinished(2000);
            timedOut = true;
            break;
        }
    }

    output = QString::fromLocal8Bit(p.readAllStandardOutput())
           + QString::fromLocal8Bit(p.readAllStandardError());

    setEnabled(true);

    if (timedOut) {
        output += QString("\n\nTimed out after %1 seconds.").arg(timeoutMs / 1000);
        return false;
    }

    return (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
}

void ReticulumPage::reportTask(QLabel *status, const QString &what, bool ok,
                               const QString &output)
{
    if (status) {
        status->setVisible(true);
        status->setText(ok
            ? QString("<span style='color:#7CFC00;'>%1: done.</span>").arg(what.toHtmlEscaped())
            : QString("<span style='color:#FF5555;'>%1: FAILED. See details below.</span>")
                  .arg(what.toHtmlEscaped()));
    }

    if (!ok) {
        QString tail = output.trimmed();
        if (tail.length() > 2000)
            tail = tail.right(2000);
        if (tail.isEmpty())
            tail = "(no output)";
        QMessageBox::warning(this, what + " failed", what + " failed.\n\n" + tail);
    }
}

// -----------------------------------------------------
// Info panels
// -----------------------------------------------------
QString ReticulumPage::buildMainInfoHtml()
{
    if (!rnsInstalled()) {
        return "Reticulum is not installed.<br>"
               "<span style='color:#FFAA00;'>Use Install / setup to add mesh networking.</span>";
    }

    const QString ver = runCmd("rnsd --version 2>&1 | head -n1").trimmed();
    QString html = "Version: " + (ver.isEmpty() ? QString("unknown") : ver.toHtmlEscaped()) + "<br>";

    if (!rnsRunning()) {
        html += "<span style='color:#FFAA00;'>Stopped.</span> "
                "Press Start to bring the mesh up.";
        return html;
    }

    QString transport;
    const QVector<RnsIfaceStatus> ifs =
        rnsParseStatus(runCmd("rnstatus 2>&1", 12000), &transport);

    html += "<span style='color:#7CFC00;'>Running.</span><br>";
    if (!transport.isEmpty())
        html += "Identity: " + shortHash(transport, 16) + "<br>";

    int up = 0, total = 0;
    for (const RnsIfaceStatus &s : ifs) {
        if (s.type.compare("Shared Instance", Qt::CaseInsensitive) == 0) continue;
        ++total;
        if (s.status.startsWith("Up", Qt::CaseInsensitive)) ++up;
    }
    html += QString("Interfaces: %1 of %2 up").arg(up).arg(total);

    if (rnsBootEnabled())
        html += "<br>Starts at boot.";

    return html;
}

QString ReticulumPage::buildNetInfoHtml()
{
    if (!rnsInstalled())
        return "<span style='color:#FFAA00;'>Not installed yet.</span>";

    if (!rnsRunning())
        return "Reticulum is stopped — no network information available.";

    const QVector<RnsPath> paths = rnsParsePaths(runCmd("rnpath -t 2>&1", 12000));

    QString transport;
    const QVector<RnsIfaceStatus> ifs =
        rnsParseStatus(runCmd("rnstatus 2>&1", 12000), &transport);

    QString peersLine;
    for (const RnsIfaceStatus &s : ifs) {
        if (s.type.compare("Shared Instance", Qt::CaseInsensitive) == 0) continue;
        if (s.peers.isEmpty()) continue;
        peersLine += s.name.toHtmlEscaped() + ": " + s.peers.toHtmlEscaped() + "<br>";
    }

    QString html;
    html += QString("Known destinations: %1<br>").arg(paths.size());
    if (!peersLine.isEmpty())
        html += peersLine;
    if (paths.isEmpty())
        html += "<span style='color:#FFAA00;'>Nothing discovered yet — this is "
                "normal until another Reticulum device is reachable.</span>";

    return html;
}

QString ReticulumPage::buildSetupInfoHtml()
{
    if (!rnsInstalled()) {
        return "Not installed.<br>"
               "<span style='color:#FFAA00;'>Install adds python3-cryptography and "
               "python3-serial from apt, then the rns package into "
               "/opt/reticulum/venv.</span>";
    }

    const QString ver = runCmd("rnsd --version 2>&1 | head -n1").trimmed();
    QString html = "<span style='color:#7CFC00;'>Installed.</span><br>";
    if (!ver.isEmpty())
        html += ver.toHtmlEscaped() + "<br>";

    html += QString("Daemon: %1<br>")
              .arg(rnsRunning() ? "<span style='color:#7CFC00;'>running</span>"
                                : "<span style='color:#FFAA00;'>stopped</span>");
    html += QString("At boot: %1<br>").arg(rnsBootEnabled() ? "enabled" : "disabled");
    html += "Config: " + rnsConfigPath().toHtmlEscaped();
    return html;
}

void ReticulumPage::refreshMain()
{
    if (m_mainInfo) m_mainInfo->setText(buildMainInfoHtml());
    if (m_netInfo)  m_netInfo->setText(buildNetInfoHtml());
}

void ReticulumPage::refreshSetup()
{
    if (m_setupDetail) m_setupDetail->setText(buildSetupInfoHtml());

    const bool installed = rnsInstalled();
    if (m_btnInstall)  m_btnInstall->setText(installed ? "Reinstall / update" : "Install Reticulum");
    if (m_btnRemove)   m_btnRemove->setEnabled(installed);
    if (m_btnBootOn)   m_btnBootOn->setEnabled(installed && !rnsBootEnabled());
    if (m_btnBootOff)  m_btnBootOff->setEnabled(installed && rnsBootEnabled());
}

void ReticulumPage::refreshInterfaces()
{
    if (!m_ifList) return;
    m_ifList->clear();

    if (!rnsInstalled()) {
        QListWidgetItem *it = new QListWidgetItem("Reticulum is not installed.");
        it->setForeground(QColor("#CC6666"));
        it->setFlags(it->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
        m_ifList->addItem(it);
        if (m_btnTransport) m_btnTransport->setEnabled(false);
        return;
    }

    rnsCfgEnsure();
    const QStringList lines = rnsCfgRead();
    const QVector<RIface> ifs = rnsCfgInterfaces(lines);

    if (ifs.isEmpty()) {
        QListWidgetItem *it = new QListWidgetItem("No interfaces configured.");
        it->setForeground(QColor("#FFAA00"));
        it->setFlags(it->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
        m_ifList->addItem(it);
    } else {
        for (const RIface &f : ifs) {
            QString extra;
            if (f.kv.contains("target_host"))
                extra = f.kv.value("target_host") + ":" + f.kv.value("target_port", "?");
            else if (f.kv.contains("port"))
                extra = QFileInfo(f.kv.value("port")).fileName();
            else if (f.kv.contains("devices"))
                extra = f.kv.value("devices");

            // A /dev/serial/by-id name is a single unbreakable token that word
            // wrap cannot split, so it has to be cut rather than wrapped.
            if (extra.length() > 30)
                extra = extra.left(29) + "…";
            if (!extra.isEmpty())
                extra.prepend("  ");

            QListWidgetItem *it = new QListWidgetItem(
                QString("%1%2\n%3%4")
                    .arg(f.enabled ? "" : "(off)  ")
                    .arg(f.name)
                    .arg(f.type)
                    .arg(extra));
            it->setForeground(QColor(f.enabled ? "#7CFC00" : "#999999"));
            it->setData(Qt::UserRole, f.name);
            m_ifList->addItem(it);
        }
    }

    const QString tr = rnsCfgGetTopKey(lines, "reticulum", "enable_transport", "False").toLower();
    const bool trOn = (tr == "true" || tr == "yes" || tr == "1" || tr == "on");
    if (m_btnTransport) {
        m_btnTransport->setEnabled(true);
        m_btnTransport->setText(trOn ? "Transport: on" : "Transport: off");
        m_btnTransport->setStyleSheet(altBtnStyle(trOn ? "#7CFC00" : "white"));
    }
}

// -----------------------------------------------------
// Daemon control
// -----------------------------------------------------
void ReticulumPage::startRns()
{
    if (!rnsInstalled()) {
        QMessageBox::information(this, "Reticulum",
            "Reticulum is not installed yet.\n\nUse Install / setup first.");
        return;
    }

    rnsCfgEnsure();

    const QString cmd = QString(SUDO_PREAMBLE) +
        "if [ -x /etc/init.d/rnsd ]; then\n"
        "  $SUDOX /etc/init.d/rnsd start\n"
        "else\n"
        "  echo 'ERROR: /etc/init.d/rnsd is missing. Run Install / setup again.'\n"
        "  exit 1\n"
        "fi\n";

    QString out;
    const bool ok = runTask(m_setupStatus, "Starting Reticulum", cmd, out, 60000);
    if (!ok)
        reportTask(m_setupStatus, "Start Reticulum", false, out);
}

void ReticulumPage::stopRns()
{
    const QString cmd = QString(SUDO_PREAMBLE) +
        "if [ -x /etc/init.d/rnsd ]; then $SUDOX /etc/init.d/rnsd stop; fi\n"
        "exit 0\n";

    QString out;
    runTask(m_setupStatus, "Stopping Reticulum", cmd, out, 60000);
}

void ReticulumPage::restartRns()
{
    if (!rnsInstalled()) return;

    const QString cmd = QString(SUDO_PREAMBLE) +
        "if [ -x /etc/init.d/rnsd ]; then\n"
        "  $SUDOX /etc/init.d/rnsd restart\n"
        "else\n"
        "  echo 'ERROR: /etc/init.d/rnsd is missing. Run Install / setup again.'\n"
        "  exit 1\n"
        "fi\n";

    QString out;
    const bool ok = runTask(m_ifStatus, "Restarting Reticulum", cmd, out, 90000);
    reportTask(m_ifStatus, "Restart Reticulum", ok, out);
}

void ReticulumPage::showMap()
{
    QWidget *host = window();
    if (!host) host = this;

    NetworkMapOverlay *ov = new NetworkMapOverlay(host);
    ov->setGeometry(host->rect());
    ov->show();
    ov->raise();
    ov->reload();
}

// -----------------------------------------------------
// The sysvinit / OpenRC service.
//
// rnsd runs as the desktop user rather than root so that it uses the same
// ~/.reticulum identity and shared instance socket that mesh applications
// will connect to. --config is passed explicitly instead of relying on HOME
// being inherited through start-stop-daemon.
// -----------------------------------------------------
static QString rnsdInitScript(const QString &user, const QString &home)
{
    QString s;
    s += "#!/bin/sh\n";
    s += "### BEGIN INIT INFO\n";
    s += "# Provides:          rnsd\n";
    s += "# Required-Start:    $network $remote_fs\n";
    s += "# Required-Stop:     $network $remote_fs\n";
    s += "# Default-Start:     2 3 4 5\n";
    s += "# Default-Stop:      0 1 6\n";
    s += "# Short-Description: Reticulum Network Stack daemon\n";
    s += "# Description:       Runs rnsd as the desktop user so that mesh\n";
    s += "#                    applications share one Reticulum instance.\n";
    s += "### END INIT INFO\n";
    s += "\n";
    s += "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n";
    s += "NAME=rnsd\n";
    s += "DAEMON=/usr/local/bin/rnsd\n";
    s += "RUNAS=\"" + user + "\"\n";
    s += "RUNHOME=\"" + home + "\"\n";
    s += "CONFIGDIR=\"$RUNHOME/.reticulum\"\n";
    s += "PIDFILE=/run/rnsd.pid\n";
    s += "\n";
    s += "[ -x \"$DAEMON\" ] || exit 0\n";
    s += "\n";
    s += "case \"$1\" in\n";
    s += "  start)\n";
    s += "    echo \"Starting $NAME\"\n";
    s += "    start-stop-daemon --start --quiet --background --make-pidfile \\\n";
    s += "      --pidfile \"$PIDFILE\" --chuid \"$RUNAS\" \\\n";
    s += "      --startas /bin/sh -- -c \"HOME='$RUNHOME' exec '$DAEMON' --config '$CONFIGDIR'\"\n";
    s += "    ;;\n";
    s += "  stop)\n";
    s += "    echo \"Stopping $NAME\"\n";
    s += "    start-stop-daemon --stop --quiet --retry TERM/10/KILL/5 --pidfile \"$PIDFILE\"\n";
    s += "    rm -f \"$PIDFILE\"\n";
    s += "    ;;\n";
    s += "  restart|force-reload)\n";
    s += "    \"$0\" stop\n";
    s += "    sleep 2\n";
    s += "    \"$0\" start\n";
    s += "    ;;\n";
    s += "  status)\n";
    s += "    if [ -f \"$PIDFILE\" ] && kill -0 \"$(cat \"$PIDFILE\")\" 2>/dev/null; then\n";
    s += "      echo \"$NAME is running\"\n";
    s += "      exit 0\n";
    s += "    fi\n";
    s += "    echo \"$NAME is not running\"\n";
    s += "    exit 3\n";
    s += "    ;;\n";
    s += "  *)\n";
    s += "    echo \"Usage: $0 {start|stop|restart|status}\" >&2\n";
    s += "    exit 2\n";
    s += "    ;;\n";
    s += "esac\n";
    s += "exit 0\n";
    return s;
}

// -----------------------------------------------------
// Setup actions
// -----------------------------------------------------
void ReticulumPage::installReticulum()
{
    const QString user = rnsUser();
    const QString home = QDir::homePath();

    if (user.isEmpty()) {
        QMessageBox::warning(this, "Install Reticulum",
            "Could not work out which user to run the daemon as.");
        return;
    }

    // --system-site-packages lets the venv reuse Debian's prebuilt
    // python3-cryptography wheel. Without it pip would try to build
    // cryptography from source, which pulls in a Rust toolchain and does not
    // finish on an Atom tablet.
    QString cmd = QString(SUDO_PREAMBLE);
    cmd += "set -e\n";
    cmd += "echo '== Installing base packages =='\n";
    cmd += "$SUDOX env DEBIAN_FRONTEND=noninteractive apt-get update\n";
    cmd += "$SUDOX env DEBIAN_FRONTEND=noninteractive apt-get -y install "
           "python3 python3-venv python3-pip python3-cryptography python3-serial\n";
    cmd += "\n";
    cmd += "echo '== Creating the Reticulum environment =='\n";
    cmd += "$SUDOX mkdir -p /opt/reticulum\n";
    cmd += "if [ ! -x /opt/reticulum/venv/bin/python3 ]; then\n";
    cmd += "  $SUDOX python3 -m venv --system-site-packages /opt/reticulum/venv\n";
    cmd += "fi\n";
    cmd += "\n";
    cmd += "echo '== Installing the rns package =='\n";
    cmd += "$SUDOX /opt/reticulum/venv/bin/pip install --upgrade rns\n";
    cmd += "\n";
    cmd += "echo '== Linking utilities into /usr/local/bin =='\n";
    cmd += QString("for b in %1; do\n").arg(RNS_BINARIES);
    cmd += "  if [ -x \"/opt/reticulum/venv/bin/$b\" ]; then\n";
    cmd += "    $SUDOX ln -sf \"/opt/reticulum/venv/bin/$b\" \"/usr/local/bin/$b\"\n";
    cmd += "  fi\n";
    cmd += "done\n";
    cmd += "\n";
    cmd += "echo '== Installing the rnsd service =='\n";
    cmd += "$SUDOX tee /etc/init.d/rnsd >/dev/null <<'RNSDINIT'\n";
    cmd += rnsdInitScript(user, home);
    cmd += "RNSDINIT\n";
    cmd += "$SUDOX chmod 755 /etc/init.d/rnsd\n";
    cmd += "echo 'Install complete.'\n";

    QString out;
    const bool ok = runTask(m_setupStatus, "Installing Reticulum", cmd, out, 30 * 60 * 1000);

    if (ok)
        rnsCfgEnsure();

    reportTask(m_setupStatus, "Install Reticulum", ok, out);
    refreshSetup();
}

void ReticulumPage::removeReticulum()
{
    const QMessageBox::StandardButton r = QMessageBox::question(
        this, "Remove Reticulum",
        "Remove the Reticulum software?\n\n"
        "Your identity and configuration in ~/.reticulum are kept, so "
        "reinstalling later restores the same address on the mesh.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes)
        return;

    QString cmd = QString(SUDO_PREAMBLE);
    cmd += "if [ -x /etc/init.d/rnsd ]; then $SUDOX /etc/init.d/rnsd stop || true; fi\n";
    cmd += "$SUDOX rc-update del rnsd default 2>/dev/null || true\n";
    cmd += "$SUDOX update-rc.d -f rnsd remove 2>/dev/null || true\n";
    cmd += "$SUDOX rm -f /etc/init.d/rnsd\n";
    cmd += QString("for b in %1; do $SUDOX rm -f \"/usr/local/bin/$b\"; done\n").arg(RNS_BINARIES);
    cmd += "$SUDOX rm -rf /opt/reticulum\n";
    cmd += "echo 'Removed. ~/.reticulum was left in place.'\n";

    QString out;
    const bool ok = runTask(m_setupStatus, "Removing Reticulum", cmd, out, 10 * 60 * 1000);
    reportTask(m_setupStatus, "Remove Reticulum", ok, out);
    refreshSetup();
}

void ReticulumPage::setBootEnabled(bool on)
{
    QString cmd = QString(SUDO_PREAMBLE);
    if (on) {
        // OpenRC first, sysvinit second — Alternix can be running either.
        cmd += "if $SUDOX rc-update add rnsd default 2>/dev/null; then\n";
        cmd += "  echo 'Enabled via rc-update.'\n";
        cmd += "elif $SUDOX update-rc.d rnsd defaults 2>/dev/null; then\n";
        cmd += "  echo 'Enabled via update-rc.d.'\n";
        cmd += "else\n";
        cmd += "  echo 'ERROR: neither rc-update nor update-rc.d could enable rnsd.'\n";
        cmd += "  exit 1\n";
        cmd += "fi\n";
    } else {
        cmd += "$SUDOX rc-update del rnsd default 2>/dev/null || true\n";
        cmd += "$SUDOX update-rc.d -f rnsd remove 2>/dev/null || true\n";
        cmd += "echo 'Disabled.'\n";
    }

    QString out;
    const bool ok = runTask(m_setupStatus,
                            on ? "Enabling at boot" : "Disabling at boot",
                            cmd, out, 60000);
    reportTask(m_setupStatus, on ? "Enable at boot" : "Disable at boot", ok, out);
    refreshSetup();
}

// -----------------------------------------------------
// Interface actions
// -----------------------------------------------------
QString ReticulumPage::selectedInterfaceName() const
{
    if (!m_ifList) return QString();
    QListWidgetItem *it = m_ifList->currentItem();
    if (!it) return QString();
    return it->data(Qt::UserRole).toString();
}

// A name that does not collide with an existing sub-section.
static QString rnsUniqueName(const QStringList &lines, const QString &base)
{
    const QVector<RIface> ifs = rnsCfgInterfaces(lines);
    QStringList taken;
    for (const RIface &f : ifs) taken << f.name;

    if (!taken.contains(base)) return base;
    for (int i = 2; i < 100; ++i) {
        const QString cand = base + " " + QString::number(i);
        if (!taken.contains(cand)) return cand;
    }
    return base + " new";
}

void ReticulumPage::addAutoInterface()
{
    rnsCfgEnsure();
    QStringList lines = rnsCfgRead();

    // AutoInterface can be pinned to one NIC, which matters on a tablet with
    // both Wi-Fi and a USB Ethernet dock attached.
    QStringList devs = runCmd("ls /sys/class/net 2>/dev/null | grep -vx lo").split('\n');
    devs.removeAll("");
    QStringList choices;
    choices << "All interfaces";
    choices += devs;

    bool ok = false;
    const QString pick = QInputDialog::getItem(
        this, "Add local mesh interface",
        "Discover other Reticulum devices on:", choices, 0, false, &ok);
    if (!ok) return;

    QStringList opts;
    opts << "type = AutoInterface";
    opts << "interface_enabled = True";
    if (pick != "All interfaces" && !pick.isEmpty())
        opts << "devices = " + pick;

    const QString name = rnsUniqueName(lines, "Local Mesh");
    rnsCfgAddIface(lines, name, opts);

    if (!rnsCfgWrite(lines)) {
        QMessageBox::warning(this, "Add interface",
            "Could not write " + rnsConfigPath());
        return;
    }

    // AutoInterface finds peers over IPv6 link-local multicast. A
    // NetworkManager profile with ipv6.method=ignore leaves the link with no
    // link-local address at all, and discovery then silently finds nothing.
    const QString v6 = runCmd(
        "ip -6 addr show scope link 2>/dev/null | grep -c 'inet6 fe80'").trimmed();
    if (v6 == "0") {
        QMessageBox::information(this, "Add interface",
            "Added \"" + name + "\".\n\n"
            "No IPv6 link-local address was found on this device. Local mesh "
            "discovery needs one. In Wireless settings, make sure the active "
            "connection has IPv6 set to at least link-local rather than "
            "ignored.\n\nUse Apply / restart to bring the interface up.");
    } else {
        QMessageBox::information(this, "Add interface",
            "Added \"" + name + "\".\n\nUse Apply / restart to bring it up.");
    }

    refreshInterfaces();
}

void ReticulumPage::addTcpInterface()
{
    rnsCfgEnsure();
    QStringList lines = rnsCfgRead();

    bool ok = false;
    const QString host = QInputDialog::getText(
        this, "Add internet link", "Host name or IP address of the hub:",
        QLineEdit::Normal, "", &ok).trimmed();
    if (!ok || host.isEmpty()) return;

    const int port = QInputDialog::getInt(
        this, "Add internet link", "Port:", 4965, 1, 65535, 1, &ok);
    if (!ok) return;

    QStringList opts;
    opts << "type = TCPClientInterface";
    opts << "interface_enabled = True";
    opts << "target_host = " + host;
    opts << "target_port = " + QString::number(port);

    const QString name = rnsUniqueName(lines, "TCP to " + host);
    rnsCfgAddIface(lines, name, opts);

    if (!rnsCfgWrite(lines)) {
        QMessageBox::warning(this, "Add interface", "Could not write " + rnsConfigPath());
        return;
    }

    QMessageBox::information(this, "Add interface",
        "Added \"" + name + "\".\n\nUse Apply / restart to connect.");
    refreshInterfaces();
}

void ReticulumPage::addRNodeInterface()
{
    rnsCfgEnsure();
    QStringList lines = rnsCfgRead();

    // /dev/serial/by-id survives replugging and reboots; /dev/ttyUSB0 does not,
    // and these tablets have no built-in serial port so every RNode is USB.
    QStringList ports = runCmd(
        "ls /dev/serial/by-id/* 2>/dev/null").split('\n');
    ports.removeAll("");
    if (ports.isEmpty()) {
        QStringList fallback = runCmd("ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null").split('\n');
        fallback.removeAll("");
        ports = fallback;
    }

    if (ports.isEmpty()) {
        QMessageBox::information(this, "Add RNode",
            "No USB serial device was found.\n\n"
            "Plug the RNode in and try again.");
        return;
    }

    bool ok = false;
    const QString port = QInputDialog::getItem(
        this, "Add RNode", "RNode device:", ports, 0, false, &ok);
    if (!ok || port.isEmpty()) return;

    // 867.2 MHz / SF8 / BW125k is the usual EU868 starting point. Everyone on
    // the same mesh has to match, so it is worth asking rather than assuming.
    const double mhz = QInputDialog::getDouble(
        this, "Add RNode", "Frequency in MHz (all radios must match):",
        867.200, 100.000, 1100.000, 3, &ok);
    if (!ok) return;

    const qint64 hz = qRound64(mhz * 1000000.0);

    QStringList opts;
    opts << "type = RNodeInterface";
    opts << "interface_enabled = True";
    opts << "port = " + port;
    opts << "frequency = " + QString::number(hz);
    opts << "bandwidth = 125000";
    opts << "txpower = 7";
    opts << "spreadingfactor = 8";
    opts << "codingrate = 5";

    const QString name = rnsUniqueName(lines, "RNode LoRa");
    rnsCfgAddIface(lines, name, opts);

    if (!rnsCfgWrite(lines)) {
        QMessageBox::warning(this, "Add interface", "Could not write " + rnsConfigPath());
        return;
    }

    // Opening a serial port needs group dialout; without it the interface just
    // reports a permission error at startup and never comes up.
    const QString inGroup = runCmd("id -nG 2>/dev/null | tr ' ' '\\n' | grep -qx dialout "
                                    "&& echo yes || echo no");
    if (inGroup != "yes") {
        const QMessageBox::StandardButton r = QMessageBox::question(
            this, "Serial permission",
            "This user is not in the dialout group, so Reticulum cannot open the "
            "RNode.\n\nAdd the user to dialout now?\n\n"
            "You will need to log out and back in for it to take effect.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

        if (r == QMessageBox::Yes) {
            const QString cmd = QString(SUDO_PREAMBLE) +
                "$SUDOX adduser \"" + rnsUser() + "\" dialout\n";
            QString out;
            const bool added = runTask(m_ifStatus, "Adding user to dialout", cmd, out, 60000);
            reportTask(m_ifStatus, "Add to dialout", added, out);
        }
    }

    QMessageBox::information(this, "Add interface",
        "Added \"" + name + "\".\n\nUse Apply / restart to bring the radio up.");
    refreshInterfaces();
}

void ReticulumPage::toggleSelectedInterface()
{
    const QString name = selectedInterfaceName();
    if (name.isEmpty()) {
        QMessageBox::information(this, "Interfaces", "Select an interface first.");
        return;
    }

    QStringList lines = rnsCfgRead();
    const QVector<RIface> ifs = rnsCfgInterfaces(lines);

    bool found = false, nowEnabled = false;
    for (const RIface &f : ifs) {
        if (f.name != name) continue;
        found = true;
        nowEnabled = !f.enabled;
        break;
    }
    if (!found) return;

    rnsCfgSetIfaceKey(lines, name, "interface_enabled", nowEnabled ? "True" : "False");

    if (!rnsCfgWrite(lines)) {
        QMessageBox::warning(this, "Interfaces", "Could not write " + rnsConfigPath());
        return;
    }

    refreshInterfaces();
    if (m_ifStatus) {
        m_ifStatus->setVisible(true);
        m_ifStatus->setText("<span style='color:#FFAA00;'>Changed — "
                            "use Apply / restart to make it take effect.</span>");
    }
}

void ReticulumPage::removeSelectedInterface()
{
    const QString name = selectedInterfaceName();
    if (name.isEmpty()) {
        QMessageBox::information(this, "Interfaces", "Select an interface first.");
        return;
    }

    const QMessageBox::StandardButton r = QMessageBox::question(
        this, "Remove interface", "Remove \"" + name + "\" from the configuration?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (r != QMessageBox::Yes) return;

    QStringList lines = rnsCfgRead();
    if (!rnsCfgRemoveIface(lines, name)) return;

    if (!rnsCfgWrite(lines)) {
        QMessageBox::warning(this, "Interfaces", "Could not write " + rnsConfigPath());
        return;
    }

    refreshInterfaces();
    if (m_ifStatus) {
        m_ifStatus->setVisible(true);
        m_ifStatus->setText("<span style='color:#FFAA00;'>Removed — "
                            "use Apply / restart to make it take effect.</span>");
    }
}

void ReticulumPage::toggleTransport()
{
    rnsCfgEnsure();
    QStringList lines = rnsCfgRead();

    const QString cur = rnsCfgGetTopKey(lines, "reticulum", "enable_transport", "False").toLower();
    const bool on = (cur == "true" || cur == "yes" || cur == "1" || cur == "on");

    if (!on) {
        const QMessageBox::StandardButton r = QMessageBox::question(
            this, "Transport node",
            "Turn this device into a transport node?\n\n"
            "It will relay other people's traffic and answer path requests. That "
            "uses noticeably more battery and bandwidth, so it suits a device on "
            "mains power better than one on battery.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) return;
    }

    rnsCfgSetTopKey(lines, "reticulum", "enable_transport", on ? "False" : "True");

    if (!rnsCfgWrite(lines)) {
        QMessageBox::warning(this, "Transport node", "Could not write " + rnsConfigPath());
        return;
    }

    refreshInterfaces();
    if (m_ifStatus) {
        m_ifStatus->setVisible(true);
        m_ifStatus->setText("<span style='color:#FFAA00;'>Changed — "
                            "use Apply / restart to make it take effect.</span>");
    }
}

// -----------------------------------------------------
// Factory for osm-settings
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    (void)cfg();   // touch the shared settings object so it is created once
    return new ReticulumPage(stack);
}
