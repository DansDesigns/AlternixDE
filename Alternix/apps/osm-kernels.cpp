// osm-kernels.cpp — AlternixDE kernel manager
//
// Lists, installs and removes kernels from four sources:
//   Repository  Devuan/Debian archive (incl. backports) via apt
//   Mainline    Ubuntu mainline builds from kernel.ubuntu.com/mainline
//   Liquorix    liquorix.net apt repo (amd64 only)
//   XanMod      deb.xanmod.org apt repo (amd64 only, filtered by x86-64 psABI level)
// and sets which installed kernel GRUB boots by default
// (via /etc/default/grub.d/99-osm-kernels.cfg, so /etc/default/grub is never edited).
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -O2 -fPIC osm-kernels.cpp -o osm-kernels $(pkg-config --cflags --libs Qt5Widgets)
//
// Root actions run as:  sudo -A /bin/sh -c '<script>'   with SUDO_ASKPASS=osm-sudo
// Runtime needs: apt, dpkg, sudo, osm-sudo, curl or wget, sha256sum;
//                gpg only when enabling the Liquorix/XanMod repos (installed on demand).

#include <QtWidgets>
#include <functional>

namespace {

const QString kMainlineBase = QStringLiteral("https://kernel.ubuntu.com/mainline/");
const QString kGrubOverride = QStringLiteral("/etc/default/grub.d/99-osm-kernels.cfg");
const QString kAptOpts = QStringLiteral(
    "-o DPkg::Lock::Timeout=120 -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold");

enum Src { SrcRepo = 0, SrcMainline, SrcLiquorix, SrcXanmod };

QString srcName(int s)
{
    switch (s) {
    case SrcMainline: return QStringLiteral("Mainline");
    case SrcLiquorix: return QStringLiteral("Liquorix");
    case SrcXanmod:   return QStringLiteral("XanMod");
    default:          return QStringLiteral("Repository");
    }
}

QString readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// Single-quote for /bin/sh.
QString sq(const QString &s)
{
    QString r = s;
    r.replace(QLatin1String("'"), QLatin1String("'\\''"));
    return QLatin1Char('\'') + r + QLatin1Char('\'');
}

bool safeToken(const QString &s)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9.+~:_-]*$"));
    return re.match(s).hasMatch();
}

QString stripAnsi(QString s)
{
    static const QRegularExpression ansi(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]"));
    s.remove(ansi);
    s.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return s;
}

// Debian-ish version ordering, good enough for sorting kernel lists.
int verCmp(const QString &a, const QString &b)
{
    int i = 0, j = 0;
    const int na = a.size(), nb = b.size();
    auto ord = [](const QString &s, int k) -> int {
        if (k >= s.size()) return 0;
        const QChar c = s.at(k);
        if (c == QLatin1Char('~')) return -1;
        if (c.isDigit()) return 1;
        if (c.isLetter()) return c.unicode();
        return c.unicode() + 256;
    };
    while (i < na || j < nb) {
        if (i < na && j < nb && a.at(i).isDigit() && b.at(j).isDigit()) {
            const int si = i, sj = j;
            while (i < na && a.at(i).isDigit()) ++i;
            while (j < nb && b.at(j).isDigit()) ++j;
            const qulonglong x = a.mid(si, i - si).toULongLong();
            const qulonglong y = b.mid(sj, j - sj).toULongLong();
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        const int x = ord(a, i), y = ord(b, j);
        if (x != y) return x < y ? -1 : 1;
        ++i;
        ++j;
    }
    return 0;
}

QString releaseOf(const QString &pkg)
{
    static const QString un = QStringLiteral("linux-image-unsigned-");
    static const QString im = QStringLiteral("linux-image-");
    QString r;
    if (pkg.startsWith(un)) r = pkg.mid(un.size());
    else if (pkg.startsWith(im)) r = pkg.mid(im.size());
    if (r.isEmpty() || !r.at(0).isDigit()) return {};   // skip metapackages
    return r;
}

int classifyRelease(const QString &rel)
{
    static const QRegularExpression ml(QStringLiteral("^\\d+\\.\\d+\\.\\d+-\\d{6}(rc\\d+)?-"));
    if (rel.contains(QLatin1String("liquorix"))) return SrcLiquorix;
    if (rel.contains(QLatin1String("xanmod")))   return SrcXanmod;
    if (ml.match(rel).hasMatch())                return SrcMainline;
    return SrcRepo;
}

// x86-64 psABI level from /proc/cpuinfo flags: 0 = not x86, 1..4.
int x86Level()
{
    const QString info = readFile(QStringLiteral("/proc/cpuinfo"));
    QSet<QString> flags;
    for (const QString &line : info.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("flags"))) {
            for (const QString &f : line.section(QLatin1Char(':'), 1).split(QLatin1Char(' '), Qt::SkipEmptyParts))
                flags.insert(f);
            break;
        }
    }
    if (flags.isEmpty() || !flags.contains(QStringLiteral("lm"))) return 0;
    auto all = [&](const QStringList &need) {
        for (const QString &n : need) if (!flags.contains(n)) return false;
        return true;
    };
    int lvl = 1;
    if (all({"cx16", "lahf_lm", "popcnt", "sse4_1", "sse4_2", "ssse3"})) lvl = 2; else return lvl;
    if (all({"avx", "avx2", "bmi1", "bmi2", "f16c", "fma", "abm", "movbe", "xsave"})) lvl = 3; else return lvl;
    if (all({"avx512f", "avx512bw", "avx512cd", "avx512dq", "avx512vl"})) lvl = 4;
    return lvl;
}

QString debianCodename()
{
    QString code;
    for (const QString &line : readFile(QStringLiteral("/etc/os-release")).split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("VERSION_CODENAME="))) {
            code = line.mid(17).trimmed();
            code.remove(QLatin1Char('"'));
            break;
        }
    }
    if (code.isEmpty())
        code = readFile(QStringLiteral("/etc/devuan_version")).trimmed().section(QLatin1Char('/'), 0, 0);
    static const QMap<QString, QString> devuan = {
        {"chimaera", "bullseye"}, {"daedalus", "bookworm"}, {"excalibur", "trixie"},
        {"freia", "forky"}, {"ceres", "sid"}};
    static const QStringList debian = {"bullseye", "bookworm", "trixie", "forky", "sid"};
    if (devuan.contains(code)) return devuan.value(code);
    if (debian.contains(code)) return code;
    return {};
}

bool aptSourcesMention(const QString &needle)
{
    QStringList files = {QStringLiteral("/etc/apt/sources.list")};
    QDir d(QStringLiteral("/etc/apt/sources.list.d"));
    for (const QString &f : d.entryList({"*.list", "*.sources"}, QDir::Files))
        files << d.absoluteFilePath(f);
    for (const QString &f : files) {
        for (const QString &line : readFile(f).split(QLatin1Char('\n'))) {
            const QString t = line.trimmed();
            if (!t.startsWith(QLatin1Char('#')) && t.contains(needle)) return true;
        }
    }
    return false;
}

struct KPkg {
    QString name, release, version, suite;
    int src = SrcRepo;
};

struct MLVer {
    QString tag;              // v6.17.2
    int maj = 0, min = 0, pat = 0, rc = 0;
    QString release() const   // 6.17.2-061702-generic
    {
        return QStringLiteral("%1.%2.%3-%4%5%6%7-generic")
            .arg(maj).arg(min).arg(pat)
            .arg(maj, 2, 10, QLatin1Char('0')).arg(min, 2, 10, QLatin1Char('0')).arg(pat, 2, 10, QLatin1Char('0'))
            .arg(rc ? QStringLiteral("rc%1").arg(rc) : QString());
    }
    bool operator<(const MLVer &o) const
    {
        if (maj != o.maj) return maj < o.maj;
        if (min != o.min) return min < o.min;
        if (pat != o.pat) return pat < o.pat;
        if ((rc == 0) != (o.rc == 0)) return rc != 0;   // rc sorts before final
        return rc < o.rc;
    }
};

