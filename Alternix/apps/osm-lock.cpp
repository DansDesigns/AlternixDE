/*
compile with:
g++ osm-lock.cpp -o osm-lock -fPIC $(pkg-config --cflags --libs Qt5Widgets) 

 make it executable:
 chmod +x ~/osm-lock

 move to /.local/bin:
 sudo mv ~/osm-lock /usr/local/bin/
 
 
test with:
./osm-lock

*/


#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QScreen>
#include <QVector>
#include <QRect>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QTextStream>
#include <QLabel>
#include <QPushButton>
#include <QGridLayout>
#include <QRandomGenerator>
#include <QTimer>
#include <QElapsedTimer>
#include <QtMath>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QWindow>
#include <QProcess>
#include <QFileInfo>
#include <unistd.h>

struct ShapeItem {
    QString shape;
    QColor color;
    QRect rect;
};

static QString sha256(const QString &input) {
    return QString(QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Sha256).toHex());
}

class LockScreen : public QWidget {
    QVector<ShapeItem> shapes;
    QVector<QString> patternHash;
    QString passwordHash;
    bool enhancedSecurity = false;
    bool secureAuth = false;
    bool pamPrompt = false;

    QVector<QString> currentSeq;
    int attemptCount = 0;
    int pinAttemptCount = 0;

    // One 20-second monotonic deadline for the entire pattern -> PIN flow.
    // The countdown never restarts when switching methods or retrying.
    static constexpr int authenticationWindowMs = 20000;
    QElapsedTimer authenticationClock;
    QTimer *authenticationTimer = nullptr;
    bool authenticationStarted = false;
    bool authorizationDenied = false;

    bool pinModeActive = false;
    bool pinSetupMode = false;
    bool pinSetupConfirm = false;
    QString pinInput;
    QString pinSetupFirst;

    QLabel *titleLabel = nullptr;
    QLabel *countdownLabel = nullptr;
    QPushButton *securityToggle = nullptr;
    QPushButton *cancelButton = nullptr;
    QPushButton *pinButton = nullptr;
    QWidget *pinWidget = nullptr;

