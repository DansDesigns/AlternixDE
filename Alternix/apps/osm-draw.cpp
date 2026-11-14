#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QButtonGroup>
#include <QSlider>
#include <QLabel>
#include <QStatusBar>
#include <QFileDialog>
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
#include <QImageReader>
#include <QMessageBox>
#include <QFileInfo>
#include <QTimer>
#include <QSignalBlocker>
#include <QStack>
#include <QResizeEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QDir>
#include <QComboBox>
#include <QInputDialog>
#include <QVariant>
#include <QtMath>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QFrame>
#include <QShortcut>
#include <QKeySequence>
#include <QBitArray>
#include <QVector>
#include <QPainterPath>
#include <QIcon>
#include <QPixmap>
#include <QSizePolicy>
#include <functional>
#include <cmath>


// ─────────────────────────────────────────────
// Live brush-size preview
// ─────────────────────────────────────────────
class PenSizePreview : public QWidget {
public:
    explicit PenSizePreview(QWidget *parent = nullptr)
        : QWidget(parent), m_penSize(5), m_penColor(Qt::black) {
        setFixedSize(60, 60);
        setToolTip("Actual pen diameter at 100% zoom");
        setAccessibleName("Pen size preview");
    }

    void setPenSize(int size) {
        m_penSize = qBound(1, size, 50);
        update();
    }

    void setPenColor(const QColor &color) {
        m_penColor = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#5c5c5c"), 1));
        painter.setBrush(QColor("#f0f0f0"));
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 8, 8);

        const qreal diameter = qreal(m_penSize);
        const QPointF center(width() / 2.0, height() / 2.0);
        painter.setPen(QPen(QColor("#878787"), 0.7));
        painter.setBrush(m_penColor);
        painter.drawEllipse(QRectF(center.x() - diameter / 2.0,
                                  center.y() - diameter / 2.0,
                                  diameter, diameter));
    }

private:
    int m_penSize;
    QColor m_penColor;
};


// A checkerboard is used in opacity swatches and when the page is transparent.
// This is drawn rather than loaded from an external asset.
static void drawCheckerboard(QPainter &p, const QRect &area, int tile = 10) {
    p.fillRect(area, QColor("#ededed"));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#bcbcbc"));
    const int firstY = area.top() / tile;
    const int lastY = area.bottom() / tile;
    const int firstX = area.left() / tile;
    const int lastX = area.right() / tile;
    for (int row = firstY; row <= lastY; ++row) {
        for (int col = firstX; col <= lastX; ++col) {
            if ((row + col) & 1)
                p.drawRect(col * tile, row * tile, tile, tile);
        }
    }
}

static QIcon colorSwatchIcon(const QColor &color, int side = 28) {
    QPixmap pixmap(side, side);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRect square(1, 1, side - 2, side - 2);
    p.save();
    QPainterPath clip;
    clip.addRoundedRect(QRectF(square), 5, 5);
    p.setClipPath(clip);
    drawCheckerboard(p, square, 7);
    p.fillRect(square, color);
    p.restore();
    p.setPen(QPen(QColor("#e0e0e0"), 1.3));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(square), 5, 5);
    return QIcon(pixmap);
}

// Radial, segmented HSV palette. The outer rings are saturated, inner rings
// are pastel; the centre shows the currently selected colour over a checker.
// Brightness and alpha have independent sliders in the dialog below.
class RadialColorWheel : public QWidget {
public:
    explicit RadialColorWheel(const QColor &initial, QWidget *parent = nullptr)
        : QWidget(parent), m_color(initial) {
        setMinimumSize(260, 260);
        setMouseTracking(true);
        setAccessibleName("Radial colour wheel");
    }

    QSize sizeHint() const override { return QSize(310, 310); }
    void setColor(const QColor &color) { m_color = color; update(); }
    std::function<void(const QColor &)> onSelectionChanged;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPointF centre(width() / 2.0, height() / 2.0);
        const qreal outer = qMin(width(), height()) / 2.0 - 9;
        const qreal inner = 35.0;
        const int rings = 5;
        const int sectors = 12;
        const int saturationLevels[rings] = {35, 85, 135, 195, 255};

        for (int ring = 0; ring < rings; ++ring) {
            const qreal r1 = inner + (outer - inner) * ring / rings;
            const qreal r2 = inner + (outer - inner) * (ring + 1) / rings;
            const QRectF rectOuter(centre.x() - r2, centre.y() - r2, r2 * 2, r2 * 2);
            const QRectF rectInner(centre.x() - r1, centre.y() - r1, r1 * 2, r1 * 2);
            for (int sector = 0; sector < sectors; ++sector) {
                const qreal start = 90.0 - sector * (360.0 / sectors);
                QPainterPath wedge;
                wedge.arcMoveTo(rectOuter, start);
                wedge.arcTo(rectOuter, start, -360.0 / sectors);
                wedge.arcTo(rectInner, start - 360.0 / sectors, 360.0 / sectors);
                wedge.closeSubpath();
                const int hue = (60 + sector * 30) % 360;
                p.setBrush(QColor::fromHsv(hue, saturationLevels[ring], 255));
                p.setPen(QPen(QColor("#24272b"), 1.6));
                p.drawPath(wedge);
            }
        }

        // Neutral centre / live colour sample, including its actual alpha.
        const QRectF centerRect(centre.x() - inner + 2, centre.y() - inner + 2,
                                (inner - 2) * 2, (inner - 2) * 2);
        QPainterPath centerClip;
        centerClip.addEllipse(centerRect);
        p.save();
        p.setClipPath(centerClip);
        drawCheckerboard(p, centerRect.toAlignedRect(), 8);
        p.fillRect(centerRect, m_color);
        p.restore();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor("#ececec"), 2.0));
        p.drawEllipse(centerRect);

        // Position marker at the selected hue/saturation, regardless of value.
        const int sat = m_color.hsvSaturation();
        if (sat > 0 && m_color.hsvHue() >= 0) {
            const qreal clockwise = (m_color.hsvHue() - 60 + 360) % 360;
            const qreal radians = qDegreesToRadians(clockwise);
            const qreal radius = inner + (outer - inner) * sat / 255.0;
            const QPointF marker(centre.x() + qSin(radians) * radius,
                                 centre.y() - qCos(radians) * radius);
            p.setPen(QPen(QColor("#181818"), 4));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(marker, 7.0, 7.0);
            p.setPen(QPen(Qt::white, 2.4));
            p.drawEllipse(marker, 7.0, 7.0);
        }
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton)
            choose(e->pos());
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        if (e->buttons() & Qt::LeftButton)
            choose(e->pos());
    }

private:
    QColor m_color;
    void choose(const QPoint &pos) {
        const QPointF delta = QPointF(pos) - QPointF(width() / 2.0, height() / 2.0);
        const qreal distance = std::hypot(delta.x(), delta.y());
        const qreal outer = qMin(width(), height()) / 2.0 - 9;
        const qreal inner = 35.0;
        if (distance > outer)
            return;

        const int level[5] = {35, 85, 135, 195, 255};
        int hue = m_color.hsvHue() < 0 ? 60 : m_color.hsvHue();
        int saturation = 0;
        if (distance >= inner) {
            qreal clockwise = qRadiansToDegrees(std::atan2(delta.x(), -delta.y()));
            if (clockwise < 0)
                clockwise += 360.0;
            const int sector = qBound(0, int(clockwise / 30.0), 11);
            hue = (60 + sector * 30) % 360;
            const int ring = qBound(0, int((distance - inner) * 5.0 / (outer - inner)), 4);
            saturation = level[ring];
        }
        // Choosing a hue from an initially black stroke must be able to produce
        // a visible colour rather than silently remaining at zero brightness.
        const int value = m_color.value() == 0 ? 255 : m_color.value();
        m_color.setHsv(hue, saturation, value, m_color.alpha());
        update();
        if (onSelectionChanged)
            onSelectionChanged(m_color);
    }
};