struct Page {
    QWidget *w = nullptr;
    QLabel *note = nullptr;
    QTreeWidget *tree = nullptr;
    QPushButton *enableBtn = nullptr;
};

class KernelWindow : public QWidget
{
public:
    KernelWindow()
    {
        m_arch = archOf();
        m_cpuLevel = x86Level();
        m_codename = debianCodename();
        m_running = readFile(QStringLiteral("/proc/sys/kernel/osrelease")).trimmed();
        m_isPi = readFile(QStringLiteral("/proc/device-tree/model")).contains(QLatin1String("Raspberry Pi"));
        m_hasGrub = !QStandardPaths::findExecutable(QStringLiteral("update-grub"),
                        {"/usr/sbin", "/sbin", "/usr/bin", "/bin"}).isEmpty();
        if (!QStandardPaths::findExecutable(QStringLiteral("curl")).isEmpty()) m_fetch = QStringLiteral("curl");
        else if (!QStandardPaths::findExecutable(QStringLiteral("wget")).isEmpty()) m_fetch = QStringLiteral("wget");

        buildUi();
        refreshHeader();
        reloadLocal();
    }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (m_busy) {
            QMessageBox::warning(this, QStringLiteral("Kernels"),
                QStringLiteral("A kernel operation is still running.\nClosing now could leave packages half-configured."));
            e->ignore();
            return;
        }
        e->accept();
    }

