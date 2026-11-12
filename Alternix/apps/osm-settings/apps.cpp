#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QScroller>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QStackedWidget>
#include <QProcess>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QFileInfoList>
#include <functional>
#include <QSet>
#include <QScrollBar>
#include <QMouseEvent>
#include <QList>
#include <QMessageBox>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QStandardPaths>
#include <QUrl>
#include <memory>

// =======================================================
// Global list of inner scroll areas (for hit testing)
// =======================================================
static QList<QScrollArea*> g_innerScrollAreas;

// Forward declare for refresh system
static std::function<void()> g_refreshAllLists;

// =======================================================
// Async command runner (non-blocking)
// =======================================================
static void runAsync(const QString &cmd, std::function<void(QString)> cb)
{
    QProcess *p = new QProcess;
    QObject::connect(
        p,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        [p, cb](int, QProcess::ExitStatus) {
            QString out = QString::fromUtf8(p->readAllStandardOutput()).trimmed();
            cb(out);
            p->deleteLater();
        }
    );
    p->start("bash", {"-c", cmd});
}

// =======================================================
// Command runner with an exit status (do not report a failed uninstall as success)
// =======================================================
static void runCommand(const QString &program, const QStringList &args,
                       std::function<void(bool, QString)> cb)
{
    QProcess *p = new QProcess;
    p->setProcessChannelMode(QProcess::MergedChannels);
    auto completed = std::make_shared<bool>(false);

    QObject::connect(p,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        [p, cb, completed](int exitCode, QProcess::ExitStatus status) {
            if (*completed) return;
            *completed = true;
            const QString output = QString::fromUtf8(p->readAll()).trimmed();
            cb(status == QProcess::NormalExit && exitCode == 0, output);
            p->deleteLater();
        });

    QObject::connect(p, &QProcess::errorOccurred,
        [p, cb, completed](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart || *completed) return;
            *completed = true;
            const QString message = p->errorString();
            cb(false, message);
            p->deleteLater();
        });

    p->start(program, args);
}

// =======================================================
// Read the Desktop Entry group, preserving the complete Exec command.
// In particular, an AppImage is NOT an APT package just because its
// .desktop launcher appears under ~/.local/share/applications.
// =======================================================
struct DesktopApp {
    QString desktopPath;
    QString name;
    QString exec;
    QString workingDirectory;
    QString appImagePath;
    bool appImageLauncher = false;
};

