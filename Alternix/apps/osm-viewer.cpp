#include <QApplication>
#include <QMainWindow>
#include <QFileDialog>
#include <QTextEdit>
#include <QScrollArea>
#include <QLabel>
#include <QStackedWidget>
#include <QMessageBox>
#include <QStatusBar>
#include <QFile>
#include <QTextStream>
#include <QFileInfo>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSlider>
#include <QGestureEvent>
#include <QPinchGesture>
#include <QFont>
#include <QPalette>
#include <QtMath>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QPainter>
#include <QScrollBar>
#include <QScroller>
#include <QTextBlock>

#include <poppler-qt5.h>

/*────────────────────────────────────────────────────────────
 * CodeEditor WITH LINE NUMBERS
 *───────────────────────────────────────────────────────────*/

class CodeEditor;

class LineNumberArea : public QWidget {
public:
    LineNumberArea(CodeEditor *editor);

    QSize sizeHint() const override {
        return QSize(40, 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    CodeEditor *codeEditor;
};

class CodeEditor : public QPlainTextEdit {
public:
    CodeEditor(QWidget *parent = nullptr) : QPlainTextEdit(parent) {

        lineNumberArea = new LineNumberArea(this);

        connect(this, &QPlainTextEdit::blockCountChanged,
                this, &CodeEditor::updateLineNumberAreaWidth);
        connect(this, &QPlainTextEdit::updateRequest,
                this, &CodeEditor::updateLineNumberArea);
        connect(this, &QPlainTextEdit::cursorPositionChanged,
                this, &CodeEditor::highlightCurrentLine);

        updateLineNumberAreaWidth(0);
        highlightCurrentLine();

        setStyleSheet("QPlainTextEdit { background-color:#282828; color:#f0f0f0; border:0px; }");
    }

    int lineNumberAreaWidth() {
        int digits = 1;
        int max = qMax(1, blockCount());
        while (max >= 10) {
            max /= 10;
            digits++;
        }
        int space = 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
        return space;
    }

    void updateLineNumberAreaWidth(int) {
        setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
    }

    void updateLineNumberArea(const QRect &rect, int dy) {
        if (dy)
            lineNumberArea->scroll(0, dy);
        else
            lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());

        if (rect.contains(viewport()->rect()))
            updateLineNumberAreaWidth(0);
    }

    void resizeEvent(QResizeEvent *e) override {
        QPlainTextEdit::resizeEvent(e);

        QRect cr = contentsRect();
        lineNumberArea->setGeometry(
            QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
    }

    void highlightCurrentLine() {
        QList<QTextEdit::ExtraSelection> extra;

        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(QColor("#333333"));
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = textCursor();
        sel.cursor.clearSelection();
        extra.append(sel);

        setExtraSelections(extra);
    }

    void lineNumberAreaPaintEvent(QPaintEvent *event) {
        QPainter painter(lineNumberArea);
        painter.fillRect(event->rect(), QColor("#202020"));

        QTextBlock block = firstVisibleBlock();
        int blockNumber = block.blockNumber();
        int top = static_cast<int>(
            blockBoundingGeometry(block).translated(contentOffset()).top());
        int bottom = top + static_cast<int>(blockBoundingRect(block).height());

        QFont font("monospace");
        font.setPointSize(14);
        painter.setFont(font);
        painter.setPen(QColor("#aaaaaa"));

        while (block.isValid() && top <= event->rect().bottom()) {
            if (block.isVisible() && bottom >= event->rect().top()) {
                QString num = QString::number(blockNumber + 1);
                painter.drawText(0, top, lineNumberArea->width() - 4,
                                 fontMetrics().height(),
                                 Qt::AlignRight, num);
            }

            block = block.next();
            top = bottom;
            bottom = top + static_cast<int>(blockBoundingRect(block).height());
            blockNumber++;
        }
    }

private:
    LineNumberArea *lineNumberArea;
};

LineNumberArea::LineNumberArea(CodeEditor *editor)
    : QWidget(editor), codeEditor(editor) {}

void LineNumberArea::paintEvent(QPaintEvent *event) {
    codeEditor->lineNumberAreaPaintEvent(event);
}

/*────────────────────────────────────────────────────────────
 * PDF WIDGET
 *───────────────────────────────────────────────────────────*/

class PDFWidget : public QLabel {
public:
    explicit PDFWidget(QWidget *parent = nullptr) : QLabel(parent) {
        setAlignment(Qt::AlignCenter);
        setScaledContents(true);
    }
};

/*────────────────────────────────────────────────────────────
 * MAIN WINDOW
 *───────────────────────────────────────────────────────────*/

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("Viewer");
        resize(1000, 700);

        grabGesture(Qt::PinchGesture);

        QWidget *central = new QWidget(this);
        setCentralWidget(central);
        central->setStyleSheet("background-color:#282828;");

        QVBoxLayout *mainLayout = new QVBoxLayout(central);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(0);

        /*──────────────────────────
         * TOP BUTTON BAR
         *──────────────────────────*/
        QString btnStyle =
            "QPushButton { "
            "  background-color:#303030; color:#f0f0f0; "
            "  border-radius:6px; border:1px solid #404040; "
            "  padding:6px 14px; font-size:18px; } "
            "QPushButton:hover { background-color:#3a3a3a; } "
            "QPushButton:pressed { background-color:#505050; } "
            "QPushButton:disabled { background-color:#1e1e1e; color:#777; }";

        QHBoxLayout *topBar = new QHBoxLayout();
        topBar->setContentsMargins(8, 6, 8, 4);
        topBar->setSpacing(6);

        QHBoxLayout *leftGroup = new QHBoxLayout();
        leftGroup->setSpacing(6);

        btnNew = new QPushButton("New", this);
        btnNew->setStyleSheet(btnStyle);
        btnNew->setFixedHeight(46);
        leftGroup->addWidget(btnNew);

        btnOpen = new QPushButton("Open", this);
        btnOpen->setStyleSheet(btnStyle);
        btnOpen->setFixedHeight(46);
        leftGroup->addWidget(btnOpen);

        btnSave = new QPushButton("Save", this);
        btnSave->setStyleSheet(btnStyle);
        btnSave->setFixedHeight(46);
        leftGroup->addWidget(btnSave);

        btnSaveAs = new QPushButton("Save As", this);
        btnSaveAs->setStyleSheet(btnStyle);
        btnSaveAs->setFixedHeight(46);
        leftGroup->addWidget(btnSaveAs);

        QHBoxLayout *rightGroup = new QHBoxLayout();
        rightGroup->setSpacing(6);

        btnUndo = new QPushButton("Undo", this);
        btnUndo->setStyleSheet(btnStyle);
        btnUndo->setFixedHeight(46);
        rightGroup->addWidget(btnUndo);

        btnRedo = new QPushButton("Redo", this);
        btnRedo->setStyleSheet(btnStyle);
        btnRedo->setFixedHeight(46);
        rightGroup->addWidget(btnRedo);

        btnCopy = new QPushButton("Copy", this);
        btnCopy->setStyleSheet(btnStyle);
        btnCopy->setFixedHeight(46);
        rightGroup->addWidget(btnCopy);

        btnCut = new QPushButton("Cut", this);
        btnCut->setStyleSheet(btnStyle);
        btnCut->setFixedHeight(46);
        rightGroup->addWidget(btnCut);

        btnPaste = new QPushButton("Paste", this);
        btnPaste->setStyleSheet(btnStyle);
        btnPaste->setFixedHeight(46);
        rightGroup->addWidget(btnPaste);

        topBar->addLayout(leftGroup);
        topBar->addStretch(1);
        topBar->addLayout(rightGroup);

        mainLayout->addLayout(topBar);

        /*──────────────────────────
         * SCALE BAR
         *──────────────────────────*/
        QHBoxLayout *scaleLayout = new QHBoxLayout();
        scaleLayout->setContentsMargins(8, 0, 8, 4);
        scaleLayout->setSpacing(8);

        scaleLabel = new QLabel("Scale:");
        scaleLabel->setStyleSheet("color:#f0f0f0; font-size:18px;");
        scaleLayout->addWidget(scaleLabel, 0);

        zoomSlider = new QSlider(Qt::Horizontal);
        zoomSlider->setRange(-100, 100);
        zoomSlider->setValue(10);
        zoomSlider->setFixedHeight(32);
        zoomSlider->setMinimumWidth(220);

        zoomSlider->setStyleSheet(
            "QSlider::groove:horizontal { height: 12px; background: #505050; border-radius: 6px; }"
            "QSlider::handle:horizontal { width: 32px; height: 32px; "
            " background-color:#ffffff; border-radius: 16px; margin: -10px 0; "
            " outline:none; border:0px solid transparent; }"
            "QSlider::handle:horizontal:pressed { background-color: #3a3a3a; border-radius: 16px; "
            " outline:none; border:0px solid transparent; }"
        );

        scaleLayout->addWidget(zoomSlider, 1);

        mainLayout->addLayout(scaleLayout);

        /*──────────────────────────
         * STACKED VIEWER
         *──────────────────────────*/
        stacked = new QStackedWidget(this);
        mainLayout->addWidget(stacked, 1);

        // TEXT VIEW
        codeEdit = new CodeEditor(this);
        stacked->addWidget(codeEdit);
        textBasePointSize = codeEdit->font().pointSizeF();

        // IMAGE VIEW
        imageLabel = new QLabel(this);
        imageLabel->setAlignment(Qt::AlignCenter);
        imageLabel->setStyleSheet("background-color:#282828;");

        imageScroll = new QScrollArea(this);
        imageScroll->setWidget(imageLabel);
        imageScroll->setWidgetResizable(true);
        imageScroll->setStyleSheet("QScrollArea { background:#282828; border:0px; }");

        // ENABLE KINETIC TOUCH SCROLL
        QScroller::grabGesture(imageScroll->viewport(), QScroller::LeftMouseButtonGesture);

        stacked->addWidget(imageScroll);

        // HTML VIEW
        htmlView = new QTextBrowser(this);
        htmlView->setStyleSheet("QTextBrowser { background:#282828; color:#f0f0f0; border:0px; }");
        stacked->addWidget(htmlView);

        // PDF VIEW
        pdfLabel = new PDFWidget(this);

        pdfScroll = new QScrollArea(this);
        pdfScroll->setWidget(pdfLabel);
        pdfScroll->setWidgetResizable(true);
        pdfScroll->setStyleSheet("QScrollArea { background:#282828; border:0px; }");
        stacked->addWidget(pdfScroll);

        // DARK STATUSBAR
        QStatusBar *sb = new QStatusBar(this);
        sb->setStyleSheet("QStatusBar { background:#282828; color:white; font-size:16px;}");
        setStatusBar(sb);

        /*──────── CONNECT BUTTONS ────────*/
        connect(btnNew, &QPushButton::clicked, this, &MainWindow::newFile);
        connect(btnOpen, &QPushButton::clicked, this, &MainWindow::openFileDialog);
        connect(btnSave, &QPushButton::clicked, this, &MainWindow::saveFile);
        connect(btnSaveAs, &QPushButton::clicked, this, &MainWindow::saveFileAs);

        connect(btnUndo, &QPushButton::clicked, this, &MainWindow::doUndo);
        connect(btnRedo, &QPushButton::clicked, this, &MainWindow::doRedo);
        connect(btnCopy, &QPushButton::clicked, this, &MainWindow::doCopy);
        connect(btnCut, &QPushButton::clicked, this, &MainWindow::doCut);
        connect(btnPaste, &QPushButton::clicked, this, &MainWindow::doPaste);

        connect(zoomSlider, &QSlider::valueChanged, this, &MainWindow::onZoomSliderChanged);

        currentMode = Mode::None;
        imageZoomFactor = 1.0;
        pdfZoomFactor  = 1.0;

        updateActions();
    }

