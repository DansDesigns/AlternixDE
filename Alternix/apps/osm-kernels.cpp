// osm-kernels.cpp — AlternixDE kernel manager
//
// Lists, installs and removes kernels from five sources:
//   Repository  Devuan/Debian archive (incl. backports) via apt
//   Mainline    Ubuntu mainline builds from kernel.ubuntu.com/mainline
//   Liquorix    liquorix.net apt repo (amd64 only)
//   XanMod      deb.xanmod.org apt repo (amd64 only, filtered by x86-64 psABI level)
//   Custom      user-added, fingerprint-verified HTTPS APT kernel repositories
// and sets which installed kernel GRUB boots by default
// (via /etc/default/grub.d/99-osm-kernels.cfg, so /etc/default/grub is never edited).
//
// Build:
//   g++ -std=c++17 -Wall -Wextra -O2 -fPIC osm-kernels.cpp -o osm-kernels $(pkg-config --cflags --libs Qt5Widgets)
//
// Root actions run through osm-sudo, which authenticates using osm-lock and then
// launches commands via sudo -n. osm-sudo requires an appropriate sudoers policy;
// the pattern is not a Unix password and is never passed to sudo as one.
// Runtime needs: apt, dpkg, sudo, osm-sudo, curl or wget, sha256sum;
//                gpg for signed custom repositories and Liquorix/XanMod repos.

#include <QtWidgets>
#include <functional>
#include <QCryptographicHash>
#include <QUrl>
#include <QQueue>
#include <QTimer>

namespace {

const QString kMainlineBase = QStringLiteral("https://kernel.ubuntu.com/mainline/");
const QString kGrubOverride = QStringLiteral("/etc/default/grub.d/99-osm-kernels.cfg");
const QString kAptOpts = QStringLiteral(
    "-o DPkg::Lock::Timeout=120 -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold");

enum Src { SrcRepo = 0, SrcMainline, SrcLiquorix, SrcXanmod, SrcCustom };

QString srcName(int s)
{
    switch (s) {
    case SrcMainline: return QStringLiteral("Mainline");
    case SrcLiquorix: return QStringLiteral("Liquorix");
    case SrcXanmod:   return QStringLiteral("XanMod");
    case SrcCustom:   return QStringLiteral("Custom");
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
    QString name, release, version, suite, customName;
    int src = SrcRepo;
};

struct CustomSource {
    QString id, name, url, suite, components;
    QString listPath() const {
        return QStringLiteral("/etc/apt/sources.list.d/osm-kernels-custom-") + id + QStringLiteral(".list");
    }
    QString keyPath() const {
        return QStringLiteral("/etc/apt/keyrings/osm-kernels-custom-") + id + QStringLiteral(".gpg");
    }
};

bool validHttpsUrl(const QString &input)
{
    const QUrl u(input, QUrl::StrictMode);
    return u.isValid() && u.scheme() == QLatin1String("https") && !u.host().isEmpty()
        && u.userInfo().isEmpty() && u.query().isEmpty() && u.fragment().isEmpty()
        && !input.contains(QRegularExpression(QStringLiteral("\\s")));
}

bool validAptField(const QString &value)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9.+_~/-]*$"));
    return re.match(value).hasMatch();
}


struct MLVer {
    QString tag;              // v6.17.2
    QString archiveDate;      // archive directory last-modified date, when published
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

        loadCustomSources();
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
    // Dates are sourced from publisher changelogs or upstream release archives;
    // never from the local installation/discovery time.
    struct Published { QString date, provenance; };
    QHash<QString, Published> m_published;
    QHash<QString, QString> m_upstreamDates;
    QSet<QString> m_changelogAttempted;
    QSet<int> m_upstreamAttempted;
    QQueue<QPair<QString, QString>> m_changelogQueue; // cache key, pkg=version
    int m_changelogActive = 0;
    QSettings m_publishSettings{QSettings::IniFormat, QSettings::UserScope,
                                QStringLiteral("AlternixOS"), QStringLiteral("osm-kernels-published")};
    QSet<QString> m_installed;            // fully configured linux-* packages
    QSet<QString> m_present;              // also include unpacked / half-configured packages for removal
    QVector<KPkg> m_apt;
    QSet<QString> m_aptHeaders;
    QVector<MLVer> m_ml;
    QVector<CustomSource> m_customSources;
    QString m_grubDefaultRel, m_grubDefaultDesc;

    // ---------------------------------------------------------------- widgets
    QLabel *m_hRunning = nullptr, *m_hDefault = nullptr, *m_hSystem = nullptr, *m_hBoot = nullptr, *m_hCmdline = nullptr;
    QStackedWidget *m_stack = nullptr;
    Page m_pInst, m_pRepo, m_pMl, m_pLiq, m_pXan, m_pCustom;
    QCheckBox *m_headers = nullptr, *m_showRc = nullptr;
    QPushButton *m_removeBtn = nullptr, *m_customRemoveBtn = nullptr;
    QListWidget *m_customRepos = nullptr;
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

    QString osmSudoPath(QString *errOut) const
    {
        // osm-sudo accepts a COMMAND, not a sudo password prompt. Never invoke
        // it as SUDO_ASKPASS or fall back to terminal password authentication.
        QString path = QStandardPaths::findExecutable(QStringLiteral("osm-sudo"));
        if (path.isEmpty())
            path = QStandardPaths::findExecutable(QStringLiteral("osm-sudo"),
                                                  {QStringLiteral("/usr/local/bin"),
                                                   QStringLiteral("/usr/bin")});
        if (path.isEmpty())
            *errOut = QStringLiteral("osm-sudo is not installed or is not executable. "
                                     "Administrator actions require the AlternixOS pattern authenticator.");
        return path;
    }

    // Read only repositories managed by this application; never modify unrelated APT sources.
    void loadCustomSources()
    {
        m_customSources.clear();
        QDir d(QStringLiteral("/etc/apt/sources.list.d"));
        const QStringList names = d.entryList({QStringLiteral("osm-kernels-custom-*.list")}, QDir::Files);
        static const QRegularExpression idRe(QStringLiteral("^osm-kernels-custom-([a-f0-9]{12})\\.list$"));
        static const QRegularExpression lineRe(QStringLiteral(
            "^deb \\[arch=[A-Za-z0-9_]+ signed-by=/etc/apt/keyrings/osm-kernels-custom-([a-f0-9]{12})\\.gpg\\] "
            "(https://[^ ]+) ([^ ]+) ([^\\n]+)$"));
        for (const QString &filename : names) {
            const auto idMatch = idRe.match(filename);
            if (!idMatch.hasMatch()) continue;
            const QString id = idMatch.captured(1);
            const QString contents = readFile(d.absoluteFilePath(filename));
            CustomSource source;
            source.id = id;
            for (const QString &raw : contents.split(QLatin1Char('\n'))) {
                const QString line = raw.trimmed();
                if (line.startsWith(QLatin1String("# osm-kernels-name: ")))
                    source.name = line.mid(20).trimmed();
                const auto match = lineRe.match(line);
                if (match.hasMatch() && match.captured(1) == id) {
                    source.url = match.captured(2);
                    source.suite = match.captured(3);
                    source.components = match.captured(4);
                }
            }
            if (source.name.isEmpty()) source.name = id;
            if (validHttpsUrl(source.url) && validAptField(source.suite))
                m_customSources << source;
        }
    }