static DesktopApp readDesktopApp(const QString &filePath)
{
    DesktopApp entry;
    entry.desktopPath = filePath;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return entry;

    QTextStream stream(&file);
    bool inMainGroup = false;
    bool hasAppImageMarker = false;
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.startsWith('[') && line.endsWith(']')) {
            inMainGroup = (line == "[Desktop Entry]");
            continue;
        }
        if (!inMainGroup || line.startsWith('#')) continue;

        const int equals = line.indexOf('=');
        if (equals < 1) continue;
        const QString key = line.left(equals);
        const QString value = line.mid(equals + 1).trimmed();
        if (key == "Name" && entry.name.isEmpty()) entry.name = value;
        else if (key == "Exec") entry.exec = value;
        else if (key == "Path") entry.workingDirectory = value;
        else if (key.startsWith("X-AppImage-") ||
                 key.startsWith("X-AppImageLauncher-"))
            hasAppImageMarker = true;
    }

    // QProcess::splitCommand understands quoted executable paths with spaces.
    // Do not execute the desktop Exec field through a shell.
    const QStringList words = QProcess::splitCommand(entry.exec);
    for (const QString &word : words) {
        QString candidate = word;
        if (!candidate.endsWith(".AppImage", Qt::CaseInsensitive)) continue;
        entry.appImageLauncher = true;

        if (candidate.startsWith("file://")) {
            candidate = QUrl(candidate).toLocalFile();
        } else if (candidate.startsWith("~/")) {
            candidate = QDir::homePath() + candidate.mid(1);
        } else if (!QDir::isAbsolutePath(candidate)) {
            // A relative path is only reliable when the desktop entry gives
            // its working directory; a bare name may instead be in PATH.
            const QString inPath = QStandardPaths::findExecutable(candidate);
            if (!inPath.isEmpty()) candidate = inPath;
            else if (!entry.workingDirectory.isEmpty())
                candidate = QDir(entry.workingDirectory).absoluteFilePath(candidate);
            else continue;
        }
        if (QDir::isAbsolutePath(candidate))
            entry.appImagePath = QDir::cleanPath(candidate);
        break;
    }

    // Some AppImage integration tools provide an AppImage-specific key but
    // rename the bundle without the .AppImage suffix. Only accept a direct,
    // existing executable in that case; never infer it from a shell wrapper.
    if (!entry.appImageLauncher && hasAppImageMarker && !words.isEmpty()) {
        QString command = words.first();
        if (command.startsWith("~/")) command = QDir::homePath() + command.mid(1);
        // The type-1/type-2 AppImage ELF magic is "AI" followed by 1 or 2
        // at byte offset 8. Do not delete an unrelated binary just because
        // its launcher happens to contain an X-AppImage-* metadata key.
        if (QDir::isAbsolutePath(command)) {
            const QFileInfo info(command);
            if (!info.exists() && !info.isSymLink()) {
                // Orphaned launchers with an AppImage-specific marker are
                // still removable after their bundle has disappeared.
                entry.appImageLauncher = true;
                entry.appImagePath = QDir::cleanPath(command);
            } else if (info.isFile()) {
                QFile executable(command);
                if (executable.open(QIODevice::ReadOnly) && executable.seek(8)) {
                    const QByteArray magic = executable.read(3);
                    if (magic.size() == 3 && magic[0] == 'A' && magic[1] == 'I' &&
                        (magic[2] == char(1) || magic[2] == char(2))) {
                        entry.appImageLauncher = true;
                        entry.appImagePath = QDir::cleanPath(command);
                    }
                }
            }
        }
    }

    if (entry.name.isEmpty()) entry.name = QFileInfo(filePath).completeBaseName();
    return entry;
}

static QString userApplicationsDirectory()
{
    const QString xdg = qEnvironmentVariable("XDG_DATA_HOME");
    const QString dataDir = QDir::isAbsolutePath(xdg) ? xdg
                         : QDir::homePath() + "/.local/share";
    return QDir(dataDir).filePath("applications");
}