    bool firstRun = false;
    bool enhancedLocked = false;

public:
    LockScreen() {
        pamPrompt = QCoreApplication::arguments().contains(QStringLiteral("--pam-prompt"));
        setWindowTitle("OSM Lock");
        setWindowFlag(Qt::FramelessWindowHint);
        setWindowState(Qt::WindowFullScreen);
        setStyleSheet("background-color:#282828; color:white;");

        titleLabel = new QLabel(this);
        titleLabel->setStyleSheet("font-size:20px; color:white;");
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setGeometry(0, 5, width(), 30);

        countdownLabel = new QLabel(this);
        countdownLabel->setAlignment(Qt::AlignCenter);
        countdownLabel->setStyleSheet("font-size:24px; color:#dddddd; background:transparent;");
        countdownLabel->hide(); // Enrollment is not an authentication attempt.

        securityToggle = new QPushButton("Enhanced Security Mode", this);
        securityToggle->setFlat(true);
        securityToggle->setGeometry(10, 5, 220, 30);
        connect(securityToggle, &QPushButton::clicked, this, [this]() {
            if (!firstRun || enhancedLocked) return; // disable if locked or not first run
            enhancedSecurity = !enhancedSecurity;
            updateSecurityToggleStyle();
            saveConfigEnhancedOnly();
            if (pinModeActive) {
                buildPinPad();
                pinInput.clear();
                update();
            } else {
                update();
            }
        });

        cancelButton = new QPushButton("Cancel", this);
        cancelButton->setFlat(true);
        cancelButton->setGeometry(width() - 130, 5, 120, 30);
        cancelButton->setStyleSheet(
            "QPushButton { background:transparent; color:#ff8080; font-size:16px; } "
            "QPushButton:hover { color:#ffaaaa; }");
        connect(cancelButton, &QPushButton::clicked, this, &LockScreen::cancelEntry);

        if (pamPrompt) {
            pinButton = new QPushButton("Use Fallback PIN", this);
            pinButton->setFlat(true);
            pinButton->setStyleSheet(
                "QPushButton { background:transparent; color:white; font-size:16px; } "
                "QPushButton:hover { color:#bbbbbb; }");
            connect(pinButton, &QPushButton::clicked, this, [this]() {
                // Manual PIN fallback is allowed, but still has only two tries
                // and shares the same 20-second authentication deadline.
                activatePinFallback();
            });
        }

        loadConfig();
        if (pinButton) pinButton->setVisible(!firstRun);

        // Cancel only makes sense once a lock method already exists —
        // during first-run setup you have to finish creating one.
        cancelButton->setVisible(!firstRun || secureAuth);

        // toggle visibility logic
        if (firstRun) {
            securityToggle->show();
            securityToggle->setEnabled(true);
        } else {
            if (enhancedSecurity) {
                securityToggle->show();
                securityToggle->setEnabled(false);
            } else {
                securityToggle->hide();
            }
        }

        if (firstRun && secureAuth)
            titleLabel->setText("Pattern not enrolled. Run Alternix installer as root.");
        else if (firstRun)
            titleLabel->setText("Please select your pattern of shapes");
        else
            titleLabel->setText("Enter your pattern");

        generateGrid();

        authenticationTimer = new QTimer(this);
        authenticationTimer->setInterval(100);
        connect(authenticationTimer, &QTimer::timeout,
                this, [this]() { refreshAuthenticationCountdown(); });

        // KEYBOARD GRAB — DO NOT REMOVE
        // Qtile's shortcuts (Win+Enter for a terminal and the rest) are
        // grabbed on the root window, so they fire whichever window has
        // focus. Swallowing keys in keyPressEvent cannot stop them. An
        // active keyboard grab does: while this window holds it, every
        // key comes here and Qtile's shortcuts cannot trigger. It also
        // stops typing reaching the window underneath. The grab only
        // works once the window is on screen, and fails while another
        // program holds one, so keep retrying until it succeeds.
        QTimer *grabTimer = new QTimer(this);
        grabTimer->setInterval(200);
        connect(grabTimer, &QTimer::timeout, this, [this, grabTimer]() {
            if (windowHandle() && windowHandle()->setKeyboardGrabEnabled(true))
                grabTimer->stop();
        });
        grabTimer->start();
    }

protected:
    void resizeEvent(QResizeEvent *) override {
        securityToggle->setGeometry(10, 5, 220, 30);
        if (cancelButton)
            cancelButton->setGeometry(width() - 130, 5, 120, 30);
        if (pinButton)
            pinButton->setGeometry((width() - 210) / 2, 5, 210, 30);

    // estimate grid area (lower half of screen)
        int gridTop = height() * 0.5;  // start of grid roughly mid-screen
        int desiredY = gridTop - 120;  // move label about 120px above grid
        if (desiredY < 40) desiredY = 40; // clamp minimum

        titleLabel->setGeometry(0, desiredY, width(), 40);
        if (countdownLabel)
            countdownLabel->setGeometry(0, qMax(38, desiredY - 44), width(), 36);

        if (pinWidget && pinWidget->isVisible())
            positionPinPad();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        if (!pinModeActive) {
            for (const auto &s : shapes) {
                p.setBrush(s.color);
                p.setPen(QPen(Qt::white, 2));
                int x = s.rect.x(), y = s.rect.y(), w = s.rect.width(), h = s.rect.height();
                int cx = x + w / 2, cy = y + h / 2;
                if (s.shape == "circle")
                    p.drawEllipse(QRect(x, y, w, h));
                else if (s.shape == "square")
                    p.drawRect(QRect(x, y, w, h));
                else if (s.shape == "triangle") {
                    QPolygonF tri;
                    tri << QPointF(cx, y)
                        << QPointF(x, y + h)
                        << QPointF(x + w, y + h);
                    p.drawPolygon(tri);
                } else if (s.shape == "pentagon") {
                    QPolygonF pent;
                    for (int k = 0; k < 5; k++) {
                        double ang = qDegreesToRadians(72.0 * k - 90.0);
                        pent << QPointF(cx + w / 2 * qCos(ang),
                                        cy + h / 2 * qSin(ang));
                    }
                    p.drawPolygon(pent);
                }
            }
        }

        int neededDots = enhancedSecurity ? 5 : 4;
        int dotSize = 14, spacing = 20;
        int total = (dotSize + spacing) * neededDots - spacing;
        int startX = (width() - total) / 2;
        //int y = 50;
        // Position dots dynamically around mid-to-lower top area
        int gridTop = height() * 0.5;    // approximate start of grid/keypad area
        int y = gridTop - 70;            // dots ~70px above that
        if (y < 80) y = 80;              // clamp so not too high on small screens

        if (pinModeActive) {
            for (int i = 0; i < neededDots; i++) {
                bool filled = (i < pinInput.size());
                p.setBrush(filled ? Qt::white : Qt::gray);
                p.setPen(Qt::NoPen);
                p.drawEllipse(startX + i * (dotSize + spacing), y, dotSize, dotSize);
            }
        } else {
            for (int i = 0; i < neededDots; i++) {
                bool filled = (i < currentSeq.size());
                p.setBrush(filled ? Qt::white : Qt::gray);
                p.setPen(Qt::NoPen);
                p.drawEllipse(startX + i * (dotSize + spacing), y, dotSize, dotSize);
            }
        }
    }

