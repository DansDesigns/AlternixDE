#include <QApplication>
#include <QMainWindow>
#include <QWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QSlider>
#include <QLabel>
#include <QStatusBar>
#include <QColorDialog>
#include <QFileDialog>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QImage>
#include <QStack>
#include <QResizeEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QDir>
#include <QInputDialog>
#include <QVariant>
#include <QtMath>
#include <QFont>
#include <QVector>
#include <QFile>
#include <QTextStream>
#include <QLineEdit>
#include <QGestureEvent>
#include <QPinchGesture>

// ─────────────────────────────────────────────
// Drawing Canvas for a single note (256x256)
// ─────────────────────────────────────────────

class DrawingCanvas : public QWidget {
public:
    enum class Tool {
        None,
        Pen,
        Eraser,
        Line,
        Text,
        Select
    };

    explicit DrawingCanvas(QWidget *parent = nullptr)
        : QWidget(parent),
          m_tool(Tool::Pen),
          m_penColor(Qt::black),
          m_bgColor(QColor("#ffff7f")),           // note background color
          m_penSize(5),
          m_drawing(false),
          m_showPreview(false),
          m_zoomFactor(1.0),
          m_panActive(false),
          m_viewOffset(0, 0),
          m_isActive(false)
    {
        setAttribute(Qt::WA_StaticContents, true);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        setMouseTracking(true);

        // Fixed internal canvas size: 256x256
        QSize initialSize(256, 256);
        m_image = QImage(initialSize, QImage::Format_ARGB32_Premultiplied);
        // Store strokes on a transparent layer; bg is separate
        m_image.fill(Qt::transparent);
    }

    // ───── public API ─────

    void setTool(Tool t) {
        m_tool = t;
        m_showPreview = false;
        if (m_tool == Tool::Select)
            setCursor(Qt::ArrowCursor);
        else
            unsetCursor();
        update();
    }

    void setPenColor(const QColor &c) {
        m_penColor = c;
    }

    void setBackgroundColor(const QColor &c) {
        m_bgColor = c;
        update();
    }

    QColor backgroundColor() const { return m_bgColor; }

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

    bool saveLayerPng(const QString &path) const {
        if (path.isEmpty() || m_image.isNull())
            return false;
        return m_image.save(path, "PNG");
    }