static QString comparableImagePath(const QString &path)
{
    // Compare the exact referenced file, not its resolved symlink target.
    // Uninstalling a shortcut to a symlink must not remove launchers that
    // point at a separate, still-installed underlying AppImage.
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

static QStringList matchingUserLaunchers(const DesktopApp &app)
{
    QStringList shortcuts;
    const QDir dir(userApplicationsDirectory());
    if (!dir.exists()) return shortcuts;

    const QString image = app.appImagePath.isEmpty()
                        ? QString() : comparableImagePath(app.appImagePath);
    const QFileInfoList files = dir.entryInfoList({"*.desktop"}, QDir::Files);
    for (const QFileInfo &fi : files) {
        const DesktopApp other = readDesktopApp(fi.absoluteFilePath());
        if (!other.appImageLauncher) continue;
        if (!image.isEmpty() && !other.appImagePath.isEmpty() &&
            comparableImagePath(other.appImagePath) == image) {
            shortcuts << fi.absoluteFilePath();
        } else if (image.isEmpty() && fi.absoluteFilePath() == app.desktopPath) {
            // The executable was recorded by name only and is no longer in
            // PATH. Remove just the explicitly selected orphaned launcher.
            shortcuts << fi.absoluteFilePath();
        }
    }
    return shortcuts;
}

static void resetUninstallButton(QPointer<QPushButton> button)
{
    if (!button) return;
    button->setText("❌");
    button->setEnabled(true);
}

static void uninstallAppImage(const DesktopApp &app, QPushButton *button)
{
    QPointer<QPushButton> safeButton(button);
    const QStringList shortcuts = matchingUserLaunchers(app);
    const QString imagePath = app.appImagePath;
    const QFileInfo imageFile(imagePath);
    const bool imageExists = !imagePath.isEmpty() &&
                             (imageFile.exists() || imageFile.isSymLink());

    if (shortcuts.isEmpty()) {
        QMessageBox::warning(button, "Cannot uninstall AppImage",
            "No matching user-owned .desktop launcher was found. "
            "System-wide launchers are not removed from this page.");
        resetUninstallButton(safeButton);
        return;
    }

    const QString question = imageExists
        ? QString("Remove this AppImage?\n\n%1\n\n"
                  "Also remove %2 matching shortcut(s) from:\n%3")
              .arg(imagePath).arg(shortcuts.size()).arg(userApplicationsDirectory())
        : QString("The AppImage executable is already missing or cannot be "
                  "resolved. Remove %1 orphaned shortcut(s) from:\n%2")
              .arg(shortcuts.size()).arg(userApplicationsDirectory());

    if (QMessageBox::question(button, "Uninstall AppImage", question,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        resetUninstallButton(safeButton);
        return;
    }

    // Only unlink the specific AppImage file identified by Exec. The .desktop
    // files are removed only from the user's own XDG applications directory.
    if (imageExists && !QFile::remove(imagePath)) {
        QMessageBox::warning(button, "Unable to uninstall AppImage",
            "Could not remove the AppImage file:\n" + imagePath +
            "\n\nThe shortcuts have been kept. Check file permissions.");
        resetUninstallButton(safeButton);
        return;
    }

    QStringList failedShortcuts;
    for (const QString &shortcut : shortcuts) {
        if (!QFile::remove(shortcut)) failedShortcuts << shortcut;
    }

    if (!failedShortcuts.isEmpty()) {
        QMessageBox::warning(button, "AppImage shortcut removal incomplete",
            "The AppImage executable was removed (or was already missing), "
            "but these shortcuts could not be deleted:\n" +
            failedShortcuts.join("\n"));
    } else {
        QMessageBox::information(button, "AppImage uninstalled",
            imageExists
                ? "AppImage and matching user shortcuts removed successfully."
                : "Orphaned AppImage shortcut(s) removed successfully.");
    }
    if (g_refreshAllLists) g_refreshAllLists();
}

// =======================================================
// Alternix Label Helper
// =======================================================
static QLabel* makeLabel(const QString &txt, int px, bool bold = false)
{
    QLabel *l = new QLabel(txt);
    QFont f("DejaVu Sans");
    f.setPointSize(px);
    f.setBold(bold);
    l->setFont(f);
    l->setAlignment(Qt::AlignCenter);
    l->setStyleSheet("color:white;");
    return l;
}

// =======================================================
// Row widget (app name + uninstall button)
// With ⏳ indicator during uninstall
// =======================================================
static QWidget* makeRow(const QString &name, std::function<void(QString, QPushButton*)> uninstallFn)
{
    QFrame *r = new QFrame;
    r->setStyleSheet("background:#3A3A3A; border-radius:24px;");
    QHBoxLayout *h = new QHBoxLayout(r);
    h->setContentsMargins(20, 14, 20, 14);

    QLabel *lbl = makeLabel(name, 22, false);
    lbl->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    lbl->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    h->addWidget(lbl);

    QPushButton *x = new QPushButton("❌");
    x->setStyleSheet(
        "QPushButton { background:transparent; color:#ff4444; font-size:32px; }"
        "QPushButton:hover { color:#ff1616; background:#ad1236; border-radius:18px; }"
        "QPushButton:pressed { color:#ffffff; background:#550000; border-radius:18px; }"
    );
    x->setFixedWidth(60);

    QObject::connect(x, &QPushButton::clicked, [=]() {
        x->setText("⏳");
        x->setEnabled(false);
        uninstallFn(name, x);
    });

    h->addWidget(x);
    return r;
}

// =======================================================
// Outer scroll area with manual drag scrolling
// (ignores drags that start on inner cards)
// =======================================================
class OuterScrollArea : public QScrollArea {
public:
    OuterScrollArea(QWidget *parent = nullptr)
        : QScrollArea(parent), dragging(false) {}

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            lastPos = event->pos();
            dragging = !isOnInnerScroll(event->pos());
        }
        QScrollArea::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging && (event->buttons() & Qt::LeftButton)) {
            int dy = event->pos().y() - lastPos.y();
            verticalScrollBar()->setValue(verticalScrollBar()->value() - dy);
            lastPos = event->pos();
            event->accept();
            return;
        }
        QScrollArea::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            dragging = false;
        QScrollArea::mouseReleaseEvent(event);
    }

