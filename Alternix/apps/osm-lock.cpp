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
#include <QtMath>
#include <QCloseEvent>
#include <QKeyEvent>

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

    QVector<QString> currentSeq;
    int attemptCount = 0;

    bool pinModeActive = false;
    bool pinSetupMode = false;
    bool pinSetupConfirm = false;
    QString pinInput;
    QString pinSetupFirst;

    QLabel *titleLabel = nullptr;
    QPushButton *securityToggle = nullptr;
    QPushButton *cancelButton = nullptr;
    QWidget *pinWidget = nullptr;

    bool firstRun = false;
    bool enhancedLocked = false;

public:
    LockScreen() {
        setWindowTitle("OSM Lock");
        setWindowFlag(Qt::FramelessWindowHint);
        setWindowState(Qt::WindowFullScreen);
        setStyleSheet("background-color:#282828; color:white;");

        titleLabel = new QLabel(this);
        titleLabel->setStyleSheet("font-size:20px; color:white;");
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setGeometry(0, 5, width(), 30);

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

        loadConfig();

        // Cancel only makes sense once a lock method already exists —
        // during first-run setup you have to finish creating one.
        cancelButton->setVisible(!firstRun);

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

        if (firstRun)
            titleLabel->setText("Please select your pattern of shapes");
        else
            titleLabel->setText("Enter your pattern");

        generateGrid();
    }

protected:
    void resizeEvent(QResizeEvent *) override {
        securityToggle->setGeometry(10, 5, 220, 30);
        if (cancelButton)
            cancelButton->setGeometry(width() - 130, 5, 120, 30);

    // estimate grid area (lower half of screen)
        int gridTop = height() * 0.5;  // start of grid roughly mid-screen
        int desiredY = gridTop - 120;  // move label about 120px above grid
        if (desiredY < 40) desiredY = 40; // clamp minimum

        titleLabel->setGeometry(0, desiredY, width(), 40);

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

private:
    // ---------- CONFIG ----------
    void loadConfig() {
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

    void saveConfig() {
        QString path = QDir::homePath() + "/.config/Alternix/.osm_lockdata";
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            QStringList plist = QStringList::fromVector(patternHash);
            out << "pattern=" << plist.join(",") << "\n";
            out << "password=" << passwordHash << "\n";
            out << "enhanced=" << (enhancedSecurity ? "1" : "0") << "\n";
            f.close();
        }
    }

    void saveConfigEnhancedOnly() {
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
                    saveConfig();
                    qApp->quit();
                } else {
                    pinInput.clear();
                    pinSetupFirst.clear();
                    pinSetupConfirm = false;
                    titleLabel->setText("Set your Fallback PIN");
                    update();
                }
            }
        } else {
            if (sha256(pinInput) == passwordHash) qApp->quit();
            else { pinInput.clear(); update(); }
        }
    }

    void verifyPattern() {
        bool ok = (currentSeq == patternHash);
        if (ok) qApp->quit();
        else {
            attemptCount++;
            currentSeq.clear();
            if (attemptCount < 3) generateGrid();
            update();
            if (attemptCount >= 3) {
                pinModeActive = true;
                pinSetupMode = false;
                pinSetupConfirm = false;
                pinInput.clear();
                titleLabel->setText("Enter Fallback PIN");
                buildPinPad();
                update();
            }
        }
    }

    // Cancel button on the pattern/PIN *entry* pages (not shown during
    // first-run setup — see constructor). Exits with a nonzero code so
    // osm-lockscreen's `proc.exitCode() == 0` check treats this exactly
    // like a failed attempt: it slides the lock screen back down instead
    // of unlocking.
    void cancelEntry() {
        if (firstRun) return;
        qApp->exit(1);
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    LockScreen lock;
    lock.show();
    return app.exec();
}