    /*──────────────────────────────────────*/

    void openFileFromPath(const QString &path) {
        if (!path.isEmpty())
            openFile(path);
    }

protected:

    bool event(QEvent *e) override {
        if (e->type() == QEvent::Gesture)
            return handleGesture(static_cast<QGestureEvent*>(e));
        return QMainWindow::event(e);
    }

private:
    enum class Mode { None, Text, Image, HTML, PDF };

    /* UI ELEMENTS */
    QStackedWidget *stacked;
    CodeEditor     *codeEdit;

    QLabel      *imageLabel;
    QScrollArea *imageScroll;

    QTextBrowser *htmlView;

    PDFWidget   *pdfLabel;
    QScrollArea *pdfScroll;

    QLabel  *scaleLabel;
    QSlider *zoomSlider;

    QPushButton *btnNew;
    QPushButton *btnOpen;
    QPushButton *btnSave;
    QPushButton *btnSaveAs;
    QPushButton *btnUndo;
    QPushButton *btnRedo;
    QPushButton *btnCopy;
    QPushButton *btnCut;
    QPushButton *btnPaste;

    /* STATE */
    QString currentFilePath;
    Mode currentMode;

    double textBasePointSize;
    double imageZoomFactor;
    double pdfZoomFactor;
    double pinchStartZoom;