class RadialColorDialog : public QDialog {
public:
    RadialColorDialog(const QString &title, const QColor &initial, QWidget *parent = nullptr)
        : QDialog(parent), m_color(initial.isValid() ? initial : QColor(Qt::black)) {
        setWindowTitle(title);
        setModal(true);
        setStyleSheet(
            "QDialog { background-color:#222428; color:#f6f6f6; }"
            "QLabel { color:#efefef; font-size:14px; }"
            "QSlider::groove:horizontal { height:8px; background:#50545c; border-radius:4px; }"
            "QSlider::handle:horizontal { background:#ffffff; width:20px; height:20px;"
            " margin:-6px 0; border-radius:10px; }"
            "QPushButton { background:#353a41; border:1px solid #626971;"
            " color:white; border-radius:6px; padding:8px 15px; }"
            "QPushButton:hover { background:#485260; }"
        );
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(17, 12, 17, 14);
        layout->setSpacing(12);

        m_wheel = new RadialColorWheel(m_color, this);
        layout->addWidget(m_wheel, 0, Qt::AlignCenter);
        m_wheel->onSelectionChanged = [this](const QColor &c) { setSelected(c); };

        auto addSlider = [this, layout](const QString &name, QSlider *&slider,
                                        QLabel *&valueLabel, int value) {
            auto *line = new QHBoxLayout();
            QLabel *nameLabel = new QLabel(name, this);
            nameLabel->setFixedWidth(76);
            line->addWidget(nameLabel);
            slider = new QSlider(Qt::Horizontal, this);
            slider->setRange(0, 100);
            slider->setValue(value);
            line->addWidget(slider, 1);
            valueLabel = new QLabel(QString::number(value) + "%", this);
            valueLabel->setMinimumWidth(48);
            valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            line->addWidget(valueLabel);
            layout->addLayout(line);
        };
        addSlider("Brightness", m_valueSlider, m_valueLabel,
                  qRound(m_color.valueF() * 100));
        addSlider("Opacity", m_alphaSlider, m_alphaLabel,
                  qRound(m_color.alphaF() * 100));

        connect(m_valueSlider, &QSlider::valueChanged, this, [this](int value) {
            QColor next;
            const int hue = m_color.hsvHue() < 0 ? 60 : m_color.hsvHue();
            next.setHsv(hue, m_color.hsvSaturation(), qRound(value * 2.55), m_color.alpha());
            setSelected(next);
        });
        connect(m_alphaSlider, &QSlider::valueChanged, this, [this](int percent) {
            QColor next = m_color;
            next.setAlpha(qRound(percent * 2.55));
            setSelected(next);
        });

        auto *sampleRow = new QHBoxLayout();
        m_sample = new QLabel(this);
        m_sample->setFixedSize(68, 36);
        sampleRow->addWidget(m_sample);
        m_colorDetails = new QLabel(this);
        sampleRow->addWidget(m_colorDetails, 1);
        layout->addLayout(sampleRow);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText("Use colour");
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        setSelected(m_color);
    }

    QColor selectedColor() const { return m_color; }

private:
    QColor m_color;
    RadialColorWheel *m_wheel = nullptr;
    QSlider *m_valueSlider = nullptr;
    QSlider *m_alphaSlider = nullptr;
    QLabel *m_valueLabel = nullptr;
    QLabel *m_alphaLabel = nullptr;
    QLabel *m_sample = nullptr;
    QLabel *m_colorDetails = nullptr;

    void setSelected(const QColor &c) {
        m_color = c;
        m_wheel->setColor(c);
        const int brightness = qRound(c.valueF() * 100);
        const int opacity = qRound(c.alphaF() * 100);
        {
            QSignalBlocker block(m_valueSlider);
            m_valueSlider->setValue(brightness);
        }
        {
            QSignalBlocker block(m_alphaSlider);
            m_alphaSlider->setValue(opacity);
        }
        m_valueLabel->setText(QString::number(brightness) + "%");
        m_alphaLabel->setText(QString::number(opacity) + "%");
        QPixmap swatch(m_sample->size());
        swatch.fill(Qt::transparent);
        QPainter p(&swatch);
        const QRect box = swatch.rect().adjusted(1, 1, -2, -2);
        drawCheckerboard(p, box, 9);
        p.fillRect(box, c);
        p.setPen(QColor("#ededed"));
        p.setBrush(Qt::NoBrush);
        p.drawRect(box);
        p.end();
        m_sample->setPixmap(swatch);
        m_colorDetails->setText(QString("R %1  G %2  B %3  A %4")
                               .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()));
    }
};

class FloatingToolbar : public QFrame {
public:
    explicit FloatingToolbar(QWidget *parent = nullptr) : QFrame(parent) {
        setObjectName("floatingToolbar");
        setAttribute(Qt::WA_StyledBackground, true);
        setStyleSheet("QFrame#floatingToolbar { background:rgba(25,27,32,239);"
                      " border:1px solid #717985; border-radius:11px; }"
                      "QPushButton {background:#363b42; color:#ffffff; border:1px solid #525963;"
                      " border-radius:5px; font-size:19px; padding:0px;}"
                      "QPushButton:hover {background:#505c68;}"
                      "QPushButton:checked {background:#305c80; border:2px solid #91d0fa;}"
                      "QLabel { color:#d6dce5; font-size:16px; }");
    }
    void setDragHandle(QWidget *handle) {
        m_handle = handle;
        m_handle->setCursor(Qt::SizeAllCursor);
        m_handle->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_handle) {
            if (event->type() == QEvent::MouseButtonPress) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                if (mouse->button() == Qt::LeftButton) {
                    m_dragging = true;
                    m_dragStart = mouse->globalPos();
                    m_initialPos = pos();
                    return true;
                }
            } else if (event->type() == QEvent::MouseMove && m_dragging) {
                auto *mouse = static_cast<QMouseEvent *>(event);
                QPoint next = m_initialPos + mouse->globalPos() - m_dragStart;
                if (parentWidget()) {
                    next.setX(qBound(0, next.x(), qMax(0, parentWidget()->width() - width())));
                    next.setY(qBound(0, next.y(), qMax(0, parentWidget()->height() - height())));
                }
                move(next);
                return true;
            } else if (event->type() == QEvent::MouseButtonRelease && m_dragging) {
                m_dragging = false;
                return true;
            }
        }
        return QFrame::eventFilter(watched, event);
    }

private:
    QWidget *m_handle = nullptr;
    bool m_dragging = false;
    QPoint m_dragStart;
    QPoint m_initialPos;
};

// ─────────────────────────────────────────────
// Drawing Canvas
// ─────────────────────────────────────────────

class DrawingCanvas : public QWidget {
public:
    enum class Tool {
        None,
        Pen,
        Eraser,
        Line,
        Rect,
        Ellipse,
        Fill,
        Grab
    };

