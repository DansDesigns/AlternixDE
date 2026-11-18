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
#include <QList>
#include <QFont>
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
            "QSlider::groove:horizontal { height:10px; background:#50545c; border-radius:5px; }"
            "QSlider::handle:horizontal { background:#ffffff; width:38px; height:38px;"
            " margin:-14px 0; border:2px solid #879eaf; border-radius:19px; }"
            "QSlider::handle:horizontal:pressed { background:#c8e5fc; }"
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
            slider->setFixedHeight(48);  // Room for a touch-friendly 38px handle.
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
                      " border-radius:5px; padding:0px;}"
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

    // Background is a non-destructive page colour: translucent brush pixels
    // remain editable, and opaque PNG exports composite against this colour.
    void setDarkBackground(bool enabled) {
        m_bgColor = enabled ? QColor("#202124") : QColor(Qt::white);
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
        // Do not turn a click OUTSIDE the displayed page into a fill at its
        // nearest edge, which could unintentionally recolour the whole page.
        if (m_tool == Tool::Fill &&
            !QRect(QPoint(0, 0), m_image.size()).contains(imgPos))
            return;
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

        // The canvas edge is a boundary, not a reason to reject a fill.
        // Scanline runs avoid a per-pixel stack: filling an empty 4096x4096
        // canvas remains bounded in memory even on lower-powered devices.
        struct FillRun { int row; int left; int right; };
        QBitArray visited(w * h, false);
        QVector<QPoint> seeds;
        QVector<FillRun> runs;
        seeds.reserve(256);
        runs.reserve(256);
        seeds.append(start);

        while (!seeds.isEmpty()) {
            const QPoint seed = seeds.takeLast();
            const int x = seed.x();
            const int y = seed.y();
            const int at = y * w + x;
            if (visited.testBit(at) || !matches(x, y))
                continue;

            int left = x;
            while (left > 0 && !visited.testBit(y * w + left - 1) &&
                   matches(left - 1, y))
                --left;
            int right = x;
            while (right + 1 < w && !visited.testBit(y * w + right + 1) &&
                   matches(right + 1, y))
                ++right;

            for (int px = left; px <= right; ++px)
                visited.setBit(y * w + px);
            runs.append({y, left, right});

            // Queue exactly one seed for each adjacent, unvisited span.
            // Bounds checks stop at page edges, so the entire page may fill.
            for (int neighborY : {y - 1, y + 1}) {
                if (neighborY < 0 || neighborY >= h)
                    continue;
                bool inSpan = false;
                for (int px = left; px <= right; ++px) {
                    const int neighborIndex = neighborY * w + px;
                    const bool candidate = !visited.testBit(neighborIndex) &&
                                           matches(px, neighborY);
                    if (candidate && !inSpan)
                        seeds.append(QPoint(px, neighborY));
                    inSpan = candidate;
                }
            }
        }

        if (runs.isEmpty())
            return false;
        pushUndo();
        m_redoStack.clear();
        for (const FillRun &run : runs) {
            QRgb *scan = reinterpret_cast<QRgb *>(m_image.scanLine(run.row));
            for (int px = run.left; px <= run.right; ++px)
                scan[px] = replacement;
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

        // Keep osm-draw above desktop overlays such as osm-notify's toggle handles.
        // This applies to both the normal window and fullscreen canvas mode.
        setWindowFlag(Qt::WindowStaysOnTopHint, true);

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
        styleIconButton(btnFill, "Fill connected area (canvas edges are boundaries)");
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

        // Fullscreen is the far-right button of the normal toolbar.
        btnFullscreen = new QPushButton("⛶\nFull", this);
        styleIconButton(btnFullscreen, "Enter fullscreen canvas (F11)");
        btnFullscreen->setFixedSize(58, 46);
        btnFullscreen->setStyleSheet(btnStyle + "QPushButton { font-size:14px; }");
        rightGroup->addWidget(btnFullscreen);

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

        btnDarkBackground = new QPushButton("☾ Dark", this);
        btnDarkBackground->setCheckable(true);
        btnDarkBackground->setFixedHeight(32);
        btnDarkBackground->setMinimumWidth(77);
        btnDarkBackground->setStyleSheet(
            "QPushButton {background:#363b42; color:#ffffff; border-radius:5px;"
            "border:1px solid #66717b; padding:2px 8px; font-size:14px;}"
            "QPushButton:checked {background:#47617b; border:2px solid #a4d6ff;}"
            "QPushButton:hover {background:#4b515c;}");
        btnDarkBackground->setToolTip("Use a dark canvas background (also used for opaque PNG exports)");
        controlBar->addWidget(btnDarkBackground);
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
        // White (neither selected), Dark, and Transparent are exclusive page
        // backgrounds. This changes the page backing, not the drawn pixels.
        connect(btnDarkBackground, &QPushButton::toggled, this, [this](bool enabled) {
            if (enabled && transparentCheck->isChecked())
                transparentCheck->setChecked(false);
            canvas->setDarkBackground(enabled);
            if (floatingDarkBackground)
                floatingDarkBackground->setChecked(enabled);
            statusBar()->showMessage(enabled ? "Canvas background: dark" : "Canvas background: white", 1800);
        });
        connect(transparentCheck, &QCheckBox::toggled, this, [this](bool enabled) {
            if (enabled && btnDarkBackground->isChecked())
                btnDarkBackground->setChecked(false);
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
    QPushButton *btnDarkBackground = nullptr;

    QWidget *toolbarContainer = nullptr;
    QWidget *controlContainer = nullptr;
    QWidget *zoomContainer = nullptr;
    QWidget *canvasContainer = nullptr;
    QVBoxLayout *canvasLayout = nullptr;
    FloatingToolbar *floatingToolbar = nullptr;
    QPushButton *floatingStroke = nullptr;
    QPushButton *floatingFill = nullptr;
    QPushButton *floatingTransparency = nullptr;
    QPushButton *floatingDarkBackground = nullptr;
    QPushButton *floatingSizeButton = nullptr;
    QFrame *floatingSizePopup = nullptr;
    QSlider *floatingSizePopupSlider = nullptr;
    PenSizePreview *floatingSizePreview = nullptr;
    QHBoxLayout *floatingToolLayout = nullptr;
    QLabel *floatingDragHandle = nullptr;
    QList<QPushButton *> floatingToolButtons;
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
            floatingStroke->setIcon(colorSwatchIcon(c, 34));
        if (floatingSizePreview)
            floatingSizePreview->setPenColor(c);
    }

    void applyFillColor(const QColor &c) {
        fillColor = c;
        canvas->setFillColor(c);
        fillColorBtn->setIcon(colorSwatchIcon(c));
        if (floatingFill)
            floatingFill->setIcon(colorSwatchIcon(c, 34));
    }

    void keepFloatingMenuVisible() {
        if (!floatingToolbar || !canvasContainer || !floatingToolLayout)
            return;

        // All buttons share a single row, with no QScrollArea. Use their normal
        // widths on large screens, and compact the actual controls as needed
        // after a window resize or a landscape/portrait rotation.
        // Leave 8px outside each edge and room for the frame border.
        const int available = qMax(1, canvasContainer->width() - 18);
        const int buttonCount = floatingToolButtons.size();
        if (buttonCount > 0) {
            const int fullWidth = 14 + 30 + buttonCount * 3
                                + buttonCount * 48 + 52;
            const bool compact = available < fullWidth;
            const int margin = !compact ? 7 : (available < 460 ? 3 : 5);
            const int gap = !compact ? 3 : (available < 560 ? 1 : 2);
            const int dragWidth = !compact ? 30 : (available < 460 ? 18 : 24);
            const int sizeExtra = !compact ? 52 : (available < 560 ? 18 : 36);
            // The brush-size control is wider than an ordinary tool button.
            const int each = qMin(48, qMax(8, (available - 2 * margin - dragWidth
                                               - buttonCount * gap - sizeExtra)
                                              / buttonCount));
            floatingToolLayout->setContentsMargins(margin, 5, margin, 5);
            floatingToolLayout->setSpacing(gap);
            if (floatingDragHandle)
                floatingDragHandle->setFixedWidth(dragWidth);
            for (QPushButton *button : floatingToolButtons) {
                const int buttonWidth = each + (button == floatingSizeButton ? sizeExtra : 0);
                button->setFixedSize(buttonWidth, 50);
                QFont font = button->font();
                font.setPixelSize(qBound(10, buttonWidth - 7, button == floatingSizeButton ? 18 : 26));
                button->setFont(font);
            }
            if (floatingStroke)
                floatingStroke->setIconSize(QSize(qBound(10, each - 6, 34), qBound(10, each - 6, 34)));
            if (floatingFill)
                floatingFill->setIconSize(QSize(qBound(10, each - 6, 34), qBound(10, each - 6, 34)));
            // Even the compact size control continues to show the current size.
            if (floatingSizeButton)
                floatingSizeButton->setText(buttonWidthForSizeText() >= 50
                    ? QString("%1px ▾").arg(sizeSlider->value())
                    : QString::number(sizeSlider->value()));
        }

        // Allow the frame to grow to the combined button widths, rather than
        // clipping its last actions behind a viewport or a scrollbar.
        floatingToolLayout->activate();
        floatingToolbar->setFixedSize(floatingToolLayout->sizeHint() + QSize(2, 2));
        QPoint current = floatingToolbar->pos();
        current.setX(qBound(0, current.x(),
                            qMax(0, canvasContainer->width() - floatingToolbar->width())));
        current.setY(qBound(0, current.y(),
                            qMax(0, canvasContainer->height() - floatingToolbar->height())));
        floatingToolbar->move(current);
        floatingToolbar->raise();
    }

    int buttonWidthForSizeText() const {
        return floatingSizeButton ? floatingSizeButton->width() : 0;
    }

    void toggleCanvasFullscreen() {
        const bool enable = !isFullScreen();
        if (floatingSizePopup)
            floatingSizePopup->hide();
        toolbarContainer->setVisible(!enable);
        controlContainer->setVisible(!enable);
        zoomContainer->setVisible(!enable);
        statusBar()->setVisible(!enable);
        canvasLayout->setContentsMargins(enable ? 0 : 32, enable ? 0 : 16,
                                        enable ? 0 : 32, enable ? 0 : 16);
        if (enable) {
            showFullScreen();
            raise();
            floatingToolbar->show();
            floatingToolbar->adjustSize();
            floatingToolbar->move(12, 12);
            floatingToolbar->raise();
        } else {
            floatingToolbar->hide();
            showNormal();
            raise();
        }
        QTimer::singleShot(0, this, [this]() { keepFloatingMenuVisible(); });
    }

    void createFloatingToolbar() {
        floatingToolbar = new FloatingToolbar(canvasContainer);
        floatingToolLayout = new QHBoxLayout(floatingToolbar);
        floatingToolLayout->setContentsMargins(7, 5, 7, 5);
        floatingToolLayout->setSpacing(3);

        floatingDragHandle = new QLabel("⠿", floatingToolbar);
        floatingDragHandle->setFixedSize(30, 50);
        floatingDragHandle->setToolTip("Drag to reposition tools");
        floatingDragHandle->setAlignment(Qt::AlignCenter);
        floatingToolbar->setDragHandle(floatingDragHandle);
        floatingToolLayout->addWidget(floatingDragHandle);

        // Keep every tool directly in the frame's one-row layout; there is no
        // scrollable viewport to obscure the tools at either end.
        auto makeButton = [this](const QString &symbol, const QString &tip,
                                 int width = 48) {
            auto *button = new QPushButton(symbol, floatingToolbar);
            button->setFixedSize(width, 50);
            button->setToolTip(tip);
            button->setAccessibleName(tip);
            floatingToolLayout->addWidget(button);
            floatingToolButtons.append(button);
            QFont font = button->font();
            font.setPixelSize(26);
            button->setFont(font);
            return button;
        };

        auto *tools = new QButtonGroup(floatingToolbar);
        tools->setExclusive(true);
        const QList<QPushButton *> originals = {
            btnPen, btnEraser, btnLine, btnRect, btnEllipse, btnFill, btnGrab
        };
        for (QPushButton *original : originals) {
            QPushButton *button = makeButton(original->text(), original->toolTip());
            button->setCheckable(true);
            tools->addButton(button);
            connect(button, &QPushButton::clicked, original, &QPushButton::click);
            connect(original, &QPushButton::toggled, button, &QPushButton::setChecked);
            button->setChecked(original->isChecked());
        }

        auto *undo = makeButton("↩", "Undo");
        auto *redo = makeButton("↪", "Redo");
        connect(undo, &QPushButton::clicked, btnUndo, &QPushButton::click);
        connect(redo, &QPushButton::clicked, btnRedo, &QPushButton::click);

        // A compact single-row button opens a large, touch-friendly slider
        // with a live 1:1 preview. It shares the normal size slider's value.
        floatingSizeButton = makeButton("5px ▾", "Brush size", 100);
        floatingSizeButton->setStyleSheet(
            "QPushButton {background:#363b42;color:#ffffff;border:1px solid #68717b;"
            "border-radius:5px;}"
            "QPushButton:hover {background:#505c68;}");
        connect(sizeSlider, &QSlider::valueChanged, this, [this](int value) {
            if (floatingSizeButton)
                floatingSizeButton->setText(buttonWidthForSizeText() >= 50
                    ? QString("%1px ▾").arg(value)
                    : QString::number(value));
            if (floatingSizePopupSlider)
                floatingSizePopupSlider->setValue(value);
            if (floatingSizePreview)
                floatingSizePreview->setPenSize(value);
        });
        floatingSizePopup = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
        floatingSizePopup->setObjectName("floatingSizePopup");
        floatingSizePopup->setAttribute(Qt::WA_StyledBackground);
        floatingSizePopup->setStyleSheet(
            "QFrame#floatingSizePopup {background:#22262d;border:1px solid #788492;border-radius:12px;}"
            "QLabel {color:#ffffff;font-size:15px;}"
            "QSlider::groove:horizontal {height:14px;background:#4b5460;border-radius:7px;}"
            "QSlider::handle:horizontal {width:46px;height:46px;margin:-16px 0;"
            "background:#e0eefb;border:2px solid #84c6fa;border-radius:23px;}"
            "QSlider::handle:horizontal:pressed {background:#accfe8;}");
        auto *popupLayout = new QVBoxLayout(floatingSizePopup);
        popupLayout->setContentsMargins(18, 14, 18, 18);
        popupLayout->setSpacing(14);
        auto *previewRow = new QHBoxLayout();
        auto *previewLabel = new QLabel("Brush size", floatingSizePopup);
        previewLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        previewRow->addWidget(previewLabel, 1);
        floatingSizePreview = new PenSizePreview(floatingSizePopup);
        floatingSizePreview->setPenColor(strokeColor);
        floatingSizePreview->setPenSize(sizeSlider->value());
        previewRow->addWidget(floatingSizePreview);
        popupLayout->addLayout(previewRow);
        floatingSizePopupSlider = new QSlider(Qt::Horizontal, floatingSizePopup);
        floatingSizePopupSlider->setRange(sizeSlider->minimum(), sizeSlider->maximum());
        floatingSizePopupSlider->setValue(sizeSlider->value());
        floatingSizePopupSlider->setFixedHeight(60);
        floatingSizePopupSlider->setMinimumWidth(275);
        floatingSizePopupSlider->setToolTip("Drag the large handle to adjust pen / eraser size");
        popupLayout->addWidget(floatingSizePopupSlider);
        connect(floatingSizePopupSlider, &QSlider::valueChanged, sizeSlider, &QSlider::setValue);
        floatingSizePopup->adjustSize();
        connect(floatingSizeButton, &QPushButton::clicked, this, [this]() {
            floatingSizePopupSlider->setValue(sizeSlider->value());
            floatingSizePreview->setPenSize(sizeSlider->value());
            floatingSizePopup->adjustSize();
            QPoint location = floatingSizeButton->mapToGlobal(
                QPoint(0, floatingSizeButton->height() + 7));
            QScreen *targetScreen = QGuiApplication::screenAt(location);
            if (!targetScreen)
                targetScreen = QGuiApplication::primaryScreen();
            if (targetScreen) {
                const QRect area = targetScreen->availableGeometry();
                if (location.x() + floatingSizePopup->width() > area.right())
                    location.setX(area.right() - floatingSizePopup->width() + 1);
                if (location.y() + floatingSizePopup->height() > area.bottom())
                    location.setY(floatingSizeButton->mapToGlobal(QPoint(0, 0)).y()
                                  - floatingSizePopup->height() - 7);
                location.setX(qMax(area.left(), location.x()));
                location.setY(qMax(area.top(), location.y()));
            }
            floatingSizePopup->move(location);
            floatingSizePopup->show();
            floatingSizePopup->raise();
        });

        floatingStroke = makeButton("", "Stroke colour / opacity", 48);
        floatingStroke->setIconSize(QSize(34, 34));
        floatingStroke->setIcon(colorSwatchIcon(strokeColor, 34));
        floatingFill = makeButton("", "Fill colour / opacity", 48);
        floatingFill->setIconSize(QSize(34, 34));
        floatingFill->setIcon(colorSwatchIcon(fillColor, 34));
        connect(floatingStroke, &QPushButton::clicked, strokeColorBtn, &QPushButton::click);
        connect(floatingFill, &QPushButton::clicked, fillColorBtn, &QPushButton::click);

        floatingTransparency = makeButton("◩", "Transparent background / PNG alpha");
        floatingTransparency->setCheckable(true);
        connect(floatingTransparency, &QPushButton::toggled,
                transparentCheck, &QCheckBox::setChecked);
        floatingDarkBackground = makeButton("☾", "Dark canvas background / dark PNG export");
        floatingDarkBackground->setCheckable(true);
        connect(floatingDarkBackground, &QPushButton::toggled,
                btnDarkBackground, &QPushButton::setChecked);
        connect(btnDarkBackground, &QPushButton::toggled,
                floatingDarkBackground, &QPushButton::setChecked);

        auto *clear = makeButton("🗑", "Clear");
        auto *open = makeButton("📂", "Open image");
        auto *save = makeButton("💾", "Save PNG");
        auto *fit = makeButton("⤢", "Fit to screen");
        auto *exit = makeButton("⛶", "Exit fullscreen (F11 / Esc)");
        connect(clear, &QPushButton::clicked, btnClear, &QPushButton::click);
        connect(open, &QPushButton::clicked, btnOpen, &QPushButton::click);
        connect(save, &QPushButton::clicked, btnSave, &QPushButton::click);
        connect(fit, &QPushButton::clicked, btnFit, &QPushButton::click);
        connect(exit, &QPushButton::clicked, this, [this]() { toggleCanvasFullscreen(); });

        // Fit the complete row to the available screen width, with no scrollbar.
        keepFloatingMenuVisible();
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