private:
    bool dragging;
    QPoint lastPos;

    bool isOnInnerScroll(const QPoint &p)
    {
        QWidget *vp = viewport();
        for (QScrollArea *sa : g_innerScrollAreas) {
            if (!sa || !sa->viewport()) continue;

            QPoint topLeft = sa->viewport()->mapTo(vp, QPoint(0,0));
            QRect rect(topLeft, sa->viewport()->size());
            if (rect.contains(p))
                return true;
        }
        return false;
    }
};

// =======================================================
// Populate card list (6 rows visible max)
// =======================================================
static void populateList(QVBoxLayout *list, QScrollArea *area,
                         QStringList entries,
                         std::function<void(QString, QPushButton*)> uninstallFn)
{
    if (!list || !area) return;

    QLayoutItem *c;
    while ((c = list->takeAt(0))) {
        if (c->widget()) c->widget()->deleteLater();
        delete c;
    }

    if (entries.isEmpty()) {
        list->addWidget(makeLabel("No applications installed", 20));
    } else {
        for (const QString &e : entries)
            list->addWidget(makeRow(e, uninstallFn));
    }

    list->addStretch();

    int rowH = 72;
    int count = entries.size();
    int visible = qMin(count == 0 ? 1 : count, 6);

    area->setFixedHeight(rowH * visible);
}

// =======================================================
// Build card (inner scroll area)
// =======================================================
static QFrame* makeAppCard(const QString &title,
                           QVBoxLayout **outList,
                           QScrollArea **outScroll)
{
    QFrame *card = new QFrame;
    card->setStyleSheet("background:#444444; border-radius:30px;");

    QVBoxLayout *v = new QVBoxLayout(card);
    v->setContentsMargins(30, 30, 30, 30);
    v->setSpacing(20);

    QLabel *t = makeLabel(title, 26, true);
    v->addWidget(t);

    QScrollArea *sa = new QScrollArea;
    sa->setWidgetResizable(true);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    if (title.startsWith("Snap")) sa->setObjectName("inner_scroll_snap");
    else if (title.startsWith("Flatpak")) sa->setObjectName("inner_scroll_flatpak");
    else if (title.startsWith("APT")) sa->setObjectName("inner_scroll_apt");
    else sa->setObjectName("inner_scroll_generic");

    QScroller::grabGesture(sa->viewport(), QScroller::LeftMouseButtonGesture);
    sa->viewport()->setAttribute(Qt::WA_AcceptTouchEvents, true);

    QWidget *wrap = new QWidget;
    QVBoxLayout *list = new QVBoxLayout(wrap);
    list->setSpacing(14);
    list->setContentsMargins(0, 0, 0, 0);
    list->addWidget(makeLabel("Calculating..", 22));
    list->addStretch();

    sa->setWidget(wrap);
    v->addWidget(sa);

    g_innerScrollAreas.append(sa);

    *outList = list;
    *outScroll = sa;
    return card;
}