    void fillCustomRepos()
    {
        if (!m_customRepos) return;
        const QString previousId = m_customRepos->currentItem()
            ? m_customRepos->currentItem()->data(Qt::UserRole).toString() : QString();
        m_customRepos->clear();
        for (const CustomSource &source : m_customSources) {
            auto *item = new QListWidgetItem(source.name + QStringLiteral("  —  ")
                + source.url + QStringLiteral("  [") + source.suite + QLatin1Char(']'));
            item->setData(Qt::UserRole, source.id);
            item->setToolTip(source.components + QStringLiteral(" · ") + source.listPath());
            m_customRepos->addItem(item);
            if (source.id == previousId) m_customRepos->setCurrentItem(item);
        }
        updateCustomRemoveButton();
    }

    void updateCustomRemoveButton()
    {
        if (m_customRemoveBtn)
            m_customRemoveBtn->setEnabled(!m_busy && m_customRepos && m_customRepos->currentItem());
    }

    void updateRemoveButton()
    {
        if (!m_removeBtn || !m_pInst.tree) return;
        const auto chosen = m_pInst.tree->selectedItems();
        const QString rel = chosen.isEmpty() ? QString() : chosen.first()->data(0, Qt::UserRole + 1).toString();
        const bool isRunning = rel == m_running;
        const bool lastKernel = m_images.size() <= 1;
        m_removeBtn->setEnabled(!m_busy && !rel.isEmpty() && !isRunning && !lastKernel);
        m_removeBtn->setToolTip(rel.isEmpty() ? QStringLiteral("Select an installed kernel first")
            : isRunning ? QStringLiteral("Cannot uninstall the running kernel")
            : lastKernel ? QStringLiteral("Cannot uninstall the only installed kernel")
            : QStringLiteral("Uninstall the selected kernel, its modules and matching headers"));
    }

    // APT's Packages files have no per-binary publication timestamp. The
    // changelog date describes that exact package release (not its install date).
    // A kernel.org ChangeLog file timestamp is used only when package-specific
    // metadata is unavailable; its tooltip makes that distinction explicit.
    static QString aptPublishKey(const KPkg &k)
    {
        // Flavor binaries often share the same source changelog/version.
        // Keep third-party and custom repositories distinct from Debian.
        return QStringLiteral("apt:%1:%2:%3:%4")
            .arg(k.src).arg(k.customName,
                k.src == SrcCustom ? k.name : QString(), k.version);
    }

    static QString installedPublishKey(const QString &pkg, const QString &version)
    {
        return QStringLiteral("installed:") + pkg + QLatin1Char('@') + version;
    }

    QString exactKeyForInstalled(const QString &pkg, const QString &version) const
    {
        for (const KPkg &k : m_apt)
            if (k.name == pkg && k.version == version) return aptPublishKey(k);
        return installedPublishKey(pkg, version);
    }

    static QString upstreamVersion(const QString &release)
    {
        static const QRegularExpression re(QStringLiteral("^v?(\\d+\\.\\d+(?:\\.\\d+)?)(?=[^0-9.]|$)"));
        return re.match(release).captured(1);
    }

    Published cachedPublished(const QString &key) const
    {
        auto it = m_published.constFind(key);
        return it == m_published.cend() ? Published{} : it.value();
    }

    Published publishedFor(const QString &key, const QString &release) const
    {
        const Published exact = cachedPublished(key);
        if (!exact.date.isEmpty()) return exact;
        QString upstream = upstreamVersion(release);
        QString date = m_upstreamDates.value(upstream);
        // kernel.org names initial releases 6.12, not 6.12.0.
        if (date.isEmpty() && upstream.endsWith(QLatin1String(".0"))) {
            upstream.chop(2);
            date = m_upstreamDates.value(upstream);
        }
        if (!date.isEmpty())
            return {date, QStringLiteral("Upstream Linux %1 release (kernel.org ChangeLog timestamp). "
                                         "This is not the distribution package's publication date.").arg(upstream)};
        return {QStringLiteral("—"), QStringLiteral("Publication date unavailable from publisher metadata; "
                                                    "no installation or first-seen date is substituted.")};
    }

    Published publishedForInstalled(const QString &pkg, const QString &version, const QString &release) const
    {
        // Installed mainline packages are not APT source packages.
        if (classifyRelease(release) == SrcMainline) {
            for (const MLVer &v : m_ml)
                if (v.release() == release && !v.archiveDate.isEmpty())
                    return {v.archiveDate, QStringLiteral("Ubuntu mainline archive directory last-modified "
                                                         "date (approximation of build publication date)")};
            return {QStringLiteral("—"), QStringLiteral("Open the Mainline tab to load "
                                                            "Ubuntu's build publication metadata")};
        }
        const Published local = cachedPublished(installedPublishKey(pkg, version));
        return local.date.isEmpty() ? publishedFor(exactKeyForInstalled(pkg, version), release) : local;
    }

    void storePublished(const QString &key, const Published &p)
    {
        if (!QDate::fromString(p.date, Qt::ISODate).isValid()) return;
        m_published.insert(key, p);
        const QString id = QString::fromLatin1(QCryptographicHash::hash(
            key.toUtf8(), QCryptographicHash::Sha256).toHex());
        m_publishSettings.beginGroup(QStringLiteral("verified"));
        m_publishSettings.setValue(id + QStringLiteral("/date"), p.date);
        m_publishSettings.setValue(id + QStringLiteral("/source"), p.provenance);
        m_publishSettings.endGroup();
    }

    bool loadPublishedCache(const QString &key)
    {
        if (m_published.contains(key)) return true;
        const QString id = QString::fromLatin1(QCryptographicHash::hash(
            key.toUtf8(), QCryptographicHash::Sha256).toHex());
        m_publishSettings.beginGroup(QStringLiteral("verified"));
        const QString date = m_publishSettings.value(id + QStringLiteral("/date")).toString();
        const QString source = m_publishSettings.value(id + QStringLiteral("/source")).toString();
        m_publishSettings.endGroup();
        if (!QDate::fromString(date, Qt::ISODate).isValid() || source.isEmpty()) return false;
        m_published.insert(key, {date, source});
        return true;
    }