    explicit DrawingCanvas(QWidget *parent = nullptr)
        : QWidget(parent),
          m_tool(Tool::Pen),
          m_penColor(Qt::black),
          m_fillColor(Qt::red),
          m_bgColor(Qt::white),
          m_penSize(5),
          m_drawing(false),
          m_showPreview(false),
          m_zoomFactor(1.0),
          m_panActive(false),
          m_viewOffset(0, 0),
          m_viewInitialized(false)
    {
        // Optimised for incremental painting
        setAttribute(Qt::WA_StaticContents, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setMouseTracking(true);

        // Default canvas size
        QSize initialSize(1280, 720);
        m_image = QImage(initialSize, QImage::Format_ARGB32_Premultiplied);
        m_image.fill(Qt::transparent);
    }

    // ───── public API ─────

    void setTool(Tool t) {
        m_tool = t;
        m_showPreview = false;
        if (m_tool == Tool::Grab)
            setCursor(Qt::OpenHandCursor);
        else
            unsetCursor();
        update();
    }

    void setPenColor(const QColor &c) {
        m_penColor = c;
    }

    void setFillColor(const QColor &c) {
        m_fillColor = c;
    }

    void setTransparentBackground(bool enabled) {
        m_transparentBackground = enabled;
        update();
    }

    void setPenSize(int s) {
        m_penSize = qMax(1, s);
    }

    void clearCanvas() {
        pushUndo();
        m_redoStack.clear();
        if (!m_image.isNull()) {
            m_image.fill(Qt::transparent);
        }
        update();
    }

    // Import a picture onto the canvas at its native pixel dimensions.
    // A new picture is a new document, not an undoable paint stroke.
    bool loadImage(const QImage &source) {
        if (source.isNull())
            return false;
        QImage converted = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (converted.isNull())
            return false;

        m_drawing = false;
        m_showPreview = false;
        m_panActive = false;
        m_undoStack.clear();
        m_redoStack.clear();
        m_image = converted;
        m_zoomFactor = 1.0;
        centerCanvas();
        update();
        return true;
    }

    bool savePng(const QString &path) {
        if (path.isEmpty() || m_image.isNull())
            return false;

        // Keep actual pixel alpha whenever the transparent-background option
        // is on. Otherwise composite on white to match the visible page.
        if (m_transparentBackground)
            return m_image.save(path, "PNG");
        QImage out(m_image.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(m_bgColor);
        QPainter p(&out);
        p.drawImage(0, 0, m_image);
        p.end();
        return out.save(path, "PNG");
    }

    void setZoom(double factor) {
        m_zoomFactor = qBound(0.05, factor, 4.0);
        update();
    }

    double zoom() const { return m_zoomFactor; }

    void fitToWidget() {
        if (m_image.isNull())
            return;

        QSizeF imgSize = m_image.size();
        QSizeF widgetSize = size();

        if (imgSize.isEmpty() || widgetSize.isEmpty())
            return;

        double sx = widgetSize.width()  / imgSize.width();
        double sy = widgetSize.height() / imgSize.height();
        double factor = qMin(sx, sy);

        factor = qBound(0.05, factor, 4.0);
        m_zoomFactor = factor;

        centerCanvas();
        update();
    }

    void resizeCanvasTo(const QSize &requestedSize) {
        // Allow larger canvases again, but add a sanity cap
        const int MAX_CANVAS_W = 4096;
        const int MAX_CANVAS_H = 4096;

        int w = qBound(1, requestedSize.width(),  MAX_CANVAS_W);
        int h = qBound(1, requestedSize.height(), MAX_CANVAS_H);
        QSize newSize(w, h);

        pushUndo();
        m_redoStack.clear();

        QImage newImage(newSize, QImage::Format_ARGB32_Premultiplied);
        newImage.fill(Qt::transparent);

        if (!m_image.isNull()) {
            QPainter p(&newImage);
            p.drawImage(0, 0, m_image);
            p.end();
        }

        m_image = newImage;
        centerCanvas();
        update();
    }

    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

    void undo() {
        if (!canUndo())
            return;
        m_redoStack.push(m_image);
        m_image = m_undoStack.pop();
        update();
    }

    void redo() {
        if (!canRedo())
            return;
        m_undoStack.push(m_image);
        m_image = m_redoStack.pop();
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter p(this);

        // Fill widget background with app dark colour
        p.fillRect(event->rect(), QColor("#282828"));

        if (m_image.isNull())
            return;

        p.save();

        // Apply pan (widget space) then zoom
        p.translate(m_viewOffset);
        p.scale(m_zoomFactor, m_zoomFactor);

        // Show transparent pixels against a checkerboard when enabled.
        QRect imageRect(QPoint(0, 0), m_image.size());
        if (m_transparentBackground) {
            p.save();
            p.setClipRect(imageRect);
            drawCheckerboard(p, imageRect, 12);
            p.restore();
        } else {
            p.fillRect(imageRect, m_bgColor);
        }
        p.drawImage(0, 0, m_image);

        // Shape preview
        if (m_showPreview && (m_tool == Tool::Line || m_tool == Tool::Rect || m_tool == Tool::Ellipse)) {
            QPen pen(m_penColor, m_penSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);

            QRect r = rectFromPoints(m_startPoint, m_currentPoint);

            if (m_tool == Tool::Line) {
                p.drawLine(m_startPoint, m_currentPoint);
            } else if (m_tool == Tool::Rect) {
                p.setBrush(m_fillColor);
                p.drawRect(r);
            } else if (m_tool == Tool::Ellipse) {
                p.setBrush(m_fillColor);
                p.drawEllipse(r);
            }
        }

        p.restore();
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);

        if (event->size().isEmpty())
            return;

        if (!m_viewInitialized) {
            // Centre the drawing page once the canvas has its real layout size.
            centerCanvas();
            m_viewInitialized = true;
        } else if (event->oldSize().isValid()) {
            // Keep the page centred relative to a resized window, while
            // preserving any offset the user introduced with the Grab tool.
            const QSize delta = event->size() - event->oldSize();
            m_viewOffset += QPointF(delta.width() / 2.0, delta.height() / 2.0);
        }

        update();
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton)
            return;

        if (m_tool == Tool::Grab) {
            m_panActive = true;
            m_panLastPos = e->pos();
            setCursor(Qt::ClosedHandCursor);
            return;
        }

        if (m_image.isNull())
            return;

        QPoint imgPos = widgetToImage(e->pos());
        clampPointToImage(imgPos);

        m_drawing = true;
        m_startPoint = imgPos;
        m_lastPoint = imgPos;
        m_currentPoint = imgPos;
        m_showPreview = false;

        // Fill finds and validates the entire bounded area first, then pushes
        // a single undo snapshot only if it can actually apply a fill.
        if (m_tool == Tool::Fill) {
            if (floodFill(imgPos, m_fillColor))
                update();
            m_drawing = false;
            return;
        }

        if (m_tool != Tool::None) {
            pushUndo();
            m_redoStack.clear();
        }

        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) {
            drawLineOnImage(m_lastPoint, m_lastPoint); // dot
        } else if (m_tool == Tool::Line ||
                   m_tool == Tool::Rect ||
                   m_tool == Tool::Ellipse) {
            m_showPreview = true;
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (m_tool == Tool::Grab && m_panActive) {
            QPoint delta = e->pos() - m_panLastPos;
            m_panLastPos = e->pos();
            m_viewOffset += delta;

            update();
            return;
        }

        if (!m_drawing || m_image.isNull())
            return;

        QPoint imgPos = widgetToImage(e->pos());
        clampPointToImage(imgPos);

        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) {
            drawLineOnImage(m_lastPoint, imgPos);
            m_lastPoint = imgPos;
        } else if (m_tool == Tool::Line ||
                   m_tool == Tool::Rect ||
                   m_tool == Tool::Ellipse) {
            m_currentPoint = imgPos;
            m_showPreview = true;
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton)
            return;