// =======================================================
// Desktop launcher loader (APT apps and AppImage integrations)
// =======================================================
static void loadAPT(QVBoxLayout *list, QScrollArea *area)
{
    QStringList dirs;
    dirs << userApplicationsDirectory() << "/usr/share/applications";

    QStringList out;
    QSet<QString> seen;
    QHash<QString, DesktopApp> entriesByLabel;

    for (const QString &directory : dirs) {
        const QDir dir(directory);
        if (!dir.exists()) continue;

        const QFileInfoList files = dir.entryInfoList({"*.desktop"}, QDir::Files);
        for (const QFileInfo &fi : files) {
            const DesktopApp app = readDesktopApp(fi.absoluteFilePath());
            if (app.exec.isEmpty()) continue;

            const QStringList command = QProcess::splitCommand(app.exec);
            if (command.isEmpty()) continue;
            const QString executable = command.first();
            // Wrapper-based AppImages may all start with 'env' or 'sh'.
            // Deduplicate them by bundle, not by the wrapper command.
            const QString uniqueKey = app.appImageLauncher
                ? (app.appImagePath.isEmpty()
                   ? "appimage-shortcut:" + app.desktopPath
                   : "appimage:" + comparableImagePath(app.appImagePath))
                : "command:" + executable;
            if (seen.contains(uniqueKey)) continue;
            seen.insert(uniqueKey);

            const QString label = app.name + " — " +
                (app.appImagePath.isEmpty() ? executable : app.appImagePath);
            out << label;
            entriesByLabel.insert(label, app);
        }
    }

    out.sort(Qt::CaseInsensitive);

    auto uninstall = [=](QString label, QPushButton *button) {
        QPointer<QPushButton> safeButton(button);
        const DesktopApp app = entriesByLabel.value(label);

        if (app.appImageLauncher) {
            uninstallAppImage(app, button);
            return;
        }

        // Refuse to treat shell/interpreter wrappers as uninstallable packages:
        // e.g. 'env' must not result in an attempt to remove coreutils.
        const QStringList command = QProcess::splitCommand(app.exec);
        if (command.isEmpty()) {
            QMessageBox::warning(button, "Cannot uninstall", "No executable was found.");
            resetUninstallButton(safeButton);
            return;
        }
        const QString executable = command.first();
        const QString exeName = QFileInfo(executable).fileName();
        static const QSet<QString> wrappers = {
            "env", "sh", "bash", "dash", "python", "python3", "xdg-open"
        };
        if (wrappers.contains(exeName)) {
            QMessageBox::warning(button, "Cannot identify package",
                "This launcher uses a wrapper command. The application package "
                "cannot be identified safely from its .desktop entry.");
            resetUninstallButton(safeButton);
            return;
        }

        const QString path = QStandardPaths::findExecutable(executable);
        if (path.isEmpty()) {
            QMessageBox::warning(button, "Cannot identify package",
                "The launcher executable could not be found:\n" + executable +
                "\n\nNo package or shortcut was removed.");
            resetUninstallButton(safeButton);
            return;
        }

        runCommand("dpkg-query", {"-S", path}, [=](bool found, QString output) {
            if (!found || !output.contains(": ")) {
                QMessageBox::warning(safeButton, "Not an APT package",
                    "No installed APT package owns this executable:\n" + path +
                    "\n\nNothing was uninstalled.");
                resetUninstallButton(safeButton);
                return;
            }
            const QString packageName = output.section(": ", 0, 0).trimmed();
            if (packageName.isEmpty() || packageName.contains('\n')) {
                QMessageBox::warning(safeButton, "Cannot identify package",
                    "The package lookup returned an ambiguous result. Nothing was removed.");
                resetUninstallButton(safeButton);
                return;
            }

            // All privileged package operations must use AlternixOS authentication.
            runCommand("osm-sudo", {"apt", "remove", "-y", packageName},
                       [=](bool ok, QString details) {
                if (ok) {
                    QMessageBox::information(safeButton, "Uninstalled",
                        label + " has been uninstalled");
                    if (g_refreshAllLists) g_refreshAllLists();
                } else {
                    QMessageBox::warning(safeButton, "Uninstall failed",
                        "The package could not be removed.\n\n" + details);
                    resetUninstallButton(safeButton);
                }
            });
        });
    };

    populateList(list, area, out, uninstall);
}

// =======================================================
// Flatpak loader
// =======================================================
static void loadFlatpak(QWidget *ctx,
                        QVBoxLayout *list, QScrollArea *area,
                        QVBoxLayout *aptL, QScrollArea *aptA)
{
    runAsync("flatpak list --app --columns=application 2>/dev/null",
    [=](QString out) {

        QStringList apps = out.split("\n", Qt::SkipEmptyParts);
        apps.sort(Qt::CaseInsensitive);

        auto uninstall = [=](QString p, QPushButton *btn) {
            QPointer<QPushButton> safeButton(btn);
            runCommand("flatpak", {"uninstall", "-y", p}, [=](bool ok, QString details) {
                if (ok) {
                    QMessageBox::information(safeButton, "Uninstalled",
                                             p + " has been uninstalled");
                    if (g_refreshAllLists) g_refreshAllLists();
                } else {
                    QMessageBox::warning(safeButton, "Uninstall failed",
                                         "Flatpak could not be removed.\n\n" + details);
                    resetUninstallButton(safeButton);
                }
            });
        };

        populateList(list, area, apps, uninstall);

        QTimer::singleShot(0, ctx, [=]() {
            loadAPT(aptL, aptA);
        });
    });
}