    bool loadLayerPng(const QString &path) {
        if (path.isEmpty())
            return false;
        QImage loaded;
        if (!loaded.load(path))
            return false;
        // Ensure correct size
        if (loaded.size() != QSize(256, 256)) {
            loaded = loaded.scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        m_image = loaded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        update();
        return true;
    }

    // Composed image (bg + strokes) for export
    QImage composedImage() const {
        QImage out(m_image.size(), QImage::Format_ARGB32_Premultiplied);
        out.fill(m_bgColor);
        QPainter p(&out);
        p.drawImage(0, 0, m_image);
        p.end();
        return out;
    }

    bool savePngComposed(const QString &path) const {
        if (path.isEmpty() || m_image.isNull())
            return false;
        QImage out = composedImage();
        return out.save(path, "PNG");
    }

    void setZoom(double factor) {
        m_zoomFactor = qBound(0.25, factor, 4.0);
        centerCanvas();
        update();
    }

    double zoom() const { return m_zoomFactor; }

    void fitToWidget() {
        if (m_image.isNull())
            return;

        QRect innerRect = paintInnerRect();
        QSizeF imgSize = m_image.size();
        QSizeF widgetSize = innerRect.size();

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

    void setActivatedCallback(const std::function<void(DrawingCanvas*)> &cb) {
        m_activatedCallback = cb;
    }

    void setActive(bool active) {
        m_isActive = active;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        Q_UNUSED(event);
        QPainter p(this);

        // App background
        p.fillRect(rect(), QColor("#282828"));

        // Border
        QRectF borderRect = rect().adjusted(2, 2, -2, -2);
        QPen borderPen(m_isActive ? QColor("#4da3ff") : QColor("#808080"));
        borderPen.setWidth(2);
        p.setPen(borderPen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(borderRect, 12, 12);

        if (m_image.isNull())
            return;

        QRect innerRect = paintInnerRect();

        p.save();
        p.setClipRect(innerRect);

        // Paint area inside card
        p.translate(innerRect.topLeft() + m_viewOffset);
        p.scale(m_zoomFactor, m_zoomFactor);

        QRect imageRect(QPoint(0, 0), m_image.size());
        p.fillRect(imageRect, m_bgColor);
        p.drawImage(0, 0, m_image);

        // Line preview
        if (m_showPreview && (m_tool == Tool::Line)) {
            QPen pen(m_penColor, m_penSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawLine(m_startPoint, m_currentPoint);
        }

        p.restore();
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        centerCanvas();
    }

    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton)
            return;

        // Notify main window that this note is active or to delete it (depending on mode)
        if (m_activatedCallback) {
            m_activatedCallback(this);
        }

        if (m_tool == Tool::Select) {
            // Selection only, no drawing
            return;
        }

        if (m_image.isNull())
            return;

        QPoint imgPos = widgetToImage(e->pos());
        clampPointToImage(imgPos);

        // Text tool: click = place text
        if (m_tool == Tool::Text) {
            QInputDialog dlg(this);
            dlg.setInputMode(QInputDialog::TextInput);
            dlg.setWindowTitle("Note text");
            dlg.setLabelText("Enter text:");

            if (QLineEdit *edit = dlg.findChild<QLineEdit*>()) {
                edit->setStyleSheet(
                    "QLineEdit { color:white; background-color:#202020; }"
                );
            }

            if (dlg.exec() != QDialog::Accepted)
                return;

            QString text = dlg.textValue();
            if (text.isEmpty())
                return;

            pushUndo();
            m_redoStack.clear();

            QPainter p(&m_image);
            p.setRenderHint(QPainter::TextAntialiasing, true);
            QPen pen(m_penColor);
            p.setPen(pen);

            QFont font = p.font();
            font.setPointSize(16);
            p.setFont(font);

            p.drawText(imgPos, text);
            p.end();

            update();
            return;
        }

        // Drawing tools
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
        } else if (m_tool == Tool::Line) {
            m_showPreview = true;
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if (!m_drawing || m_image.isNull())
            return;

        QPoint imgPos = widgetToImage(e->pos());
        clampPointToImage(imgPos);

        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) {
            drawLineOnImage(m_lastPoint, imgPos);
            m_lastPoint = imgPos;
        } else if (m_tool == Tool::Line) {
            m_currentPoint = imgPos;
            m_showPreview = true;
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton)
            return;

        if (!m_drawing || m_image.isNull())
            return;

        QPoint imgPos = widgetToImage(e->pos());
        clampPointToImage(imgPos);

        if (m_tool == Tool::Pen || m_tool == Tool::Eraser) {
            drawLineOnImage(m_lastPoint, imgPos);
        } else if (m_tool == Tool::Line) {
            QPainter p(&m_image);
            QPen pen(m_penColor, m_penSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.drawLine(m_startPoint, imgPos);
            p.end();
            update();
        }

        m_drawing = false;
        m_showPreview = false;
    }

private:
    Tool   m_tool;
    QColor m_penColor;
    QColor m_bgColor;
    int    m_penSize;

    QImage m_image;
    bool   m_drawing;
    bool   m_showPreview;

    double m_zoomFactor;

    QPoint m_startPoint;
    QPoint m_lastPoint;
    QPoint m_currentPoint;

    bool    m_panActive;   // unused now but kept for simplicity
    QPoint  m_panLastPos;  // unused
    QPointF m_viewOffset;

    QStack<QImage> m_undoStack;
    QStack<QImage> m_redoStack;

    std::function<void(DrawingCanvas*)> m_activatedCallback;
    bool m_isActive;

    void pushUndo() {
        if (!m_image.isNull()) {
            m_undoStack.push(m_image);  // implicit sharing
        }
    }

    void clampPointToImage(QPoint &pt) {
        pt.setX(qBound(0, pt.x(), m_image.width() - 1));
        pt.setY(qBound(0, pt.y(), m_image.height() - 1));
    }

    QRect paintInnerRect() const {
        // Area inside the rounded card used for the note
        return rect().adjusted(10, 10, -10, -10);
    }

    QPoint widgetToImage(const QPoint &widgetPos) const {
        if (m_zoomFactor <= 0.0)
            return widgetPos;

        QRect innerRect = paintInnerRect();
        QPointF local = widgetPos - innerRect.topLeft() - m_viewOffset;
        QPointF pf = local / m_zoomFactor;
        return QPoint(int(pf.x()), int(pf.y()));
    }

    QRect imageRectToWidgetRect(const QRect &imgRect) const {
        QRect innerRect = paintInnerRect();
        QPointF topLeft = imgRect.topLeft() * m_zoomFactor + m_viewOffset + innerRect.topLeft();
        QSizeF size = imgRect.size() * m_zoomFactor;
        return QRect(topLeft.toPoint(), size.toSize());
    }

    void drawLineOnImage(const QPoint &from, const QPoint &to) {
        QPainter p(&m_image);

        if (m_tool == Tool::Eraser) {
            // Erase to transparency so bg color shows through
            p.setCompositionMode(QPainter::CompositionMode_Clear);
            QPen pen(Qt::transparent, m_penSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
        } else {
            QPen pen(m_penColor, m_penSize, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
        }

        p.drawLine(from, to);
        p.end();

        QRect dirtyImgRect = QRect(from, to).normalized()
                                 .adjusted(-m_penSize, -m_penSize,
                                           m_penSize, m_penSize);
        QRect dirtyWidgetRect = imageRectToWidgetRect(dirtyImgRect);
        update(dirtyWidgetRect);
    }

    void centerCanvas() {
        if (m_image.isNull())
            return;

        QRect innerRect = paintInnerRect();
        QSizeF imgSize = m_image.size() * m_zoomFactor;
        QSizeF widgetSize = innerRect.size();

        QPointF offset(
            (widgetSize.width()  - imgSize.width())  / 2.0,
            (widgetSize.height() - imgSize.height()) / 2.0
        );
        m_viewOffset = offset;
    }
};

// ─────────────────────────────────────────────
// Main Window (OSM Notes)
// ─────────────────────────────────────────────

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("📝Notes");

        // Enable pinch gestures on this window
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

        baseBtnStyle =
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
        btnPen->setStyleSheet(baseBtnStyle);
        btnPen->setFixedHeight(46);
        leftGroup->addWidget(btnPen);

        btnText = new QPushButton("📝 Text", this);
        btnText->setStyleSheet(baseBtnStyle);
        btnText->setFixedHeight(46);
        leftGroup->addWidget(btnText);

        btnEraser = new QPushButton("🧽 Erase", this);
        btnEraser->setStyleSheet(baseBtnStyle);
        btnEraser->setFixedHeight(46);
        leftGroup->addWidget(btnEraser);

        btnLine = new QPushButton("📏 Line", this);
        btnLine->setStyleSheet(baseBtnStyle);
        btnLine->setFixedHeight(46);
        leftGroup->addWidget(btnLine);

        btnSelect = new QPushButton("🖱 Select", this);
        btnSelect->setStyleSheet(baseBtnStyle);
        btnSelect->setFixedHeight(46);
        leftGroup->addWidget(btnSelect);

        QHBoxLayout *rightGroup = new QHBoxLayout();
        rightGroup->setSpacing(6);

        btnUndo = new QPushButton("↩ Undo", this);
        btnUndo->setStyleSheet(baseBtnStyle);
        btnUndo->setFixedHeight(46);
        rightGroup->addWidget(btnUndo);

        btnRedo = new QPushButton("↪ Redo", this);
        btnRedo->setStyleSheet(baseBtnStyle);
        btnRedo->setFixedHeight(46);
        rightGroup->addWidget(btnRedo);

        btnClear = new QPushButton("🗑 Clear", this);
        btnClear->setStyleSheet(baseBtnStyle);
        btnClear->setFixedHeight(46);
        rightGroup->addWidget(btnClear);

        btnSave = new QPushButton("💾 Save", this);          // saves app state
        btnSave->setStyleSheet(baseBtnStyle);
        btnSave->setFixedHeight(46);
        rightGroup->addWidget(btnSave);

        btnExport = new QPushButton("⇩ Export PNG", this);   // exports active note
        btnExport->setStyleSheet(baseBtnStyle);
        btnExport->setFixedHeight(46);
        rightGroup->addWidget(btnExport);

        topBar->addLayout(leftGroup);
        topBar->addStretch(1);
        topBar->addLayout(rightGroup);

        mainLayout->addLayout(topBar);

        // ─────────────────────────────────
        // SIZE + COLOR BAR + "+/- Note"
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

        QLabel *strokeLabel = new QLabel("Ink:", this);
        strokeLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        controlBar->addWidget(strokeLabel);

        strokeColorBtn = new QPushButton(this);
        strokeColorBtn->setFixedSize(32, 32);
        strokeColorBtn->setStyleSheet(colorButtonStyle(QColor(Qt::black)));
        controlBar->addWidget(strokeColorBtn);

        QLabel *bgLabel = new QLabel("Note BG:", this);
        bgLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        controlBar->addWidget(bgLabel);

        bgColorBtn = new QPushButton(this);
        bgColorBtn->setFixedSize(32, 32);
        bgColorBtn->setStyleSheet(colorButtonStyle(QColor("#ffff7f")));
        controlBar->addWidget(bgColorBtn);

        controlBar->addStretch(1);

        btnNewNote = new QPushButton("+ Note", this);
        btnNewNote->setStyleSheet(baseBtnStyle);
        btnNewNote->setFixedHeight(38);
        controlBar->addWidget(btnNewNote);

        btnDelNote = new QPushButton("- Note", this);
        btnDelNote->setStyleSheet(baseBtnStyle);
        btnDelNote->setFixedHeight(38);
        controlBar->addWidget(btnDelNote);

        mainLayout->addLayout(controlBar);

        // ─────────────────────────────────
        // CANVAS CONTAINER: grid of notes
        // ─────────────────────────────────
        canvasContainer = new QWidget(this);
        canvasContainer->setStyleSheet("background:#282828;");
        canvasLayout = new QGridLayout(canvasContainer);
        canvasLayout->setContentsMargins(32, 16, 32, 16);
        canvasLayout->setSpacing(24);

        mainLayout->addWidget(canvasContainer, 1);

        // No bottom zoom slider / Fit anymore

        // ─────────────────────────────────
        // STATUSBAR
        // ─────────────────────────────────
        QStatusBar *sb = new QStatusBar(this);
        sb->setStyleSheet("QStatusBar { background:#282828; color:white; font-size:16px; }");
        setStatusBar(sb);
        statusBar()->showMessage("Ready");

        // Either load previous state or start with a single note
        if (!loadState()) {
            addNoteCanvas(true);
        }
        applyZoomToNotes(); // ensure initial zoom propagated

        // ─────────────────────────────────
        // CONNECTIONS
        // ─────────────────────────────────
        connect(btnPen, &QPushButton::clicked, this, [this]() {
            if (!currentCanvas) return;
            currentCanvas->setTool(DrawingCanvas::Tool::Pen);
            statusBar()->showMessage("Tool: Pen");
        });
        connect(btnText, &QPushButton::clicked, this, [this]() {
            if (!currentCanvas) return;
            currentCanvas->setTool(DrawingCanvas::Tool::Text);
            statusBar()->showMessage("Tool: Text (click to place)");
        });
        connect(btnEraser, &QPushButton::clicked, this, [this]() {
            if (!currentCanvas) return;
            currentCanvas->setTool(DrawingCanvas::Tool::Eraser);
            statusBar()->showMessage("Tool: Eraser");
        });
        connect(btnLine, &QPushButton::clicked, this, [this]() {
            if (!currentCanvas) return;
            currentCanvas->setTool(DrawingCanvas::Tool::Line);
            statusBar()->showMessage("Tool: Line");
        });
        connect(btnSelect, &QPushButton::clicked, this, [this]() {
            if (!currentCanvas) return;
            currentCanvas->setTool(DrawingCanvas::Tool::Select);
            statusBar()->showMessage("Tool: Select (click notes)");
        });

        connect(btnClear, &QPushButton::clicked, this, [this]() {
            if (!currentCanvas) return;
            currentCanvas->clearCanvas();
            statusBar()->showMessage("Note cleared");
        });

        // Save whole app state
        connect(btnSave, &QPushButton::clicked, this, [this]() {
            saveState();
            statusBar()->showMessage("Notes saved", 2000);
        });

        // Export active note as PNG
        connect(btnExport, &QPushButton::clicked, this, [this]() {
            if (!currentCanvas) return;
            QString path = QFileDialog::getSaveFileName(
                this, "Export Note PNG", QDir::homePath() + "/note.png",
                "PNG Image (*.png)");
            if (!path.isEmpty()) {
                if (currentCanvas->savePngComposed(path))
                    statusBar()->showMessage("Exported: " + path, 3000);
                else
                    statusBar()->showMessage("Failed to export", 3000);
            }
        });

        connect(btnNewNote, &QPushButton::clicked, this, [this]() {
            addNoteCanvas(true);
            statusBar()->showMessage("New note created", 1500);
        });

        connect(btnDelNote, &QPushButton::clicked, this, [this]() {
            deleteMode = !deleteMode;
            if (deleteMode) {
                btnDelNote->setStyleSheet(
                    "QPushButton { "
                    "  background-color:#c93030; color:white; font-family:Sans;"
                    "  border-radius:6px; border:1px solid #ff8080; "
                    "  padding:6px 14px; font-size:22px; } "
                    "QPushButton:hover { background-color:#e04040; } "
                    "QPushButton:pressed { background-color:#b02020; }"
                );
                statusBar()->showMessage("Delete mode: click a note to remove it", 3000);
            } else {
                btnDelNote->setStyleSheet(baseBtnStyle);
                statusBar()->showMessage("Delete mode off", 2000);
            }
        });

        connect(sizeSlider, &QSlider::valueChanged, this, [this](int v) {
            if (currentCanvas) {
                currentCanvas->setPenSize(v);
            }
            statusBar()->showMessage(QString("Pen size: %1").arg(v), 1500);
        });

        connect(strokeColorBtn, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(Qt::black, this, "Select Ink Color");
            if (c.isValid()) {
                strokeColorBtn->setStyleSheet(colorButtonStyle(c));
                if (currentCanvas) {
                    currentCanvas->setPenColor(c);
                }
            }
        });

        connect(bgColorBtn, &QPushButton::clicked, this, [this]() {
            QColor c = QColorDialog::getColor(QColor("#ffff7f"), this, "Select Note Background");
            if (c.isValid()) {
                bgColorBtn->setStyleSheet(colorButtonStyle(c));
                if (currentCanvas) {
                    currentCanvas->setBackgroundColor(c);
                }
            }
        });

        connect(btnUndo, &QPushButton::clicked, this, [this]() {
            if (!currentCanvas) return;
            currentCanvas->undo();
            statusBar()->showMessage("Undo");
        });

        connect(btnRedo, &QPushButton::clicked, this, [this]() {
            if (!currentCanvas) return;
            currentCanvas->redo();
            statusBar()->showMessage("Redo");
        });

        // Default states for current note (if any)
        if (currentCanvas) {
            currentCanvas->setPenSize(sizeSlider->value());
            currentCanvas->setPenColor(Qt::black);
            currentCanvas->setBackgroundColor(QColor("#ffff7f"));
            currentCanvas->setTool(DrawingCanvas::Tool::Pen);
            currentCanvas->setZoom(globalZoom);
        }
    }

protected:
    bool event(QEvent *ev) override {
        if (ev->type() == QEvent::Gesture) {
            return gestureEvent(static_cast<QGestureEvent*>(ev));
        }
        return QMainWindow::event(ev);
    }

    void wheelEvent(QWheelEvent *event) override {
        // Two-finger touchpad or mouse wheel zoom for the whole grid
        int delta = event->angleDelta().y();
        if (delta == 0) {
            QMainWindow::wheelEvent(event);
            return;
        }

        int steps = delta / 120; // each notch ~120
        if (steps == 0) {
            QMainWindow::wheelEvent(event);
            return;
        }

        double factor = 1.0 + 0.1 * steps; // 10% per step
        double newZoom = globalZoom * factor;
        newZoom = qBound(0.5, newZoom, 3.0);

        if (qFuzzyCompare(newZoom, globalZoom)) {
            QMainWindow::wheelEvent(event);
            return;
        }

        globalZoom = newZoom;
        applyZoomToNotes();
        statusBar()->showMessage(
            QString("Grid zoom: %1%").arg(int(globalZoom * 100.0)),
            1000
        );
        event->accept();
    }

private:
    // Canvas management
    QWidget *canvasContainer = nullptr;
    QGridLayout *canvasLayout = nullptr;
    QVector<DrawingCanvas*> canvases;
    DrawingCanvas *currentCanvas = nullptr;

    // Controls
    QPushButton *btnPen = nullptr;
    QPushButton *btnText = nullptr;
    QPushButton *btnEraser = nullptr;
    QPushButton *btnLine = nullptr;
    QPushButton *btnSelect = nullptr;
    QPushButton *btnClear = nullptr;
    QPushButton *btnSave = nullptr;
    QPushButton *btnExport = nullptr;
    QPushButton *btnUndo = nullptr;
    QPushButton *btnRedo = nullptr;
    QPushButton *btnNewNote = nullptr;
    QPushButton *btnDelNote = nullptr;

    QSlider   *sizeSlider = nullptr;
    QPushButton *strokeColorBtn = nullptr;
    QPushButton *bgColorBtn = nullptr;

    bool deleteMode = false;
    QString baseBtnStyle;

    double globalZoom = 1.0;
    const int baseNoteSize = 256 + 40; // logical note widget size including padding

    // helpers
    bool gestureEvent(QGestureEvent *event) {
        if (QGesture *g = event->gesture(Qt::PinchGesture)) {
            auto *pinch = static_cast<QPinchGesture*>(g);

            if (pinch->state() == Qt::GestureUpdated) {
                qreal factor = pinch->scaleFactor();          // relative factor since last update
                double newZoom = globalZoom * factor;
                newZoom = qBound(0.5, newZoom, 3.0);         // same bounds as wheel zoom

                if (!qFuzzyCompare(newZoom, globalZoom)) {
                    globalZoom = newZoom;
                    applyZoomToNotes();
                    statusBar()->showMessage(
                        QString("Grid zoom: %1%").arg(int(globalZoom * 100.0)),
                        1000
                    );
                }
            }

            event->accept(g);
            return true;
        }
        return false;
    }

    DrawingCanvas* addNoteCanvas(bool makeCurrent) {
        if (!canvasLayout || !canvasContainer)
            return nullptr;

        DrawingCanvas *note = new DrawingCanvas(canvasContainer);
        note->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

        canvases.append(note);
        relayoutNotes();
        applyZoomToNotes(); // ensure new note respects current zoom

        // Activation callback so clicking a note either selects or deletes
        note->setActivatedCallback([this](DrawingCanvas *which) {
            if (deleteMode) {
                removeNoteCanvas(which);
                statusBar()->showMessage("Note removed", 1500);
            } else {
                setCurrentCanvas(which);
                statusBar()->showMessage("Active note changed", 800);
            }
        });

        if (makeCurrent) {
            setCurrentCanvas(note);
            if (sizeSlider) {
                note->setPenSize(sizeSlider->value());
            }
            note->setPenColor(Qt::black);
            note->setBackgroundColor(QColor("#ffff7f"));
            note->setTool(DrawingCanvas::Tool::Pen);
            note->setZoom(globalZoom);
        } else {
            note->setActive(false);
        }

        return note;
    }

    void relayoutNotes() {
        if (!canvasLayout) return;

        while (canvasLayout->count() > 0) {
            QLayoutItem *item = canvasLayout->takeAt(0);
            delete item;
        }

        const int NOTES_PER_ROW = 3;
        for (int i = 0; i < canvases.size(); ++i) {
            int row = i / NOTES_PER_ROW;
            int col = i % NOTES_PER_ROW;
            canvasLayout->addWidget(canvases[i], row, col, Qt::AlignCenter);
        }
    }

    void clearAllNotes() {
        for (DrawingCanvas *c : canvases) {
            canvasLayout->removeWidget(c);
            c->deleteLater();
        }
        canvases.clear();
        currentCanvas = nullptr;
    }

    void removeNoteCanvas(DrawingCanvas *note) {
        int idx = canvases.indexOf(note);
        if (idx < 0)
            return;

        canvasLayout->removeWidget(note);
        canvases.removeAt(idx);
        note->deleteLater();

        if (currentCanvas == note) {
            currentCanvas = nullptr;
            if (!canvases.isEmpty())
                currentCanvas = canvases.first();
        }

        for (DrawingCanvas *c : canvases) {
            c->setActive(c == currentCanvas);
        }

        relayoutNotes();

        // Ensure at least one note exists; remove this block if you want to allow zero.
        if (canvases.isEmpty()) {
            addNoteCanvas(true);
        }
    }

    void setCurrentCanvas(DrawingCanvas *note) {
        currentCanvas = note;
        for (DrawingCanvas *c : canvases) {
            c->setActive(c == currentCanvas);
        }
    }

    void applyZoomToNotes() {
        int s = int(baseNoteSize * globalZoom);
        if (s < 100) s = 100; // minimum safety

        for (DrawingCanvas *note : canvases) {
            note->setMinimumSize(s, s);
            note->setZoom(globalZoom);
        }
        if (canvasContainer)
            canvasContainer->updateGeometry();
    }

    QString sessionDirPath() const {
        QDir home(QDir::homePath());
        return home.filePath(".osm-notes");
    }

    void saveState() {
        QString basePath = sessionDirPath();
        QDir baseDir(basePath);
        if (!baseDir.exists()) {
            baseDir.mkpath(".");
        }

        // Save config
        QFile cfg(baseDir.filePath("session.txt"));
        if (!cfg.open(QIODevice::WriteOnly | QIODevice::Text))
            return;

        QTextStream out(&cfg);
        out << canvases.size() << '\n';
        out << globalZoom << '\n';
        for (int i = 0; i < canvases.size(); ++i) {
            DrawingCanvas *note = canvases[i];
            out << note->backgroundColor().name(QColor::HexArgb) << '\n';
        }
        cfg.close();

        // Save each note's layer
        for (int i = 0; i < canvases.size(); ++i) {
            DrawingCanvas *note = canvases[i];
            QString layerPath = baseDir.filePath(QString("note_%1.png").arg(i));
            note->saveLayerPng(layerPath);
        }
    }

    bool loadState() {
        QString basePath = sessionDirPath();
        QDir baseDir(basePath);
        if (!baseDir.exists())
            return false;

        QFile cfg(baseDir.filePath("session.txt"));
        if (!cfg.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;

        QTextStream in(&cfg);
        QString firstLine = in.readLine().trimmed();
        bool ok = false;
        int noteCount = firstLine.toInt(&ok);
        if (!ok || noteCount <= 0) {
            cfg.close();
            return false;
        }

        // Optional stored zoom (if present)
        QString zoomLine = in.readLine().trimmed();
        bool okZoom = false;
        double storedZoom = zoomLine.toDouble(&okZoom);
        if (okZoom && storedZoom > 0.0)
            globalZoom = qBound(0.5, storedZoom, 3.0);

        clearAllNotes();

        for (int i = 0; i < noteCount; ++i) {
            QString line = in.readLine().trimmed();
            QColor bg(line);
            if (!bg.isValid())
                bg = QColor("#ffff7f");

            DrawingCanvas *note = addNoteCanvas(false);
            if (!note)
                continue;

            note->setBackgroundColor(bg);

            QString layerPath = baseDir.filePath(QString("note_%1.png").arg(i));
            note->loadLayerPng(layerPath);
        }

        cfg.close();

        if (!canvases.isEmpty()) {
            setCurrentCanvas(canvases.first());
        }

        return true;
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