        if (m_tool == Tool::Grab && m_panActive) {
            m_panActive = false;
            setCursor(Qt::OpenHandCursor);
            return;
        }

        if (!m_drawing || m_image.isNull())
            return;

        QPoint imgPos = widgetToImage(e->pos());
        clampPointToImage(imgPos);

        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) {
            drawLineOnImage(m_lastPoint, imgPos);
        } else if (m_tool == Tool::Line ||
                   m_tool == Tool::Rect ||
                   m_tool == Tool::Ellipse) {

            QPainter p(&m_image);
            QPen pen(m_penColor, m_penSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);

            QRect r = rectFromPoints(m_startPoint, imgPos);

            if (m_tool == Tool::Line) {
                p.drawLine(m_startPoint, imgPos);
            } else if (m_tool == Tool::Rect) {
                p.setBrush(m_fillColor);
                p.drawRect(r);
            } else if (m_tool == Tool::Ellipse) {
                p.setBrush(m_fillColor);
                p.drawEllipse(r);
            }
            p.end();

            update();
        }

        m_drawing = false;
        m_showPreview = false;
    }

private:
    Tool   m_tool;
    QColor m_penColor;
    QColor m_fillColor;
    QColor m_bgColor;
    bool   m_transparentBackground = false;
    int    m_penSize;

    QImage m_image;
    bool   m_drawing;
    bool   m_showPreview;

    double m_zoomFactor;

    QPoint m_startPoint;
    QPoint m_lastPoint;
    QPoint m_currentPoint;

    bool    m_panActive;
    QPoint  m_panLastPos;
    QPointF m_viewOffset;
    bool    m_viewInitialized;

    QStack<QImage> m_undoStack;
    QStack<QImage> m_redoStack;

    void pushUndo() {
        if (!m_image.isNull()) {
            m_undoStack.push(m_image);  // implicit sharing
        }
    }

    void clampPointToImage(QPoint &pt) {
        pt.setX(qBound(0, pt.x(), m_image.width() - 1));
        pt.setY(qBound(0, pt.y(), m_image.height() - 1));
    }

    QRect rectFromPoints(const QPoint &a, const QPoint &b) const {
        return QRect(a, b).normalized();
    }

    QPoint widgetToImage(const QPoint &widgetPos) const {
        if (m_zoomFactor <= 0.0)
            return widgetPos;

        QPointF pf = (widgetPos - m_viewOffset) / m_zoomFactor;
        return QPoint(int(pf.x()), int(pf.y()));
    }

    QRect imageRectToWidgetRect(const QRect &imgRect) const {
        QPointF topLeft = imgRect.topLeft() * m_zoomFactor + m_viewOffset;
        QSizeF size = imgRect.size() * m_zoomFactor;
        return QRect(topLeft.toPoint(), size.toSize());
    }

    void drawLineOnImage(const QPoint &from, const QPoint &to) {
        QPainter p(&m_image);
        // Eraser removes real alpha instead of merely overpainting white.
        if (m_tool == Tool::Eraser)
            p.setCompositionMode(QPainter::CompositionMode_Clear);
        QPen pen(m_penColor, m_penSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.drawLine(from, to);
        p.end();

        QRect dirtyImgRect = rectFromPoints(from, to)
                                 .adjusted(-m_penSize, -m_penSize,
                                           m_penSize, m_penSize);
        QRect dirtyWidgetRect = imageRectToWidgetRect(dirtyImgRect);
        update(dirtyWidgetRect);
    }

    bool floodFill(const QPoint &start, const QColor &newColor) {
        const int w = m_image.width();
        const int h = m_image.height();
        if (!QRect(0, 0, w, h).contains(start))
            return false;

        const QRgb target = m_image.pixel(start);
        const QRgb replacement = qPremultiply(newColor.rgba());
        if (target == replacement)
            return false;

        // Treat small differences from anti-aliasing as part of the same area,
        // while keeping actual outlines/colour boundaries impermeable.
        const int tolerance = 12;
        auto matches = [&](int x, int y) -> bool {
            const QRgb sample = reinterpret_cast<const QRgb *>(m_image.constScanLine(y))[x];
            return qAbs(qAlpha(sample) - qAlpha(target)) <= tolerance &&
                   qAbs(qRed(sample) - qRed(target)) <= tolerance &&
                   qAbs(qGreen(sample) - qGreen(target)) <= tolerance &&
                   qAbs(qBlue(sample) - qBlue(target)) <= tolerance;
        };

        // Collect horizontal runs without modifying the original image. If the
        // selected region reaches ANY page edge, it is uncontained: do nothing.
        // This prevents a click on an unclosed shape from painting the page.
        QBitArray visited(w * h, false);
        QVector<int> pending;
        QVector<int> region;
        pending.reserve(1024);
        region.reserve(4096);
        pending.append(start.y() * w + start.x());
        while (!pending.isEmpty()) {
            const int index = pending.takeLast();
            const int x = index % w;
            const int y = index / w;
            if (visited.testBit(index) || !matches(x, y))
                continue;

            int left = x;
            while (left > 0 && !visited.testBit(y * w + left - 1) &&
                   matches(left - 1, y))
                --left;
            for (int px = left; px < w; ++px) {
                const int at = y * w + px;
                if (visited.testBit(at) || !matches(px, y))
                    break;
                // No image modification or undo record has occurred yet.
                if (px == 0 || px == w - 1 || y == 0 || y == h - 1)
                    return false;
                visited.setBit(at);
                region.append(at);
                const int above = at - w;
                const int below = at + w;
                if (!visited.testBit(above) && matches(px, y - 1))
                    pending.append(above);
                if (!visited.testBit(below) && matches(px, y + 1))
                    pending.append(below);
            }
        }

        if (region.isEmpty())
            return false;
        pushUndo();
        m_redoStack.clear();
        for (int index : region) {
            QRgb *scan = reinterpret_cast<QRgb *>(m_image.scanLine(index / w));
            scan[index % w] = replacement;
        }
        return true;
    }

    void centerCanvas() {
        if (m_image.isNull())
            return;

        QSizeF imgSize = m_image.size() * m_zoomFactor;
        QSizeF widgetSize = size();

        QPointF offset(
            (widgetSize.width()  - imgSize.width())  / 2.0,
            (widgetSize.height() - imgSize.height()) / 2.0
        );
        m_viewOffset = offset;
    }
};