private:
    // ---------------------------------------------------------------- state
    QString m_arch, m_codename, m_running, m_fetch;
    int m_cpuLevel = 0;
    bool m_isPi = false, m_hasGrub = false, m_sbOn = false, m_busy = false, m_mlLoaded = false;
    QMap<QString, QString> m_images;      // installed concrete image pkg -> version
    QSet<QString> m_installed;            // all installed linux-* packages
    QVector<KPkg> m_apt;
    QSet<QString> m_aptHeaders;
    QVector<MLVer> m_ml;
    QString m_grubDefaultRel, m_grubDefaultDesc;

    // ---------------------------------------------------------------- widgets
    QLabel *m_hRunning = nullptr, *m_hDefault = nullptr, *m_hSystem = nullptr, *m_hBoot = nullptr, *m_hCmdline = nullptr;
    QStackedWidget *m_stack = nullptr;
    Page m_pInst, m_pRepo, m_pMl, m_pLiq, m_pXan;
    QCheckBox *m_headers = nullptr, *m_showRc = nullptr;
    QLabel *m_banner = nullptr, *m_status = nullptr;
    QPushButton *m_bannerClose = nullptr, *m_logToggle = nullptr;
    QWidget *m_bannerRow = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QList<QWidget *> m_actions, m_grubBtns;

    // ================================================================ helpers
    static QString archOf()
    {
        QProcess p;
        p.start(QStringLiteral("dpkg"), {QStringLiteral("--print-architecture")});
        if (p.waitForFinished(3000) && p.exitCode() == 0)
            return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        const QString a = QSysInfo::currentCpuArchitecture();
        if (a == QLatin1String("x86_64")) return QStringLiteral("amd64");
        if (a == QLatin1String("arm64")) return QStringLiteral("arm64");
        return a;
    }

    using Done = std::function<void(int code, const QString &out, const QString &err)>;

    void runAsync(const QString &prog, const QStringList &args, Done cb, const QProcessEnvironment *env = nullptr)
    {
        auto *p = new QProcess(this);
        QProcessEnvironment e = env ? *env : QProcessEnvironment::systemEnvironment();
        e.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        p->setProcessEnvironment(e);
        auto fired = std::make_shared<bool>(false);
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [p, cb, fired](int code, QProcess::ExitStatus st) {
                    if (*fired) return;
                    *fired = true;
                    cb(st == QProcess::NormalExit ? code : -1,
                       QString::fromUtf8(p->readAllStandardOutput()),
                       QString::fromUtf8(p->readAllStandardError()));
                    p->deleteLater();
                });
        connect(p, &QProcess::errorOccurred, this, [p, cb, prog, fired](QProcess::ProcessError err) {
            if (err != QProcess::FailedToStart || *fired) return;
            *fired = true;
            cb(-1, QString(), prog + QStringLiteral(": could not be started (is it installed?)"));
            p->deleteLater();
        });
        p->start(prog, args);
    }

    void fetchText(const QString &url, std::function<void(bool, const QString &)> cb)
    {
        if (m_fetch.isEmpty()) { cb(false, QStringLiteral("Neither curl nor wget is installed.")); return; }
        QStringList args = m_fetch == QLatin1String("curl")
            ? QStringList{"-fsSL", "--retry", "2", "--max-time", "60", url}
            : QStringList{"-q", "-T", "60", "-O-", url};
        runAsync(m_fetch, args, [cb](int code, const QString &out, const QString &err) {
            cb(code == 0, code == 0 ? out : err.trimmed());
        });
    }

    QString dlCmd(const QString &url, const QString &out) const
    {
        if (m_fetch == QLatin1String("curl"))
            return QStringLiteral("curl -fsSL --retry 3 -o %1 %2").arg(sq(out), sq(url));
        return QStringLiteral("wget -q -O %1 %2").arg(sq(out), sq(url));
    }

    QProcessEnvironment *askpassEnv(QString *errOut)
    {
        static QProcessEnvironment env;
        env = QProcessEnvironment::systemEnvironment();
        QString ap = QStandardPaths::findExecutable(QStringLiteral("osm-sudo"));
        if (ap.isEmpty()) ap = env.value(QStringLiteral("SUDO_ASKPASS"));
        if (ap.isEmpty()) {
            *errOut = QStringLiteral("osm-sudo was not found and SUDO_ASKPASS is not set, so administrator access cannot be requested.");
            return nullptr;
        }
        env.insert(QStringLiteral("SUDO_ASKPASS"), ap);
        return &env;
    }

    // ================================================================ UI
    QPushButton *btn(const QString &text, std::function<void()> fn, bool action = true)
    {
        auto *b = new QPushButton(text);
        b->setMinimumHeight(48);
        b->setFocusPolicy(Qt::NoFocus);
        connect(b, &QPushButton::clicked, this, [fn]() { fn(); });
        if (action) m_actions << b;
        return b;
    }

    QWidget *grubBtn(QPushButton *b)
    {
        m_grubBtns << b;
        return b;
    }

    QTreeWidget *makeTree(const QStringList &cols)
    {
        auto *t = new QTreeWidget;
        t->setColumnCount(cols.size());
        t->setHeaderLabels(cols);
        t->setRootIsDecorated(false);
        t->setUniformRowHeights(true);
        t->setAlternatingRowColors(true);
        t->setSelectionMode(QAbstractItemView::SingleSelection);
        t->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        t->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int i = 1; i < cols.size(); ++i)
            t->header()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
        t->header()->setStretchLastSection(false);
        QScroller::grabGesture(t->viewport(), QScroller::LeftMouseButtonGesture);
        return t;
    }

    Page makePage(const QStringList &cols, const QList<QWidget *> &buttons, QWidget *extraTop = nullptr)
    {
        Page p;
        p.w = new QWidget;
        auto *v = new QVBoxLayout(p.w);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(8);
        p.note = new QLabel;
        p.note->setObjectName(QStringLiteral("note"));
        p.note->setWordWrap(true);
        p.note->hide();
        v->addWidget(p.note);
        if (extraTop) v->addWidget(extraTop);
        p.tree = makeTree(cols);
        v->addWidget(p.tree, 1);
        auto *row = new QHBoxLayout;
        row->setSpacing(8);
        for (QWidget *b : buttons) row->addWidget(b);
        row->addStretch();
        v->addLayout(row);
        return p;
    }

    void buildUi()
    {
        setWindowTitle(QStringLiteral("Kernels"));
        setMinimumWidth(720);
        resize(900, 760);

        auto *root = new QVBoxLayout(this);
        root->setContentsMargins(14, 14, 14, 14);
        root->setSpacing(10);

        // ---- header card
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("card"));
        auto *cv = new QVBoxLayout(card);
        cv->setContentsMargins(16, 12, 16, 12);
        cv->setSpacing(4);
        m_hRunning = new QLabel;  m_hRunning->setObjectName(QStringLiteral("big"));
        m_hDefault = new QLabel;
        m_hSystem  = new QLabel;  m_hSystem->setWordWrap(true);
        m_hBoot    = new QLabel;
        m_hCmdline = new QLabel;  m_hCmdline->setObjectName(QStringLiteral("small"));
        m_hCmdline->setWordWrap(true);
        m_hCmdline->setTextInteractionFlags(Qt::TextSelectableByMouse);
        for (QLabel *l : {m_hRunning, m_hDefault, m_hSystem, m_hBoot, m_hCmdline}) cv->addWidget(l);
        root->addWidget(card);

        // ---- nav
        auto *nav = new QHBoxLayout;
        nav->setSpacing(6);
        auto *group = new QButtonGroup(this);
        group->setExclusive(true);
        m_stack = new QStackedWidget;
        const QStringList names = {"Installed", "Repository", "Mainline", "Liquorix", "XanMod"};
        for (int i = 0; i < names.size(); ++i) {
            auto *b = new QPushButton(names.at(i));
            b->setCheckable(true);
            b->setMinimumHeight(48);
            b->setFocusPolicy(Qt::NoFocus);
            b->setObjectName(QStringLiteral("nav"));
            group->addButton(b, i);
            nav->addWidget(b, 1);
            if (i == 0) b->setChecked(true);
        }
        connect(group, &QButtonGroup::idClicked, this, [this](int id) {
            m_stack->setCurrentIndex(id);
            if (id == 2 && !m_mlLoaded) loadMainline();
        });
        root->addLayout(nav);

        // ---- pages
        m_pInst = makePage({"Kernel", "Source", "Status"},
            {grubBtn(btn("Boot by default", [this] { setDefault(); })),
             grubBtn(btn("Newest by default", [this] { resetDefault(); })),
             btn("Remove", [this] { removeSelected(); }),
             grubBtn(btn("Update GRUB menu", [this] { updateGrub(); })),
             btn("Refresh", [this] { reloadLocal(); })});
        m_pRepo = makePage({"Kernel", "Version", "Suite", "Status"},
            {btn("Install", [this] { installApt(m_pRepo); }),
             btn("Update package lists", [this] { aptUpdate(); }),
             btn("Refresh", [this] { reloadLocal(); })});

        m_showRc = new QCheckBox(QStringLiteral("Show release candidates"));
        connect(m_showRc, &QCheckBox::toggled, this, [this](bool) { fillMainline(); });
        m_pMl = makePage({"Version", "Kernel", "Status"},
            {btn("Install", [this] { installMainline(); }),
             btn("Refresh", [this] { loadMainline(); })}, m_showRc);

        m_pLiq = makePage({"Kernel", "Version", "Suite", "Status"},
            {btn("Install", [this] { installApt(m_pLiq); }),
             btn("Refresh", [this] { reloadLocal(); })});
        m_pLiq.enableBtn = btn("Enable Liquorix repository", [this] { enableRepo(SrcLiquorix); });
        m_pLiq.w->layout()->addWidget(m_pLiq.enableBtn);

        m_pXan = makePage({"Kernel", "Version", "Suite", "Status"},
            {btn("Install", [this] { installApt(m_pXan); }),
             btn("Refresh", [this] { reloadLocal(); })});
        m_pXan.enableBtn = btn("Enable XanMod repository", [this] { enableRepo(SrcXanmod); });
        m_pXan.w->layout()->addWidget(m_pXan.enableBtn);

        for (Page *p : {&m_pInst, &m_pRepo, &m_pMl, &m_pLiq, &m_pXan}) m_stack->addWidget(p->w);
        root->addWidget(m_stack, 1);

        // ---- banner (persistent until dismissed or replaced)
        m_bannerRow = new QWidget;
        auto *br = new QHBoxLayout(m_bannerRow);
        br->setContentsMargins(0, 0, 0, 0);
        m_banner = new QLabel;
        m_banner->setWordWrap(true);
        m_banner->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_bannerClose = new QPushButton(QStringLiteral("Dismiss"));
        m_bannerClose->setMinimumHeight(44);
        m_bannerClose->setFocusPolicy(Qt::NoFocus);
        connect(m_bannerClose, &QPushButton::clicked, m_bannerRow, &QWidget::hide);
        br->addWidget(m_banner, 1);
        br->addWidget(m_bannerClose);
        m_bannerRow->hide();
        root->addWidget(m_bannerRow);

        // ---- footer
        auto *foot = new QHBoxLayout;
        m_headers = new QCheckBox(QStringLiteral("Also install headers (for DKMS drivers)"));
        m_headers->setChecked(true);
        m_status = new QLabel;
        m_status->setObjectName(QStringLiteral("small"));
        m_logToggle = new QPushButton(QStringLiteral("Show log"));
        m_logToggle->setMinimumHeight(44);
        m_logToggle->setFocusPolicy(Qt::NoFocus);
        connect(m_logToggle, &QPushButton::clicked, this, [this] { showLog(!m_log->isVisible()); });
        foot->addWidget(m_headers);
        foot->addStretch();
        foot->addWidget(m_status);
        foot->addWidget(m_logToggle);
        root->addLayout(foot);

        m_log = new QPlainTextEdit;
        m_log->setReadOnly(true);
        m_log->setMaximumBlockCount(5000);
        m_log->setFixedHeight(190);
        m_log->hide();
        QScroller::grabGesture(m_log->viewport(), QScroller::LeftMouseButtonGesture);
        root->addWidget(m_log);

        applyNotes();
    }

    void showLog(bool on)
    {
        m_log->setVisible(on);
        m_logToggle->setText(on ? QStringLiteral("Hide log") : QStringLiteral("Show log"));
    }

    void banner(const QString &kind, const QString &text)
    {
        m_banner->setObjectName(kind);   // error | warn | ok
        m_banner->setText(text);
        m_banner->style()->unpolish(m_banner);
        m_banner->style()->polish(m_banner);
        m_bannerRow->show();
    }

    void fail(const QString &text)
    {
        banner(QStringLiteral("error"), text);
        log(QStringLiteral("ERROR: ") + text);
    }

    void log(const QString &text)
    {
        for (const QString &line : stripAnsi(text).split(QLatin1Char('\n'))) {
            if (!line.trimmed().isEmpty()) m_log->appendPlainText(line);
        }
    }

    void setBusy(bool on, const QString &msg = QString())
    {
        m_busy = on;
        for (QWidget *w : m_actions) w->setEnabled(!on);
        m_status->setText(on ? msg : QString());
        if (!on) applyNotes();   // re-apply per-page enable rules
    }

    void applyNotes()
    {
        const bool amd64 = m_arch == QLatin1String("amd64");
        const bool liqOn = aptSourcesMention(QStringLiteral("liquorix.net"));
        const bool xanOn = aptSourcesMention(QStringLiteral("deb.xanmod.org"));

        auto note = [](Page &p, const QString &t, const QString &kind = QStringLiteral("note")) {
            p.note->setObjectName(kind);
            p.note->style()->unpolish(p.note);
            p.note->style()->polish(p.note);
            p.note->setText(t);
            p.note->setVisible(!t.isEmpty());
        };

        QString inst;
        if (!m_hasGrub) inst = QStringLiteral("update-grub was not found: this system does not boot through GRUB, so boot-default controls are disabled.");
        if (m_isPi) inst = QStringLiteral("Raspberry Pi boots the kernel chosen by its firmware (config.txt). Kernels installed here will not be used unless the boot configuration points at them.");
        note(m_pInst, inst, QStringLiteral("warnnote"));
        for (QWidget *b : m_grubBtns) b->setEnabled(!m_busy && m_hasGrub);

        note(m_pRepo, QStringLiteral("Signed kernels from your configured apt sources, including backports if enabled. These receive security updates."));

        QString ml = QStringLiteral("Upstream kernels built by Ubuntu. Unsigned, Ubuntu configuration, no automatic security updates.");
        if (m_sbOn) ml += QStringLiteral(" Secure Boot is ON: these kernels will not boot until it is disabled.");
        note(m_pMl, ml, m_sbOn ? QStringLiteral("warnnote") : QStringLiteral("note"));

        auto thirdParty = [&](Page &p, const QString &name, bool on, const QString &extra) {
            QString t;
            if (!amd64) t = name + QStringLiteral(" only publishes amd64 kernels; this system is ") + m_arch + QLatin1Char('.');
            else if (!on) t = name + QStringLiteral(" repository is not enabled. Enabling it adds its signing key and an apt source for Debian ")
                              + (m_codename.isEmpty() ? QStringLiteral("(unknown release)") : m_codename) + QLatin1Char('.');
            else t = name + QStringLiteral(" repository is enabled. Kernels are unsigned") + (m_sbOn ? QStringLiteral(" and Secure Boot is ON, so they will not boot.") : QStringLiteral(".")) + extra;
            note(p, t, (!amd64 || m_sbOn) ? QStringLiteral("warnnote") : QStringLiteral("note"));
            p.enableBtn->setVisible(amd64 && !on);
            p.enableBtn->setEnabled(!m_busy && !m_codename.isEmpty());
        };
        thirdParty(m_pLiq, QStringLiteral("Liquorix"), liqOn, QString());
        thirdParty(m_pXan, QStringLiteral("XanMod"), xanOn,
                   m_cpuLevel ? QStringLiteral(" Showing builds for x86-64-v%1 and below (this CPU's level).").arg(m_cpuLevel) : QString());
    }

    // ================================================================ header
    void refreshHeader()
    {
        m_hRunning->setText(QStringLiteral("Running  ") + (m_running.isEmpty() ? QStringLiteral("unknown") : m_running)
                            + QStringLiteral("   ·   ") + srcName(classifyRelease(m_running)));

        readGrubDefault();
        m_hDefault->setText(QStringLiteral("Boot default  ") + m_grubDefaultDesc);

        QString fw;
        if (!QDir(QStringLiteral("/sys/firmware/efi")).exists()) {
            fw = QStringLiteral("Legacy BIOS boot");
        } else {
            const QString bits = readFile(QStringLiteral("/sys/firmware/efi/fw_platform_size")).trimmed();
            QFile f(QStringLiteral("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c"));
            QString sb = QStringLiteral("unknown");
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray b = f.readAll();
                if (b.size() >= 5) { m_sbOn = b.at(4) != 0; sb = m_sbOn ? QStringLiteral("ON") : QStringLiteral("off"); }
            }
            fw = QStringLiteral("UEFI%1 · Secure Boot %2").arg(bits.isEmpty() ? QString() : QLatin1Char(' ') + bits + QStringLiteral("-bit"), sb);
        }
        QString sys = m_arch;
        if (m_cpuLevel) sys += QStringLiteral(" · x86-64-v%1").arg(m_cpuLevel);
        sys += QStringLiteral(" · ") + fw;
        m_hSystem->setText(sys);

        QString boot;
        for (const QString &line : readFile(QStringLiteral("/proc/stat")).split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("btime "))) {
                const QDateTime t = QDateTime::fromSecsSinceEpoch(line.mid(6).trimmed().toLongLong());
                boot = QStringLiteral("Booted  ") + t.toString(QStringLiteral("ddd d MMM yyyy, HH:mm"));
                break;
            }
        }
        const qint64 up = static_cast<qint64>(readFile(QStringLiteral("/proc/uptime")).section(QLatin1Char(' '), 0, 0).toDouble());
        if (up > 0) boot += QStringLiteral("   ·   up %1d %2h %3m").arg(up / 86400).arg((up % 86400) / 3600).arg((up % 3600) / 60);
        m_hBoot->setText(boot);
        m_hCmdline->setText(readFile(QStringLiteral("/proc/cmdline")).trimmed());
        applyNotes();
    }

    void readGrubDefault()
    {
        m_grubDefaultRel.clear();
        static const QRegularExpression defRe(QStringLiteral("^\\s*GRUB_DEFAULT=\"?([^\"\\n]*)\"?"),
                                              QRegularExpression::MultilineOption);
        static const QRegularExpression relRe(QStringLiteral("gnulinux-(.+)-advanced-[0-9A-Fa-f-]+$"));
        if (!m_hasGrub) { m_grubDefaultDesc = QStringLiteral("not managed (no GRUB)"); return; }

        auto lastDef = [&](const QString &text) {
            QString v;
            auto it = defRe.globalMatch(text);
            while (it.hasNext()) v = it.next().captured(1).trimmed();
            return v;
        };
        const QString ov = lastDef(readFile(kGrubOverride));
        if (!ov.isEmpty()) {
            const auto m = relRe.match(ov.section(QLatin1Char('>'), -1));
            m_grubDefaultRel = m.hasMatch() ? m.captured(1) : QString();
            m_grubDefaultDesc = m_grubDefaultRel.isEmpty() ? ov : m_grubDefaultRel;
            return;
        }
        const QString v = lastDef(readFile(QStringLiteral("/etc/default/grub")));
        if (v.isEmpty() || v == QLatin1String("0")) m_grubDefaultDesc = QStringLiteral("newest kernel (GRUB first entry)");
        else if (v == QLatin1String("saved")) m_grubDefaultDesc = QStringLiteral("last saved entry (GRUB_DEFAULT=saved)");
        else m_grubDefaultDesc = QStringLiteral("GRUB_DEFAULT=") + v;
    }

    // ================================================================ loading
    void reloadLocal()
    {
        setBusy(true, QStringLiteral("Reading package database…"));
        runAsync(QStringLiteral("dpkg-query"),
                 {"-W", "-f=${Package}\t${Version}\t${db:Status-Abbrev}\n", "linux-*"},
                 [this](int code, const QString &out, const QString &err) {
            m_images.clear();
            m_installed.clear();
            if (code != 0 && out.isEmpty()) {
                setBusy(false);
                fail(QStringLiteral("dpkg-query failed: ") + err.trimmed());
                return;
            }
            for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                const QStringList f = line.split(QLatin1Char('\t'));
                if (f.size() < 3 || !f.at(2).startsWith(QLatin1String("ii"))) continue;
                m_installed.insert(f.at(0));
                if (!releaseOf(f.at(0)).isEmpty()) m_images.insert(f.at(0), f.at(1));
            }
            refreshHeader();
            fillInstalled();
            loadApt();
        });
    }

    void loadApt()
    {
        setBusy(true, QStringLiteral("Reading apt package lists…"));
        runAsync(QStringLiteral("apt-cache"), {"search", "--names-only", "^linux-(image|headers)-[0-9]"},
                 [this](int code, const QString &out, const QString &err) {
            if (code != 0) {
                setBusy(false);
                fail(QStringLiteral("apt-cache search failed: ") + err.trimmed());
                return;
            }
            QStringList images;
            m_aptHeaders.clear();
            for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                const QString n = line.section(QStringLiteral(" - "), 0, 0).trimmed();
                if (!safeToken(n) || n.endsWith(QLatin1String("-dbg")) || n.contains(QLatin1String("dbgsym"))
                    || n.contains(QLatin1String("-unsigned"))) continue;
                if (n.startsWith(QLatin1String("linux-headers-"))) m_aptHeaders.insert(n);
                else images << n;
            }
            if (images.isEmpty()) {
                m_apt.clear();
                fillApt();
                setBusy(false);
                return;
            }
            runAsync(QStringLiteral("apt-cache"), QStringList{"madison"} + images,
                     [this](int c2, const QString &o2, const QString &e2) {
                setBusy(false);
                if (c2 != 0) { fail(QStringLiteral("apt-cache madison failed: ") + e2.trimmed()); return; }
                QMap<QString, KPkg> best;
                for (const QString &line : o2.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                    const QStringList f = line.split(QLatin1Char('|'));
                    if (f.size() < 3) continue;
                    const QString src = f.at(2).trimmed();
                    if (src.endsWith(QLatin1String("Sources"))) continue;
                    const QStringList tok = src.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                    if (tok.size() < 2) continue;
                    KPkg k;
                    k.name = f.at(0).trimmed();
                    k.version = f.at(1).trimmed();
                    k.release = releaseOf(k.name);
                    k.suite = tok.at(1).section(QLatin1Char('/'), 0, 0);
                    if (k.release.isEmpty()) continue;
                    const QString url = tok.at(0);
                    if (url.contains(QLatin1String("liquorix"))) k.src = SrcLiquorix;
                    else if (url.contains(QLatin1String("xanmod"))) k.src = SrcXanmod;
                    else k.src = SrcRepo;
                    if (k.src == SrcXanmod) {
                        static const QRegularExpression lv(QStringLiteral("x64v(\\d)"));
                        const auto m = lv.match(k.name);
                        if (m.hasMatch() && m.captured(1).toInt() > m_cpuLevel) continue;
                    }
                    auto it = best.find(k.name);
                    if (it == best.end() || verCmp(k.version, it->version) > 0) best.insert(k.name, k);
                }
                m_apt = best.values().toVector();
                std::sort(m_apt.begin(), m_apt.end(), [](const KPkg &a, const KPkg &b) {
                    const int c = verCmp(a.release, b.release);
                    return c != 0 ? c > 0 : a.name < b.name;
                });
                fillApt();
            });
        });
    }

    void loadMainline()
    {
        setBusy(true, QStringLiteral("Fetching mainline index…"));
        fetchText(kMainlineBase, [this](bool ok, const QString &html) {
            setBusy(false);
            if (!ok) { fail(QStringLiteral("Could not read ") + kMainlineBase + QStringLiteral(": ") + html); return; }
            static const QRegularExpression re(QStringLiteral("href=\"(v(\\d+)\\.(\\d+)(?:\\.(\\d+))?(?:-rc(\\d+))?)/\""));
            QSet<QString> seen;
            m_ml.clear();
            auto it = re.globalMatch(html);
            while (it.hasNext()) {
                const auto m = it.next();
                if (seen.contains(m.captured(1))) continue;
                seen.insert(m.captured(1));
                MLVer v;
                v.tag = m.captured(1);
                v.maj = m.captured(2).toInt();
                v.min = m.captured(3).toInt();
                v.pat = m.captured(4).toInt();
                v.rc  = m.captured(5).toInt();
                m_ml << v;
            }
            std::sort(m_ml.begin(), m_ml.end(), [](const MLVer &a, const MLVer &b) { return b < a; });
            if (m_ml.isEmpty()) { fail(QStringLiteral("The mainline index was fetched but no versions could be parsed from it.")); return; }
            m_mlLoaded = true;
            fillMainline();
        });
    }

    // ================================================================ filling
    QString statusFor(const QString &release) const
    {
        QStringList s;
        if (release == m_running) s << QStringLiteral("Running");
        for (auto it = m_images.cbegin(); it != m_images.cend(); ++it)
            if (releaseOf(it.key()) == release) { s << QStringLiteral("Installed"); break; }
        return s.join(QStringLiteral(" · "));
    }

    void fillInstalled()
    {
        m_pInst.tree->clear();
        QStringList rels;
        QMap<QString, QString> pkgOf;
        for (auto it = m_images.cbegin(); it != m_images.cend(); ++it) {
            rels << releaseOf(it.key());
            pkgOf.insert(releaseOf(it.key()), it.key());
        }
        std::sort(rels.begin(), rels.end(), [](const QString &a, const QString &b) { return verCmp(a, b) > 0; });
        const bool newestIsDefault = m_hasGrub && m_grubDefaultRel.isEmpty()
                                     && m_grubDefaultDesc.startsWith(QLatin1String("newest"));
        for (int i = 0; i < rels.size(); ++i) {
            const QString &r = rels.at(i);
            QStringList st;
            if (r == m_running) st << QStringLiteral("Running");
            if (r == m_grubDefaultRel || (newestIsDefault && i == 0)) st << QStringLiteral("Boot default");
            auto *item = new QTreeWidgetItem({r, srcName(classifyRelease(r)), st.join(QStringLiteral(" · "))});
            item->setData(0, Qt::UserRole, pkgOf.value(r));
            item->setData(0, Qt::UserRole + 1, r);
            m_pInst.tree->addTopLevelItem(item);
        }
    }

    void fillApt()
    {
        for (Page *p : {&m_pRepo, &m_pLiq, &m_pXan}) p->tree->clear();
        for (const KPkg &k : m_apt) {
            Page &p = k.src == SrcLiquorix ? m_pLiq : k.src == SrcXanmod ? m_pXan : m_pRepo;
            auto *item = new QTreeWidgetItem({k.release, k.version, k.suite, statusFor(k.release)});
            item->setData(0, Qt::UserRole, k.name);
            item->setData(0, Qt::UserRole + 1, k.release);
            item->setData(0, Qt::UserRole + 2, k.suite);
            p.tree->addTopLevelItem(item);
        }
    }

    void fillMainline()
    {
        m_pMl.tree->clear();
        int shown = 0;
        for (const MLVer &v : m_ml) {
            if (v.rc && !m_showRc->isChecked()) continue;
            auto *item = new QTreeWidgetItem({v.tag, v.release(), statusFor(v.release())});
            item->setData(0, Qt::UserRole, v.tag);
            item->setData(0, Qt::UserRole + 1, v.release());
            m_pMl.tree->addTopLevelItem(item);
            if (++shown >= 120) break;
        }
    }

    // ================================================================ root jobs
    void runRoot(const QString &title, const QString &script, std::function<void(bool)> done = nullptr)
    {
        QString err;
        QProcessEnvironment *env = askpassEnv(&err);
        if (!env) { fail(err); return; }

        showLog(true);
        m_log->appendPlainText(QStringLiteral("\n=== ") + title + QStringLiteral(" ==="));
        setBusy(true, title + QStringLiteral("…"));

        auto *p = new QProcess(this);
        QProcessEnvironment e = *env;
        e.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
        p->setProcessEnvironment(e);
        p->setProcessChannelMode(QProcess::MergedChannels);
        auto output = std::make_shared<QString>();
        auto fired = std::make_shared<bool>(false);
        connect(p, &QProcess::readyReadStandardOutput, this, [this, p, output]() {
            const QString chunk = QString::fromUtf8(p->readAllStandardOutput());
            output->append(chunk);
            log(chunk);
        });
        auto finish = [this, p, title, done, output, fired](int code) {
            if (*fired) return;
            *fired = true;
            setBusy(false);
            const bool ok = code == 0;
            if (!ok) {
                banner(QStringLiteral("error"), title + QStringLiteral(" failed (exit code %1). The log below shows what went wrong.").arg(code));
                log(QStringLiteral("ERROR: ") + title + QStringLiteral(" failed with exit code %1").arg(code));
            } else if (output->contains(QLatin1String("WARNING:"))) {
                QString w;
                for (const QString &l : output->split(QLatin1Char('\n')))
                    if (l.contains(QLatin1String("WARNING:"))) w += l.trimmed() + QLatin1Char('\n');
                banner(QStringLiteral("warn"), title + QStringLiteral(" finished with warnings:\n") + w.trimmed());
            } else {
                banner(QStringLiteral("ok"), title + QStringLiteral(" finished."));
            }
            p->deleteLater();
            if (done) done(ok);
        };
        connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [finish](int code, QProcess::ExitStatus st) { finish(st == QProcess::NormalExit ? code : -1); });
        connect(p, &QProcess::errorOccurred, this, [finish, this](QProcess::ProcessError err) {
            if (err == QProcess::FailedToStart) {
                log(QStringLiteral("ERROR: sudo could not be started (is it installed?)"));
                finish(-1);
            }
        });
        const QString full = QStringLiteral("set -eu\nexport DEBIAN_FRONTEND=noninteractive\n") + script;
        p->start(QStringLiteral("sudo"), {"-A", "--", "/bin/sh", "-c", full});
    }

    QTreeWidgetItem *selected(const Page &p)
    {
        const auto items = p.tree->selectedItems();
        if (items.isEmpty()) { fail(QStringLiteral("Select a kernel in the list first.")); return nullptr; }
        return items.first();
    }

    bool confirm(const QString &title, const QString &text)
    {
        QMessageBox box(QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No, this);
        box.setDefaultButton(QMessageBox::No);
        return box.exec() == QMessageBox::Yes;
    }

    QString sbWarning() const
    {
        return m_sbOn ? QStringLiteral("\n\nSecure Boot is ON. This kernel is unsigned and will not boot until Secure Boot is disabled in firmware.")
                      : QString();
    }

    // ================================================================ actions
    void aptUpdate()
    {
        runRoot(QStringLiteral("Update package lists"),
                QStringLiteral("apt-get %1 update\n").arg(kAptOpts),
                [this](bool) { reloadLocal(); });
    }

    void installApt(Page &page)
    {
        QTreeWidgetItem *it = selected(page);
        if (!it) return;
        const QString pkg = it->data(0, Qt::UserRole).toString();
        const QString rel = it->data(0, Qt::UserRole + 1).toString();
        const QString suite = it->data(0, Qt::UserRole + 2).toString();
        if (!safeToken(pkg) || !safeToken(suite)) { fail(QStringLiteral("Refusing unexpected package name: ") + pkg); return; }

        QStringList pkgs = {pkg};
        const QString hdr = QStringLiteral("linux-headers-") + rel;
        const bool wantHdr = m_headers->isChecked();
        if (wantHdr && m_aptHeaders.contains(hdr)) pkgs << hdr;

        QString text = QStringLiteral("Install %1?\n\nPackages: %2\nFrom: %3").arg(rel, pkgs.join(QStringLiteral(", ")), suite);
        if (wantHdr && !m_aptHeaders.contains(hdr)) text += QStringLiteral("\n\nNo matching headers package is available, so headers will be skipped.");
        if (&page != &m_pRepo) text += sbWarning();
        if (!confirm(QStringLiteral("Install kernel"), text)) return;

        QStringList quoted;
        for (const QString &p : pkgs) quoted << sq(p);
        const QString target = suite.endsWith(QLatin1String("-backports")) ? QStringLiteral("-t ") + sq(suite) + QLatin1Char(' ') : QString();
        QString s;
        s += QStringLiteral("echo '==> Updating package lists'\napt-get %1 update\n").arg(kAptOpts);
        s += QStringLiteral("echo '==> Installing %1'\napt-get %2 install -y %3%4\n").arg(rel, kAptOpts, target, quoted.join(QLatin1Char(' ')));
        if (wantHdr && !m_aptHeaders.contains(hdr)) s += QStringLiteral("echo 'WARNING: no headers package for %1; DKMS modules will not build for it'\n").arg(rel);
        s += QStringLiteral("echo '==> Done. Reboot to use the new kernel.'\n");
        runRoot(QStringLiteral("Install ") + rel, s, [this](bool) { reloadLocal(); });
    }

    static QStringList parseDebs(const QString &html)
    {
        static const QRegularExpression re(QStringLiteral("href=\"([^\"/]+\\.deb)\""));
        QStringList out;
        auto it = re.globalMatch(html);
        while (it.hasNext()) {
            const QString f = QUrl::fromPercentEncoding(it.next().captured(1).toUtf8());
            if (!out.contains(f)) out << f;
        }
        out.sort();
        return out;
    }

    static QString pickLast(const QStringList &files, const QString &pattern)
    {
        const QRegularExpression re(pattern);
        QString hit;
        for (const QString &f : files) if (re.match(f).hasMatch()) hit = f;
        return hit;
    }

    void installMainline()
    {
        QTreeWidgetItem *it = selected(m_pMl);
        if (!it) return;
        const QString tag = it->data(0, Qt::UserRole).toString();
        setBusy(true, QStringLiteral("Looking up build files for ") + tag + QStringLiteral("…"));
        mlTryDirs(tag, {kMainlineBase + tag + QLatin1Char('/') + m_arch + QLatin1Char('/'),
                        kMainlineBase + tag + QLatin1Char('/')});
    }

    void mlTryDirs(const QString &tag, QStringList dirs)
    {
        const QString dir = dirs.takeFirst();
        fetchText(dir, [this, tag, dir, dirs](bool ok, const QString &html) {
            const QStringList debs = ok ? parseDebs(html) : QStringList();
            const QString a = QRegularExpression::escape(m_arch);
            const QString img = pickLast(debs, QStringLiteral("^linux-image-(unsigned-)?[^_]+-generic_[^_]+_%1\\.deb$").arg(a));
            if (img.isEmpty()) {
                if (!dirs.isEmpty()) { mlTryDirs(tag, dirs); return; }
                setBusy(false);
                fail(QStringLiteral("No %1 generic kernel build was found for %2. Ubuntu's build for this version may have failed for your architecture.").arg(m_arch, tag));
                return;
            }
            QStringList files = {img};
            const QString mods = pickLast(debs, QStringLiteral("^linux-modules-[^_]+-generic_[^_]+_%1\\.deb$").arg(a));
            if (!mods.isEmpty()) files << mods;
            const int required = files.size();
            if (m_headers->isChecked()) {
                const QString hAll = pickLast(debs, QStringLiteral("^linux-headers-[^_]+_[^_]+_all\\.deb$"));
                const QString hGen = pickLast(debs, QStringLiteral("^linux-headers-[^_]+-generic_[^_]+_%1\\.deb$").arg(a));
                if (!hAll.isEmpty()) files << hAll;
                if (!hGen.isEmpty()) files << hGen;
            }
            for (const QString &f : files)
                if (!safeToken(f)) { setBusy(false); fail(QStringLiteral("Refusing unexpected file name: ") + f); return; }

            fetchText(dir + QStringLiteral("CHECKSUMS"), [this, tag, dir, files, required](bool okSum, const QString &sums) {
                setBusy(false);
                QMap<QString, QString> sha;
                if (okSum) {
                    static const QRegularExpression line(QStringLiteral("^([0-9a-f]{64})\\s+\\*?(\\S+)\\s*$"),
                                                         QRegularExpression::MultilineOption);
                    auto it = line.globalMatch(sums);
                    while (it.hasNext()) { const auto m = it.next(); sha.insert(m.captured(2), m.captured(1)); }
                }
                bool verified = true;
                for (const QString &f : files) if (!sha.contains(f)) verified = false;

                QString text = QStringLiteral("Install mainline %1?\n\nFiles:\n  %2").arg(tag, files.join(QStringLiteral("\n  ")));
                text += QStringLiteral("\n\nThis kernel is unsigned, uses Ubuntu's configuration and will not receive updates through apt.");
                if (!verified) text += QStringLiteral("\n\nWARNING: SHA-256 checksums are not published for every file, so the download cannot be fully verified.");
                text += sbWarning();
                if (!confirm(QStringLiteral("Install mainline kernel"), text)) return;
                mlRun(tag, dir, files, required, sha);
            });
        });
    }

    void mlRun(const QString &tag, const QString &dir, const QStringList &files, int required,
               const QMap<QString, QString> &sha)
    {
        QString s;
        s += QStringLiteral("work=$(mktemp -d /var/tmp/osm-kernels.XXXXXX)\ntrap 'rm -rf \"$work\"' EXIT\ncd \"$work\"\n");
        for (const QString &f : files) {
            s += QStringLiteral("echo '==> Downloading %1'\n").arg(f);
            s += dlCmd(dir + f, f) + QLatin1Char('\n');
        }
        s += QStringLiteral("echo '==> Verifying SHA-256 checksums'\n");
        for (const QString &f : files) {
            if (sha.contains(f)) s += QStringLiteral("printf '%s  %s\\n' %1 %2 | sha256sum -c -\n").arg(sq(sha.value(f)), sq(f));
            else s += QStringLiteral("echo 'WARNING: no published checksum for %1; installed unverified'\n").arg(f);
        }
        QStringList q;
        for (const QString &f : files) q << sq(f);
        s += QStringLiteral("echo '==> Installing packages'\n");
        s += QStringLiteral("if ! dpkg -i %1; then\n  echo '==> dpkg reported problems; asking apt to resolve dependencies'\n  apt-get %2 install -f -y\nfi\n")
                 .arg(q.join(QLatin1Char(' ')), kAptOpts);
        for (int i = 0; i < files.size(); ++i) {
            const QString pkg = files.at(i).section(QLatin1Char('_'), 0, 0);
            const QString check = QStringLiteral("dpkg-query -W -f='${db:Status-Abbrev}' %1 2>/dev/null | grep -q '^ii'").arg(sq(pkg));
            if (i < required)
                s += QStringLiteral("if ! %1; then echo 'ERROR: %2 is not installed after dpkg/apt' >&2; exit 1; fi\n").arg(check, pkg);
            else
                s += QStringLiteral("if ! %1; then echo 'WARNING: %2 could not be installed (often a newer libc requirement); the kernel itself is fine but DKMS modules will not build'; fi\n").arg(check, pkg);
        }
        s += QStringLiteral("echo '==> Done. Reboot to use the new kernel.'\n");
        runRoot(QStringLiteral("Install mainline ") + tag, s, [this](bool) { reloadLocal(); });
    }

    void enableRepo(int which)
    {
        if (m_codename.isEmpty()) { fail(QStringLiteral("Could not work out the Debian release this system is based on, so the repository cannot be added.")); return; }
        if (m_fetch.isEmpty()) { fail(QStringLiteral("Neither curl nor wget is installed.")); return; }
        const bool liq = which == SrcLiquorix;
        const QString name = liq ? QStringLiteral("Liquorix") : QStringLiteral("XanMod");
        const QString keyUrl = liq ? QStringLiteral("https://liquorix.net/liquorix-keyring.gpg")
                                   : QStringLiteral("https://dl.xanmod.org/archive.key");
        const QString keyring = liq ? QStringLiteral("/etc/apt/keyrings/liquorix-keyring.gpg")
                                    : QStringLiteral("/etc/apt/keyrings/xanmod-archive-keyring.gpg");
        const QString list = liq ? QStringLiteral("/etc/apt/sources.list.d/liquorix.list")
                                 : QStringLiteral("/etc/apt/sources.list.d/xanmod-release.list");
        const QString line = liq
            ? QStringLiteral("deb [arch=amd64 signed-by=%1] https://liquorix.net/debian %2 main").arg(keyring, m_codename)
            : QStringLiteral("deb [signed-by=%1] http://deb.xanmod.org %2 main").arg(keyring, m_codename);

        if (!confirm(QStringLiteral("Enable ") + name,
                     QStringLiteral("Add the %1 signing key and this apt source?\n\n%2\n\nThird-party kernels are not supported by Devuan.").arg(name, line)))
            return;

        QString s;
        s += QStringLiteral("if ! command -v gpg >/dev/null 2>&1; then\n  echo '==> Installing gpg'\n  apt-get %1 install -y --no-install-recommends gpg\nfi\n").arg(kAptOpts);
        s += QStringLiteral("mkdir -p /etc/apt/keyrings\nchmod 0755 /etc/apt/keyrings\n");
        s += QStringLiteral("tmp=$(mktemp)\ntrap 'rm -f \"$tmp\"' EXIT\n");
        s += QStringLiteral("echo '==> Downloading %1 signing key'\n").arg(name);
        s += dlCmd(keyUrl, QStringLiteral("$tmp")).replace(QStringLiteral("'$tmp'"), QStringLiteral("\"$tmp\"")) + QLatin1Char('\n');
        s += QStringLiteral("gpg --batch --yes --dearmor -o %1 \"$tmp\"\nchmod 0644 %1\n").arg(sq(keyring));
        s += QStringLiteral("echo '==> Writing %1'\nprintf '%s\\n' %2 > %1\n").arg(list, sq(line));
        s += QStringLiteral("echo '==> Updating package lists'\napt-get %1 update\n").arg(kAptOpts);
        runRoot(QStringLiteral("Enable ") + name + QStringLiteral(" repository"), s, [this](bool) { applyNotes(); reloadLocal(); });
    }

    void removeSelected()
    {
        QTreeWidgetItem *it = selected(m_pInst);
        if (!it) return;
        const QString rel = it->data(0, Qt::UserRole + 1).toString();
        if (rel == m_running) { fail(QStringLiteral("%1 is the running kernel and cannot be removed. Boot another kernel first.").arg(rel)); return; }
        if (m_images.size() <= 1) { fail(QStringLiteral("This is the only installed kernel; removing it would leave the system unbootable.")); return; }

        QStringList cands = {"linux-image-" + rel, "linux-image-unsigned-" + rel, "linux-modules-" + rel,
                             "linux-modules-extra-" + rel, "linux-headers-" + rel};
        if (rel.endsWith(QLatin1String("-generic"))) cands << QStringLiteral("linux-headers-") + rel.chopped(8);
        QStringList pkgs;
        for (const QString &c : cands) if (m_installed.contains(c) && safeToken(c)) pkgs << c;
        if (pkgs.isEmpty()) { fail(QStringLiteral("No installed packages found for ") + rel); return; }

        setBusy(true, QStringLiteral("Checking what removal would affect…"));
        runAsync(QStringLiteral("apt-get"), QStringList{"-s", "purge"} + pkgs,
                 [this, rel, pkgs](int code, const QString &out, const QString &err) {
            setBusy(false);
            if (code != 0) { fail(QStringLiteral("apt refused the removal plan:\n") + (err.trimmed().isEmpty() ? out.trimmed() : err.trimmed())); return; }
            QStringList extra;
            for (const QString &line : out.split(QLatin1Char('\n'))) {
                if (!line.startsWith(QLatin1String("Purg ")) && !line.startsWith(QLatin1String("Remv "))) continue;
                const QString n = line.section(QLatin1Char(' '), 1, 1);
                if (!pkgs.contains(n) && !extra.contains(n)) extra << n;
            }
            QString text = QStringLiteral("Remove kernel %1?\n\nPackages: %2").arg(rel, pkgs.join(QStringLiteral(", ")));
            if (!extra.isEmpty())
                text += QStringLiteral("\n\nWARNING: apt will ALSO remove:\n  %1\n\nIf that includes a metapackage such as linux-image-amd64, you will stop receiving kernel updates from it.")
                            .arg(extra.join(QStringLiteral("\n  ")));
            if (!confirm(QStringLiteral("Remove kernel"), text)) return;

            QStringList q;
            for (const QString &p : pkgs) q << sq(p);
            QString s = QStringLiteral("echo '==> Removing %1'\napt-get %2 purge -y %3\n").arg(rel, kAptOpts, q.join(QLatin1Char(' ')));
            if (rel == m_grubDefaultRel && m_hasGrub)
                s += QStringLiteral("echo '==> Removed kernel was the boot default; reverting to newest'\nrm -f %1\nupdate-grub\n").arg(sq(kGrubOverride));
            runRoot(QStringLiteral("Remove ") + rel, s, [this](bool) { reloadLocal(); });
        });
    }

    static QString grubPathFor(const QString &cfg, const QString &rel)
    {
        static const QRegularExpression idRe(QStringLiteral("\\$menuentry_id_option\\s+'([^']+)'"));
        const QString want = QStringLiteral("gnulinux-") + rel + QStringLiteral("-advanced-");
        QString submenu;
        int depth = 0, submenuDepth = -1;
        for (const QString &raw : cfg.split(QLatin1Char('\n'))) {
            const QString t = raw.trimmed();
            if (t.startsWith(QLatin1String("submenu ")) || t.startsWith(QLatin1String("menuentry "))) {
                const auto m = idRe.match(t);
                const QString id = m.hasMatch() ? m.captured(1) : QString();
                if (t.startsWith(QLatin1String("submenu "))) {
                    submenu = id;
                    submenuDepth = depth;
                } else if (id.startsWith(want) && !id.mid(want.size()).contains(QLatin1String("recovery"))) {
                    return (submenuDepth >= 0 && !submenu.isEmpty()) ? submenu + QLatin1Char('>') + id : id;
                }
            }
            depth += t.count(QLatin1Char('{')) - t.count(QLatin1Char('}'));
            if (submenuDepth >= 0 && depth <= submenuDepth) { submenuDepth = -1; submenu.clear(); }
        }
        return {};
    }

    void setDefault()
    {
        if (!m_hasGrub) { fail(QStringLiteral("GRUB is not in use on this system.")); return; }
        QTreeWidgetItem *it = selected(m_pInst);
        if (!it) return;
        const QString rel = it->data(0, Qt::UserRole + 1).toString();
        const QString cfgPath = QStringLiteral("/boot/grub/grub.cfg");

        auto proceed = [this, rel](const QString &cfg) {
            const QString path = grubPathFor(cfg, rel);
            static const QRegularExpression ok(QStringLiteral("^[A-Za-z0-9.+_~>-]+$"));
            if (path.isEmpty() || !ok.match(path).hasMatch()) {
                fail(QStringLiteral("GRUB has no menu entry for %1. Press \"Update GRUB menu\" and try again.").arg(rel));
                return;
            }
            QString s;
            s += QStringLiteral("mkdir -p /etc/default/grub.d\n");
            s += QStringLiteral("printf '%s\\n' '# Managed by osm-kernels: boot default kernel' %1 > %2\n")
                     .arg(sq(QStringLiteral("GRUB_DEFAULT=\"") + path + QLatin1Char('"')), sq(kGrubOverride));
            s += QStringLiteral("echo '==> Regenerating GRUB menu'\nupdate-grub\n");
            runRoot(QStringLiteral("Set boot default to ") + rel, s, [this](bool) { reloadLocal(); });
        };

        QFile f(cfgPath);
        if (f.open(QIODevice::ReadOnly)) { proceed(QString::fromUtf8(f.readAll())); return; }

        QString err;
        QProcessEnvironment *env = askpassEnv(&err);
        if (!env) { fail(err); return; }
        setBusy(true, QStringLiteral("Reading GRUB menu…"));
        runAsync(QStringLiteral("sudo"), {"-A", "--", "cat", cfgPath},
                 [this, proceed, cfgPath](int code, const QString &out, const QString &e) {
            setBusy(false);
            if (code != 0) { fail(QStringLiteral("Could not read %1: %2").arg(cfgPath, e.trimmed())); return; }
            proceed(out);
        }, env);
    }

    void resetDefault()
    {
        if (!m_hasGrub) { fail(QStringLiteral("GRUB is not in use on this system.")); return; }
        if (!QFile::exists(kGrubOverride)) {
            banner(QStringLiteral("ok"), QStringLiteral("No kernel is pinned by this app; GRUB already uses its own default (%1).").arg(m_grubDefaultDesc));
            return;
        }
        runRoot(QStringLiteral("Boot newest kernel by default"),
                QStringLiteral("rm -f %1\necho '==> Regenerating GRUB menu'\nupdate-grub\n").arg(sq(kGrubOverride)),
                [this](bool) { reloadLocal(); });
    }

    void updateGrub()
    {
        if (!m_hasGrub) { fail(QStringLiteral("GRUB is not in use on this system.")); return; }
        runRoot(QStringLiteral("Update GRUB menu"), QStringLiteral("update-grub\n"));
    }
};