// =======================================================
// Snap loader
// =======================================================
static void loadSnap(QWidget *ctx,
                     QVBoxLayout *list, QScrollArea *area,
                     QVBoxLayout *flatL, QScrollArea *flatA,
                     QVBoxLayout *aptL,  QScrollArea *aptA)
{
    runAsync("snap list 2>/dev/null | tail -n +2 | awk '{print $1}'",
    [=](QString out) {

        QStringList apps = out.split("\n", Qt::SkipEmptyParts);
        apps.sort(Qt::CaseInsensitive);

        auto uninstall = [=](QString p, QPushButton *btn) {
            QPointer<QPushButton> safeButton(btn);
            runCommand("osm-sudo", {"snap", "remove", p}, [=](bool ok, QString details) {
                if (ok) {
                    QMessageBox::information(safeButton, "Uninstalled",
                                             p + " has been uninstalled");
                    if (g_refreshAllLists) g_refreshAllLists();
                } else {
                    QMessageBox::warning(safeButton, "Uninstall failed",
                                         "Snap could not be removed.\n\n" + details);
                    resetUninstallButton(safeButton);
                }
            });
        };

        populateList(list, area, apps, uninstall);

        QTimer::singleShot(0, ctx, [=]() {
            loadFlatpak(ctx, flatL, flatA, aptL, aptA);
        });
    });
}

// =======================================================
// PAGE FACTORY
// =======================================================
extern "C" QWidget* make_page(QStackedWidget *stack)
{
    g_innerScrollAreas.clear();

    QWidget *root = new QWidget;
    root->setStyleSheet("background:#282828; color:white;");

    QVBoxLayout *rootLay = new QVBoxLayout(root);
    rootLay->setContentsMargins(40, 40, 40, 40);
    rootLay->setSpacing(10);

    QLabel *title = new QLabel("Installed Apps");
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet("font-size:42px; font-weight:bold;");
    rootLay->addWidget(title);

    OuterScrollArea *scroll = new OuterScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget *wrap = new QWidget;
    QVBoxLayout *wrapLay = new QVBoxLayout(wrap);
    wrapLay->setSpacing(20);
    wrapLay->setContentsMargins(0, 0, 0, 0);

    QFrame *outer = new QFrame;
    outer->setStyleSheet("background:#3a3a3a; border-radius:40px;");
    QVBoxLayout *outerLay = new QVBoxLayout(outer);
    outerLay->setContentsMargins(50, 30, 50, 30);
    outerLay->setSpacing(30);

    QVBoxLayout *snapL;  QScrollArea *snapA;
    QVBoxLayout *flatL;  QScrollArea *flatA;
    QVBoxLayout *aptL;   QScrollArea *aptA;

    outerLay->addWidget(makeAppCard("Snap Applications",   &snapL, &snapA));
    outerLay->addWidget(makeAppCard("Flatpak Applications",&flatL, &flatA));
    outerLay->addWidget(makeAppCard("APT Installed Apps",  &aptL, &aptA));

    wrapLay->addWidget(outer);
    wrapLay->addStretch();

    scroll->setWidget(wrap);
    rootLay->addWidget(scroll);

    // REFRESH SYSTEM
    g_refreshAllLists = [=]() {
        loadSnap(root, snapL, snapA, flatL, flatA, aptL, aptA);
    };

    // BACK BUTTON
    QPushButton *back = new QPushButton("❮");
    back->setStyleSheet(
        "QPushButton { background:#444; border-radius:20px; "
        "font-size:32px; font-weight:bold; padding:10px 20px; }"
        "QPushButton:hover { background:#555; }"
        "QPushButton:pressed { background:#333; }"
    );
    back->setFixedSize(140, 60);
    QObject::connect(back, &QPushButton::clicked, [stack]() {
        stack->setCurrentIndex(0);
    });
    rootLay->addWidget(back, 0, Qt::AlignCenter);

    // INITIAL LOAD
    QTimer::singleShot(0, root, [=]() {
        g_refreshAllLists();
    });

    return root;
}
