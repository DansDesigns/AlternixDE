#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QGuiApplication>
#include <QClipboard>
#include <QBuffer>
#include <QIODevice>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QVector>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QDesktopServices>
#include <QGraphicsDropShadowEffect>
#include <QDebug>
#include <QFileInfo>
#include <QRegularExpression>

class Circle2Overlay : public QWidget {
public:
    enum SelectionMode { Freehand, Circle, Rectangle };

    Circle2Overlay() {
        setWindowFlags(Qt::FramelessWindowHint |
                       Qt::WindowStaysOnTopHint |
                       Qt::BypassWindowManagerHint |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        setFocusPolicy(Qt::NoFocus);

        screen = QGuiApplication::primaryScreen();
        if (!screen) {
            qFatal("Circle2: no primary screen available");
        }

        screenGeometry = screen->geometry();
        setGeometry(screenGeometry);

        // Keep a clean copy of the desktop before the overlay is displayed.
        // Selection actions are cropped from this image, not from the overlay.
        desktopImage = screen->grabWindow(0).toImage();

        buildMenu();
        update();
    }

    void gpioUp()     { moveSelection(-1); }
    void gpioDown()   { moveSelection(+1); }
    void gpioSelect() { activateCurrent(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // Dim the desktop while keeping it visible.
        p.fillRect(rect(), QColor(0, 0, 0, 112));

        if (selecting || hasSelection) {
            QPainterPath selectedPath = currentSelectionPath();
            if (!selectedPath.isEmpty()) {
                // Clear the dim layer inside the selected shape.
                p.save();
                p.setCompositionMode(QPainter::CompositionMode_Clear);
                p.fillPath(selectedPath, Qt::transparent);
                p.restore();

                QPen pen(QColor(92, 190, 255), 3);
                pen.setCosmetic(true);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                p.drawPath(selectedPath);
            }
        }

        if (!selecting && !hasSelection) {
            p.setPen(QColor(255, 255, 255, 245));
            QFont heading = p.font();
            heading.setPixelSize(qBound(28, screenGeometry.width() / 22, 54));
            heading.setBold(true);
            p.setFont(heading);
            p.drawText(rect().adjusted(30, 0, -30, 0),
                       Qt::AlignCenter, "Circle to...");

            QFont sub = p.font();
            sub.setPixelSize(qBound(15, screenGeometry.width() / 55, 25));
            sub.setBold(false);
            p.setFont(sub);
            p.setPen(QColor(255, 255, 255, 190));
            QRect instruction = rect().adjusted(35, screenGeometry.height() / 2 + 48,
                                                -35, -35);
            p.drawText(instruction, Qt::AlignHCenter | Qt::AlignTop,
                       "Draw around anything on the screen, then choose an action");
        } else if (hasSelection && !selecting) {
            p.setPen(QColor(255, 255, 255, 230));
            QFont f = p.font();
            f.setPixelSize(18);
            p.setFont(f);
            p.drawText(QRect(20, height() - 54, width() - 40, 34),
                       Qt::AlignCenter, "Selection ready  •  choose Search, Copy, Screenshot or Share");
        }
    }

    void keyPressEvent(QKeyEvent *e) override {
        switch (e->key()) {
        case Qt::Key_Up:
        case Qt::Key_VolumeUp:
            moveSelection(-1);
            break;
        case Qt::Key_Down:
        case Qt::Key_VolumeDown:
            moveSelection(+1);
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            activateCurrent();
            break;
        case Qt::Key_Escape:
            if (hasSelection) {
                hasSelection = false;
                selectionPoints.clear();
                update();
            } else {
                close();
            }
            break;
        default:
            QWidget::keyPressEvent(e);
            break;
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        const QPoint pos = e->pos();

        if (panelRectInOverlay().contains(pos)) {
            pressedLabel = labelAt(pos);
            if (pressedLabel) {
                pressedLabel->setStyleSheet(pressedStyle());
                return;
            }
            return;
        }

        if (e->button() != Qt::LeftButton)
            return;

        // Starting a new gesture anywhere outside the pop-out menu.
        hasSelection = false;
        selecting = true;
        selectionStart = pos;
        selectionEnd = pos;
        selectionPoints.clear();
        selectionPoints.append(pos);
        update();
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!selecting)
            return;

        selectionEnd = e->pos();
        if (mode == Freehand)
            selectionPoints.append(selectionEnd);
        update();
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (pressedLabel) {
            QLabel *released = labelAt(e->pos());
            QLabel *wasPressed = pressedLabel;
            pressedLabel = nullptr;
            updateHighlight();
            if (released && released == wasPressed) {
                currentIndex = menuLabels.indexOf(released);
                updateHighlight();
                activateCurrent();
            }
            return;
        }

        if (!selecting || e->button() != Qt::LeftButton)
            return;

        selecting = false;
        selectionEnd = e->pos();
        if (mode == Freehand)
            selectionPoints.append(selectionEnd);

        if (selectionBounds().width() < 5 || selectionBounds().height() < 5) {
            selectionPoints.clear();
            hasSelection = false;
        } else {
            hasSelection = true;
        }
        update();
    }

private:
    QScreen *screen = nullptr;
    QRect screenGeometry;
    QImage desktopImage;
    QWidget *panel = nullptr;
    QVector<QLabel*> menuLabels;
    QLabel *pressedLabel = nullptr;
    int currentIndex = 0;
    SelectionMode mode = Freehand; // Freehand/lasso is the default.
    bool selecting = false;
    bool hasSelection = false;
    QPoint selectionStart;
    QPoint selectionEnd;
    QVector<QPoint> selectionPoints;

    QStringList actionItems = {
        "Search",
        "Copy",
        "Screenshot",
        "Share",
        "Selection: Freehand",
        "Selection: Circle",
        "Selection: Rectangle",
        "Clear selection",
        "Close"
    };

    void buildMenu() {
        auto *root = new QHBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);

        QWidget *rightContainer = new QWidget(this);
        auto *rightLayout = new QVBoxLayout(rightContainer);
        rightLayout->setContentsMargins(0, 0, 0, 0);

        panel = new QWidget(rightContainer);
        panel->setObjectName("circle2Panel");
        panel->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(16, 18, 16, 18);
        layout->setSpacing(7);
        panel->setStyleSheet(
            "#circle2Panel {"
            " background-color: rgba(35, 39, 48, 238);"
            " border: 1px solid rgba(255,255,255,45);"
            " border-top-left-radius: 22px;"
            " border-bottom-left-radius: 22px;"
            " border-top-right-radius: 0px;"
            " border-bottom-right-radius: 0px;"
            "}"
        );

        auto *shadow = new QGraphicsDropShadowEffect(panel);
        shadow->setOffset(0, 0);
        shadow->setBlurRadius(28);
        shadow->setColor(QColor(0, 0, 0, 210));
        panel->setGraphicsEffect(shadow);

        for (const QString &item : actionItems) {
            QLabel *label = new QLabel(item, panel);
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
            label->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            label->setMinimumHeight(34);
            layout->addWidget(label);
            menuLabels.append(label);
        }

        rightLayout->addSpacing(qMax(20, int(screenGeometry.height() * 0.12)));
        rightLayout->addWidget(panel, 0, Qt::AlignRight);
        rightLayout->addStretch();

        root->addStretch();
        root->addWidget(rightContainer);

        updateHighlight();
    }

    QRect panelRectInOverlay() const {
        QRect r = panel->geometry();
        r.moveTopLeft(panel->mapTo(const_cast<Circle2Overlay*>(this), QPoint(0, 0)));
        return r;
    }

    QLabel *labelAt(const QPoint &pos) const {
        for (QLabel *label : menuLabels) {
            QRect r = label->geometry();
            r.moveTopLeft(label->mapTo(const_cast<Circle2Overlay*>(this), QPoint(0, 0)));
            if (r.contains(pos))
                return label;
        }
        return nullptr;
    }

    QString normalStyle() const {
        return "color: white; font-size: 19px; padding: 5px 9px;"
               " background: transparent; border-radius: 9px;";
    }

    QString selectedStyle() const {
        return "color: white; font-size: 21px; font-weight: bold;"
               " padding: 5px 9px; background: #315b78; border-radius: 9px;";
    }

    QString pressedStyle() const {
        return "color: white; font-size: 19px; padding: 5px 9px;"
               " background: #52616d; border-radius: 9px;";
    }

    void updateHighlight() {
        for (int i = 0; i < menuLabels.size(); ++i)
            menuLabels[i]->setStyleSheet(i == currentIndex ? selectedStyle() : normalStyle());
    }

    void moveSelection(int delta) {
        if (menuLabels.isEmpty())
            return;
        currentIndex = (currentIndex + delta + menuLabels.size()) % menuLabels.size();
        updateHighlight();
    }

    void activateCurrent() {
        if (currentIndex < 0 || currentIndex >= actionItems.size())
            return;

        const QString item = actionItems[currentIndex];
        if (item == "Search") {
            doSearch();
        } else if (item == "Copy") {
            doCopy();
        } else if (item == "Screenshot") {
            doScreenshot();
        } else if (item == "Share") {
            doShare();
        } else if (item == "Selection: Freehand") {
            setMode(Freehand);
        } else if (item == "Selection: Circle") {
            setMode(Circle);
        } else if (item == "Selection: Rectangle") {
            setMode(Rectangle);
        } else if (item == "Clear selection") {
            hasSelection = false;
            selectionPoints.clear();
            update();
        } else if (item == "Close") {
            close();
        }
    }

    void setMode(SelectionMode newMode) {
        mode = newMode;
        hasSelection = false;
        selectionPoints.clear();
        update();
        updateHighlight();
    }

    QRect selectionBounds() const {
        if (mode == Freehand && !selectionPoints.isEmpty()) {
            int minX = selectionPoints.first().x();
            int maxX = minX;
            int minY = selectionPoints.first().y();
            int maxY = minY;
            for (const QPoint &pt : selectionPoints) {
                minX = qMin(minX, pt.x());
                maxX = qMax(maxX, pt.x());
                minY = qMin(minY, pt.y());
                maxY = qMax(maxY, pt.y());
            }
            return QRect(QPoint(minX, minY), QPoint(maxX, maxY)).normalized();
        }
        return QRect(selectionStart, selectionEnd).normalized();
    }

    QPainterPath currentSelectionPath() const {
        QPainterPath path;
        if (mode == Rectangle) {
            path.addRect(QRectF(selectionStart, selectionEnd).normalized());
        } else if (mode == Circle) {
            path.addEllipse(QRectF(selectionStart, selectionEnd).normalized());
        } else if (!selectionPoints.isEmpty()) {
            path.moveTo(selectionPoints.first());
            for (int i = 1; i < selectionPoints.size(); ++i)
                path.lineTo(selectionPoints[i]);
            path.closeSubpath();
        }
        return path;
    }

    QImage selectedImage() const {
        if (desktopImage.isNull() || !hasSelection)
            return QImage();

        QRect bounds = selectionBounds().intersected(rect());
        if (bounds.isEmpty())
            return QImage();

        QImage result(bounds.size(), QImage::Format_ARGB32);
        result.fill(Qt::transparent);

        QPainter painter(&result);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath mask;
        QPainterPath screenPath = currentSelectionPath();
        QTransform translate;
        translate.translate(-bounds.x(), -bounds.y());
        mask = translate.map(screenPath);
        painter.setClipPath(mask);
        painter.drawImage(QPoint(-bounds.x(), -bounds.y()), desktopImage);
        painter.end();
        return result;
    }

    QString outputDirectory() const {
        QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (dir.isEmpty())
            dir = QDir::homePath();
        QDir().mkpath(dir);
        return dir;
    }

    QString saveSelection() {
        QImage image = selectedImage();
        if (image.isNull())
            return QString();

        const QString name = "circle2-" +
            QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".png";
        const QString path = QDir(outputDirectory()).filePath(name);
        if (!image.save(path, "PNG"))
            return QString();
        return path;
    }

    QString extractText(const QImage &image) const {
        if (image.isNull())
            return QString();

        // Tesseract is optional. If it is not installed, image-based actions
        // still work and Copy falls back to copying the selected image.
        QProcess process;
        process.start("tesseract",
                      QStringList() << "stdin" << "stdout" << "-l" << "eng");
        if (!process.waitForStarted(1200))
            return QString();

        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        process.write(png);
        process.closeWriteChannel();

        if (!process.waitForFinished(10000)) {
            process.kill();
            process.waitForFinished();
            return QString();
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            return QString();

        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }

    bool requireSelection() {
        if (hasSelection)
            return true;
        showToast("Draw a selection first");
        return false;
    }

    void doSearch() {
        if (!requireSelection())
            return;

        const QString text = extractText(selectedImage());
        if (!text.isEmpty()) {
            const QUrl url("https://www.google.com/search?q=" +
                           QString::fromLatin1(QUrl::toPercentEncoding(text)));
            QDesktopServices::openUrl(url);
            showToast("Searching recognised text");
        } else {
            const QString path = saveSelection();
            if (!path.isEmpty()) {
                QDesktopServices::openUrl(QUrl("https://lens.google.com/"));
                showToast("Image saved; open Google Lens to upload it");
            } else {
                showToast("Could not save the selected image");
            }
        }
    }

    void doCopy() {
        if (!requireSelection())
            return;

        QImage image = selectedImage();
        const QString text = extractText(image);
        if (!text.isEmpty()) {
            QGuiApplication::clipboard()->setText(text);
            showToast("Recognised text copied");
        } else if (!image.isNull()) {
            QGuiApplication::clipboard()->setImage(image);
            showToast("Selected image copied");
        } else {
            showToast("Nothing could be copied");
        }
    }

    void doScreenshot() {
        if (!requireSelection())
            return;

        const QString path = saveSelection();
        if (path.isEmpty()) {
            showToast("Could not save screenshot");
            return;
        }
        showToast("Selection saved in Pictures");
    }

    void doShare() {
        if (!requireSelection())
            return;

        const QString path = saveSelection();
        if (path.isEmpty()) {
            showToast("Could not prepare image for sharing");
            return;
        }

        // Linux desktops do not provide one universal share sheet. Open the
        // saved image with the desktop's configured handler.
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        QGuiApplication::clipboard()->setText(path);
        showToast("Image saved; file path copied for sharing");
    }

    void showToast(const QString &message) {
        QWidget *toast = new QWidget(nullptr);
        toast->setWindowFlags(Qt::FramelessWindowHint |
                              Qt::WindowStaysOnTopHint |
                              Qt::Tool);
        toast->setAttribute(Qt::WA_TranslucentBackground);

        const QRect g = screenGeometry;
        toast->setGeometry(g.x(), g.y() + 12, g.width(), 54);

        QWidget *box = new QWidget(toast);
        box->setGeometry(20, 0, g.width() - 40, 54);
        box->setStyleSheet("background-color: #252a32; border-radius: 12px;");

        auto *layout = new QHBoxLayout(box);
        layout->setContentsMargins(16, 5, 16, 5);
        QLabel *label = new QLabel(message, box);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet("color: white; font-size: 17px;");
        layout->addWidget(label);

        toast->show();
        toast->raise();
        QTimer::singleShot(2200, toast, [toast]() {
            toast->close();
            toast->deleteLater();
        });
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Circle2");

    Circle2Overlay overlay;
    const QRect geometry = QGuiApplication::primaryScreen()->geometry();
    overlay.setGeometry(geometry);
    overlay.show(); // Like osm-rocker: normal screen-sized window, not showFullScreen().
    overlay.raise();
    overlay.activateWindow();
    overlay.setFocus();

    return app.exec();
}