const char *kStyle = R"(
QWidget { background: #282828; color: #ebdbb2; font-family: 'DejaVu Sans'; font-size: 11pt; }
QFrame#card { background: #32302f; border-radius: 12px; }
QFrame#card QLabel { background: transparent; }
QLabel#big { font-family: 'DejaVu Sans'; font-size: 15pt; font-weight: bold; color: #fbf1c7; }
QLabel#small { font-family: 'DejaVu Sans'; font-size: 9pt; color: #a89984; }
QLabel#note { font-family: 'DejaVu Sans'; color: #bdae93; padding: 4px 2px; }
QLabel#warnnote { font-family: 'DejaVu Sans'; color: #fabd2f; padding: 4px 2px; }
QLabel#error { font-family: 'DejaVu Sans'; background: #9d0006; color: #fbf1c7; padding: 10px; border-radius: 8px; }
QLabel#warn { font-family: 'DejaVu Sans'; background: #b57614; color: #282828; padding: 10px; border-radius: 8px; }
QLabel#ok { font-family: 'DejaVu Sans'; background: #79740e; color: #fbf1c7; padding: 10px; border-radius: 8px; }
QPushButton { background: #3c3836; border: 1px solid #504945; border-radius: 8px; padding: 8px 16px; }
QPushButton:pressed { background: #504945; }
QPushButton:checked { background: #458588; color: #fbf1c7; border-color: #83a598; }
QPushButton:disabled { color: #7c6f64; }
QTreeWidget { background: #1d2021; alternate-background-color: #232323; border: 1px solid #3c3836; border-radius: 8px; }
QTreeWidget::item { min-height: 44px; padding: 0 6px; }
QTreeWidget::item:selected { background: #458588; color: #fbf1c7; }
QHeaderView::section { background: #32302f; color: #a89984; padding: 6px; border: none; }
QPlainTextEdit { background: #1d2021; border: 1px solid #3c3836; border-radius: 8px; font-family: 'DejaVu Sans Mono'; font-size: 9pt; }
QCheckBox { spacing: 10px; }
QCheckBox::indicator { width: 26px; height: 26px; }
QScrollBar:vertical { width: 14px; background: #282828; }
QScrollBar::handle:vertical { background: #504945; border-radius: 6px; min-height: 40px; }
QMessageBox QLabel { font-family: 'DejaVu Sans'; }
)";

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("osm-kernels"));
    app.setFont(QFont(QStringLiteral("DejaVu Sans"), 11));
    app.setStyleSheet(QString::fromLatin1(kStyle));
    KernelWindow w;
    w.show();
    return app.exec();
}