    void mousePressEvent(QMouseEvent *ev) override {
        if (authenticationTimedOut()) return;
        if (securityToggle->isVisible() && securityToggle->geometry().contains(ev->pos())) {
            QWidget::mousePressEvent(ev);
            return;
        }

        if (pinModeActive) {
            QWidget::mousePressEvent(ev);
            return;
        }

        for (const auto &s : shapes) {
            if (s.rect.contains(ev->pos())) {
                QString key = s.shape + "-" + colorName(s.color);
                QString h = sha256(key);
                currentSeq.append(h);
                update();

                int reqShapes = requiredShapeCount();
                if (firstRun) {
                    if (currentSeq.size() == reqShapes) {
                        // Lock enhanced mode once pattern chosen
                        enhancedLocked = true;
                        securityToggle->setEnabled(false);
                        saveConfigEnhancedOnly();
                        startPinSetupMode();
                    }
                } else {
                    if (currentSeq.size() == reqShapes)
                        verifyPattern();
                }
                break;
            }
        }
    }

    void closeEvent(QCloseEvent *e) override { e->ignore(); }

    void keyPressEvent(QKeyEvent *e) override {
        if (authenticationTimedOut()) { e->accept(); return; }
        if ((e->key() == Qt::Key_F4 && (e->modifiers() & Qt::AltModifier)) ||
            ((e->key() == Qt::Key_W || e->key() == Qt::Key_Q) &&
             (e->modifiers() & (Qt::MetaModifier | Qt::ControlModifier)))) {
            e->ignore();
            return;
        }

        if (!pinModeActive) {
            e->ignore();
            return;
        }

        bool isEnhanced = enhancedSecurity;
        int req = requiredPinLength();
        int key = e->key();
        QString text = e->text();

        if (key == Qt::Key_Backspace) {
            if (!pinInput.isEmpty()) {
                pinInput.chop(1);
                update();
            }
            return;
        }

        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            submitPinEntry();
            return;
        }

        if (key >= Qt::Key_0 && key <= Qt::Key_9) {
            if (pinInput.size() < req) {
                pinInput.append(QString(QChar('0' + (key - Qt::Key_0))));
                update();
                if (pinInput.size() == req)
                    submitPinEntry();
            }
            return;
        }

        if (isEnhanced && (text == "!" || text == "?" || text == "<" || text == ">")) {
            if (pinInput.size() < req) {
                pinInput.append(text);
                update();
                if (pinInput.size() == req)
                    submitPinEntry();
            }
            return;
        }

        e->ignore();
    }

public:
    // Called just after the fullscreen window is shown. PAM receives a READY
    // handshake before starting its own independent 20-second watchdog.
    void beginAuthenticationCountdown() {
        if (firstRun || authenticationStarted) return;
        authenticationStarted = true;
        authenticationClock.start();
        countdownLabel->show();
        refreshAuthenticationCountdown();
        if (pamPrompt) {
            static const char ready[] = "READY\n";
            if (::write(3, ready, sizeof(ready) - 1) != ssize_t(sizeof(ready) - 1))
                denyAuthorization();
        }
        authenticationTimer->start();
    }