    QPixmap originalImage;

    /*──────────────────────────── Zoom math ───────────────────────────*/
    static double zoomFactorFromSlider(int v) {
        return qPow(2.0, v / 50.0);
    }

    static int sliderFromZoomFactor(double f) {
        return int(qRound(qLn(f) / qLn(2.0) * 50.0));
    }

    double currentZoom() const {
        switch (currentMode) {
        case Mode::Text:  return zoomFactorFromSlider(zoomSlider->value());
        case Mode::Image: return imageZoomFactor;
        case Mode::PDF:   return pdfZoomFactor;
        default:          return 1.0;
        }
    }

    /*──────────────────────────── Button states ───────────────────────────*/
    void updateActions() {
        bool textMode = (currentMode == Mode::Text);

        btnSave->setEnabled(textMode);
        btnSaveAs->setEnabled(textMode);
        btnUndo->setEnabled(textMode);
        btnRedo->setEnabled(textMode);
        btnCopy->setEnabled(textMode);
        btnCut->setEnabled(textMode);
        btnPaste->setEnabled(textMode);
    }

    /*──────────────────────────── File helper ───────────────────────────*/
    static QString extLower(const QString &p) {
        return QFileInfo(p).suffix().toLower();
    }

    static bool isImage(const QString &e) {
        return QStringList{"png","jpg","jpeg","bmp","gif","webp","svg"}.contains(e);
    }
    static bool isPdf(const QString &e) { return e == "pdf"; }
    static bool isHtml(const QString &e) { return e == "html" || e == "htm"; }