    // Debian changelogs use an RFC-2822 date in each signed-off version entry.
    // Prefer an entry for the requested version, never an unrelated newer entry.
    static QString changelogDate(const QString &changelog, const QString &version)
    {
        static const QRegularExpression heading(QStringLiteral(
            "^\\S+\\s+\\(([^)]+)\\)\\s+[^\\n]*$"), QRegularExpression::MultilineOption);
        static const QRegularExpression dated(QStringLiteral(
            "^\\s+--\\s+[^\\n]*?\\s{2}([^\\n]+)$"), QRegularExpression::MultilineOption);
        auto it = heading.globalMatch(changelog);
        while (it.hasNext()) {
            const auto match = it.next();
            QString foundVersion = match.captured(1);
            // Debian epochs may be suppressed in a source changelog.
            const QString wanted = version.section(QLatin1Char(':'), -1);
            foundVersion = foundVersion.section(QLatin1Char(':'), -1);
            if (foundVersion != wanted) continue;
            const int start = match.capturedEnd(0);
            const auto next = heading.match(changelog, start);
            const QString stanza = changelog.mid(start,
                next.hasMatch() ? next.capturedStart(0) - start : 16000);
            const auto d = dated.match(stanza);
            if (!d.hasMatch()) return {};
            const QString rfcDate = d.captured(1).trimmed();
            QDateTime dt = QDateTime::fromString(rfcDate, Qt::RFC2822Date);
            if (!dt.isValid()) {
                // Some Debian changelogs use single-digit days or abbreviated TZs.
                const QString withoutZone = rfcDate.section(QLatin1Char('+'), 0, 0).trimmed();
                dt = QDateTime::fromString(withoutZone, QStringLiteral("ddd, d MMM yyyy HH:mm:ss"));
            }
            if (dt.isValid()) return dt.date().toString(Qt::ISODate);
        }
        return {};
    }

    // Installed package documentation can give an accurate date without network.
    void loadLocalPublished(const QString &pkg, const QString &version)
    {
        if (!safeToken(pkg) || !safeToken(version)) return;
        const QString key = installedPublishKey(pkg, version);
        if (loadPublishedCache(key)) return;
        const QString dir = QStringLiteral("/usr/share/doc/") + pkg + QLatin1Char('/');
        QString content;
        for (const QString &file : {QStringLiteral("changelog.Debian.gz"), QStringLiteral("changelog.gz"),
                                    QStringLiteral("changelog.Debian"), QStringLiteral("changelog")}) {
            if (!QFile::exists(dir + file)) continue;
            if (file.endsWith(QLatin1String(".gz"))) {
                QProcess gz;
                gz.start(QStringLiteral("gzip"), {QStringLiteral("-cd"), dir + file});
                if (gz.waitForFinished(1500) && gz.exitCode() == 0)
                    content = QString::fromUtf8(gz.readAllStandardOutput());
            } else content = readFile(dir + file);
            if (!content.isEmpty()) break;
        }
        const QString date = changelogDate(content, version);
        if (!date.isEmpty())
            storePublished(key, {date, QStringLiteral("Installed package's changelog entry for version %1 "
                                                     "(maintainer release date)").arg(version)});
    }

    void queueChangelog(const QString &key, const QString &pkg, const QString &version)
    {
        if (loadPublishedCache(key) || m_changelogAttempted.contains(key)) return;
        if (!safeToken(pkg) || !safeToken(version)) return;
        m_changelogAttempted.insert(key);
        m_changelogQueue.enqueue({key, pkg + QLatin1Char('=') + version});
    }

