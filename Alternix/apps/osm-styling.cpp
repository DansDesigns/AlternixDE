// AlternixOS appearance manager. A single dark policy for Qt, GTK and native VLC.
// This never exports environment variables from a child process or paints GTK
// backgrounds with a global '*' selector (both were broken in the old app).
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

static bool saveGtk2(const QString &filename) {
    QFile source(filename);
    QStringList lines;
    if (source.exists()) {
        if (!source.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
        lines = QString::fromUtf8(source.readAll()).split('\n');
    }
    QStringList output;
    bool found = false;
    const QRegularExpression key(R"(^\s*gtk-theme-name\s*=)");
    for (const auto &line : lines) {
        if (key.match(line).hasMatch()) {
            if (!found) output << "gtk-theme-name=\"AlternixDark\"";
            found = true;
        } else output << line;
    }
    if (!found) output << "gtk-theme-name=\"AlternixDark\"";
    QSaveFile dst(filename);
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    dst.write(output.join('\n').toUtf8());
    return dst.commit();
}

static bool removeOldGeneratedCss(const QString &path) {
    QFile css(path);
    if (!css.exists()) return true;
    if (!css.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString data = QString::fromUtf8(css.readAll());
    css.close();
    const QRegularExpression old(
        R"(^\s*\*\s*\{\s*background-color:\s*rgb\(40,\s*40,\s*40\);\s*color:\s*white;\s*font-size:\s*\d+px;\s*\}\s*$)",
        QRegularExpression::CaseInsensitiveOption);
    return !old.match(data).hasMatch() || css.remove(); // Never remove user CSS.
}

static bool applyDark(QString &message) {
    const QString home = QDir::homePath();
    if (!QFile::exists("/usr/share/Kvantum/AlternixDark/AlternixDark.svg")) {
        message = "AlternixDark is not installed. Run the updated AlternixOS installer or updater first.";
        return false;
    }
    bool ok = true;
    QDir().mkpath(home + "/.config/Kvantum");
    QSettings kv(home + "/.config/Kvantum/kvantum.kvconfig", QSettings::IniFormat);
    kv.setValue("General/theme", "AlternixDark");
    kv.remove("Applications"); // No conflicting per-app style exceptions.
    kv.sync();
    ok = kv.status() == QSettings::NoError && ok;
    for (const QString folder : {QStringLiteral("gtk-3.0"), QStringLiteral("gtk-4.0")}) {
        QDir().mkpath(home + "/.config/" + folder);
        QSettings gtk(home + "/.config/" + folder + "/settings.ini", QSettings::IniFormat);
        gtk.setValue("Settings/gtk-theme-name", "Adwaita-dark");
        gtk.setValue("Settings/gtk-application-prefer-dark-theme", 1);
        gtk.sync();
        ok = (gtk.status() == QSettings::NoError) && ok;
    }
    ok = saveGtk2(home + "/.gtkrc-2.0") && ok;
    ok = removeOldGeneratedCss(home + "/.config/gtk-3.0/gtk.css") && ok;
    QDir().mkpath(home + "/.config/vlc");
    QSettings vlc(home + "/.config/vlc/vlc-qt-interface.conf", QSettings::IniFormat);
    vlc.setValue("MainWindow/QtStyle", "kvantum");
    vlc.sync();
    ok = (vlc.status() == QSettings::NoError) && ok;
    // GNOME/libadwaita use the current session's settings and dark preference.
    bool gnome = true;
    for (const QStringList args : {
             QStringList{"set", "org.gnome.desktop.interface", "gtk-theme", "Adwaita-dark"},
             QStringList{"set", "org.gnome.desktop.interface", "color-scheme", "prefer-dark"}}) {
        QProcess p;
        p.start("gsettings", args);
        if (!p.waitForFinished(6000) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0)
            gnome = false;
    }
    message = ok ? "Unified dark settings saved. Restart applications, then log out/in for all Qtile apps."
                 : "Some settings could not be saved. Check user-file permissions.";
    if (!gnome) message += "\nGNOME preferences will use the system defaults until a session bus is available.";
    return ok;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QWidget window;
    window.setWindowTitle("AlternixOS Appearance");
    window.resize(570, 325);
    window.setMinimumWidth(420);
    window.setStyleSheet(
        "QWidget{background:#292B2D;color:#E9EEEA;font-size:14px;}"
        "QLabel#title{font-size:21px;font-weight:700;color:#FFFFFF;}"
        "QLabel#status{background:#202225;border:1px solid #434C45;border-radius:9px;padding:12px;}"
        "QPushButton{background:#28B754;color:#071E0D;border:1px solid #41E875;"
        "border-radius:9px;padding:11px 18px;font-weight:700;}"
        "QPushButton:hover{background:#4BE779;}"
    );
    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    auto *title = new QLabel("AlternixOS · Unified Dark");
    title->setObjectName("title");
    layout->addWidget(title);
    auto *description = new QLabel(
        "Qt5 / Qt6 / VLC (native Qt): AlternixDark Kvantum\n"
        "GTK2: AlternixDark · GTK3/GTK4/GNOME: Adwaita dark\n"
        "A shared charcoal palette with green accents where supported.");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *button = new QPushButton("Apply unified dark appearance");
    layout->addWidget(button);
    auto *status = new QLabel("Applies settings to your account without changing other GTK CSS or icon themes.");
    status->setObjectName("status");
    status->setWordWrap(true);
    status->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    layout->addWidget(status, 1);
    QObject::connect(button, &QPushButton::clicked, &window, [status]() {
        QString message;
        bool ok = applyDark(message);
        status->setText(QString(ok ? "Applied: " : "Warning: ") + message);
    });
    window.show();
    return app.exec();
}