// ─────────────────────────────────────────────
// Main Window
// ─────────────────────────────────────────────

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("🌅Draw");

        // Enable pinch gesture for this window
        grabGesture(Qt::PinchGesture);

        QScreen *screen = QGuiApplication::primaryScreen();
        QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 720);
        const int targetW = 1280;
        const int targetH = 720;
        int w = qMin(targetW, avail.width());
        int h = qMin(targetH, avail.height());
        resize(w, h);

        int minW = qMin(720, avail.width());
        int minH = qMin(480, avail.height());
        setMinimumSize(minW, minH);

        QWidget *central = new QWidget(this);
        setCentralWidget(central);
        central->setStyleSheet("background-color:#282828;");

        QVBoxLayout *mainLayout = new QVBoxLayout(central);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(0);

        QString btnStyle =
            "QPushButton { "
            "  background-color:#303030; color:white; font-family:Sans;"
            "  border-radius:6px; border:1px solid #404040; "
            "  padding:0px; font-size:22px; } "
            "QPushButton:hover { background-color:#3a3a3a; } "
            "QPushButton:pressed { background-color:#505050; } "
            "QPushButton:checked { background-color:#365b78; border:2px solid #89c9f7; color:#ffffff; } "
            "QPushButton:checked:hover { background-color:#416f90; border:2px solid #a5d8fa; } "
            "QPushButton:disabled { background-color:#1e1e1e; color:#777; }";

        // Icon-only buttons remain discoverable to mouse, touch and accessibility clients.
        auto styleIconButton = [&btnStyle](QPushButton *button, const QString &name) {
            button->setStyleSheet(btnStyle);
            button->setFixedSize(46, 46);
            button->setToolTip(name);
            button->setAccessibleName(name);
        };

        // ─────────────────────────────────
        // TOP TOOLBAR
        // ─────────────────────────────────
        QHBoxLayout *topBar = new QHBoxLayout();
        topBar->setContentsMargins(8, 6, 8, 4);
        topBar->setSpacing(6);

        QHBoxLayout *leftGroup = new QHBoxLayout();
        leftGroup->setSpacing(6);

        // Keep the fullscreen control at the start of the top toolbar so it
        // remains visible even when a small display clips the far-right tools.
        btnFullscreen = new QPushButton("⛶\nFull", this);
        styleIconButton(btnFullscreen, "Enter fullscreen canvas (F11)");
        btnFullscreen->setFixedSize(58, 46);
        btnFullscreen->setStyleSheet(btnStyle + "QPushButton { font-size:14px; }");
        leftGroup->addWidget(btnFullscreen);

        btnPen = new QPushButton("✒", this);
        styleIconButton(btnPen, "Pen");
        leftGroup->addWidget(btnPen);

        btnEraser = new QPushButton("🧽", this);
        styleIconButton(btnEraser, "Eraser");
        leftGroup->addWidget(btnEraser);

        btnLine = new QPushButton("📏", this);
        styleIconButton(btnLine, "Line");
        leftGroup->addWidget(btnLine);

        btnRect = new QPushButton("☐", this);
        styleIconButton(btnRect, "Rectangle");
        leftGroup->addWidget(btnRect);

        btnEllipse = new QPushButton("◯", this);
        styleIconButton(btnEllipse, "Ellipse");
        leftGroup->addWidget(btnEllipse);

        btnFill = new QPushButton("🪣", this);
        styleIconButton(btnFill, "Fill enclosed region");
        leftGroup->addWidget(btnFill);

        btnGrab = new QPushButton("👋", this);
        styleIconButton(btnGrab, "Grab / Pan");
        leftGroup->addWidget(btnGrab);

        // Exclusive checked state keeps exactly one drawing mode highlighted.
        QButtonGroup *toolButtons = new QButtonGroup(this);
        toolButtons->setExclusive(true);
        for (QPushButton *button : {btnPen, btnEraser, btnLine, btnRect,
                                    btnEllipse, btnFill, btnGrab}) {
            button->setCheckable(true);
            toolButtons->addButton(button);
        }

        QHBoxLayout *rightGroup = new QHBoxLayout();
        rightGroup->setSpacing(6);

        btnUndo = new QPushButton("↩", this);
        styleIconButton(btnUndo, "Undo");
        rightGroup->addWidget(btnUndo);

        btnRedo = new QPushButton("↪", this);
        styleIconButton(btnRedo, "Redo");
        rightGroup->addWidget(btnRedo);

        btnClear = new QPushButton("🗑", this);
        styleIconButton(btnClear, "Clear canvas");
        rightGroup->addWidget(btnClear);

        btnOpen = new QPushButton("📂", this);
        styleIconButton(btnOpen, "Open picture");
        rightGroup->addWidget(btnOpen);

        btnSave = new QPushButton("💾", this);
        styleIconButton(btnSave, "Save PNG");
        rightGroup->addWidget(btnSave);

        topBar->addLayout(leftGroup);
        topBar->addStretch(1);
        topBar->addLayout(rightGroup);

        toolbarContainer = new QWidget(central);
        toolbarContainer->setLayout(topBar);
        mainLayout->addWidget(toolbarContainer);

        // ─────────────────────────────────
        // SIZE + COLOR BAR + CANVAS PRESETS
        // ─────────────────────────────────
        QHBoxLayout *controlBar = new QHBoxLayout();
        controlBar->setContentsMargins(8, 0, 8, 6);
        controlBar->setSpacing(12);

        QLabel *sizeLabel = new QLabel("Size:", this);
        sizeLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        controlBar->addWidget(sizeLabel);

        sizeSlider = new QSlider(Qt::Horizontal, this);
        sizeSlider->setRange(1, 50);
        sizeSlider->setValue(5);
        sizeSlider->setFixedHeight(32);
        sizeSlider->setMinimumWidth(160);
        sizeSlider->setStyleSheet(
            "QSlider::groove:horizontal { height: 12px; background: #505050; border-radius: 6px; }"
            "QSlider::handle:horizontal { width: 32px; height: 32px; "
            " background-color:#ffffff; border-radius: 16px; margin: -10px 0; "
            " outline:none; border:0px solid transparent; }"
            "QSlider::handle:horizontal:pressed { background-color: #3a3a3a; border-radius: 16px; "
            " outline:none; border:0px solid transparent; }"
        );
        controlBar->addWidget(sizeSlider, 1);

        // Show the actual brush diameter, updated as the size changes.
        penPreview = new PenSizePreview(this);
        controlBar->addWidget(penPreview);

        QLabel *strokeLabel = new QLabel("Stroke:", this);
        strokeLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        controlBar->addWidget(strokeLabel);

        strokeColorBtn = new QPushButton(this);
        strokeColorBtn->setFixedSize(32, 32);
        strokeColorBtn->setStyleSheet(colorButtonStyle());
        strokeColorBtn->setIcon(colorSwatchIcon(strokeColor));
        strokeColorBtn->setIconSize(QSize(28, 28));
        strokeColorBtn->setToolTip("Stroke colour and opacity");
        controlBar->addWidget(strokeColorBtn);

        QLabel *fillLabel = new QLabel("Fill:", this);
        fillLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        controlBar->addWidget(fillLabel);

        fillColorBtn = new QPushButton(this);
        fillColorBtn->setFixedSize(32, 32);
        fillColorBtn->setStyleSheet(colorButtonStyle());
        fillColorBtn->setIcon(colorSwatchIcon(fillColor));
        fillColorBtn->setIconSize(QSize(28, 28));
        fillColorBtn->setToolTip("Fill colour and opacity");
        controlBar->addWidget(fillColorBtn);

        QLabel *canvasLabel = new QLabel("Canvas:", this);
        canvasLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        controlBar->addWidget(canvasLabel);

        sizePresetBox = new QComboBox(this);
        sizePresetBox->setStyleSheet(
            "QComboBox { background-color:#303030; color:#f0f0f0; "
            " border-radius:6px; border:1px solid #404040; padding:4px 8px; font-size:16px; } "
            "QComboBox QAbstractItemView { background-color:#303030; color:#f0f0f0; "
            " selection-background-color:#3a3a3a; }");
        sizePresetBox->addItem("640 x 480",  QSize(640, 480));
        sizePresetBox->addItem("800 x 600",  QSize(800, 600));
        sizePresetBox->addItem("1024 x 768", QSize(1024, 768));
        sizePresetBox->addItem("1280 x 720", QSize(1280, 720));
        sizePresetBox->addItem("1920 x 1080", QSize(1920, 1080));
        sizePresetBox->addItem("Custom...");
        controlBar->addWidget(sizePresetBox);

        transparentCheck = new QCheckBox("Transparent", this);
        transparentCheck->setStyleSheet("QCheckBox {color:#eeeeee;font-size:14px;}"
                                        "QCheckBox::indicator {width:17px;height:17px;}");
        transparentCheck->setToolTip("Display an alpha checkerboard and save PNGs with transparency");
        controlBar->addWidget(transparentCheck);
        controlBar->addStretch(1);

        controlContainer = new QWidget(central);
        controlContainer->setLayout(controlBar);
        mainLayout->addWidget(controlContainer);

        // ─────────────────────────────────
        // CANVAS CONTAINER
        // ─────────────────────────────────
        canvasContainer = new QWidget(central);
        canvasContainer->setStyleSheet("background:#282828;");
        canvasLayout = new QVBoxLayout(canvasContainer);
        canvasLayout->setContentsMargins(32, 16, 32, 16);
        canvasLayout->setSpacing(0);

        canvas = new DrawingCanvas(canvasContainer);
        canvas->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        canvasLayout->addWidget(canvas);
        mainLayout->addWidget(canvasContainer, 1);

        // ─────────────────────────────────
        // ZOOM SLIDER (BOTTOM) + FIT BUTTON
        // ─────────────────────────────────
        QHBoxLayout *zoomLayout = new QHBoxLayout();
        zoomLayout->setContentsMargins(8, 4, 8, 8);
        zoomLayout->setSpacing(8);

        QLabel *zoomLabel = new QLabel("Zoom:", this);
        zoomLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        zoomLayout->addWidget(zoomLabel);

        zoomSlider = new QSlider(Qt::Horizontal, this);
        zoomSlider->setRange(5, 400);
        zoomSlider->setValue(100);
        zoomSlider->setFixedHeight(24);
        zoomSlider->setMinimumWidth(260);
        zoomSlider->setStyleSheet(
            "QSlider::groove:horizontal { height: 8px; background: #505050; border-radius: 4px; }"
            "QSlider::handle:horizontal { width: 22px; height: 22px; "
            " background-color:#ffffff; border-radius: 11px; margin: -7px 0; "
            " outline:none; border:0px solid transparent; }"
            "QSlider::handle:horizontal:pressed { background-color: #3a3a3a; border-radius: 11px; "
            " outline:none; border:0px solid transparent; }"
        );
        zoomLayout->addWidget(zoomSlider, 1);

        zoomValueLabel = new QLabel("100%", this);
        zoomValueLabel->setStyleSheet("color:#f0f0f0; font-size:16px;");
        zoomLayout->addWidget(zoomValueLabel);

        zoomLayout->addStretch(1);

        btnFit = new QPushButton("⤢", this);
        btnFit->setStyleSheet(btnStyle);
        btnFit->setFixedSize(40, 32);
        btnFit->setToolTip("Fit image to window");
        btnFit->setAccessibleName("Fit image to window");
        zoomLayout->addWidget(btnFit);

        zoomContainer = new QWidget(central);
        zoomContainer->setLayout(zoomLayout);
        mainLayout->addWidget(zoomContainer);

        // ─────────────────────────────────
        // STATUSBAR
        // ─────────────────────────────────
        QStatusBar *sb = new QStatusBar(this);
        sb->setStyleSheet("QStatusBar { background:#282828; color:white; font-size:16px; }");
        setStatusBar(sb);
        statusBar()->showMessage("Ready");

        createFloatingToolbar();
        canvasContainer->installEventFilter(this);
        auto *fullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_F11), this);
        connect(fullscreenShortcut, &QShortcut::activated, this,
                [this]() { toggleCanvasFullscreen(); });
        auto *exitFullscreenShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        connect(exitFullscreenShortcut, &QShortcut::activated, this, [this]() {
            if (isFullScreen()) toggleCanvasFullscreen();
        });
        connect(btnFullscreen, &QPushButton::clicked, this,
                [this]() { toggleCanvasFullscreen(); });
        connect(transparentCheck, &QCheckBox::toggled, this, [this](bool enabled) {
            canvas->setTransparentBackground(enabled);
            if (floatingTransparency)
                floatingTransparency->setChecked(enabled);
        });

        // ─────────────────────────────────
        // CONNECTIONS
        // ─────────────────────────────────
        connect(btnPen, &QPushButton::clicked, this, [this]() {
            canvas->setTool(DrawingCanvas::Tool::Pen);
            statusBar()->showMessage("Tool: Pen");
        });
        connect(btnEraser, &QPushButton::clicked, this, [this]() {
            canvas->setTool(DrawingCanvas::Tool::Eraser);
            statusBar()->showMessage("Tool: Eraser");
        });
        connect(btnLine, &QPushButton::clicked, this, [this]() {
            canvas->setTool(DrawingCanvas::Tool::Line);
            statusBar()->showMessage("Tool: Line");
        });
        connect(btnRect, &QPushButton::clicked, this, [this]() {
            canvas->setTool(DrawingCanvas::Tool::Rect);
            statusBar()->showMessage("Tool: Rect");
        });
        connect(btnEllipse, &QPushButton::clicked, this, [this]() {
            canvas->setTool(DrawingCanvas::Tool::Ellipse);
            statusBar()->showMessage("Tool: Ellipse");
        });
        connect(btnFill, &QPushButton::clicked, this, [this]() {
            canvas->setTool(DrawingCanvas::Tool::Fill);
            statusBar()->showMessage("Tool: Fill");
        });
        connect(btnGrab, &QPushButton::clicked, this, [this]() {
            canvas->setTool(DrawingCanvas::Tool::Grab);
            statusBar()->showMessage("Tool: Grab (drag to pan)");
        });

        connect(btnClear, &QPushButton::clicked, this, [this]() {
            canvas->clearCanvas();
            statusBar()->showMessage("Canvas cleared");
        });

        connect(btnOpen, &QPushButton::clicked, this, [this]() {
            QStringList patterns;
            for (const QByteArray &format : QImageReader::supportedImageFormats())
                patterns.append("*." + QString::fromLatin1(format.constData()));
            patterns.removeDuplicates();
            patterns.sort();
            const QString filter = patterns.isEmpty()
                ? QStringLiteral("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;All files (*)")
                : QStringLiteral("Images (%1);;All files (*)").arg(patterns.join(' '));
            const QString directory = openedImagePath.isEmpty()
                ? QDir::homePath() : QFileInfo(openedImagePath).absolutePath();
            const QString path = QFileDialog::getOpenFileName(this, "Open picture", directory, filter);
            if (!path.isEmpty())
                openImageFromPath(path);
        });

        connect(btnSave, &QPushButton::clicked, this, [this]() {
            QString path = QFileDialog::getSaveFileName(
                this, "Save PNG",
                openedImagePath.isEmpty()
                    ? QDir::homePath() + "/drawing.png"
                    : QDir(QFileInfo(openedImagePath).absolutePath()).filePath(
                        QFileInfo(openedImagePath).completeBaseName() + ".png"),
                "PNG Image (*.png)");
            if (!path.isEmpty()) {
                if (canvas->savePng(path)) {
                    openedImagePath = QFileInfo(path).absoluteFilePath();
                    setWindowTitle("🌅Draw - " + QFileInfo(path).fileName());
                    statusBar()->showMessage("Saved: " + path, 3000);
                }
                else
                    statusBar()->showMessage("Failed to save", 3000);
            }
        });

        connect(sizeSlider, &QSlider::valueChanged, this, [this](int v) {
            canvas->setPenSize(v);
            penPreview->setPenSize(v);
            statusBar()->showMessage(QString("Pen size: %1").arg(v), 1500);
        });

        connect(strokeColorBtn, &QPushButton::clicked, this, [this]() {
            RadialColorDialog dialog("Select stroke colour", strokeColor, this);
            if (dialog.exec() == QDialog::Accepted)
                applyStrokeColor(dialog.selectedColor());
        });

        connect(fillColorBtn, &QPushButton::clicked, this, [this]() {
            RadialColorDialog dialog("Select fill colour", fillColor, this);
            if (dialog.exec() == QDialog::Accepted)
                applyFillColor(dialog.selectedColor());
        });

        connect(btnUndo, &QPushButton::clicked, this, [this]() {
            canvas->undo();
            statusBar()->showMessage("Undo");
        });

        connect(btnRedo, &QPushButton::clicked, this, [this]() {
            canvas->redo();
            statusBar()->showMessage("Redo");
        });

        // Slider zoom
        connect(zoomSlider, &QSlider::valueChanged, this, [this](int v) {
            double factor = double(v) / 100.0;
            canvas->setZoom(factor);
            zoomValueLabel->setText(QString::number(v) + "%");
            statusBar()->showMessage(QString("Zoom: %1%").arg(v), 500);
        });

        // Fit-to-window button
        connect(btnFit, &QPushButton::clicked, this, [this]() {
            canvas->fitToWidget();
            int v = int(canvas->zoom() * 100.0);
            v = qBound(zoomSlider->minimum(), v, zoomSlider->maximum());
            zoomSlider->blockSignals(true);
            zoomSlider->setValue(v);
            zoomSlider->blockSignals(false);
            zoomValueLabel->setText(QString::number(v) + "%");
            statusBar()->showMessage("Zoom: Fit to window", 1500);
        });

        // Canvas size presets + Custom...
        connect(sizePresetBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int index) {
            if (index < 0)
                return;

            QString text = sizePresetBox->itemText(index);
            if (text == "Custom...") {
                QString input = QInputDialog::getText(
                    this,
                    "Custom canvas size",
                    "Enter size as WIDTHxHEIGHT (e.g. 1920x1080):"
                );
                if (input.isEmpty())
                    return;

                int w = 0, h = 0;
                QString s = input.trimmed();
                s.replace('X', 'x');
                const QStringList parts = s.split('x');
                if (parts.size() == 2) {
                    bool okW = false, okH = false;
                    w = parts[0].trimmed().toInt(&okW);
                    h = parts[1].trimmed().toInt(&okH);
                    if (okW && okH && w > 0 && h > 0) {
                        canvas->resizeCanvasTo(QSize(w, h));
                        statusBar()->showMessage(
                            QString("Canvas resized to %1 x %2").arg(w).arg(h), 2000);
                    }
                }
                return;
            }

            QVariant data = sizePresetBox->itemData(index);
            if (data.canConvert<QSize>()) {
                QSize sz = data.toSize();
                canvas->resizeCanvasTo(sz);
                statusBar()->showMessage(
                    QString("Canvas resized to %1 x %2")
                        .arg(sz.width()).arg(sz.height()),
                    2000);
            }
        });

        // Default setup
        canvas->setPenSize(sizeSlider->value());
        applyStrokeColor(strokeColor);
        applyFillColor(fillColor);
        canvas->setTool(DrawingCanvas::Tool::Pen);
        btnPen->setChecked(true);
        penPreview->setPenSize(sizeSlider->value());
        penPreview->setPenColor(strokeColor);
        canvas->setZoom(1.0);
    }

    // Used both by the Open button and by `osm-draw /path/to/picture`.
    bool openImageFromPath(const QString &path) {
        QImageReader reader(path);
        reader.setAutoTransform(true);  // Honor camera EXIF orientation.
        const QImage image = reader.read();
        if (image.isNull() || !canvas->loadImage(image)) {
            QMessageBox::warning(this, "Open picture",
                "Could not open the picture:\n" + path + "\n\n" + reader.errorString());
            return false;
        }

        openedImagePath = QFileInfo(path).absoluteFilePath();
        setWindowTitle("🌅Draw - " + QFileInfo(path).fileName());
        // Images carrying an alpha channel should not be flattened by default.
        if (image.hasAlphaChannel())
            transparentCheck->setChecked(true);

        // Opening an image (toolbar or command-line) always starts in Grab mode,
        // preventing an accidental stroke while adjusting the view.
        canvas->setTool(DrawingCanvas::Tool::Grab);
        btnGrab->setChecked(true);

        // Imported images keep their dimensions, then fit into the visible page.
        canvas->fitToWidget();
        const int percent = qBound(zoomSlider->minimum(),
                                  qRound(canvas->zoom() * 100.0),
                                  zoomSlider->maximum());
        {
            QSignalBlocker block(zoomSlider);
            zoomSlider->setValue(percent);
        }
        zoomValueLabel->setText(QString::number(percent) + "%");

        // A preset should not resize an imported image behind the user's back.
        int preset = -1;
        for (int i = 0; i < sizePresetBox->count(); ++i) {
            if (sizePresetBox->itemData(i).canConvert<QSize>() &&
                sizePresetBox->itemData(i).toSize() == image.size()) {
                preset = i;
                break;
            }
        }
        {
            QSignalBlocker block(sizePresetBox);
            sizePresetBox->setCurrentIndex(preset);
        }
        statusBar()->showMessage(QString("Opened: %1 (%2 × %3)")
                                 .arg(path).arg(image.width()).arg(image.height()), 4000);
        return true;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *ev) override {
        if (watched == canvasContainer && ev->type() == QEvent::Resize &&
            floatingToolbar && floatingToolbar->isVisible()) {
            QTimer::singleShot(0, this, [this]() { keepFloatingMenuVisible(); });
        }
        return QMainWindow::eventFilter(watched, ev);
    }

    bool event(QEvent *ev) override {
        if (ev->type() == QEvent::Gesture) {
            return gestureEvent(static_cast<QGestureEvent*>(ev));
        }
        return QMainWindow::event(ev);
    }