    static bool isText(const QString &e) {
        return QStringList{
            "txt","log","md","cpp","c","h","hpp","py","sh","bat","ini","conf",
            "json","yaml","yml","xml","csv","desktop","service","qml","js","ts"
        }.contains(e);
    }

    /*──────────────────────────── Gesture (pinch-zoom) ───────────────────────────*/
    bool handleGesture(QGestureEvent *ev) {
        if (QGesture *g = ev->gesture(Qt::PinchGesture)) {
            auto *pinch = static_cast<QPinchGesture*>(g);

            if (pinch->state() == Qt::GestureStarted)
                pinchStartZoom = currentZoom();

            double newZoom = pinchStartZoom * pinch->scaleFactor();
            newZoom = qBound(0.25, newZoom, 4.0);

            if (currentMode == Mode::Image) {
                imageZoomFactor = newZoom;

                zoomSlider->blockSignals(true);
                zoomSlider->setValue(sliderFromZoomFactor(newZoom));
                zoomSlider->blockSignals(false);

                applyImageZoom();
            }

            else if (currentMode == Mode::PDF) {
                pdfZoomFactor = newZoom;

                zoomSlider->blockSignals(true);
                zoomSlider->setValue(sliderFromZoomFactor(newZoom));
                zoomSlider->blockSignals(false);

                renderPdf(currentFilePath);
            }

            else if (currentMode == Mode::Text) {
                zoomSlider->blockSignals(true);
                zoomSlider->setValue(sliderFromZoomFactor(newZoom));
                zoomSlider->blockSignals(false);

                applyTextZoom(newZoom);
            }

            return true;
        }
        return false;
    }

    /*──────────────────────────── Open dialog ───────────────────────────*/
    void openFileDialog() {
        QString f = QFileDialog::getOpenFileName(
            this, "Open", QDir::homePath(),
            "All files (*.*);;Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.svg);;"
            "Text (*.txt *.cpp *.h *.hpp *.py *.sh *.bat *.json *.ini *.conf *.md);;"
            "PDF (*.pdf);;HTML (*.html *.htm)"
        );
        if (!f.isEmpty())
            openFile(f);
    }

    /*──────────────────────────── Open file ───────────────────────────*/
    void openFile(const QString &path) {
        QString ext = extLower(path);

        zoomSlider->blockSignals(true);
        zoomSlider->setValue(0);
        zoomSlider->blockSignals(false);

        bool opened = false;

        if (isImage(ext)) opened = openImage(path);
        else if (isPdf(ext)) opened = openPDF(path);
        else if (isHtml(ext)) opened = openHTML(path);
        else if (isText(ext)) opened = openText(path);
        else {
            if (!(opened = openText(path)))
                if (!(opened = openImage(path)))
                    if (!(opened = openPDF(path)))
                        opened = openHTML(path);
        }

        if (opened) {
            currentFilePath = path;
            setWindowTitle("OSM Viewer - " + QFileInfo(path).fileName());
            statusBar()->showMessage("Opened: " + path, 3000);
        }

        updateActions();
    }

