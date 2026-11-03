#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QGridLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QStandardPaths>
#include <QScrollArea>
#include <QScroller>
#include <QFrame>
#include <QStackedWidget>
#include <QProcess>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QMap>
#include <QScreen>
#include <QGuiApplication>
#include <QSizePolicy>
#include <QDateTime>
#include <QStringList>
#include <QList>
#include <QRegExp>

#include <unistd.h>  // sysconf

// -----------------------------------------------------
// Alternix compact button style
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
// CONFIG helpers
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
// Helper: run command and capture output
// -----------------------------------------------------
static QString runCmd(const QString &cmd, const QStringList &args = QStringList())
{
    QProcess p;
    p.start(cmd, args);
    p.waitForFinished(500);
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

// -----------------------------------------------------
// Human-readable time
// -----------------------------------------------------
static QString humanDuration(qint64 secs)
{
    if (secs <= 0) return "Just now";

    qint64 days = secs / 86400;
    secs %= 86400;
    qint64 hours = secs / 3600;
    secs %= 3600;
    qint64 mins = secs / 60;

    QStringList parts;
    if (days)  parts << QString("%1d").arg(days);
    if (hours) parts << QString("%1h").arg(hours);
    if (mins)  parts << QString("%1m").arg(mins);
    if (parts.isEmpty()) return "<1m";

    return parts.join(" ");
}

// -----------------------------------------------------
// AccountsPage
// -----------------------------------------------------
class AccountsPage : public QWidget
{
public:
    explicit AccountsPage(QStackedWidget *stack)
        : QWidget(stack), m_stack(stack)
    {
        setStyleSheet("background:#282828; color:white; font-family:Sans;");

        cfg = loadCfg();
        gatherUserInfo();

        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(40, 40, 40, 40);
        root->setSpacing(10);

        QLabel *title = new QLabel("Accounts");
        title->setAlignment(Qt::AlignCenter);
        title->setStyleSheet("font-size:42px; font-weight:bold;");
        root->addWidget(title);

        QScrollArea *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

        QWidget *wrap = new QWidget(scroll);
        QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
        wrapLay->setSpacing(10);
        wrapLay->setContentsMargins(0,0,0,0);

        QFrame *outer = new QFrame(wrap);
        outer->setStyleSheet("QFrame { background:#3a3a3a; border-radius:40px; }");
        QVBoxLayout *outerLay = new QVBoxLayout(outer);
        outerLay->setContentsMargins(50, 30, 50, 30);
        outerLay->setSpacing(30);

        outerLay->addWidget(makeCurrentUserCard());
        outerLay->addWidget(makeSessionCard());
        outerLay->addWidget(makeAccountHistoryCard());
        outerLay->addWidget(makeGroupsCard());
        outerLay->addWidget(makeLoginHistoryCard());

        wrapLay->addWidget(outer);
        wrapLay->addStretch();

        scroll->setWidget(wrap);
        root->addWidget(scroll);

        QPushButton *back = makeBtn("❮");
        back->setFixedSize(140, 60);
        connect(back, &QPushButton::clicked, this, [this]() {
            if (m_stack) m_stack->setCurrentIndex(0);
        });
        root->addWidget(back, 0, Qt::AlignCenter);
    }

private:
    struct LoginEntry {
        QString dateTime;
        QString tty;
        QString from;
    };

    QStackedWidget *m_stack = nullptr;
    QMap<QString,QString> cfg;

    QString m_username;
    QString m_fullName;
    int     m_uid = -1;

    bool        m_sessionValid = false;
    QDateTime   m_sessionLoginTime;
    qint64      m_sessionSeconds = 0;

    bool        m_accountCreatedValid = false;
    QDate       m_accountCreatedDate;
    qint64      m_accountAgeSeconds = 0;

    int         m_loginCount = -1;
    QList<LoginEntry> m_loginHistory;
    QStringList       m_groups;
    QGridLayout      *m_groupsGrid = nullptr;
    QPushButton      *m_addGroupButton = nullptr;
    bool              m_groupChangePending = false;

    // -------------------------------------------------
    // Data gathering
    // -------------------------------------------------
    void gatherUserInfo()
    {
        m_username = qgetenv("USER");
        if (m_username.isEmpty()) m_username = qgetenv("LOGNAME");
        if (m_username.isEmpty()) m_username = QDir::home().dirName();
        if (m_username.isEmpty()) m_username = "unknown";

        readPasswd();
        computeSession();
        computeAccountAge();
        computeLoginCount();
        computeLoginHistory();
        computeGroups();
    }

    void readPasswd()
    {
        QFile f("/etc/passwd");
        if (!f.open(QFile::ReadOnly)) return;

        QTextStream s(&f);
        while (!s.atEnd())
        {
            QString line = s.readLine().trimmed();
            if (line.isEmpty() || line.startsWith("#"))
                continue;

            QStringList p = line.split(":");
            if (p.size() < 7) continue;

            if (p[0] == m_username)
            {
                m_uid = p[2].toInt();
                m_fullName = p[4].split(",").value(0).trimmed();
                break;
            }
        }
    }

    void computeSession()
    {
        if (trySessionWho())
            return;

        trySessionProc();
    }

    bool trySessionWho()
    {
        QString out = runCmd("who", QStringList() << "-u");
        if (out.isEmpty()) return false;

        QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines)
        {
            QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;

            QStringList p = trimmed.split(QRegExp("\\s+"), Qt::SkipEmptyParts);
            if (p.size() < 4) continue;
            if (p[0] != m_username) continue;

            QDateTime dt;

            // Format A: yyyy-MM-dd hh:mm
            if (p[2].contains("-"))
            {
                dt = QDateTime::fromString(p[2] + " " + p[3], "yyyy-MM-dd hh:mm");
            }
            else
            {
                // Format B: "Mon  d hh:mm" (no year -> current year)
                if (p.size() >= 5)
                {
                    QString full = QString("%1 %2 %3 %4")
                        .arg(p[2])
                        .arg(p[3])
                        .arg(QDate::currentDate().year())
                        .arg(p[4]);

                    dt = QDateTime::fromString(full, "MMM d yyyy hh:mm");
                    if (!dt.isValid())
                        dt = QDateTime::fromString(full, "MMM  d yyyy hh:mm");
                }
            }

            if (!dt.isValid()) continue;

            m_sessionValid = true;
            m_sessionLoginTime = dt;
            m_sessionSeconds   = dt.secsTo(QDateTime::currentDateTime());
            if (m_sessionSeconds < 0) m_sessionSeconds = 0;
            return true;
        }

        return false;
    }

    void trySessionProc()
    {
        double uptime = 0.0;

        QFile u("/proc/uptime");
        if (u.open(QFile::ReadOnly))
        {
            QString line = QString::fromUtf8(u.readAll());
            uptime = line.split(" ").value(0).toDouble();
        }
        if (uptime <= 0.0) return;

        QFile st("/proc/self/stat");
        if (!st.open(QFile::ReadOnly)) return;

        QString content = QString::fromUtf8(st.readAll());
        int r = content.lastIndexOf(')');
        if (r == -1) return;

        QStringList p = content.mid(r+2).split(" ", Qt::SkipEmptyParts);
        if (p.size() < 20) return;

        bool ok = false;
        long long startTicks = p[19].toLongLong(&ok);
        if (!ok) return;

        long hz = sysconf(_SC_CLK_TCK);
        double startSec = (double)startTicks / (double)hz;
        double elapsed = uptime - startSec;
        if (elapsed < 0) elapsed = 0;

        m_sessionValid = true;
        m_sessionSeconds = (qint64)elapsed;
        m_sessionLoginTime = QDateTime::currentDateTime().addSecs(-(qint64)elapsed);
    }

    // -------------------------------------------------
    // Account age (fixed: chage -> home dir -> /etc/passwd)
// -------------------------------------------------
    void computeAccountAge()
    {
        bool found = false;

        // 1) Try chage (method 2 you chose)
        QString out = runCmd("chage", QStringList() << "-l" << m_username);
        if (!out.isEmpty())
        {
            QStringList lines = out.split('\n', Qt::SkipEmptyParts);
            for (const QString &line : lines)
            {
                if (!line.contains("Account created"))
                    continue;

                int idx = line.indexOf(':');
                if (idx == -1)
                    continue;

                QString datePart = line.mid(idx + 1).trimmed();  // e.g. "Jul 16, 2024"

                QDate d = QDate::fromString(datePart, "MMM d, yyyy");
                if (!d.isValid())
                    d = QDate::fromString(datePart, "MMM dd, yyyy");

                if (d.isValid())
                {
                    found = true;
                    m_accountCreatedValid = true;
                    m_accountCreatedDate = d;

                    QDateTime dt(d, QTime(0,0), Qt::LocalTime);
                    m_accountAgeSeconds = dt.secsTo(QDateTime::currentDateTime());
                    if (m_accountAgeSeconds < 0) m_accountAgeSeconds = 0;
                    break;
                }
            }
        }

        if (found) return;

        // 2) Fallback: home directory birth time
        QFileInfo homeInfo(QDir::homePath());
        QDateTime createdDT = homeInfo.birthTime();
#if (QT_VERSION < QT_VERSION_CHECK(5, 10, 0))
        // older Qt: birthTime may not exist; use created()
        if (!createdDT.isValid())
            createdDT = homeInfo.created();
#endif
        if (!createdDT.isValid())
            createdDT = homeInfo.metadataChangeTime();  // best-effort

        if (createdDT.isValid())
        {
            m_accountCreatedValid = true;
            m_accountCreatedDate = createdDT.date();
            m_accountAgeSeconds = createdDT.secsTo(QDateTime::currentDateTime());
            if (m_accountAgeSeconds < 0) m_accountAgeSeconds = 0;
            return;
        }

        // 3) Last fallback: /etc/passwd metadata
        QFileInfo pwInfo("/etc/passwd");
        createdDT = pwInfo.birthTime();
#if (QT_VERSION < QT_VERSION_CHECK(5, 10, 0))
        if (!createdDT.isValid())
            createdDT = pwInfo.created();
#endif
        if (!createdDT.isValid())
            createdDT = pwInfo.metadataChangeTime();

        if (createdDT.isValid())
        {
            m_accountCreatedValid = true;
            m_accountCreatedDate = createdDT.date();
            m_accountAgeSeconds = createdDT.secsTo(QDateTime::currentDateTime());
            if (m_accountAgeSeconds < 0) m_accountAgeSeconds = 0;
        }
    }

    void computeLoginCount()
    {
        QString out = runCmd("last", QStringList() << m_username);
        if (out.isEmpty()) { m_loginCount = -1; return; }

        QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        int count = 0;

        for (const QString &line : lines)
        {
            QString trimmed = line.trimmed();
            if (trimmed.startsWith("wtmp begins"))
                continue;

            if (trimmed.startsWith(m_username + " "))
                count++;
        }

        m_loginCount = count;
    }

    void computeLoginHistory()
    {
        m_loginHistory.clear();

        QString out = runCmd("last", QStringList() << m_username);
        if (out.isEmpty()) return;

        QStringList lines = out.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines)
        {
            QString trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;
            if (trimmed.startsWith("wtmp begins"))
                continue;

            QStringList p = trimmed.split(QRegExp("\\s+"), Qt::SkipEmptyParts);
            if (p.size() < 7) continue;

            LoginEntry e;
            e.tty  = p[1];
            e.from = p[2];

            // Combine a few tokens into a human string: e.g. "Thu Dec 4 22:03"
            QStringList dtTokens;
            for (int i = 3; i < p.size(); ++i)
            {
                QString token = p[i];
                if (token.contains("(")) break;  // stop at duration "(01:23)"
                dtTokens << token;
                if (dtTokens.size() >= 5) break; // enough
            }
            e.dateTime = dtTokens.join(" ");

            m_loginHistory.append(e);
            if (m_loginHistory.size() >= 5)
                break;
        }
    }

    void computeGroups()
    {
        m_groups.clear();

        QString out = runCmd("groups", QStringList() << m_username);
        if (out.isEmpty())
            out = runCmd("groups");

        if (out.isEmpty())
            return;

        QString text = out;
        int colonIdx = text.indexOf(':');
        if (colonIdx != -1)
            text = text.mid(colonIdx + 1);

        QStringList parts = text.split(QRegExp("\\s+"), Qt::SkipEmptyParts);
        for (const QString &g : parts)
        {
            QString trimmed = g.trimmed();
            if (!trimmed.isEmpty())
                m_groups << trimmed;
        }
        m_groups.removeDuplicates();
    }

    // -------------------------------------------------
    // UI cards
    // -------------------------------------------------
    QWidget *makeCurrentUserCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30,20,30,20);
        v->setSpacing(10);

        QLabel *header = new QLabel("Current User");
        header->setStyleSheet("font-size:30px; font-weight:bold;");
        v->addWidget(header);

        auto row = [&](const QString &name, const QString &val){
            QHBoxLayout *h = new QHBoxLayout();
            QLabel *l = new QLabel(name);
            QLabel *r = new QLabel(val.isEmpty() ? "Unknown" : val);
            r->setStyleSheet("color:#e0e0e0;");
            h->setContentsMargins(0,0,0,0);
            h->setSpacing(6);
            h->addWidget(l); h->addStretch(); h->addWidget(r);
            v->addLayout(h);
        };

        row("Username", m_username);
        row("Full Name", m_fullName.isEmpty() ? m_username : m_fullName);
        row("UID", (m_uid >= 0) ? QString::number(m_uid) : "Unknown");

        return card;
    }

    QWidget *makeSessionCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30,20,30,20);
        v->setSpacing(10);

        QLabel *header = new QLabel("Current Session");
        header->setStyleSheet("font-size:30px; font-weight:bold;");
        v->addWidget(header);

        auto row = [&](const QString &name, const QString &val){
            QHBoxLayout *h = new QHBoxLayout();
            QLabel *l = new QLabel(name);
            QLabel *r = new QLabel(val);
            r->setStyleSheet("color:#e0e0e0;");
            h->setContentsMargins(0,0,0,0);
            h->setSpacing(6);
            h->addWidget(l); h->addStretch(); h->addWidget(r);
            v->addLayout(h);
        };

        if (m_sessionValid)
        {
            row("Logged in since", m_sessionLoginTime.toString("yyyy-MM-dd hh:mm"));
            row("Session duration", humanDuration(m_sessionSeconds));
        }
        else
        {
            row("Logged in since", "Unknown");
            row("Session duration", "Unknown");
        }

        return card;
    }

    QWidget *makeAccountHistoryCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30,20,30,20);
        v->setSpacing(10);

        QLabel *header = new QLabel("Account History");
        header->setStyleSheet("font-size:30px; font-weight:bold;");
        v->addWidget(header);

        auto row = [&](const QString &name, const QString &val){
            QHBoxLayout *h = new QHBoxLayout();
            QLabel *l = new QLabel(name);
            QLabel *r = new QLabel(val);
            r->setStyleSheet("color:#e0e0e0;");
            h->setContentsMargins(0,0,0,0);
            h->setSpacing(6);
            h->addWidget(l); h->addStretch(); h->addWidget(r);
            v->addLayout(h);
        };

        if (m_accountCreatedValid)
        {
            QString created = m_accountCreatedDate.toString("yyyy-MM-dd");
            QString age     = humanDuration(m_accountAgeSeconds) + " ago";
            row("Account created", created);
            row("Account age", age);
        }
        else
        {
            row("Account created", "Unknown");
            row("Account age", "Unknown");
        }

        row("Login count",
            (m_loginCount >= 0) ? QString::number(m_loginCount) : "Unknown");

        return card;
    }

    // -------------------------------------------------
    // Group management: a pill-shaped + at the end of the list
    // -------------------------------------------------
    void refreshGroupPills()
    {
        if (!m_groupsGrid) return;

        // Rebuild only the group grid, without disturbing the rest of Accounts.
        QLayoutItem *item = nullptr;
        while ((item = m_groupsGrid->takeAt(0)) != nullptr)
        {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        m_addGroupButton = nullptr;

        const int colCount = 3;
        int row = 0, col = 0;
        auto advance = [&]() {
            if (++col >= colCount) { col = 0; ++row; }
        };

        if (m_groups.isEmpty())
        {
            QLabel *none = new QLabel("No groups detected");
            none->setStyleSheet("color:#e0e0e0;");
            m_groupsGrid->addWidget(none, row, col);
            advance();
        }

        for (const QString &g : m_groups)
        {
            QLabel *pill = new QLabel(g);
            pill->setStyleSheet(
                "QLabel {"
                " background:#555555;"
                " border-radius:18px;"
                " padding:6px 18px;"
                " font-size:22px;"
                "}"
            );
            pill->setAlignment(Qt::AlignCenter);
            pill->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            m_groupsGrid->addWidget(pill, row, col);
            advance();
        }

        QPushButton *plus = new QPushButton("+");
        plus->setToolTip("Add this user to a group");
        plus->setAccessibleName("Add user to group");
        plus->setStyleSheet(
            "QPushButton {"
            " background:#555555;"
            " color:white;"
            " border:0px;"
            " border-radius:18px;"
            " padding:6px 18px;"
            " font-size:22px;"
            "}"
            "QPushButton:hover { background:#666666; }"
            "QPushButton:pressed { background:#444444; }"
            "QPushButton:disabled { color:#aaaaaa; }"
        );
        plus->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        plus->setMinimumHeight(43);
        plus->setEnabled(!m_groupChangePending);
        m_groupsGrid->addWidget(plus, row, col);
        m_addGroupButton = plus;
        connect(plus, &QPushButton::clicked, this, [this]() { promptForGroup(); });
    }

    void promptForGroup()
    {
        if (m_groupChangePending) return;

        QInputDialog dialog(this);
        dialog.setWindowTitle("Add User to Group");
        dialog.setLabelText("Enter the group name:");
        dialog.setInputMode(QInputDialog::TextInput);
        dialog.setTextEchoMode(QLineEdit::Normal);
        dialog.setOkButtonText("Add");
        dialog.setCancelButtonText("Cancel");
        dialog.setMinimumWidth(420);
        dialog.setStyleSheet(
            "QInputDialog { background:#282828; color:white; }"
            "QLabel { background:transparent; color:white; font-size:20px; }"
            "QLineEdit { background:#3a3a3a; color:white; border:1px solid #777777;"
            " border-radius:10px; padding:9px; font-size:20px; }"
            "QPushButton { background:#555555; color:white; border:0px;"
            " border-radius:12px; padding:8px 18px; font-size:18px; }"
            "QPushButton:hover { background:#666666; }"
        );
        if (dialog.exec() != QDialog::Accepted) return;

        const QString group = dialog.textValue().trimmed();
        if (!QRegExp("^[a-z_][a-z0-9_-]{0,31}$").exactMatch(group))
        {
            QMessageBox::warning(this, "Invalid Group Name",
                "Use 1–32 characters: lower-case letters, numbers, underscores "
                "or hyphens. The first character must be a letter or underscore.");
            return;
        }
        if (m_groups.contains(group))
        {
            QMessageBox::information(this, "User Groups",
                QString("%1 is already a member of '%2'.").arg(m_username, group));
            return;
        }

        // Keep the command fixed and pass the two values as positional arguments.
        // This avoids interpolating untrusted group names into shell syntax.
        const QString script =
            "group=\"$1\"; user=\"$2\"; "
            "if ! /usr/bin/getent group \"$group\" >/dev/null 2>&1; then "
            " /usr/sbin/groupadd -- \"$group\" || exit $?; "
            "fi; "
            "exec /usr/sbin/usermod -a -G \"$group\" -- \"$user\"";
        const QStringList args = QStringList() << "-c" << script << "alternix-group-add"
                                               << group << m_username;

        QString program = "/bin/sh";
        QStringList processArgs = args;
        if (geteuid() != 0)
        {
            program = QStandardPaths::findExecutable("osm-sudo");
            if (program.isEmpty())
            {
                QMessageBox::critical(this, "Authentication Unavailable",
                    "osm-sudo could not be found. Group membership requires "
                    "administrator authentication.");
                return;
            }
            processArgs.prepend("/bin/sh");
        }

        m_groupChangePending = true;
        if (m_addGroupButton) m_addGroupButton->setEnabled(false);

        QProcess *process = new QProcess(this);
        process->setProgram(program);
        process->setArguments(processArgs);

        connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || !m_groupChangePending) return;
                m_groupChangePending = false;
                if (m_addGroupButton) m_addGroupButton->setEnabled(true);
                QMessageBox::critical(this, "Group Management Failed",
                    "Could not start the group-management command: " + process->errorString());
                process->deleteLater();
            });

        connect(process,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, process, group](int exitCode, QProcess::ExitStatus status) {
                if (!m_groupChangePending) return;
                m_groupChangePending = false;
                if (m_addGroupButton) m_addGroupButton->setEnabled(true);

                if (status != QProcess::NormalExit || exitCode != 0)
                {
                    QString details = QString::fromUtf8(process->readAllStandardError()).trimmed();
                    if (details.isEmpty())
                        details = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
                    if (details.isEmpty())
                        details = QString("The command exited with code %1.").arg(exitCode);
                    QMessageBox::critical(this, "Group Management Failed",
                        "Unable to add the user to the group.\n\n" + details.left(900));
                }
                else
                {
                    computeGroups();
                    refreshGroupPills();
                    if (m_groups.contains(group))
                    {
                        QMessageBox::information(this, "Group Added",
                            QString("%1 is now a member of '%2'.\n\n"
                                    "Log out and back in for the new group "
                                    "permissions to take effect in your session.")
                                .arg(m_username, group));
                    }
                    else
                    {
                        QMessageBox::warning(this, "Refresh Needed",
                            "The command completed, but the new group membership "
                            "was not yet reported by the system. Try reopening Accounts.");
                    }
                }
                process->deleteLater();
            });
        process->start();
    }

    QWidget *makeGroupsCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30,20,30,20);
        v->setSpacing(10);

        QLabel *header = new QLabel("User Groups");
        header->setStyleSheet("font-size:30px; font-weight:bold;");
        v->addWidget(header);

        m_groupsGrid = new QGridLayout();
        m_groupsGrid->setContentsMargins(0,0,0,0);
        m_groupsGrid->setHorizontalSpacing(10);
        m_groupsGrid->setVerticalSpacing(10);
        v->addLayout(m_groupsGrid);
        refreshGroupPills();
        return card;
    }

    QWidget *makeLoginHistoryCard()
    {
        QFrame *card = new QFrame;
        card->setStyleSheet("QFrame { background:#444444; border-radius:30px; }");

        QVBoxLayout *v = new QVBoxLayout(card);
        v->setContentsMargins(30,20,30,20);
        v->setSpacing(10);

        QLabel *header = new QLabel("Login History");
        header->setStyleSheet("font-size:30px; font-weight:bold;");
        v->addWidget(header);

        if (m_loginHistory.isEmpty())
        {
            QLabel *none = new QLabel("No login records found");
            none->setStyleSheet("color:#e0e0e0;");
            v->addWidget(none);
            return card;
        }

        // Header row (table style)
        auto makeHeaderLabel = [](const QString &text){
            QLabel *lbl = new QLabel(text);
            lbl->setStyleSheet("font-size:22px; font-weight:bold;");
            return lbl;
        };

        QHBoxLayout *headerRow = new QHBoxLayout();
        headerRow->setContentsMargins(0,0,0,0);
        headerRow->setSpacing(6);
        headerRow->addWidget(makeHeaderLabel("Date & Time"));
        headerRow->addStretch();
        headerRow->addWidget(makeHeaderLabel("TTY"));
        headerRow->addSpacing(20);
        headerRow->addWidget(makeHeaderLabel("From"));
        v->addLayout(headerRow);

        // Entries
        for (const LoginEntry &e : m_loginHistory)
        {
            QHBoxLayout *row = new QHBoxLayout();
            row->setContentsMargins(0,0,0,0);
            row->setSpacing(6);

            QLabel *dt   = new QLabel(e.dateTime);
            QLabel *tty  = new QLabel(e.tty);
            QLabel *from = new QLabel(e.from);

            dt->setStyleSheet("color:#e0e0e0;");
            tty->setStyleSheet("color:#e0e0e0;");
            from->setStyleSheet("color:#e0e0e0;");

            row->addWidget(dt);
            row->addStretch();
            row->addWidget(tty);
            row->addSpacing(20);
            row->addWidget(from);

            v->addLayout(row);
        }

        return card;
    }
};

// -----------------------------------------------------
// Factory export
// -----------------------------------------------------
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new AccountsPage(stack);
}