private:
    DrawingCanvas *canvas;

    QPushButton *btnPen;
    QPushButton *btnEraser;
    QPushButton *btnLine;
    QPushButton *btnRect;
    QPushButton *btnEllipse;
    QPushButton *btnFill;
    QPushButton *btnGrab;
    QPushButton *btnClear;
    QPushButton *btnOpen;
    QPushButton *btnSave;
    QPushButton *btnUndo;
    QPushButton *btnRedo;
    QPushButton *btnFit;
    QPushButton *btnFullscreen;

    QWidget *toolbarContainer = nullptr;
    QWidget *controlContainer = nullptr;
    QWidget *zoomContainer = nullptr;
    QWidget *canvasContainer = nullptr;
    QVBoxLayout *canvasLayout = nullptr;
    FloatingToolbar *floatingToolbar = nullptr;
    QPushButton *floatingStroke = nullptr;
    QPushButton *floatingFill = nullptr;
    QPushButton *floatingTransparency = nullptr;
    QCheckBox *transparentCheck = nullptr;
    QColor strokeColor = Qt::black;
    QColor fillColor = Qt::red;

    QSlider   *sizeSlider;
    PenSizePreview *penPreview;
    QPushButton *strokeColorBtn;
    QPushButton *fillColorBtn;
    QComboBox *sizePresetBox;

    QSlider *zoomSlider;
    QLabel  *zoomValueLabel;
    QString openedImagePath;

    bool gestureEvent(QGestureEvent *event) {
        if (QGesture *g = event->gesture(Qt::PinchGesture)) {
            auto *pinch = static_cast<QPinchGesture*>(g);

            if (pinch->state() == Qt::GestureUpdated) {
                qreal factor = pinch->scaleFactor();
                double currentZoom = canvas->zoom();
                double newZoom = qBound(0.05, currentZoom * factor, 4.0);

                if (!qFuzzyCompare(newZoom, currentZoom)) {
                    canvas->setZoom(newZoom);

                    int v = int(newZoom * 100.0);
                    v = qBound(zoomSlider->minimum(), v, zoomSlider->maximum());
                    zoomSlider->blockSignals(true);
                    zoomSlider->setValue(v);
                    zoomSlider->blockSignals(false);
                    zoomValueLabel->setText(QString::number(v) + "%");

                    statusBar()->showMessage(
                        QString("Zoom: %1%").arg(v),
                        800
                    );
                }
            }

            event->accept(g);
            return true;
        }
        return false;
    }

    static QString colorButtonStyle() {
        return QStringLiteral(
            "QPushButton {background:#30343a;border-radius:5px;"
            "border:1px solid #a0a7af;padding:0px;}"
            "QPushButton:hover {border:2px solid #ffffff;}");
    }

    void applyStrokeColor(const QColor &c) {
        strokeColor = c;
        canvas->setPenColor(c);
        penPreview->setPenColor(c);
        strokeColorBtn->setIcon(colorSwatchIcon(c));
        if (floatingStroke)
            floatingStroke->setIcon(colorSwatchIcon(c, 26));
    }

    void applyFillColor(const QColor &c) {
        fillColor = c;
        canvas->setFillColor(c);
        fillColorBtn->setIcon(colorSwatchIcon(c));
        if (floatingFill)
            floatingFill->setIcon(colorSwatchIcon(c, 26));
    }

    void keepFloatingMenuVisible() {
        if (!floatingToolbar || !canvasContainer)
            return;
        floatingToolbar->adjustSize();
        QPoint current = floatingToolbar->pos();
        current.setX(qBound(0, current.x(),
                            qMax(0, canvasContainer->width() - floatingToolbar->width())));
        current.setY(qBound(0, current.y(),
                            qMax(0, canvasContainer->height() - floatingToolbar->height())));
        floatingToolbar->move(current);
        floatingToolbar->raise();
    }

    void toggleCanvasFullscreen() {
        const bool enable = !isFullScreen();
        toolbarContainer->setVisible(!enable);
        controlContainer->setVisible(!enable);
        zoomContainer->setVisible(!enable);
        statusBar()->setVisible(!enable);
        canvasLayout->setContentsMargins(enable ? 0 : 32, enable ? 0 : 16,
                                        enable ? 0 : 32, enable ? 0 : 16);
        if (enable) {
            showFullScreen();
            floatingToolbar->show();
            floatingToolbar->adjustSize();
            floatingToolbar->move(12, 12);
            floatingToolbar->raise();
        } else {
            floatingToolbar->hide();
            showNormal();
        }
        QTimer::singleShot(0, this, [this]() { keepFloatingMenuVisible(); });
    }

    void createFloatingToolbar() {
        floatingToolbar = new FloatingToolbar(canvasContainer);
        auto *layout = new QVBoxLayout(floatingToolbar);
        layout->setContentsMargins(7, 6, 7, 7);
        layout->setSpacing(5);
        auto *top = new QHBoxLayout();
        top->setSpacing(4);
        auto *bottom = new QHBoxLayout();
        bottom->setSpacing(5);

        auto *handle = new QLabel("⠿", floatingToolbar);
        handle->setFixedSize(22, 34);
        handle->setToolTip("Drag to reposition tools");
        handle->setAlignment(Qt::AlignCenter);
        floatingToolbar->setDragHandle(handle);
        top->addWidget(handle);

        auto makeButton = [this](const QString &icon, const QString &tip,
                                 QHBoxLayout *row, int width = 34) {
            auto *b = new QPushButton(icon, floatingToolbar);
            b->setFixedSize(width, 34);
            b->setToolTip(tip);
            b->setAccessibleName(tip);
            row->addWidget(b);
            return b;
        };
        auto *tools = new QButtonGroup(floatingToolbar);
        tools->setExclusive(true);
        const QList<QPushButton *> originals = {
            btnPen, btnEraser, btnLine, btnRect, btnEllipse, btnFill, btnGrab
        };
        for (QPushButton *original : originals) {
            QPushButton *b = makeButton(original->text(), original->toolTip(), top);
            b->setCheckable(true);
            tools->addButton(b);
            connect(b, &QPushButton::clicked, original, &QPushButton::click);
            connect(original, &QPushButton::toggled, b, &QPushButton::setChecked);
        }
        auto *undo = makeButton("↩", "Undo", top);
        auto *redo = makeButton("↪", "Redo", top);
        connect(undo, &QPushButton::clicked, btnUndo, &QPushButton::click);
        connect(redo, &QPushButton::clicked, btnRedo, &QPushButton::click);
        layout->addLayout(top);

        auto *sizeCaption = new QLabel("Size", floatingToolbar);
        bottom->addWidget(sizeCaption);
        auto *miniSizeSlider = new QSlider(Qt::Horizontal, floatingToolbar);
        miniSizeSlider->setRange(1, 50);
        miniSizeSlider->setValue(sizeSlider->value());
        miniSizeSlider->setFixedWidth(82);
        miniSizeSlider->setToolTip("Pen and eraser size");
        bottom->addWidget(miniSizeSlider);
        connect(miniSizeSlider, &QSlider::valueChanged, sizeSlider, &QSlider::setValue);
        connect(sizeSlider, &QSlider::valueChanged, miniSizeSlider, &QSlider::setValue);

        floatingStroke = makeButton("", "Stroke colour / opacity", bottom, 32);
        floatingStroke->setIconSize(QSize(26, 26));
        floatingStroke->setIcon(colorSwatchIcon(strokeColor, 26));
        floatingFill = makeButton("", "Fill colour / opacity", bottom, 32);
        floatingFill->setIconSize(QSize(26, 26));
        floatingFill->setIcon(colorSwatchIcon(fillColor, 26));
        connect(floatingStroke, &QPushButton::clicked, strokeColorBtn, &QPushButton::click);
        connect(floatingFill, &QPushButton::clicked, fillColorBtn, &QPushButton::click);

        floatingTransparency = makeButton("◩", "Transparent background for PNG", bottom);
        floatingTransparency->setCheckable(true);
        connect(floatingTransparency, &QPushButton::toggled,
                transparentCheck, &QCheckBox::setChecked);
        auto *clear = makeButton("🗑", "Clear", bottom);
        auto *open = makeButton("📂", "Open image", bottom);
        auto *save = makeButton("💾", "Save PNG", bottom);
        auto *fit = makeButton("⤢", "Fit to screen", bottom);
        auto *exit = makeButton("⛶", "Exit fullscreen (F11 / Esc)", bottom);
        connect(clear, &QPushButton::clicked, btnClear, &QPushButton::click);
        connect(open, &QPushButton::clicked, btnOpen, &QPushButton::click);
        connect(save, &QPushButton::clicked, btnSave, &QPushButton::click);
        connect(fit, &QPushButton::clicked, btnFit, &QPushButton::click);
        connect(exit, &QPushButton::clicked, this, [this]() { toggleCanvasFullscreen(); });
        layout->addLayout(bottom);
        floatingToolbar->adjustSize();
        floatingToolbar->hide();
    }
};

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    MainWindow w;
    w.show();

    if (app.arguments().size() > 1) {
        const QString picturePath = app.arguments().at(1);
        // Wait until Qt has laid out the canvas before centring/fitting it.
        QTimer::singleShot(0, &w, [&w, picturePath]() {
            w.openImageFromPath(picturePath);
        });
    }

    return app.exec();
}