private:
    void denyAuthorization() {
        if (authorizationDenied) return;
        authorizationDenied = true;
        if (authenticationTimer) authenticationTimer->stop();
        qApp->exit(1); // PAM, --auth and osm-lockscreen all treat nonzero as DENIED.
    }

    bool authenticationTimedOut() {
        if (authorizationDenied) return true;
        if (authenticationStarted && authenticationClock.elapsed() >= authenticationWindowMs) {
            countdownLabel->setText("Time expired");
            denyAuthorization();
            return true;
        }
        return false;
    }

    void refreshAuthenticationCountdown() {
        if (!authenticationStarted || authorizationDenied) return;
        if (authenticationTimedOut()) return;
        const int left = (authenticationWindowMs - int(authenticationClock.elapsed()) + 999) / 1000;
        countdownLabel->setText(QString("%1s remaining").arg(left));
        countdownLabel->setStyleSheet(left <= 5
            ? "font-size:24px; color:#ff8080; background:transparent;"
            : "font-size:24px; color:#dddddd; background:transparent;");
    }

    void activatePinFallback() {
        if (firstRun || authenticationTimedOut() || pinModeActive) return;
        pinModeActive = true;
        pinSetupMode = false;
        pinSetupConfirm = false;
        pinInput.clear();
        pinAttemptCount = 0;
        titleLabel->setText("Enter Fallback PIN (2 attempts)");
        if (pinButton) pinButton->hide();
        buildPinPad();
        update();
    }

    // ---------- CONFIG ----------
    static QString authPath() { return "/usr/local/libexec/alternix-auth"; }

    bool authOperation(const QString &action, const QByteArray &payload = QByteArray()) {
        QProcess p;
        p.start(authPath(), {action});
        if (!p.waitForStarted(3000)) return false;
        if (!payload.isEmpty()) {
            if (p.write(payload) != payload.size()) { p.kill(); p.waitForFinished(500); return false; }
        }
        p.closeWriteChannel();
        if (!p.waitForFinished(10000)) { p.kill(); p.waitForFinished(500); return false; }
        return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    }

    bool submitPam(const QByteArray &payload) {
        if (!pamPrompt || payload.size() > 900 || authenticationTimedOut()) return false;
        const QByteArray message = payload + "\n";
        const ssize_t written = ::write(3, message.constData(), size_t(message.size()));
        if (written != message.size()) { qApp->exit(1); return false; }
        qApp->exit(0);
        return true;
    }

    void loadConfig() {
        secureAuth = QFileInfo(authPath()).isExecutable();
        if (secureAuth) {
            QProcess status;
            status.start(authPath(), {"--status"});
            if (!status.waitForStarted(2000) || !status.waitForFinished(3000)) {
                firstRun = true;
                titleLabel->setText("Alternix authentication unavailable");
                return;
            }
            QByteArray result = status.readAllStandardOutput().trimmed();
            if (result == "READY:0" || result == "READY:1") {
                firstRun = false;
                enhancedSecurity = (result == "READY:1");
            } else {
                firstRun = true;
                enhancedSecurity = false;
            }
            updateSecurityToggleStyle();
            if (pamPrompt && firstRun) QTimer::singleShot(0, qApp, [](){ qApp->exit(1); });
            return;
        }
        QString cfg = QDir::homePath() + "/.config/Alternix/.osm_lockdata";
        QFile file(cfg);
        if (!file.exists()) {
            firstRun = true;
            enhancedSecurity = false;
            updateSecurityToggleStyle();
            return;
        }

        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            while (!in.atEnd()) {
                QString line = in.readLine();
                if (line.startsWith("pattern=")) {
                    QStringList list = line.mid(8).split(",");
                    patternHash = QVector<QString>::fromList(list);
                } else if (line.startsWith("password=")) {
                    passwordHash = line.mid(9);
                } else if (line.startsWith("enhanced=")) {
                    enhancedSecurity = (line.mid(9).trimmed() == "1");
                }
            }
            file.close();
            firstRun = false;
            updateSecurityToggleStyle();
        }
    }

    bool saveConfig() {
        if (secureAuth) {
            const QString pattern = QStringList::fromVector(currentSeq).join(",");
            const QByteArray payload = ("P:" + pattern + "\nI:" + passwordHash +
                                        "\nE:" + (enhancedSecurity ? "1" : "0") + "\n").toLatin1();
            // New sudo patterns are enrolled only by the privileged installer.
            // No unprivileged process may create a new root credential.
            Q_UNUSED(payload);
            return false;
        }
        QString path = QDir::homePath() + "/.config/Alternix/.osm_lockdata";
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            QStringList plist = QStringList::fromVector(patternHash);
            out << "pattern=" << plist.join(",") << "\n";
            out << "password=" << passwordHash << "\n";
            out << "enhanced=" << (enhancedSecurity ? "1" : "0") << "\n";
            f.close();
            return true;
        }
        return false;
    }

    void saveConfigEnhancedOnly() {
        if (secureAuth) return;
        QString path = QDir::homePath() + "/.config/Alternix/.osm_lockdata";
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << "enhanced=" << (enhancedSecurity ? "1" : "0") << "\n";
            f.close();
        }
    }

    void updateSecurityToggleStyle() {
        if (enhancedSecurity)
            securityToggle->setStyleSheet("QPushButton { background:transparent; color:#00ff00; font-size:16px; }");
        else
            securityToggle->setStyleSheet("QPushButton { background:transparent; color:white; font-size:16px; }");
    }

    // ---------- PATTERN ----------
    int requiredShapeCount() const { return enhancedSecurity ? 5 : 4; }

    void generateGrid() {
        shapes.clear();
        QStringList shapeList = {"circle","triangle","square","pentagon"};
        QList<QColor> colors = {Qt::red, Qt::blue, Qt::green, Qt::white};
        QVector<QPair<QString,QColor>> pool;
        for (const auto &sh : shapeList)
            for (const auto &cl : colors)
                pool.append(qMakePair(sh, cl));

        std::shuffle(pool.begin(), pool.end(), *QRandomGenerator::global());

        int size = 100, pad = 10, cols = 4, rows = 4;
        QRect screen = QApplication::primaryScreen()->geometry();
        int totalW = cols * (size + pad) - pad;
        int totalH = rows * (size + pad) - pad;
        int startX = (screen.width() - totalW) / 2;
        int startY = screen.height() / 2 + (screen.height() / 4 - totalH / 2);

        int idx = 0;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                ShapeItem it;
                it.shape = pool[idx].first;
                it.color = pool[idx].second;
                it.rect = QRect(startX + c * (size + pad),
                                startY + r * (size + pad),
                                size, size);
                shapes.append(it);
                idx++;
            }
        }
        update();
    }

    QString colorName(const QColor &c) const {
        if (c == Qt::red) return "red";
        if (c == Qt::blue) return "blue";
        if (c == Qt::green) return "green";
        if (c == Qt::white) return "white";
        return "unknown";
    }

    // ---------- PIN ----------
    int requiredPinLength() const { return enhancedSecurity ? 5 : 4; }

    void startPinSetupMode() {
        pinModeActive = true;
        pinSetupMode = true;
        pinSetupConfirm = false;
        pinInput.clear();
        titleLabel->setText("Set your Fallback PIN");
        buildPinPad();
        update();
    }

    void positionPinPad() {
        if (!pinWidget) return;
        int cols = enhancedSecurity ? 4 : 3;
        int btnSize = 90;
        int spacing = 10;
        int totalW = cols * (btnSize + spacing) - spacing;
        int x = (width() - totalW) / 2;
        int y = height() / 2 - 20;
        pinWidget->setGeometry(x, y, totalW, btnSize * 4 + spacing * 3);
    }

    void buildPinPad() {
        if (pinWidget) {
            pinWidget->deleteLater();
            pinWidget = nullptr;
        }

        pinWidget = new QWidget(this);
        QGridLayout *grid = new QGridLayout(pinWidget);
        grid->setSpacing(10);
        grid->setContentsMargins(0, 0, 0, 0);

        QStringList keys;
        if (enhancedSecurity) {
            keys << "1" << "2" << "3" << "!"
                 << "4" << "5" << "6" << "?"
                 << "7" << "8" << "9" << "<"
                 << "⌫" << "0" << "↵" << ">";
        } else {
            keys << "1" << "2" << "3"
                 << "4" << "5" << "6"
                 << "7" << "8" << "9"
                 << "⌫" << "0" << "↵";
        }

        int cols = enhancedSecurity ? 4 : 3;
        int btnSize = 90;
        for (int i = 0; i < keys.size(); i++) {
            QString label = keys[i];
            QPushButton *btn = new QPushButton(label);
            btn->setFixedSize(btnSize, btnSize);
            btn->setStyleSheet("QPushButton { border:2px solid white; border-radius:45px; font-size:28px; color:white; background:#333; } QPushButton:hover { background:#555; }");
            int r = i / cols;
            int c = i % cols;
            grid->addWidget(btn, r, c);
            connect(btn, &QPushButton::clicked, this, [this, label]() { handlePinPress(label); });
        }

        pinWidget->setLayout(grid);
        pinWidget->show();
        positionPinPad();
    }

    void handlePinPress(const QString &label) {
        if (authenticationTimedOut()) return;
        int req = requiredPinLength();

        if (label == "⌫") {
            if (!pinInput.isEmpty()) {
                pinInput.chop(1);
                update();
            }
            return;
        }

        if (label == "↵") {
            submitPinEntry();
            return;
        }

        if (pinInput.size() < req) {
            pinInput.append(label);
            update();
            if (pinInput.size() == req)
                submitPinEntry();
        }
    }

    void submitPinEntry() {
        if (authenticationTimedOut()) return;
        int req = requiredPinLength();
        if (pinInput.size() < req) return;

        if (pinSetupMode) {
            if (!pinSetupConfirm) {
                pinSetupFirst = pinInput;
                pinInput.clear();
                pinSetupConfirm = true;
                titleLabel->setText("Confirm your Fallback PIN");
                update();
            } else {
                if (pinInput == pinSetupFirst) {
                    passwordHash = sha256(pinInput);
                    patternHash = currentSeq;
                    if (saveConfig()) qApp->quit();
                    else { titleLabel->setText("Could not securely save your pattern/PIN"); pinInput.clear(); update(); }
                } else {
                    pinInput.clear();
                    pinSetupFirst.clear();
                    pinSetupConfirm = false;
                    titleLabel->setText("Set your Fallback PIN");
                    update();
                }
            }
        } else {
            // The root-owned verifier checks EVERY entry, including incorrect
            // entries in PAM mode. The PAM parent verifies the successful
            // candidate again, so the GUI never grants privileges on its own.
            const QString hash = sha256(pinInput);
            const QByteArray candidate = ("I:" + hash).toLatin1();
            const bool valid = secureAuth
                    ? authOperation("--verify", candidate + "\n")
                    : (hash == passwordHash);
            if (authenticationTimedOut()) return;
            if (valid) {
                if (pamPrompt) submitPam(candidate);
                else qApp->quit();
                return;
            }
            pinInput.clear();
            ++pinAttemptCount;
            if (pinAttemptCount >= 2) {
                denyAuthorization();
                return;
            }
            titleLabel->setText("Incorrect PIN - 1 attempt left");
            update();
        }
    }

    void verifyPattern() {
        if (authenticationTimedOut()) return;
        const QByteArray candidate =
            ("P:" + QStringList::fromVector(currentSeq).join(",")).toLatin1();
        const bool valid = secureAuth
                ? authOperation("--verify", candidate + "\n")
                : (currentSeq == patternHash);
        if (authenticationTimedOut()) return;
        if (valid) {
            if (pamPrompt) submitPam(candidate);
            else qApp->quit();
            return;
        }

        ++attemptCount;
        currentSeq.clear();
        if (attemptCount >= 3) {
            activatePinFallback();
            return;
        }
        titleLabel->setText(QString("Incorrect pattern - %1 attempt%2 left")
                            .arg(3 - attemptCount).arg(attemptCount == 2 ? "" : "s"));
        generateGrid();
        update();
    }

    // Cancel button on the pattern/PIN *entry* pages (not shown during
    // first-run setup — see constructor). Exits with a nonzero code so
    // osm-lockscreen's `proc.exitCode() == 0` check treats this exactly
    // like a failed attempt: it slides the lock screen back down instead
    // of unlocking.
    void cancelEntry() {
        if (firstRun && !secureAuth) return;
        denyAuthorization();
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    LockScreen lock;

    // --auth: ON SCREEN WITHOUT THE WINDOW MANAGER — DO NOT REMOVE
    // With --auth this is the unlock prompt for osm-lockscreen and
    // osm-sudo. As an ordinary window it needed Qtile to put it on
    // screen, so when Qtile was stuck after waking from sleep the prompt
    // never appeared and the lockscreen's slider sat stuck at the top.
    // Bypassing the window manager, as osm-lockscreen itself does, puts
    // it on screen straight away and above the lockscreen. Typed PINs
    // still arrive through the keyboard grab in the constructor.
    if (app.arguments().contains(QStringLiteral("--auth")) ||
        app.arguments().contains(QStringLiteral("--pam-prompt"))) {
        lock.setWindowFlags(lock.windowFlags()
                            | Qt::BypassWindowManagerHint
                            | Qt::WindowStaysOnTopHint);
        if (QScreen *scr = QGuiApplication::primaryScreen())
            lock.setGeometry(scr->geometry());
    }

    lock.show();
    lock.raise();
    QTimer::singleShot(0, &lock, [&lock]() { lock.beginAuthenticationCountdown(); });
    return app.exec();
}

