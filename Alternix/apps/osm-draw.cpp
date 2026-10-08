#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QStatusBar>
#include <QColorDialog>
#include <QFileDialog>
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
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
          m_viewOffset(0, 0)
    {
        // Optimised for incremental painting
        setAttribute(Qt::WA_StaticContents, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setMouseTracking(true);

        // Default canvas size
        QSize initialSize(1280, 720);
        m_image = QImage(initialSize, QImage::Format_ARGB32_Premultiplied);
        m_image.fill(m_bgColor);
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

    void setPenSize(int s) {
        m_penSize = qMax(1, s);
    }

    void clearCanvas() {
        pushUndo();
        m_redoStack.clear();
        if (!m_image.isNull()) {
            m_image.fill(m_bgColor);
        }
        update();
    }

    bool savePng(const QString &path) {
        if (path.isEmpty() || m_image.isNull())
            return false;

        QImage out(m_image.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(m_bgColor);
        QPainter p(&out);
        p.drawImage(0, 0, m_image);
        p.end();

        return out.save(path, "PNG");
    }

    void setZoom(double factor) {
        m_zoomFactor = qBound(0.25, factor, 4.0);
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

        factor = qBound(0.25, factor, 4.0);
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
        newImage.fill(m_bgColor);

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

        // Draw white "page" under the image
        QRect imageRect(QPoint(0, 0), m_image.size());
        p.fillRect(imageRect, m_bgColor);
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

        if (m_tool != Tool::None) {
            pushUndo();
            m_redoStack.clear();
        }

        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) {
            drawLineOnImage(m_lastPoint, m_lastPoint); // dot
        } else if (m_tool == Tool::Fill) {
            floodFill(imgPos, m_fillColor);
            m_drawing = false;
            update();
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
        QColor c = (m_tool == Tool::Eraser) ? m_bgColor : m_penColor;

        QPen pen(c, m_penSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.drawLine(from, to);
        p.end();

        QRect dirtyImgRect = rectFromPoints(from, to)
                                 .adjusted(-m_penSize, -m_penSize,
                                           m_penSize, m_penSize);
        QRect dirtyWidgetRect = imageRectToWidgetRect(dirtyImgRect);
        update(dirtyWidgetRect);
    }

    void floodFill(const QPoint &start, const QColor &newColor) {
        if (!QRect(0, 0, m_image.width(), m_image.height()).contains(start))
            return;

        QColor targetColor = QColor::fromRgba(m_image.pixel(start));
        if (targetColor == newColor)
            return;

        QStack<QPoint> stack;
        stack.push(start);

        const int w = m_image.width();
        const int h = m_image.height();

        while (!stack.isEmpty()) {
            QPoint pt = stack.pop();
            int x = pt.x();
            int y = pt.y();

            if (x < 0 || x >= w || y < 0 || y >= h)
                continue;

            QColor current = QColor::fromRgba(m_image.pixel(x, y));
            if (current != targetColor)
                continue;

            m_image.setPixelColor(x, y, newColor);

            stack.push(QPoint(x + 1, y));
            stack.push(QPoint(x - 1, y));
            stack.push(QPoint(x, y + 1));
            stack.push(QPoint(x, y - 1));
        }
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
        setWindowTitle("Draw");

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
            "  padding:6px 14px; font-size:22px; } "
            "QPushButton:hover { background-color:#3a3a3a; } "
            "QPushButton:pressed { background-color:#505050; } "
            "QPushButton:disabled { background-color:#1e1e1e; color:#777; }";

        // ─────────────────────────────────
        // TOP TOOLBAR
        // ─────────────────────────────────
        QHBoxLayout *topBar = new QHBoxLayout();
        topBar->setContentsMargins(8, 6, 8, 4);
        topBar->setSpacing(6);

        QHBoxLayout *leftGroup = new QHBoxLayout();
        leftGroup->setSpacing(6);

        btnPen = new QPushButton("✒ Pen", this);
        btnPen->setStyleSheet(btnStyle);
        btnPen->setFixedHeight(46);
        leftGroup->addWidget(btnPen);

        btnEraser = new QPushButton("🧽 Erase", this);
        btnEraser->setStyleSheet(btnStyle);
        btnEraser->setFixedHeight(46);
        leftGroup->addWidget(btnEraser);

        btnLine = new QPushButton("📏 Line", this);
        btnLine->setStyleSheet(btnStyle);
        btnLine->setFixedHeight(46);
        leftGroup->addWidget(btnLine);

        btnRect = new QPushButton("☐ Rect", this);
        btnRect->setStyleSheet(btnStyle);
        btnRect->setFixedHeight(46);
        leftGroup->addWidget(btnRect);

        btnEllipse = new QPushButton("◯ Ellipse", this);
        btnEllipse->setStyleSheet(btnStyle);
        btnEllipse->setFixedHeight(46);
        leftGroup->addWidget(btnEllipse);

        btnFill = new QPushButton("🌢 Fill", this);
        btnFill->setStyleSheet(btnStyle);
        btnFill->setFixedHeight(46);
        leftGroup->addWidget(btnFill);

        btnGrab = new QPushButton("👋 Grab", this);
        btnGrab->setStyleSheet(btnStyle);
        btnGrab->setFixedHeight(46);
        leftGroup->addWidget(btnGrab);

        QHBoxLayout *rightGroup = new QHBoxLayout();
        rightGroup->setSpacing(6);

        btnUndo = new QPushButton("↩ Undo", this);
        btnUndo->setStyleSheet(btnStyle);
        btnUndo->setFixedHeight(46);
        rightGroup->addWidget(btnUndo);

        btnRedo = new QPushButton("↪ Redo", this);
        btnRedo->setStyleSheet(btnStyle);
        btnRedo->setFixedHeight(46);
        rightGroup->addWidget(btnRedo);

        btnClear = new QPushButton("🗑 Clear", this);
        btnClear->setStyleSheet(btnStyle);
        btnClear->setFixedHeight(46);
        rightGroup->addWidget(btnClear);

        btnSave = new QPushButton("💾 Save PNG", this);
        btnSave->setStyleSheet(btnStyle);
        btnSave->setFixedHeight(46);
        rightGroup->addWidget(btnSave);

        topBar->addLayout(leftGroup);
        topBar->addStretch(1);
        topBar->addLayout(rightGroup);

        mainLayout->addLayout(topBar);

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
        sizeSlider->setMinimumWidth(220);
        sizeSlider->setStyleSheet(
            "QSlider::groove:horizontal { height: 12px; background: #505050; border-radius: 6px; }"
            "QSlider::handle:horizontal { width: 32px; height: 32px; "
            " background-color:#ffffff; border-radius: 16px; margin: -10px 0; "
            " outline:none; border:0px solid transparent; }"
            "QSlider::handle:horizontal:pressed { background-color: #3a3a3a; border-radius: 16px; "
            " outline:none; border:0px solid transparent; }"
        );
        controlBar->addWidget(sizeSlider, 1);

        QLabel *strokeLabel = new QLabel("Stroke:", this);
        strokeLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        controlBar->addWidget(strokeLabel);

        strokeColorBtn = new QPushButton(this);
        strokeColorBtn->setFixedSize(32, 32);
        strokeColorBtn->setStyleSheet(colorButtonStyle(QColor(Qt::black)));
        controlBar->addWidget(strokeColorBtn);

        QLabel *fillLabel = new QLabel("Fill:", this);
        fillLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        controlBar->addWidget(fillLabel);

        fillColorBtn = new QPushButton(this);
        fillColorBtn->setFixedSize(32, 32);
        fillColorBtn->setStyleSheet(colorButtonStyle(QColor(Qt::red)));
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

        controlBar->addStretch(1);

        mainLayout->addLayout(controlBar);

        // ─────────────────────────────────
        // CANVAS CONTAINER
        // ─────────────────────────────────
        QWidget *canvasContainer = new QWidget(this);
        canvasContainer->setStyleSheet("background:#282828;");
        QVBoxLayout *canvasLayout = new QVBoxLayout(canvasContainer);
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
        zoomSlider->setRange(25, 400);
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

        btnFit = new QPushButton("Fit", this);
        btnFit->setStyleSheet(btnStyle);
        btnFit->setFixedHeight(32);
        zoomLayout->addWidget(btnFit);

        mainLayout->addLayout(zoomLayout);

        // ─────────────────────────────────
        // STATUSBAR
        // ─────────────────────────────────
        QStatusBar *sb = new QStatusBar(this);
        sb->setStyleSheet("QStatusBar { background:#282828; color:white; font-size:16px; }");
        setStatusBar(sb);
        statusBar()->showMessage("Ready");

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

        connect(btnSave, &QPushButton::clicked, this, [this]() {
            QString path = QFileDialog::getSaveFileName(
                this, "Save PNG", QDir::homePath() + "/drawing.png",
                "PNG Image (*.png)");
            if (!path.isEmpty()) {
                if (canvas->savePng(path))
                    statusBar()->showMessage("Saved: " + path, 3000);
                else
                    statusBar()->showMessage("Failed to save", 3000);
            }
        });

        connect(sizeSlider, &QSlider::valueChanged, this, [this](int v) {
            canvas->setPenSize(v);
            statusBar()->showMessage(QString("Pen size: %1").arg(v), 1500);
        });

        connect(strokeColorBtn, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(Qt::black, this, "Select Stroke Color");
            if (c.isValid()) {
                canvas->setPenColor(c);
                strokeColorBtn->setStyleSheet(colorButtonStyle(c));
            }
        });

        connect(fillColorBtn, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(Qt::red, this, "Select Fill Color");
            if (c.isValid()) {
                canvas->setFillColor(c);
                fillColorBtn->setStyleSheet(colorButtonStyle(c));
            }
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
            v = qBound(25, v, 400);
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
        canvas->setPenColor(Qt::black);
        canvas->setFillColor(Qt::red);
        canvas->setTool(DrawingCanvas::Tool::Pen);
        canvas->setZoom(1.0);
    }

protected:
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
    QPushButton *btnSave;
    QPushButton *btnUndo;
    QPushButton *btnRedo;
    QPushButton *btnFit;

    QSlider   *sizeSlider;
    QPushButton *strokeColorBtn;
    QPushButton *fillColorBtn;
    QComboBox *sizePresetBox;

    QSlider *zoomSlider;
    QLabel  *zoomValueLabel;

    bool gestureEvent(QGestureEvent *event) {
        if (QGesture *g = event->gesture(Qt::PinchGesture)) {
            auto *pinch = static_cast<QPinchGesture*>(g);

            if (pinch->state() == Qt::GestureUpdated) {
                qreal factor = pinch->scaleFactor();
                double currentZoom = canvas->zoom();
                double newZoom = qBound(0.25, currentZoom * factor, 4.0);

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

    static QString colorButtonStyle(const QColor &c) {
        return QString(
                   "QPushButton {"
                   " border-radius:4px; border:2px solid #f0f0f0;"
                   " background-color:%1;"
                   "}"
                   "QPushButton:hover { border:2px solid #ffffff; }")
            .arg(c.name());
    }
};

// ─────────────────────────────────────────────
// main
// ─────────────────────────────────────────────

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    MainWindow w;
    w.show();

    return app.exec();
}