    /*──────────────────────────── TEXT ───────────────────────────*/
    bool openText(const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;

        codeEdit->setPlainText(QString::fromUtf8(f.readAll()));
        stacked->setCurrentWidget(codeEdit);
        currentMode = Mode::Text;

        applyTextZoom(1.0);
        return true;
    }

    void applyTextZoom(double z) {
        QFont f = codeEdit->font();
        f.setPointSizeF(textBasePointSize * z);
        codeEdit->setFont(f);
    }

    bool saveFile() {
        if (currentMode != Mode::Text)
            return false;

        if (currentFilePath.isEmpty())
            return saveFileAs();

        QFile f(currentFilePath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;

        f.write(codeEdit->toPlainText().toUtf8());
        statusBar()->showMessage("Saved");
        return true;
    }

    bool saveFileAs() {
        QString f = QFileDialog::getSaveFileName(this, "Save As", currentFilePath);
        if (f.isEmpty())
            return false;

        currentFilePath = f;
        return saveFile();
    }

    void newFile() {
        codeEdit->clear();
        stacked->setCurrentWidget(codeEdit);
        currentMode = Mode::Text;
        currentFilePath.clear();
        setWindowTitle("OSM Viewer - Untitled");
        applyTextZoom(1.0);

        zoomSlider->blockSignals(true);
        zoomSlider->setValue(0);
        zoomSlider->blockSignals(false);

        updateActions();
    }

    void doUndo()  { if (currentMode == Mode::Text) codeEdit->undo(); }
    void doRedo()  { if (currentMode == Mode::Text) codeEdit->redo(); }
    void doCopy()  { if (currentMode == Mode::Text) codeEdit->copy(); }
    void doCut()   { if (currentMode == Mode::Text) codeEdit->cut(); }
    void doPaste() { if (currentMode == Mode::Text) codeEdit->paste(); }

    /*──────────────────────────── IMAGE ───────────────────────────*/
    bool openImage(const QString &path) {
        QImage img(path);
        if (img.isNull())
            return false;

        originalImage = QPixmap::fromImage(img);
        imageZoomFactor = 1.0;

        applyImageZoom();
        stacked->setCurrentWidget(imageScroll);
        currentMode = Mode::Image;
        return true;
    }

    void applyImageZoom() {
        if (originalImage.isNull())
            return;

        double z = imageZoomFactor;
        QSize newSize = originalImage.size() * z;

        imageLabel->setPixmap(originalImage.scaled(
            newSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    /*──────────────────────────── HTML ───────────────────────────*/
    bool openHTML(const QString &path) {
        htmlView->setSource(QUrl::fromLocalFile(path));
        stacked->setCurrentWidget(htmlView);
        currentMode = Mode::HTML;
        return true;
    }

    /*──────────────────────────── PDF ───────────────────────────*/
    bool openPDF(const QString &path) {
        pdfZoomFactor = 1.0;
        renderPdf(path);
        stacked->setCurrentWidget(pdfScroll);
        currentMode = Mode::PDF;
        return true;
    }

    void renderPdf(const QString &path) {
        Poppler::Document *doc = Poppler::Document::load(path);
        if (!doc)
            return;

        Poppler::Page *page = doc->page(0);
        if (!page) { delete doc; return; }

        double dpi = 96 * pdfZoomFactor;
        QImage img = page->renderToImage(dpi, dpi);

        if (!img.isNull())
            pdfLabel->setPixmap(QPixmap::fromImage(img));

        delete page;
        delete doc;
    }

    /*──────────────────────────── slider ───────────────────────────*/
    void onZoomSliderChanged(int v) {
        double z = zoomFactorFromSlider(v);

        if (currentMode == Mode::Text)
            applyTextZoom(z);

        else if (currentMode == Mode::Image) {
            imageZoomFactor = z;
            applyImageZoom();
        }

        else if (currentMode == Mode::PDF) {
            pdfZoomFactor = z;
            renderPdf(currentFilePath);
        }
    }
};

/*────────────────────────────────────────────────────────────
 * main
 *───────────────────────────────────────────────────────────*/

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    MainWindow w;

    if (argc > 1)
        w.openFileFromPath(QString::fromLocal8Bit(argv[1]));

    w.show();
    return app.exec();
}