    void runChangelogQueue()
    {
        // Keep requests bounded and asynchronous; unavailable changelog servers
        // must not freeze the interface or hold the kernel manager 'busy'.
        while (m_changelogActive < 2 && !m_changelogQueue.isEmpty()) {
            const auto job = m_changelogQueue.dequeue();
            const QString version = job.second.section(QLatin1Char('='), -1);
            auto *proc = new QProcess(this);
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
            env.insert(QStringLiteral("PAGER"), QStringLiteral("cat"));
            env.insert(QStringLiteral("DEBIAN_FRONTEND"), QStringLiteral("noninteractive"));
            proc->setProcessEnvironment(env);
            ++m_changelogActive;
            auto finished = std::make_shared<bool>(false);
            auto complete = [this, job, version, proc, finished](bool ok) {
                if (*finished) return;
                *finished = true;
                if (ok) {
                    const QString date = changelogDate(QString::fromUtf8(proc->readAllStandardOutput()), version);
                    if (!date.isEmpty())
                        storePublished(job.first, {date, QStringLiteral("Package changelog entry for version %1 "
                                                                    "(maintainer release date)").arg(version)});
                }
                proc->deleteLater();
                --m_changelogActive;
                refreshPublishedCells();
                runChangelogQueue();
            };
            connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [complete](int code, QProcess::ExitStatus status) {
                complete(status == QProcess::NormalExit && code == 0);
            });
            connect(proc, &QProcess::errorOccurred, this,
                    [complete](QProcess::ProcessError err) {
                if (err == QProcess::FailedToStart) complete(false);
            });
            QTimer::singleShot(18000, proc, [proc]() {
                if (proc->state() != QProcess::NotRunning) proc->kill();
            });
            proc->start(QStringLiteral("apt-get"), {QStringLiteral("-qq"),
                                                        QStringLiteral("changelog"), job.second});
        }
    }

    void queueAllChangelogs()
    {
        for (auto it = m_images.cbegin(); it != m_images.cend(); ++it) {
            const QString release = releaseOf(it.key());
            if (classifyRelease(release) == SrcMainline) continue;
            loadLocalPublished(it.key(), it.value());
            const QString key = installedPublishKey(it.key(), it.value());
            if (!m_published.contains(key)) queueChangelog(key, it.key(), it.value());
        }
        for (const KPkg &k : m_apt) queueChangelog(aptPublishKey(k), k.name, k.version);
        // The persistent cache may have supplied dates without starting jobs.
        refreshPublishedCells();
        runChangelogQueue();
    }

    void loadUpstreamDates()
    {
        // Official per-release ChangeLog directory timestamps; can provide
        // meaningful upstream dates even if the distribution has no changelog.
        QSet<int> majors;
        for (auto it = m_images.cbegin(); it != m_images.cend(); ++it) {
            const int major = upstreamVersion(releaseOf(it.key())).section(QLatin1Char('.'), 0, 0).toInt();
            if (major >= 3 && major <= 20) majors.insert(major);
        }
        for (const KPkg &k : m_apt) {
            const int major = upstreamVersion(k.release).section(QLatin1Char('.'), 0, 0).toInt();
            if (major >= 3 && major <= 20) majors.insert(major);
        }
        for (int major : majors) {
            if (m_upstreamAttempted.contains(major)) continue;
            m_upstreamAttempted.insert(major);
            fetchText(QStringLiteral("https://www.kernel.org/pub/linux/kernel/v%1.x/").arg(major),
                      [this](bool ok, const QString &html) {
                if (!ok) return;
                static const QRegularExpression re(QStringLiteral(
                    "ChangeLog-(\\d+\\.\\d+(?:\\.\\d+)?)</a>\\s+(\\d{2}-[A-Za-z]{3}-\\d{4})"));
                auto it = re.globalMatch(html);
                while (it.hasNext()) {
                    const auto m = it.next();
                    const QDate date = QDate::fromString(m.captured(2), QStringLiteral("dd-MMM-yyyy"));
                    if (date.isValid()) m_upstreamDates.insert(m.captured(1), date.toString(Qt::ISODate));
                }
                refreshPublishedCells();
            });
        }
    }

    void refreshPublishedCells()
    {
        if (!m_pInst.tree || !m_pRepo.tree) return;
        auto update = [](QTreeWidgetItem *item, int col, const Published &p) {
            item->setText(col, p.date);
            item->setToolTip(col, p.provenance);
        };
        for (int i = 0; i < m_pInst.tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *item = m_pInst.tree->topLevelItem(i);
            const QString pkg = item->data(0, Qt::UserRole).toString();
            update(item, 2, publishedForInstalled(pkg, m_images.value(pkg), releaseOf(pkg)));
        }
        for (Page *p : {&m_pRepo, &m_pLiq, &m_pXan, &m_pCustom}) {
            const int column = p == &m_pCustom ? 4 : 3;
            for (int i = 0; i < p->tree->topLevelItemCount(); ++i) {
                QTreeWidgetItem *item = p->tree->topLevelItem(i);
                const QString pkg = item->data(0, Qt::UserRole).toString();
                const QString version = item->text(1);
                for (const KPkg &k : m_apt) {
                    if (k.name == pkg && k.version == version &&
                        ((p == &m_pCustom && k.src == SrcCustom) ||
                         (p == &m_pLiq && k.src == SrcLiquorix) ||
                         (p == &m_pXan && k.src == SrcXanmod) ||
                         (p == &m_pRepo && k.src == SrcRepo))) {
                        update(item, column, publishedFor(aptPublishKey(k), k.release));
                        break;
                    }
                }
            }
        }
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
        const int dateCol = cols.indexOf(QStringLiteral("Date published"));
        if (dateCol >= 0) t->headerItem()->setToolTip(dateCol, QStringLiteral(
            "APT packages: dated publisher changelog entry where available. "
            "Mainline: Ubuntu archive directory date (publication approximation). "
            "If unavailable, a kernel.org upstream release date may be shown. "
            "Unknown means no reliable date was found. Hover each date for its source."));
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
        const QStringList names = {"Installed", "Repository", "Mainline", "Liquorix", "XanMod", "Custom"};
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
        m_pInst = makePage({"Kernel", "Source", "Date published", "Status"},
            {grubBtn(btn("Boot by default", [this] { setDefault(); })),
             grubBtn(btn("Newest by default", [this] { resetDefault(); })),
             (m_removeBtn = btn("Uninstall selected", [this] { removeSelected(); })),
             grubBtn(btn("Update GRUB menu", [this] { updateGrub(); })),
             btn("Refresh", [this] { reloadLocal(); })});
        connect(m_pInst.tree, &QTreeWidget::itemSelectionChanged, this, [this] { updateRemoveButton(); });
        updateRemoveButton();
        m_pRepo = makePage({"Kernel", "Version", "Suite", "Date published", "Status"},
            {btn("Install", [this] { installApt(m_pRepo); }),
             btn("Update package lists", [this] { aptUpdate(); }),
             btn("Refresh", [this] { reloadLocal(); })});

        m_showRc = new QCheckBox(QStringLiteral("Show release candidates"));
        connect(m_showRc, &QCheckBox::toggled, this, [this](bool) { fillMainline(); });
        m_pMl = makePage({"Version", "Kernel", "Date published", "Status"},
            {btn("Install", [this] { installMainline(); }),
             btn("Retry headers", [this] { installMainline(true); }),
             btn("Refresh", [this] { loadMainline(); })}, m_showRc);

        m_pLiq = makePage({"Kernel", "Version", "Suite", "Date published", "Status"},
            {btn("Install", [this] { installApt(m_pLiq); }),
             btn("Refresh", [this] { reloadLocal(); })});
        m_pLiq.enableBtn = btn("Enable Liquorix repository", [this] { enableRepo(SrcLiquorix); });
        m_pLiq.w->layout()->addWidget(m_pLiq.enableBtn);

        m_pXan = makePage({"Kernel", "Version", "Suite", "Date published", "Status"},
            {btn("Install", [this] { installApt(m_pXan); }),
             btn("Refresh", [this] { reloadLocal(); })});
        m_pXan.enableBtn = btn("Enable XanMod repository", [this] { enableRepo(SrcXanmod); });
        m_pXan.w->layout()->addWidget(m_pXan.enableBtn);

        auto *customTop = new QWidget;
        auto *customLayout = new QVBoxLayout(customTop);
        customLayout->setContentsMargins(0, 0, 0, 0);
        customLayout->addWidget(new QLabel(QStringLiteral("Configured custom kernel repositories:")));
        m_customRepos = new QListWidget;
        m_customRepos->setMaximumHeight(110);
        m_customRepos->setSelectionMode(QAbstractItemView::SingleSelection);
        customLayout->addWidget(m_customRepos);
        auto *customButtons = new QHBoxLayout;
        customButtons->addWidget(btn("Add custom source", [this] { addCustomSource(); }));
        m_customRemoveBtn = btn("Remove source", [this] { removeCustomSource(); });
        customButtons->addWidget(m_customRemoveBtn);
        customButtons->addStretch();
        customLayout->addLayout(customButtons);
        connect(m_customRepos, &QListWidget::itemSelectionChanged, this,
                [this] { updateCustomRemoveButton(); });
        m_pCustom = makePage({"Kernel", "Version", "Suite", "Source", "Date published", "Status"},
            {btn("Install", [this] { installApt(m_pCustom); }),
             btn("Update package lists", [this] { aptUpdate(); }),
             btn("Refresh", [this] { reloadLocal(); })}, customTop);
        for (Page *p : {&m_pInst, &m_pRepo, &m_pMl, &m_pLiq, &m_pXan, &m_pCustom})
            m_stack->addWidget(p->w);
        fillCustomRepos();
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
        updateRemoveButton();
        updateCustomRemoveButton();
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
        note(m_pCustom, m_customSources.isEmpty()
            ? QStringLiteral("No custom sources yet. Add an HTTPS APT repository with its signing key and verified fingerprint.")
            : QStringLiteral("Custom APT repositories are signed and verified. Removing a source does not uninstall its kernels."));
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
            m_present.clear();
            if (code != 0 && out.isEmpty()) {
                setBusy(false);
                fail(QStringLiteral("dpkg-query failed: ") + err.trimmed());
                return;
            }
            for (const QString &line : out.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                const QStringList f = line.split(QLatin1Char('\t'));
                if (f.size() < 3 || f.at(2).isEmpty()) continue;
                // Status abbreviation: ii = installed, iU/iF/iH = broken/partial.
                // Ignore removed packages with only config files remaining (rc).
                if (f.at(2).at(0) == QLatin1Char('i')) m_present.insert(f.at(0));
                if (!f.at(2).startsWith(QLatin1String("ii"))) continue;
                m_installed.insert(f.at(0));
                if (!releaseOf(f.at(0)).isEmpty()) m_images.insert(f.at(0), f.at(1));
            }
            loadCustomSources();
            fillCustomRepos();
            refreshHeader();
            // Read dates shipped with installed kernels even if APT metadata is unavailable.
            for (auto it = m_images.cbegin(); it != m_images.cend(); ++it)
                if (classifyRelease(releaseOf(it.key())) != SrcMainline)
                    loadLocalPublished(it.key(), it.value());
            fillInstalled();
            loadUpstreamDates();
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
                    ) continue;
                if (n.startsWith(QLatin1String("linux-headers-"))) m_aptHeaders.insert(n);
                else images << n;
            }
            if (images.isEmpty()) {
                m_apt.clear();
                fillApt();
                setBusy(false);
                queueAllChangelogs();
                loadUpstreamDates();
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
                    // Keep custom packages out of the ordinary repository tab.
                    for (const CustomSource &source : m_customSources) {
                        if (url == source.url || url.startsWith(source.url + QLatin1Char('/'))) {
                            if (k.suite == source.suite || source.suite == QLatin1String("./")) {
                                k.src = SrcCustom;
                                k.customName = source.name;
                                break;
                            }
                        }
                    }
                    if (k.src == SrcXanmod) {
                        static const QRegularExpression lv(QStringLiteral("x64v(\\d)"));
                        const auto m = lv.match(k.name);
                        if (m.hasMatch() && m.captured(1).toInt() > m_cpuLevel) continue;
                    }
                    const QString indexKey = QString::number(k.src) + QLatin1Char(':')
                        + k.customName + QLatin1Char(':') + k.name;
                    auto it = best.find(indexKey);
                    if (it == best.end() || verCmp(k.version, it->version) > 0) best.insert(indexKey, k);
                }
                m_apt = best.values().toVector();
                fillInstalled();
                std::sort(m_apt.begin(), m_apt.end(), [](const KPkg &a, const KPkg &b) {
                    const int c = verCmp(a.release, b.release);
                    return c != 0 ? c > 0 : a.name < b.name;
                });
                fillApt();
                if (m_mlLoaded) fillMainline();
                queueAllChangelogs();
                loadUpstreamDates();
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
            static const QRegularExpression archiveDateRe(QStringLiteral("</a>\\s+(\\d{4}-\\d{2}-\\d{2})"));
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
                // Apache directory listings contain a per-directory last-modified date.
                const auto date = archiveDateRe.match(html.mid(m.capturedEnd(0), 180));
                if (date.hasMatch() && QDate::fromString(date.captured(1), Qt::ISODate).isValid())
                    v.archiveDate = date.captured(1);
                m_ml << v;
            }
            std::sort(m_ml.begin(), m_ml.end(), [](const MLVer &a, const MLVer &b) { return b < a; });
            if (m_ml.isEmpty()) { fail(QStringLiteral("The mainline index was fetched but no versions could be parsed from it.")); return; }
            m_mlLoaded = true;
            fillMainline();
            refreshPublishedCells();
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
            QString sourceLabel = srcName(classifyRelease(r));
            for (const KPkg &candidate : m_apt)
                if (candidate.release == r && candidate.src == SrcCustom) {
                    sourceLabel = candidate.customName;
                    break;
                }
            const QString pkg = pkgOf.value(r);
            const Published pub = publishedForInstalled(pkg, m_images.value(pkg), r);
            auto *item = new QTreeWidgetItem({r, sourceLabel, pub.date, st.join(QStringLiteral(" · "))});
            item->setData(0, Qt::UserRole, pkgOf.value(r));
            item->setData(0, Qt::UserRole + 1, r);
            item->setToolTip(2, pub.provenance);
            m_pInst.tree->addTopLevelItem(item);
        }
        updateRemoveButton();
    }

    void fillApt()
    {
        for (Page *p : {&m_pRepo, &m_pLiq, &m_pXan, &m_pCustom}) p->tree->clear();
        for (const KPkg &k : m_apt) {
            Page &p = k.src == SrcLiquorix ? m_pLiq : k.src == SrcXanmod ? m_pXan
                      : k.src == SrcCustom ? m_pCustom : m_pRepo;
            const Published pub = publishedFor(aptPublishKey(k), k.release);
            auto *item = new QTreeWidgetItem(k.src == SrcCustom
                ? QStringList{k.release, k.version, k.suite, k.customName, pub.date, statusFor(k.release)}
                : QStringList{k.release, k.version, k.suite, pub.date, statusFor(k.release)});
            item->setData(0, Qt::UserRole, k.name);
            item->setData(0, Qt::UserRole + 1, k.release);
            item->setData(0, Qt::UserRole + 2, k.suite);
            item->setToolTip(k.src == SrcCustom ? 4 : 3, pub.provenance);
            p.tree->addTopLevelItem(item);
        }
    }

    void fillMainline()
    {
        m_pMl.tree->clear();
        int shown = 0;
        for (const MLVer &v : m_ml) {
            if (v.rc && !m_showRc->isChecked()) continue;
            QString status = statusFor(v.release());
            if (status.contains(QLatin1String("Installed"))
                && !m_installed.contains(QStringLiteral("linux-headers-") + v.release()))
                status += QStringLiteral(" · Headers missing");
            const QString date = v.archiveDate.isEmpty() ? QStringLiteral("—") : v.archiveDate;
            auto *item = new QTreeWidgetItem({v.tag, v.release(), date, status});
            item->setData(0, Qt::UserRole, v.tag);
            item->setData(0, Qt::UserRole + 1, v.release());
            item->setToolTip(2, v.archiveDate.isEmpty()
                ? QStringLiteral("Ubuntu mainline archive build date unavailable")
                : QStringLiteral("Ubuntu mainline archive directory last-modified date "
                                 "(approximation of build publication date)"));
            m_pMl.tree->addTopLevelItem(item);
            if (++shown >= 120) break;
        }
    }

    // ================================================================ root jobs
    void runRoot(const QString &title, const QString &script, std::function<void(bool)> done = nullptr)
    {
        QString err;
        const QString osmSudo = osmSudoPath(&err);
        if (osmSudo.isEmpty()) { fail(err); return; }

        showLog(true);
        m_log->appendPlainText(QStringLiteral("\n=== ") + title + QStringLiteral(" ==="));
        setBusy(true, title + QStringLiteral("…"));

        auto *p = new QProcess(this);
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
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
            // Include any last output from osm-sudo/sudo before reporting the result.
            const QString remaining = QString::fromUtf8(p->readAllStandardOutput());
            if (!remaining.isEmpty()) { output->append(remaining); log(remaining); }
            setBusy(false);
            const bool ok = code == 0;
            if (!ok) {
                const bool sudoDenied =
                    output->contains(QLatin1String("a password is required"), Qt::CaseInsensitive) ||
                    output->contains(QLatin1String("not in the sudoers"), Qt::CaseInsensitive) ||
                    output->contains(QLatin1String("not allowed to execute"), Qt::CaseInsensitive);
                if (sudoDenied)
                    banner(QStringLiteral("error"), title + QStringLiteral(
                        " was refused by sudo after pattern authentication. "
                        "osm-sudo cannot turn a pattern into a Unix password: "
                        "AlternixOS must authorise this privileged operation. See the log below."));
                else
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
                log(QStringLiteral("ERROR: osm-sudo could not be started (is it installed and executable?)"));
                finish(-1);
            }
        });
        const QString full = QStringLiteral("set -eu\nexport DEBIAN_FRONTEND=noninteractive\n") + script;
        p->start(osmSudo, {QStringLiteral("/bin/sh"), QStringLiteral("-c"), full});
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

        // Pin the selected binary version, particularly for custom sources where
        // another configured repository may offer the same package name.
        const QString chosenVersion = it->text(1).trimmed();
        if (!safeToken(chosenVersion)) {
            fail(QStringLiteral("Refusing unexpected package version: ") + chosenVersion);
            return;
        }
        QStringList pkgs = {pkg + QLatin1Char('=') + chosenVersion};
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

    void installMainline(bool headersOnly = false)
    {
        QTreeWidgetItem *it = selected(m_pMl);
        if (!it) return;
        const QString tag = it->data(0, Qt::UserRole).toString();
        const QString release = it->data(0, Qt::UserRole + 1).toString();
        if (headersOnly && !statusFor(release).contains(QLatin1String("Installed"))) {
            fail(QStringLiteral("Install the kernel image before retrying its headers."));
            return;
        }
        setBusy(true, QStringLiteral("Looking up build files for ") + tag + QStringLiteral("…"));
        mlTryDirs(tag, release, {kMainlineBase + tag + QLatin1Char('/') + m_arch + QLatin1Char('/'),
                                 kMainlineBase + tag + QLatin1Char('/')}, headersOnly);
    }

    void mlTryDirs(const QString &tag, const QString &release, QStringList dirs, bool headersOnly)
    {
        const QString dir = dirs.takeFirst();
        fetchText(dir, [this, tag, release, dir, dirs, headersOnly](bool ok, const QString &html) {
            const QStringList debs = ok ? parseDebs(html) : QStringList();
            const QString a = QRegularExpression::escape(m_arch);
            const QString r = QRegularExpression::escape(release);
            const QString common = QRegularExpression::escape(release.chopped(8)); // strip -generic
            const QString img = pickLast(debs, QStringLiteral("^linux-image-(?:unsigned-)?%1_[^_]+_%2\\.deb$").arg(r, a));
            if (img.isEmpty()) {
                if (!dirs.isEmpty()) { mlTryDirs(tag, release, dirs, headersOnly); return; }
                setBusy(false);
                fail(QStringLiteral("No %1 generic kernel build was found for %2. Ubuntu's build for this version may have failed for your architecture.").arg(m_arch, tag));
                return;
            }
            QStringList requiredFiles;
            if (!headersOnly) {
                requiredFiles << img;
                const QString mods = pickLast(debs, QStringLiteral("^linux-modules-%1_[^_]+_%2\\.deb$").arg(r, a));
                if (!mods.isEmpty()) requiredFiles << mods;
            }
            QStringList headerFiles;
            const bool wantHeaders = headersOnly || m_headers->isChecked();
            if (wantHeaders) {
                const QString hAll = pickLast(debs, QStringLiteral("^linux-headers-%1_[^_]+_all\\.deb$").arg(common));
                const QString hGen = pickLast(debs, QStringLiteral("^linux-headers-%1_[^_]+_%2\\.deb$").arg(r, a));
                // A matching pair is mandatory. An incomplete build is not usable by DKMS.
                if (!hAll.isEmpty() && !hGen.isEmpty()) headerFiles << hAll << hGen;
                else {
                    if (!dirs.isEmpty()) { mlTryDirs(tag, release, dirs, headersOnly); return; }
                    setBusy(false);
                    fail(QStringLiteral("The matching common and generic header packages for %1 (%2) are not both published. "
                                        "Kernel installation was stopped to avoid an image without the requested DKMS headers.")
                                        .arg(tag, m_arch));
                    return;
                }
            }
            const QStringList files = requiredFiles + headerFiles;
            for (const QString &f : files)
                if (!safeToken(f)) { setBusy(false); fail(QStringLiteral("Refusing unexpected file name: ") + f); return; }

            fetchText(dir + QStringLiteral("CHECKSUMS"),
                      [this, tag, dir, files, requiredFiles, headerFiles, headersOnly, wantHeaders]
                      (bool okSum, const QString &sums) {
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

                QString text = headersOnly
                    ? QStringLiteral("Retry installing headers for %1?\n\nFiles:\n  %2").arg(tag, files.join(QStringLiteral("\n  ")))
                    : QStringLiteral("Install mainline %1?\n\nFiles:\n  %2").arg(tag, files.join(QStringLiteral("\n  ")));
                if (!headersOnly)
                    text += QStringLiteral("\n\nThis kernel is unsigned, uses Ubuntu's configuration and will not receive updates through apt.");
                if (!verified)
                    text += QStringLiteral("\n\nWARNING: SHA-256 checksums are not published for every file, so the download cannot be fully verified.");
                if (!headersOnly) text += sbWarning();
                if (tag.startsWith(QLatin1String("v7.")) && wantHeaders)
                    text += QStringLiteral("\n\nKernel 7.x headers may require a newer compiler for DKMS on Devuan Excalibur. "
                                           "The manager will not silently add foreign repositories or upgrade system libraries. "
                                           "If DKMS fails, the log will show the missing compiler/module.");
                if (!confirm(headersOnly ? QStringLiteral("Retry mainline headers") : QStringLiteral("Install mainline kernel"), text)) return;
                mlRun(tag, dir, requiredFiles, headerFiles, sha, wantHeaders);
            });
        });
    }

    void mlRun(const QString &tag, const QString &dir, const QStringList &requiredFiles,
               const QStringList &headerFiles, const QMap<QString, QString> &sha, bool wantHeaders)
    {
        const QStringList files = requiredFiles + headerFiles;
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
        // Install the image and modules first. Header postinst/DKMS hooks need
        // /lib/modules/<release> to exist before attempting an external-module build.
        if (!requiredFiles.isEmpty()) {
            QStringList q;
            for (const QString &f : requiredFiles) q << sq(QStringLiteral("./") + f);
            s += QStringLiteral("echo '==> Installing kernel image and modules'\n");
            s += QStringLiteral("if ! apt-get %1 --no-remove install -y %2; then\n")
                    .arg(kAptOpts, q.join(QLatin1Char(' ')));
            s += QStringLiteral("  echo 'ERROR: kernel image/modules installation failed; check the APT and dpkg output above'\n");
            s += QStringLiteral("  dpkg --audit || true\n  exit 1\nfi\n");
            for (const QString &f : requiredFiles) {
                const QString pkg = f.section(QLatin1Char('_'), 0, 0);
                s += QStringLiteral("dpkg-query -W -f='${db:Status-Abbrev}' %1 2>/dev/null | grep -q '^ii' || { echo 'ERROR: required kernel package %2 is not configured'; exit 1; }\n")
                        .arg(sq(pkg), pkg);
            }
        }
        if (headerFiles.size() == 2) {
            QStringList q;
            for (const QString &f : headerFiles) q << sq(QStringLiteral("./") + f);
            s += QStringLiteral("echo '==> Installing matching common and generic headers together'\n");
            s += QStringLiteral("if ! apt-get %1 --no-remove install -y %2; then\n")
                    .arg(kAptOpts, q.join(QLatin1Char(' ')));
            s += QStringLiteral("  echo 'ERROR: mainline headers failed to install or configure; this job is NOT complete'\n");
            s += QStringLiteral("  echo '==> Header package dependencies:'\n");
            for (const QString &f : headerFiles)
                s += QStringLiteral("  dpkg-deb -f %1 Depends || true\n").arg(sq(f));
            s += QStringLiteral("  echo '==> Compiler/DKMS diagnostics:'\n");
            s += QStringLiteral("  command -v gcc-15 || echo 'NOTE: gcc-15 is unavailable; newer 7.x DKMS builds may require it'\n");
            s += QStringLiteral("  dpkg --audit || true\n");
            s += QStringLiteral("  echo 'ERROR: kernel image may already be installed, but its headers are NOT ready; check the above diagnostics before rebooting'\n");
            s += QStringLiteral("  exit 1\nfi\n");
            for (const QString &f : headerFiles) {
                const QString pkg = f.section(QLatin1Char('_'), 0, 0);
                s += QStringLiteral("dpkg-query -W -f='${db:Status-Abbrev}' %1 2>/dev/null | grep -q '^ii' || { echo 'ERROR: header package %2 is not fully configured (DKMS/postinst may have failed)'; dpkg --audit || true; exit 1; }\n")
                        .arg(sq(pkg), pkg);
            }
            s += QStringLiteral("echo '==> Verified: both matching header packages are configured'\n");
        } else if (wantHeaders) {
            s += QStringLiteral("echo 'ERROR: a complete matching mainline header pair was not found'\nexit 1\n");
        }
        s += QStringLiteral("echo '==> Done. Kernel changes take effect after reboot.'\n");
        runRoot((requiredFiles.isEmpty() ? QStringLiteral("Install headers ") : QStringLiteral("Install mainline ")) + tag,
                s, [this](bool) { reloadLocal(); });
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

    // Create only a signed HTTPS APT source, with a user-supplied expected
    // fingerprint; do not install unchecked signing keys or use trusted=yes.
    void addCustomSource()
    {
        if (m_fetch.isEmpty()) { fail(QStringLiteral("curl or wget is required to add a source.")); return; }
        QDialog dlg(this);
        dlg.setWindowTitle(QStringLiteral("Add custom kernel repository"));
        dlg.setMinimumWidth(580);
        auto *layout = new QVBoxLayout(&dlg);
        auto *form = new QFormLayout;
        auto *nameEdit = new QLineEdit;
        auto *urlEdit = new QLineEdit;
        auto *suiteEdit = new QLineEdit(m_codename.isEmpty() ? QStringLiteral("stable") : m_codename);
        auto *compEdit = new QLineEdit(QStringLiteral("main"));
        auto *keyEdit = new QLineEdit;
        auto *fingerEdit = new QLineEdit;
        nameEdit->setPlaceholderText(QStringLiteral("e.g. My kernel repository"));
        urlEdit->setPlaceholderText(QStringLiteral("https://example.org/debian"));
        keyEdit->setPlaceholderText(QStringLiteral("https://example.org/repository-key.asc"));
        fingerEdit->setPlaceholderText(QStringLiteral("40- or 64-digit OpenPGP key fingerprint"));
        form->addRow(QStringLiteral("Display name"), nameEdit);
        form->addRow(QStringLiteral("APT repository URL"), urlEdit);
        form->addRow(QStringLiteral("Distribution / suite"), suiteEdit);
        form->addRow(QStringLiteral("Components"), compEdit);
        form->addRow(QStringLiteral("Signing key HTTPS URL"), keyEdit);
        form->addRow(QStringLiteral("Expected key fingerprint"), fingerEdit);
        layout->addLayout(form);
        auto *hint = new QLabel(QStringLiteral(
            "Only add repositories you trust. Verify the OpenPGP fingerprint independently "
            "with the publisher before proceeding. The key is restricted to this source."));
        hint->setWordWrap(true);
        layout->addWidget(hint);
        auto *actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(actions, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(actions, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        layout->addWidget(actions);
        if (dlg.exec() != QDialog::Accepted) return;

        const QString name = nameEdit->text().trimmed();
        const QString url = urlEdit->text().trimmed();
        const QString suite = suiteEdit->text().trimmed();
        const QString keyUrl = keyEdit->text().trimmed();
        const QStringList components = compEdit->text().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QString finger = fingerEdit->text().toUpper();
        finger.remove(QLatin1Char(' '));
        static const QRegularExpression fprRe(QStringLiteral("^(?:[A-F0-9]{40}|[A-F0-9]{64})$"));
        static const QRegularExpression nameRe(QStringLiteral("^[^\r\n#]{1,80}$"));
        if (!nameRe.match(name).hasMatch() || !validHttpsUrl(url) || !validHttpsUrl(keyUrl)
            || !validAptField(suite) || components.isEmpty() || !fprRe.match(finger).hasMatch()) {
            fail(QStringLiteral("Invalid repository details. Use HTTPS URLs, a simple suite, "
                                "components and the publisher's full signing key fingerprint."));
            return;
        }
        for (const QString &component : components)
            if (!validAptField(component)) {
                fail(QStringLiteral("Invalid APT component: ") + component);
                return;
            }
        const QString id = QString::fromLatin1(QCryptographicHash::hash(
            (url + QLatin1Char('|') + suite).toUtf8(), QCryptographicHash::Sha256).toHex().left(12));
        for (const CustomSource &source : m_customSources)
            if (source.id == id) { fail(QStringLiteral("That source is already configured. Remove it before changing its details.")); return; }
        CustomSource source;
        source.id = id; source.name = name; source.url = url;
        source.suite = suite; source.components = components.join(QLatin1Char(' '));
        const QString debLine = QStringLiteral("deb [arch=%1 signed-by=%2] %3 %4 %5")
            .arg(m_arch, source.keyPath(), url, suite, source.components);
        if (!confirm(QStringLiteral("Trust custom kernel source?"),
                     QStringLiteral("Add %1?\n\n%2\n\nRequired fingerprint:\n%3\n\n"
                        "This source will be allowed to provide APT packages. Only proceed if you verified the publisher.")
                        .arg(name, debLine, finger))) return;

        // The source path, key path, and file name are all generated locally.
        // All external inputs are shell-quoted and validated before privilege escalation.
        QString s;
        s += QStringLiteral("if ! command -v gpg >/dev/null 2>&1; then\n  echo '==> Installing gpg'\n  apt-get %1 --no-remove install -y --no-install-recommends gpg\nfi\n").arg(kAptOpts);
        s += QStringLiteral("work=$(mktemp -d /var/tmp/osm-kernels-repo.XXXXXX)\ntrap 'rm -rf \"$work\"' EXIT\n");
        s += QStringLiteral("umask 077\nmkdir -p /etc/apt/keyrings /etc/apt/sources.list.d \"$work/gnupg\"\nchmod 0755 /etc/apt/keyrings /etc/apt/sources.list.d\nchmod 700 \"$work/gnupg\"\n");
        s += QStringLiteral("test ! -e %1 || { echo 'ERROR: repository source already exists'; exit 1; }\n").arg(sq(source.listPath()));
        s += QStringLiteral("test ! -e %1 || { echo 'ERROR: repository key already exists'; exit 1; }\n").arg(sq(source.keyPath()));
        s += QStringLiteral("echo '==> Downloading repository signing key'\n");
        s += dlCmd(keyUrl, QStringLiteral("$work/download.key"))
            .replace(QStringLiteral("'$work/download.key'"), QStringLiteral("\"$work/download.key\"")) + QLatin1Char('\n');
        s += QStringLiteral("actual=$(gpg --homedir \"$work/gnupg\" --batch --show-keys --with-colons --fingerprint \"$work/download.key\" | awk -F: '$1==\"pub\" { count++ } $1==\"fpr\" && !found { finger=toupper($10); found=1 } END { if (count!=1 || !found) exit 1; print finger }')\n");
        s += QStringLiteral("test \"$actual\" = %1 || { echo 'ERROR: signing key fingerprint mismatch; source NOT added'; exit 1; }\n").arg(sq(finger));
        s += QStringLiteral("if grep -aq 'BEGIN PGP PUBLIC KEY BLOCK' \"$work/download.key\"; then\n");
        s += QStringLiteral("  gpg --homedir \"$work/gnupg\" --batch --yes --dearmor -o \"$work/keyring.gpg\" \"$work/download.key\"\n");
        s += QStringLiteral("else cp \"$work/download.key\" \"$work/keyring.gpg\"; fi\n");
        s += QStringLiteral("install -m 0644 \"$work/keyring.gpg\" %1\n").arg(sq(source.keyPath()));
        s += QStringLiteral("printf '%s\\n' %1 %2 > \"$work/source.list\"\n")
                .arg(sq(QStringLiteral("# osm-kernels-name: ") + name), sq(debLine));
        s += QStringLiteral("install -m 0644 \"$work/source.list\" %1\n").arg(sq(source.listPath()));
        s += QStringLiteral("echo '==> Updating APT lists'\n");
        s += QStringLiteral("if ! apt-get %1 update; then\n").arg(kAptOpts);
        s += QStringLiteral("  echo 'ERROR: APT update failed. Rolling back the new source.'\n");
        s += QStringLiteral("  rm -f %1 %2\n  exit 1\nfi\n").arg(sq(source.listPath()), sq(source.keyPath()));
        runRoot(QStringLiteral("Add custom source ") + name, s, [this](bool) { reloadLocal(); });
    }

    void removeCustomSource()
    {
        if (!m_customRepos || !m_customRepos->currentItem()) {
            fail(QStringLiteral("Select a custom repository first.")); return;
        }
        const QString id = m_customRepos->currentItem()->data(Qt::UserRole).toString();
        for (const CustomSource &source : m_customSources) {
            if (source.id != id) continue;
            if (!confirm(QStringLiteral("Remove custom source?"),
                         QStringLiteral("Remove %1 from APT?\n\n%2\n\n"
                                        "Already-installed kernels from this source will remain installed.")
                             .arg(source.name, source.url))) return;
            const QString s = QStringLiteral("rm -f %1 %2\napt-get %3 update\n")
                .arg(sq(source.listPath()), sq(source.keyPath()), kAptOpts);
            runRoot(QStringLiteral("Remove custom source ") + source.name, s,
                    [this](bool) { reloadLocal(); });
            return;
        }
        fail(QStringLiteral("Selected source is no longer available."));
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
        for (const QString &c : cands) if (m_present.contains(c) && safeToken(c)) pkgs << c;
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
            QString text = QStringLiteral("Uninstall kernel %1?\n\nPackages to purge: %2").arg(rel, pkgs.join(QStringLiteral(", ")));
            if (!extra.isEmpty())
                text += QStringLiteral("\n\nWARNING: apt will ALSO remove:\n  %1\n\nIf that includes a metapackage such as linux-image-amd64, you will stop receiving kernel updates from it.")
                            .arg(extra.join(QStringLiteral("\n  ")));
            text += QStringLiteral("\n\nThis deletes the kernel and its installed matching modules/headers. The running kernel is protected.");
            if (!confirm(QStringLiteral("Uninstall kernel"), text)) return;

            QStringList q;
            for (const QString &p : pkgs) q << sq(p);
            QString s = QStringLiteral("echo '==> Removing %1'\napt-get %2 purge -y %3\n").arg(rel, kAptOpts, q.join(QLatin1Char(' ')));
            if (rel == m_grubDefaultRel && m_hasGrub)
                s += QStringLiteral("echo '==> Removed kernel was the boot default; reverting to newest'\nrm -f %1\nupdate-grub\n").arg(sq(kGrubOverride));
            runRoot(QStringLiteral("Uninstall ") + rel, s, [this](bool) { reloadLocal(); });
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
        const QString osmSudo = osmSudoPath(&err);
        if (osmSudo.isEmpty()) { fail(err); return; }
        setBusy(true, QStringLiteral("Reading GRUB menu…"));
        runAsync(osmSudo, {QStringLiteral("cat"), cfgPath},
                 [this, proceed, cfgPath](int code, const QString &out, const QString &e) {
            setBusy(false);
            if (code != 0) { fail(QStringLiteral("Could not read %1: %2").arg(cfgPath, e.trimmed())); return; }
            proceed(out);
        });
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
