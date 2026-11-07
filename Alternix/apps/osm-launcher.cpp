#include <QApplication>
#include <QWidget>
#include <QScrollArea>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QDirIterator>
#include <QSettings>
#include <QIcon>
#include <QPixmap>
#include <QProcess>
#include <QDebug>
#include <QFrame>
#include <QFileInfo>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QScroller>
#include <QScrollerProperties>
#include <QScreen>
#include <QPainter>
#include <QLinearGradient>
#include <QVector>
#include <QDir>
#include <QLockFile>
#include <QScrollBar>
#include <QTouchEvent>
#include <QVariantAnimation>
#include <QGraphicsEffect>
#include <QPair>
#include <QEasingCurve>
#include <QPointer>
#include <QFile>
#include <QMessageBox>
#include <QByteArray>
#include <QDataStream>
#include <QSaveFile>
#include <QSet>
#include <QElapsedTimer>
#include <QDateTime>
#include <algorithm>
#include <functional>
#include <utility>
#include <unistd.h>

// Shared cascade timings: opening now matches the previous closing speed,
// then BOTH directions have been accelerated 2x. Keep the stages synchronized
// so a row's card and its icon/label always animate at the same cadence.
namespace LauncherTiming {
constexpr int gradientMs = 28;  // formerly 55ms on close
constexpr int backdropMs = 24;  // formerly 48ms on close
constexpr int cardMs = 25;      // formerly 50ms on close
constexpr int contentMs = 26;   // formerly 52ms on close
constexpr int rowPauseMs = 2;   // formerly 3ms on close (rounded)
constexpr int buttonMs = 28;    // formerly 55ms on close
}

// ──────────────────────────────  Paint-only scale/fade for the opening cascade.
// The grid must keep its real geometry while revealing each tile. A graphics
// effect scales only the rendered card, avoiding conflicts with QGridLayout.
class LauncherRevealEffect : public QGraphicsEffect {
    qreal progress = 0.0;
    qreal initialScale = 0.30;

public:
    explicit LauncherRevealEffect(qreal scale = 0.30, QObject *parent = nullptr)
        : QGraphicsEffect(parent), initialScale(scale) {}

    void setProgress(qreal value) {
        value = qBound(qreal(0.0), value, qreal(1.0));
        if (qAbs(progress - value) < 0.0001) return;
        progress = value;
        update();
    }

protected:
    void draw(QPainter *painter) override {
        if (progress <= 0.0) return;
        const QRectF bounds = sourceBoundingRect(Qt::LogicalCoordinates);
        const qreal scale = initialScale + (1.0 - initialScale) * progress;
        painter->save();
        painter->setOpacity(progress);
        painter->translate(bounds.center());
        painter->scale(scale, scale);
        painter->translate(-bounds.center());
        drawSource(painter);
        painter->restore();
    }
};

struct LauncherRevealStep {
    QPointer<LauncherRevealEffect> effect;
    int startMs;
    int durationMs;

    LauncherRevealStep(LauncherRevealEffect *target, int start, int duration)
        : effect(target), startMs(start), durationMs(duration) {}
};

// ──────────────────────────────  Window with built-in top/bottom fade
class LauncherWindow : public QWidget {
    bool opening = false;
    // Independent alpha for the two background layers. The top/bottom edge
    // gradients appear first; the translucent fullscreen shade comes next.
    qreal gradientOpacity = 1.0;
    qreal backdropOpacity = 1.0;
    bool dismissing = false;
    QPointer<QTimer> iconLoader;
    // Load icons in exactly the order rows enter the opening cascade, not
    // in .desktop alphabetical order (which used to go top to bottom).
    std::function<void(int)> requestRowIcons;
    // Retain the live layout targets so every close action, including a
    // pull-down gesture, can reverse the currently visible opening cascade.
    QPointer<QScrollArea> animationScroll;
    QVector<QPointer<QWidget>> animationTiles;
    QPointer<QWidget> animationCloseButton;
    int animationColumns = 4;

    // Every grid row owns effects from the first frame, including rows well
    // below the scroll viewport. Previously only visibleRows() received
    // effects, so scrolling revealed untouched, static cards.
    struct RowReveal {
        QVector<QPointer<LauncherRevealEffect>> cards;
        QVector<QPointer<LauncherRevealEffect>> contents;
        bool started = false;
        bool completed = false;
    };
    QVector<RowReveal> rowReveals;
    QVector<QPointer<QVariantAnimation>> scrollRevealAnimations;

public:
    using QWidget::QWidget;

    bool isTransitioning() const { return opening || dismissing; }
    void setIconLoader(QTimer *timer) { iconLoader = timer; }
    void setRowIconLoader(std::function<void(int)> callback) {
        requestRowIcons = std::move(callback);
    }

    // Identify rows intersecting the viewport, including partially visible
    // rows. All rows are pre-initialised separately; this method only chooses
    // which rows to cascade right now.
    QVector<QVector<QWidget*>> visibleRows() const {
        QVector<QVector<QWidget*>> rows;
        if (!animationScroll || animationColumns < 1) return rows;
        rows.resize((animationTiles.size() + animationColumns - 1) / animationColumns);

        QWidget *viewport = animationScroll->viewport();
        const QRect viewportRect = viewport->rect();
        for (int index = 0; index < animationTiles.size(); ++index) {
            QWidget *tile = animationTiles.at(index);
            if (!tile || tile->width() <= 0 || tile->height() <= 0) continue;
            // Map through the widget hierarchy instead of assuming the
            // scrollbar value equals the tile's pixel offset at startup.
            const QRect onScreen(tile->mapTo(viewport, QPoint(0, 0)), tile->size());
            if (onScreen.intersects(viewportRect))
                rows[index / animationColumns].append(tile);
        }
        return rows;
    }

    // One row always animates in two distinct stages: cards first, then icons
    // and labels. The row is never excluded merely because it is offscreen.
    void queueRowReveal(int row, int startMs, int cardMs, int contentMs,
                        QVector<LauncherRevealStep> &steps) {
        if (row < 0 || row >= rowReveals.size()) return;
        RowReveal &state = rowReveals[row];
        if (state.started) return;
        state.started = true;
        // Called bottom-to-top by the same loops that schedule the card fades.
        // Icons are decoded by the lazy timer while the row's card/background
        // stages are animating, so the icon/label stage has actual pixmaps.
        if (requestRowIcons) requestRowIcons(row);
        for (const auto &effect : state.cards)
            if (effect) steps.append(LauncherRevealStep(effect, startMs, cardMs));
        for (const auto &effect : state.contents)
            if (effect) steps.append(LauncherRevealStep(effect, startMs + cardMs, contentMs));
    }

    void finishRowReveal(int row) {
        if (row < 0 || row >= rowReveals.size() || rowReveals[row].completed)
            return;
        rowReveals[row].completed = true;
        // Remove effects once the row is revealed; there is no need to keep
        // repainting ordinary cards while the user scrolls.
        const int begin = row * animationColumns;
        const int end = qMin(begin + animationColumns, animationTiles.size());
        for (int index = begin; index < end; ++index) {
            QWidget *tile = animationTiles.at(index);
            if (!tile) continue;
            for (QLabel *label : tile->findChildren<QLabel*>())
                label->setGraphicsEffect(nullptr);
            tile->setGraphicsEffect(nullptr);
        }
        rowReveals[row].cards.clear();
        rowReveals[row].contents.clear();
    }

    // When the user scrolls to further rows, cascade all newly exposed rows
    // from bottom to top. Effects were already installed before first paint,
    // so none can suddenly appear fully opaque when the scrollbar moves.
    void revealVisiblePendingRows() {
        if (isTransitioning() || !animationScroll) return;
        const QVector<QVector<QWidget*>> rows = visibleRows();
        QVector<int> pending;
        for (int row = rows.size() - 1; row >= 0; --row)
            if (!rows.at(row).isEmpty() && row < rowReveals.size() &&
                !rowReveals[row].started)
                pending.append(row);
        if (pending.isEmpty()) return;

        constexpr int cardMs = LauncherTiming::cardMs;
        constexpr int contentMs = LauncherTiming::contentMs;
        constexpr int pauseMs = LauncherTiming::rowPauseMs;
        QVector<LauncherRevealStep> steps;
        for (int i = 0; i < pending.size(); ++i)
            queueRowReveal(pending[i], i * (cardMs + contentMs + pauseMs),
                           cardMs, contentMs, steps);

        const int totalMs = (pending.size() - 1) * (cardMs + contentMs + pauseMs)
                          + cardMs + contentMs;
        auto *animation = new QVariantAnimation(this);
        scrollRevealAnimations.append(animation);
        animation->setDuration(totalMs);
        animation->setStartValue(0.0);
        animation->setEndValue(double(totalMs));
        animation->setEasingCurve(QEasingCurve::Linear);
        connect(animation, &QVariantAnimation::valueChanged, this,
                [steps](const QVariant &value) {
            const qreal elapsed = value.toDouble();
            for (const LauncherRevealStep &step : steps) {
                if (!step.effect) continue;
                const qreal t = qBound(qreal(0.0),
                    (elapsed - step.startMs) / step.durationMs, qreal(1.0));
                step.effect->setProgress(1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t));
            }
        });
        connect(animation, &QVariantAnimation::finished, this,
                [this, pending, animation]() {
            for (int row : pending) finishRowReveal(row);
            scrollRevealAnimations.removeAll(QPointer<QVariantAnimation>(animation));
            revealVisiblePendingRows(); // Catch rows exposed during the cascade.
        });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

    // Opening: prepare ALL rows before first paint, then wait for Qt/Qtile to
    // finish the fullscreen layout BEFORE deciding which row is lowest visible.
    // Choosing immediately after showFullScreen() captured the temporary viewport
    // height, frequently making row 3 look like the bottom of the screen.
    void openWithAnimation(QScrollArea *scroll, const QVector<QWidget*> &tiles,
                           int columns, QWidget *closeButton) {
        if (isTransitioning() || !scroll || columns < 1) return;

        animationScroll = scroll;
        animationColumns = columns;
        animationCloseButton = closeButton;
        animationTiles.clear();
        animationTiles.reserve(tiles.size());
        for (QWidget *tile : tiles) animationTiles.append(tile);

        // Install hidden-scale effects on EVERY row before the window can paint.
        // This includes rows that will only become visible after scrolling.
        rowReveals.clear();
        rowReveals.resize((animationTiles.size() + animationColumns - 1) /
                          animationColumns);
        for (int index = 0; index < animationTiles.size(); ++index) {
            QWidget *tile = animationTiles.at(index);
            if (!tile) continue;
            RowReveal &row = rowReveals[index / animationColumns];
            auto *cardEffect = new LauncherRevealEffect(0.30, tile);
            tile->setGraphicsEffect(cardEffect);
            row.cards.append(cardEffect);
            for (QLabel *label : tile->findChildren<QLabel*>()) {
                auto *contentEffect = new LauncherRevealEffect(0.85, label);
                label->setGraphicsEffect(contentEffect);
                row.contents.append(contentEffect);
            }
        }

        // The close button is always last, after the whole visible cascade.
        if (closeButton) {
            auto *effect = new LauncherRevealEffect(0.75, closeButton);
            closeButton->setGraphicsEffect(effect);
        }

        opening = true;
        gradientOpacity = 0.0;
        backdropOpacity = 0.0;
        update();

        // Keep the delayed reveal for rows initially outside the viewport.
        connect(scroll->verticalScrollBar(), &QScrollBar::valueChanged,
                this, [this](int) { revealVisiblePendingRows(); });
        connect(scroll->verticalScrollBar(), &QScrollBar::rangeChanged,
                this, [this](int, int) { revealVisiblePendingRows(); });

        // Let fullscreen Show/Resize/LayoutRequest events settle. The first
        // animation tick must query the ACTUAL scroll viewport, never its
        // pre-event-loop geometry. All tiles remain invisible until then.
        QTimer::singleShot(0, this, [this]() { beginOpeningCascade(0); });
    }

    void beginOpeningCascade(int layoutChecks) {
        if (!opening || !animationScroll) return;
        if (layout()) layout()->activate();
        QWidget *content = animationScroll->widget();
        if (content && content->layout()) content->layout()->activate();

        // Some X11/Qtile layouts need an extra frame to apply fullscreen size.
        // A 3-row temporary viewport must not determine a 5-row cascade.
        const int viewportH = animationScroll->viewport()->height();
        const QRect screenRect = QApplication::primaryScreen()->geometry();
        const bool fullScreenReady = width() >= screenRect.width() - 8 &&
                                     height() >= screenRect.height() - 8;
        if (layoutChecks < 8 && (!fullScreenReady || viewportH <= 0 ||
                                 viewportH < height() - 200)) {
            QTimer::singleShot(16, this, [this, layoutChecks]() {
                beginOpeningCascade(layoutChecks + 1);
            });
            return;
        }

        constexpr int gradientMs = LauncherTiming::gradientMs;
        constexpr int backdropMs = LauncherTiming::backdropMs;
        constexpr int cardMs = LauncherTiming::cardMs;
        constexpr int contentMs = LauncherTiming::contentMs;
        constexpr int rowPauseMs = LauncherTiming::rowPauseMs;
        constexpr int closeMs = LauncherTiming::buttonMs;

        const int cardsBegin = gradientMs + backdropMs;
        const QVector<QVector<QWidget*>> rows = visibleRows();
        QVector<LauncherRevealStep> steps;
        QVector<int> initialRows;

        // This loop is deliberately based on the live viewport rather than
        // numbered rows (1/2/3). E.g. if rows 1–6 are visible, it animates
        // 6 -> 5 -> 4 -> 3 -> 2 -> 1, including the partially visible row.
        for (int row = rows.size() - 1; row >= 0; --row) {
            if (rows.at(row).isEmpty()) continue;
            const int rowStart = cardsBegin +
                initialRows.size() * (cardMs + contentMs + rowPauseMs);
            initialRows.append(row);
            queueRowReveal(row, rowStart, cardMs, contentMs, steps);
        }

        // Checkable on the target device: prints the actual rows selected.
        if (!initialRows.isEmpty())
            qInfo() << "osm-launcher: opening bottom-to-top, rows"
                    << initialRows.first() + 1 << "through"
                    << initialRows.last() + 1 << "(viewport height"
                    << viewportH << ")";

        const int closeStart = cardsBegin +
            initialRows.size() * (cardMs + contentMs + rowPauseMs);
        if (animationCloseButton) {
            // No Q_OBJECT macro on LauncherRevealEffect: qobject_cast would not
            // compile under Qt5. The effect was installed by this method.
            if (auto *effect = static_cast<LauncherRevealEffect*>(
                    animationCloseButton->graphicsEffect()))
                steps.append(LauncherRevealStep(effect, closeStart, closeMs));
        }

        const int totalMs = qMax(closeStart +
            (animationCloseButton ? closeMs : 0), cardsBegin);
        auto *animation = new QVariantAnimation(this);
        animation->setDuration(totalMs);
        animation->setStartValue(0.0);
        animation->setEndValue(double(totalMs));
        animation->setEasingCurve(QEasingCurve::Linear);
        connect(animation, &QVariantAnimation::valueChanged, this,
                [this, steps, gradientMs, backdropMs](const QVariant &value) {
            const qreal elapsed = value.toDouble();
            auto easeOutCubic = [](qreal v) -> qreal {
                v = qBound(qreal(0), v, qreal(1));
                return 1.0 - (1.0 - v) * (1.0 - v) * (1.0 - v);
            };
            gradientOpacity = easeOutCubic(elapsed / gradientMs);
            backdropOpacity = easeOutCubic((elapsed - gradientMs) / backdropMs);
            update();
            for (const LauncherRevealStep &step : steps) {
                if (step.effect)
                    step.effect->setProgress(easeOutCubic(
                        (elapsed - step.startMs) / step.durationMs));
            }
        });
        connect(animation, &QVariantAnimation::finished, this,
                [this, initialRows]() {
            for (int row : initialRows) finishRowReveal(row);
            if (animationCloseButton)
                animationCloseButton->setGraphicsEffect(nullptr);
            gradientOpacity = 1.0;
            backdropOpacity = 1.0;
            opening = false;
            update();
            revealVisiblePendingRows();
        });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

    // Closing, faster than before: close button -> TOPMOST visible row's
    // icon/label -> its card -> next row DOWN -> backdrop -> edge gradients.
    // The fullscreen window and grid never resize, preserving Qtile behaviour.
    void dismissWithAnimation() {
        if (isTransitioning()) return;
        dismissing = true;
        // Stop any on-scroll reveal before installing the reverse-close effects.
        for (const auto &animation : scrollRevealAnimations) {
            if (!animation) continue;
            animation->stop();
            animation->deleteLater();
        }
        scrollRevealAnimations.clear();
        if (iconLoader) iconLoader->stop();
        if (animationScroll)
            QScroller::scroller(animationScroll->viewport())->stop();

        // Same per-stage durations as opening, reversed, and 2x faster
        // than the previous closing sequence.
        constexpr int closeMs = LauncherTiming::buttonMs;
        constexpr int contentMs = LauncherTiming::contentMs;
        constexpr int cardMs = LauncherTiming::cardMs;
        constexpr int rowPauseMs = LauncherTiming::rowPauseMs;
        constexpr int backdropMs = LauncherTiming::backdropMs;
        constexpr int gradientMs = LauncherTiming::gradientMs;

        QVector<LauncherRevealStep> steps;
        const QVector<QVector<QWidget*>> rows = visibleRows();
        int animatedRowCount = 0;

        // Installing the effects together before repainting prevents flashes.
        setUpdatesEnabled(false);
        if (animationCloseButton) {
            auto *effect = new LauncherRevealEffect(0.75, animationCloseButton);
            effect->setProgress(1.0);
            animationCloseButton->setGraphicsEffect(effect);
            steps.append(LauncherRevealStep(effect, 0, closeMs));
        }

        // All on-screen rows, top -> bottom, including a partial bottom row.
        // As on opening, all cards in one row share their stage times.
        for (int row = 0; row < rows.size(); ++row) {
            if (rows.at(row).isEmpty()) continue;
            const int rowStart = (animationCloseButton ? closeMs : 0) +
                animatedRowCount * (contentMs + cardMs + rowPauseMs);
            ++animatedRowCount;

            for (QWidget *tile : rows.at(row)) {
                for (QLabel *label : tile->findChildren<QLabel*>()) {
                    auto *effect = new LauncherRevealEffect(0.85, label);
                    effect->setProgress(1.0);
                    label->setGraphicsEffect(effect);
                    steps.append(LauncherRevealStep(effect, rowStart, contentMs));
                }

                auto *effect = new LauncherRevealEffect(0.30, tile);
                effect->setProgress(1.0);
                tile->setGraphicsEffect(effect);
                steps.append(LauncherRevealStep(effect,
                                                rowStart + contentMs, cardMs));
            }
        }

        const int backdropStart = (animationCloseButton ? closeMs : 0) +
            animatedRowCount * (contentMs + cardMs + rowPauseMs);
        const int gradientStart = backdropStart + backdropMs;
        const int totalMs = gradientStart + gradientMs;
        gradientOpacity = 1.0;
        backdropOpacity = 1.0;
        setUpdatesEnabled(true);
        update();

        auto *animation = new QVariantAnimation(this);
        animation->setDuration(totalMs);
        animation->setStartValue(0.0);
        animation->setEndValue(double(totalMs));
        animation->setEasingCurve(QEasingCurve::Linear);
        connect(animation, &QVariantAnimation::valueChanged, this,
                [this, steps, backdropStart, backdropMs, gradientStart, gradientMs]
                (const QVariant &value) {
            const qreal elapsed = value.toDouble();
            auto easeInCubic = [](qreal v) -> qreal {
                v = qBound(qreal(0), v, qreal(1));
                return v * v * v;
            };
            for (const LauncherRevealStep &step : steps) {
                if (step.effect)
                    step.effect->setProgress(1.0 - easeInCubic(
                        (elapsed - step.startMs) / step.durationMs));
            }
            backdropOpacity = 1.0 - easeInCubic((elapsed - backdropStart) / backdropMs);
            gradientOpacity = 1.0 - easeInCubic((elapsed - gradientStart) / gradientMs);
            update();
        });
        connect(animation, &QVariantAnimation::finished, this,
                [this]() { close(); });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Background layers animate independently during opening and closing.
        p.setOpacity(backdropOpacity);
        p.fillRect(rect(), QColor(0,0,0,140));
        p.setOpacity(gradientOpacity);

        int w = width();
        int h = height();

        const int fadeH = 75;

        // Top fade — black → transparent
        QLinearGradient top(0, 0, 0, fadeH * 2);
        top.setColorAt(0.0, QColor(0, 0, 0, 255));
        top.setColorAt(0.5, QColor(0,0,0,255));   // <- extend black
        top.setColorAt(1.0, QColor(0, 0, 0, 0));
        p.fillRect(0, 0, w, fadeH * 3, top);

        // Bottom fade — transparent → black
        QLinearGradient bottom(0, h - fadeH * 2, 0, h);
        bottom.setColorAt(0.0, QColor(0, 0, 0, 0));
        bottom.setColorAt(0.5, QColor(0,0,0,255)); // <- start fade later
        bottom.setColorAt(1.0, QColor(0, 0, 0, 255));
        p.fillRect(0, h - fadeH * 2, w, fadeH * 2, bottom);
        p.setOpacity(1.0);
    }
};

// App label colour, read in main() from the UI settings config
static QString g_labelColor = "#ffffff";

// ──────────────────────────────  Clean exec fields (unchanged)
static QString cleanExec(const QString &exec) {
    QString s = exec;
    for (auto rep : {"%U","%u","%F","%f","%i","%c","%k"}) s.replace(rep, "");
    return s.trimmed();
}

struct AppEntry {
    QString name;
    QString exec;
    QString icon;
};

static QStringList standardDesktopDirs() {
    QStringList dirs;
    dirs << QDir::homePath() + "/.local/share/applications"
         << "/usr/share/applications"
         << QDir::homePath() + "/.local/share/flatpak/exports/share/applications"
         << "/var/lib/flatpak/exports/share/applications"
         << "/var/lib/snapd/desktop/applications";
    return dirs;
}

// Save parsed desktop entries between launcher invocations. QSettings has to
// open and parse every .desktop file; that was part of the startup delay even
// when no applications had changed. A small on-disk cache avoids parsing on
// ordinary launches, while checking modification time and size of every
// previously discovered desktop file AND directory detects edits, additions,
// removals, and newly-created subdirectories. The cache is only an index of
// unprivileged .desktop metadata; execution still goes through the existing
// application launch and PolicyKit paths.
struct DesktopCacheStamp {
    QString path;
    qint64 modifiedMs = -1;
    qint64 size = -1;
    bool directory = false;
};

static DesktopCacheStamp desktopStamp(const QString &path, bool directory) {
    DesktopCacheStamp stamp;
    stamp.path = path;
    stamp.directory = directory;
    const QFileInfo info(path);
    if (!info.exists() || (directory ? !info.isDir() : !info.isFile()))
        return stamp;
    stamp.modifiedMs = info.lastModified().toMSecsSinceEpoch();
    stamp.size = directory ? 0 : info.size();
    return stamp;
}

static bool desktopStampMatches(const DesktopCacheStamp &expected) {
    const DesktopCacheStamp actual = desktopStamp(expected.path,
                                                   expected.directory);
    return actual.modifiedMs == expected.modifiedMs &&
           actual.size == expected.size;
}

static QVector<DesktopCacheStamp> desktopDirectoryStamps() {
    QSet<QString> directories;
    for (const QString &root : standardDesktopDirs()) {
        directories.insert(QDir::cleanPath(root)); // also remember absent dirs
        if (!QDir(root).exists()) continue;
        QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) directories.insert(QDir::cleanPath(it.next()));
    }
    QStringList paths;
    for (const QString &path : directories) paths.append(path);
    paths.sort();
    QVector<DesktopCacheStamp> stamps;
    stamps.reserve(paths.size());
    for (const QString &path : paths) stamps.append(desktopStamp(path, true));
    return stamps;
}

static QList<AppEntry> scanDesktopEntries(QVector<DesktopCacheStamp> &files) {
    QList<AppEntry> apps;
    for (const QString &dir : standardDesktopDirs()) {
        QDirIterator it(dir, QStringList() << "*.desktop", QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            // Cache the signature of even hidden/invalid entries. Changes to
            // NoDisplay, Name or Exec must invalidate the index as well.
            files.append(desktopStamp(path, false));
            QSettings settings(path, QSettings::IniFormat);
            settings.beginGroup("Desktop Entry");
            const QString name = settings.value("Name").toString();
            const QString exec = settings.value("Exec").toString();
            const QString icon = settings.value("Icon").toString();
            const QString nodisplay = settings.value("NoDisplay", "false")
                                          .toString().toLower();
            settings.endGroup();
            if (name.isEmpty() || exec.isEmpty() || nodisplay == "true")
                continue;
            apps.append({name, cleanExec(exec), icon});
        }
    }
    std::sort(apps.begin(), apps.end(), [](const AppEntry &a, const AppEntry &b) {
        return a.name.toLower() < b.name.toLower();
    });
    return apps;
}

static constexpr quint32 launcherCacheMagic = 0x4f534d4c; // 'OSML'
static constexpr quint32 launcherCacheVersion = 1;

static bool readDesktopCache(const QString &cachePath, QList<AppEntry> &apps) {
    QFile cache(cachePath);
    if (!cache.open(QIODevice::ReadOnly)) return false;
    QDataStream stream(&cache);
    stream.setVersion(QDataStream::Qt_5_6);
    quint32 magic = 0, version = 0, directories = 0, files = 0, appCount = 0;
    stream >> magic >> version >> directories >> files >> appCount;
    if (stream.status() != QDataStream::Ok || magic != launcherCacheMagic ||
        version != launcherCacheVersion || directories > 10000 ||
        files > 50000 || appCount > 50000)
        return false;

    // Read the entire index before validating: malformed/partial cache files
    // should simply cause a fresh scan instead of displaying a partial list.
    QVector<DesktopCacheStamp> stamps;
    stamps.reserve(int(directories + files));
    for (quint32 i = 0; i < directories + files; ++i) {
        DesktopCacheStamp stamp;
        stream >> stamp.path >> stamp.modifiedMs >> stamp.size >> stamp.directory;
        if (stream.status() != QDataStream::Ok || stamp.path.isEmpty() ||
            stamp.directory != (i < directories)) return false;
        stamps.append(stamp);
    }
    QList<AppEntry> cachedApps;
    for (quint32 i = 0; i < appCount; ++i) {
        AppEntry entry;
        stream >> entry.name >> entry.exec >> entry.icon;
        if (stream.status() != QDataStream::Ok) return false;
        cachedApps.append(entry);
    }
    if (stream.status() != QDataStream::Ok) return false;

    for (const DesktopCacheStamp &stamp : stamps)
        if (!desktopStampMatches(stamp)) return false;

    apps = cachedApps;
    return true;
}

static void writeDesktopCache(const QString &cachePath,
                              const QList<AppEntry> &apps,
                              const QVector<DesktopCacheStamp> &directories,
                              const QVector<DesktopCacheStamp> &files) {
    QSaveFile cache(cachePath);
    if (!cache.open(QIODevice::WriteOnly)) return;
    cache.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QDataStream stream(&cache);
    stream.setVersion(QDataStream::Qt_5_6);
    stream << launcherCacheMagic << launcherCacheVersion
           << quint32(directories.size()) << quint32(files.size())
           << quint32(apps.size());
    for (const DesktopCacheStamp &stamp : directories)
        stream << stamp.path << stamp.modifiedMs << stamp.size << stamp.directory;
    for (const DesktopCacheStamp &stamp : files)
        stream << stamp.path << stamp.modifiedMs << stamp.size << stamp.directory;
    for (const AppEntry &entry : apps)
        stream << entry.name << entry.exec << entry.icon;
    if (stream.status() == QDataStream::Ok) cache.commit();
    else cache.cancelWriting();
}

static QList<AppEntry> loadDesktopEntries(const QString &cachePath,
                                           bool *cacheHit,
                                           QVector<DesktopCacheStamp> *pendingFiles) {
    QList<AppEntry> apps;
    if (readDesktopCache(cachePath, apps)) {
        if (cacheHit) *cacheHit = true;
        return apps;
    }
    if (cacheHit) *cacheHit = false;
    // On a cache miss only do the required desktop-entry scan before showing
    // the window. Directory enumeration and cache writing happen after the
    // first frame, not on the critical path to the opening animation.
    QVector<DesktopCacheStamp> files;
    apps = scanDesktopEntries(files);
    if (pendingFiles) *pendingFiles = files;
    return apps;
}

// Existing Alternix authentication: PolicyKit uses /etc/pam.d/polkit-1,
// while sudo / osm-sudo use /etc/pam.d/sudo.  Do NOT replace either with a
// launcher-specific sudo hack; keep the real policy authorization step.
static bool isPolkitName(const QString &path) {
    const QString name = QFileInfo(path).fileName().toLower();
    return name == "pkexec" || name.endsWith("-pkexec");
}

static bool needsPolkit(const QString &command) {
    const QStringList args = QProcess::splitCommand(command);
    for (const QString &arg : args)
        if (isPolkitName(arg)) return true;
    return false;
}

static bool polkitPamUsesAlternix() {
    QFile f("/etc/pam.d/polkit-1");
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QByteArray rules = f.readAll();
    return rules.contains("pam_exec.so") &&
           rules.contains("/usr/local/libexec/alternix-auth --pam");
}

static bool isKnownPolkitAgent(const QString &file) {
    const QString n = QFileInfo(file).fileName().toLower();
    return n == "lxpolkit" || n == "lxqt-policykit-agent" ||
           n == "polkit-mate-authentication-agent-1" ||
           n == "polkit-gnome-authentication-agent-1" ||
           n == "polkit-kde-authentication-agent-1" || n == "xfce-polkit";
}

// Use /proc rather than guessing from one package name. A package may have
// installed mate-polkit or policykit-1-gnome rather than lxpolkit.
static bool polkitAgentRunning() {
    const uid_t uid = getuid();
    QDirIterator it("/proc", QDir::Dirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString pidPath = it.next();
        const QString pidName = QFileInfo(pidPath).fileName();
        if (pidName.isEmpty() || !pidName.at(0).isDigit()) continue;
        if (QFileInfo(pidPath).ownerId() != uint(uid)) continue;
        QFile cmd(pidPath + "/cmdline");
        if (!cmd.open(QIODevice::ReadOnly)) continue;
        const QList<QByteArray> argv = cmd.read(8192).split('\0');
        for (const QByteArray &arg : argv) {
            if (arg.isEmpty()) continue;
            if (isKnownPolkitAgent(QString::fromLocal8Bit(arg))) return true;
        }
    }
    return false;
}

static bool startPolkitAgent() {
    if (polkitAgentRunning()) return true;
    // An installed agent is needed before pkexec can request graphical
    // authentication. Start it as this desktop user, never with sudo.
    // Xsession/XDG startup from alternix-auth-setup.sh covers other apps.
    const QStringList paths = {
        "/usr/bin/lxpolkit",
        "/usr/libexec/polkit-mate-authentication-agent-1",
        "/usr/lib/mate-polkit/polkit-mate-authentication-agent-1",
        "/usr/lib/x86_64-linux-gnu/polkit-mate/polkit-mate-authentication-agent-1",
        "/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1",
        "/usr/libexec/polkit-gnome-authentication-agent-1",
        "/usr/bin/lxqt-policykit-agent",
        "/usr/libexec/lxqt-policykit-agent",
        "/usr/libexec/xfce-polkit",
        "/usr/lib/polkit-kde-authentication-agent-1",
        "/usr/libexec/polkit-kde-authentication-agent-1"
    };
    for (const QString &path : paths) {
        if (QFileInfo(path).isExecutable() &&
            QProcess::startDetached(path, QStringList())) return true;
    }
    return false;
}

// Launch a .desktop command without putting it through a shell by default.
// pkexec identifies the authorizing application by its *live parent process*.
// QProcess::startDetached() followed by closing this launcher leaves pkexec
// orphaned, causing "Refusing to render service to dead parents" on polkit.
// For PolicyKit launchers, a detached unprivileged shell stays alive and waits
// for the original command; the command and arguments are supplied as argv,
// never interpolated into executable shell text.
static bool launchDesktopCommand(const QString &command) {
    QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) return false;

    const QString program = parts.takeFirst();
    const QString basename = QFileInfo(program).fileName().toLower();
    const bool needsLiveParent = isPolkitName(basename) || needsPolkit(command);
    if (needsLiveParent) {
        QStringList shellArgs = {
            "-c", "\"$@\"; status=$?; exit \"$status\"", "alternix-app", program
        };
        shellArgs.append(parts);
        return QProcess::startDetached("/bin/sh", shellArgs);
    }
    return QProcess::startDetached(program, parts);
}

// ──────────────────────────────  AppTile (lazy icon loading)
class AppTile : public QFrame {
    AppEntry entry;
    bool dragging = false;
    QPoint startPos;
    QLabel *iconLabel;

public:
    explicit AppTile(const AppEntry &e, QWidget *parent=nullptr)
        : QFrame(parent), entry(e), iconLabel(nullptr)
    {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAutoFillBackground(false);
        setStyleSheet("QFrame { background: #00000099; border: none; }");
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setMinimumSize(150, 160);

        QWidget *hoverBox = new QWidget(this);
        hoverBox->setObjectName("hoverBox");
        hoverBox->setStyleSheet(
            "#hoverBox { background: #80708099; border-radius: 20px; }"
            "#hoverBox:hover { background-color: #282828; border: 1px solid #ffffff; }"
        );

        QVBoxLayout *v = new QVBoxLayout(hoverBox);
        v->setContentsMargins(16,16,16,16);
        v->setSpacing(8);
        v->setAlignment(Qt::AlignCenter);

        int iconSize = QApplication::primaryScreen()->size().width() <= 720 ? 48 : 48;

        iconLabel = new QLabel(hoverBox);
        iconLabel->setAlignment(Qt::AlignCenter);

        // Transparent placeholder to keep layout stable
        QPixmap placeholder(iconSize, iconSize);
        placeholder.fill(Qt::transparent);
        iconLabel->setPixmap(placeholder);

        v->addWidget(iconLabel);

        QLabel *lbl = new QLabel(e.name, hoverBox);
        lbl->setStyleSheet(QString("color:%1; background:transparent;").arg(g_labelColor));
        lbl->setWordWrap(true);
        lbl->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
        lbl->setMinimumHeight(60);
        lbl->setMaximumHeight(200);
        // Font size is inherited from QApplication font (set from UI settings config)
        v->addWidget(lbl);

        QVBoxLayout *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0,0,0,0);
        outer->addWidget(hoverBox);
    }

    // Called later by the timer to actually load the icon
    void loadIcon() {
        if (!iconLabel)
            return;

        int iconSize = QApplication::primaryScreen()->size().width() <= 720 ? 48 : 48;

        // If there is no icon defined at all, go straight to the puzzle placeholder
        if (entry.icon.trimmed().isEmpty()) {
            iconLabel->setPixmap(QPixmap());
            iconLabel->setText("🧩");
            iconLabel->setStyleSheet("font-size:48px;");
            return;
        }

        QPixmap pix;
        QIcon ic = QIcon::fromTheme(entry.icon);
        if (!ic.isNull())
            pix = ic.pixmap(iconSize, iconSize);
        if (pix.isNull() && QFileInfo(entry.icon).exists())
            pix.load(entry.icon);

        if (!pix.isNull()) {
            iconLabel->setText(QString());
            iconLabel->setStyleSheet(QString());
            iconLabel->setPixmap(
                pix.scaled(iconSize, iconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            );
        } else {
            // fallback if icon cannot be resolved
            iconLabel->setPixmap(QPixmap());
            iconLabel->setText("🧩");
            iconLabel->setStyleSheet("font-size:48px;");
        }
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        dragging = false;
        startPos = e->pos();
        QFrame::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent *e) override {
        if ((e->pos() - startPos).manhattanLength() > 10)
            dragging = true;
        QFrame::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent *e) override {
        if (!dragging && e->button() == Qt::LeftButton) {
            const QString command = entry.exec;
            if (needsPolkit(command)) {
                if (!polkitPamUsesAlternix()) {
                    QMessageBox::warning(this, "AlternixOS PolicyKit authentication",
                        "The PolicyKit PAM rule is not currently connected to "
                        "the existing Alternix pattern/PIN verifier.\n\n"
                        "The existing authentication setup may have rolled back "
                        "PolicyKit after its live test failed. Run the updated "
                        "alternix-auth-setup.sh from your normal desktop session.");
                    QFrame::mouseReleaseEvent(e);
                    return;
                }
                if (!polkitAgentRunning() && !startPolkitAgent()) {
                    QMessageBox::warning(this, "PolicyKit graphical agent unavailable",
                        "No installed graphical PolicyKit agent could be started "
                        "in the current Qtile session.\n\n"
                        "An existing PolicyKit agent is required before "
                        "Synaptic can request the Alternix pattern/PIN.");
                    QFrame::mouseReleaseEvent(e);
                    return;
                }
                // A newly spawned agent must register over the D-Bus session
                // before pkexec requests authentication. Check again without
                // freezing the desktop or leaving a sudo prompt behind.
                QPointer<QWidget> launcherWindow = window();
                QTimer::singleShot(1100, this, [this, command, launcherWindow]() {
                    if (!polkitAgentRunning()) {
                        QMessageBox::warning(this, "PolicyKit agent could not register",
                            "The graphical agent did not stay running. Check "
                            "the Qtile session D-Bus address and the PolicyKit "
                            "diagnostic output before retrying Synaptic.");
                        return;
                    }
                    if (launchDesktopCommand(command)) {
                        if (auto *launcher = dynamic_cast<LauncherWindow*>(launcherWindow.data()))
                            launcher->dismissWithAnimation();
                        else if (launcherWindow) launcherWindow->close();
                    } else {
                        qWarning() << "osm-launcher: failed to launch" << command;
                        QMessageBox::warning(this, "Could not start application",
                                             "Failed to start: " + command);
                    }
                });
                QFrame::mouseReleaseEvent(e);
                return;
            }
            if (launchDesktopCommand(command)) {
                if (auto *launcher = dynamic_cast<LauncherWindow*>(window()))
                    launcher->dismissWithAnimation();
                else if (QWidget *w = window()) w->close();
            } else {
                qWarning() << "osm-launcher: failed to start desktop command:" << command;
            }
        }
        QFrame::mouseReleaseEvent(e);
    }
};

// ──────────────────────────────  Escape-to-close (unchanged)
class KeyFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (ev->type() == QEvent::KeyPress) {
            QKeyEvent *ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape) {
                if (auto w = qobject_cast<QWidget*>(obj)) {
                    while (w && !w->isWindow()) w = w->parentWidget();
                    if (auto *launcher = dynamic_cast<LauncherWindow*>(w))
                        launcher->dismissWithAnimation();
                    else if (w) w->close();
                    return true;
                }
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

// ──────────────────────────────  Pull-down-to-dismiss gesture
// An application-wide filter sees mouse/touch events even when the gesture
// starts on an app tile or its nested labels (not just on the scroll viewport).
// It does not consume ordinary events, so QScroller retains normal scrolling.
class PullDownDismissFilter : public QObject {
    enum Pointer { None, Mouse, Touch };
    LauncherWindow *launcher;
    QScrollArea *scroll;
    Pointer pointer = None;
    QPointF pressPosition;
    bool directionConfirmed = false;
    int touchId = -1;

    bool inAppList(QObject *object) const {
        const QWidget *widget = qobject_cast<QWidget*>(object);
        const QWidget *viewport = scroll->viewport();
        return widget && (widget == viewport || viewport->isAncestorOf(widget));
    }

    bool listIsAtTop() const {
        const QScrollBar *bar = scroll->verticalScrollBar();
        return bar->value() <= bar->minimum() + 1;
    }

    void start(Pointer type, const QPointF &globalPosition) {
        pointer = listIsAtTop() ? type : None;
        pressPosition = globalPosition;
        directionConfirmed = false;
    }

    void move(const QPointF &globalPosition) {
        if (pointer == None || launcher->isTransitioning()) return;
        // Only a pull that BEGINS with the list at the top may close it.
        if (!listIsAtTop()) { pointer = None; return; }

        const qreal dx = globalPosition.x() - pressPosition.x();
        const qreal dy = globalPosition.y() - pressPosition.y();

        // Never reinterpret a normal upward or sideways scroll as dismiss.
        if (!directionConfirmed && qAbs(dx) + qAbs(dy) >= 16.0) {
            if (dy <= 0 || dy < qAbs(dx) * 1.2) {
                pointer = None;
                return;
            }
            directionConfirmed = true;
        }

        // A deliberate pull: 75-125 logical pixels, scaled for screen height.
        const qreal threshold = qBound(75.0, launcher->height() * 0.11, 125.0);
        if (directionConfirmed && dy >= threshold && dy > qAbs(dx) * 1.2) {
            pointer = None;
            QScroller::scroller(scroll->viewport())->stop();
            launcher->dismissWithAnimation();
        }
    }

public:
    PullDownDismissFilter(LauncherWindow *window, QScrollArea *area, QObject *parent = nullptr)
        : QObject(parent), launcher(window), scroll(area) {}

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            if (launcher->isTransitioning()) return true;
            QMouseEvent *mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && pointer != Touch && inAppList(object))
                start(Mouse, mouse->screenPos());
            break;
        }
        case QEvent::MouseMove: {
            if (launcher->isTransitioning()) return true;
            QMouseEvent *mouse = static_cast<QMouseEvent*>(event);
            if (pointer == Mouse) {
                if (mouse->buttons() & Qt::LeftButton) move(mouse->screenPos());
                else pointer = None;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            if (launcher->isTransitioning()) return true;
            QMouseEvent *mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && pointer == Mouse) pointer = None;
            break;
        }
        case QEvent::TouchBegin: {
            if (launcher->isTransitioning()) return true;
            QTouchEvent *touch = static_cast<QTouchEvent*>(event);
            if (inAppList(object) && touch->touchPoints().size() == 1) {
                const auto point = touch->touchPoints().first();
                touchId = point.id();
                start(Touch, point.screenPos());
            }
            break;
        }
        case QEvent::TouchUpdate: {
            if (launcher->isTransitioning()) return true;
            if (pointer != Touch) break;
            QTouchEvent *touch = static_cast<QTouchEvent*>(event);
            if (touch->touchPoints().size() != 1 ||
                touch->touchPoints().first().id() != touchId) {
                pointer = None; // Multi-touch must not close the launcher.
                break;
            }
            move(touch->touchPoints().first().screenPos());
            break;
        }
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            if (launcher->isTransitioning()) return true;
            if (pointer == Touch) pointer = None;
            touchId = -1;
            break;
        default:
            break;
        }
        return QObject::eventFilter(object, event);
    }
};

// ──────────────────────────────  main
int main(int argc, char **argv) {
    QApplication a(argc, argv);
    QElapsedTimer startupClock;
    startupClock.start();
    const bool profileStartup = qEnvironmentVariableIsSet("OSM_LAUNCHER_PROFILE");

    // ───────── Read font size from UI settings
    {
        QString cfgPath = QDir::homePath() + "/.config/Alternix/osm-settings.conf";
        QSettings cfg(cfgPath, QSettings::IniFormat);
        int pt = cfg.value("UI/LauncherFontSize", 15).toInt();
        pt = qBound(14, pt, 36);
        QString col = cfg.value("UI/LauncherFontColor", "#ffffff").toString().trimmed();
        if (QColor(col).isValid())
            g_labelColor = col;
        QFont f = a.font();
        f.setPointSize(pt);
        a.setFont(f);
    }

    // ───────── Single-instance guard using QLockFile
    QDir cacheDir(QDir::homePath() + "/Alternix/.cache");
    cacheDir.mkpath(".");  // ensure directory exists

    QString lockPath = cacheDir.absoluteFilePath("osm-launcher.lock");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);  // never auto-steal stale locks

    if (!lock.tryLock(0)) {
        // Another osm-launcher is already running
        return 0;
    }

    bool usedAppCache = false;
    const QString appsCachePath = cacheDir.absoluteFilePath("osm-launcher-apps-v1.cache");
    QVector<DesktopCacheStamp> pendingCacheFiles;
    QList<AppEntry> apps = loadDesktopEntries(appsCachePath, &usedAppCache,
                                               &pendingCacheFiles);
    if (profileStartup)
        qInfo() << "osm-launcher startup: desktop entries loaded in"
                << startupClock.elapsed() << "ms (cache hit:" << usedAppCache << ")";

    LauncherWindow window;        // ← WINDOW TYPE WITH BUILT-IN FADE
    window.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    window.setAttribute(Qt::WA_TranslucentBackground, true);
    window.setAutoFillBackground(false);
    window.setStyleSheet("background:transparent;");

    QVBoxLayout *main = new QVBoxLayout(&window);
    main->setContentsMargins(20,5,20,5);
    main->setSpacing(12);

    QScrollArea *scroll = new QScrollArea(&window);
    scroll->setWidgetResizable(false);
    scroll->setStyleSheet("border: none; background:transparent;");
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QWidget *container = new QWidget();
    container->setAttribute(Qt::WA_TranslucentBackground, true);
    container->setAutoFillBackground(false);
    container->setStyleSheet("background:transparent;");

    QGridLayout *grid = new QGridLayout(container);
    grid->setSpacing(12);
    grid->setContentsMargins(10,10,10,10);

    for (int i = 0; i < 4; ++i)
        grid->setColumnStretch(i, 1);

    int cols = 4;
    int idx = 0;

    QVector<AppTile*> tiles;
    tiles.reserve(apps.size());

    // Create all tiles with lightweight placeholders only
    for (const AppEntry &e : apps) {
        AppTile *tile = new AppTile(e, container);
        grid->addWidget(tile, idx / cols, idx % cols);
        tiles.append(tile);
        ++idx;
    }

    container->setLayout(grid);
    container->adjustSize();

    int screenWidth = QApplication::primaryScreen()->size().width();

    int contentWidth;
    if (screenWidth <= 800)
        contentWidth = screenWidth - 40;
    else
        contentWidth = static_cast<int>(screenWidth * 0.9);

    scroll->setFixedWidth(contentWidth);
    container->setMinimumWidth(contentWidth);
    container->setMinimumHeight(container->sizeHint().height() + 200);

    scroll->setWidget(container);

    QScroller::grabGesture(scroll->viewport(), QScroller::TouchGesture);
    QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);
    QScrollerProperties prop;
    prop.setScrollMetric(QScrollerProperties::DecelerationFactor, 0.1);
    prop.setScrollMetric(QScrollerProperties::MaximumVelocity, 0.4);
    prop.setScrollMetric(QScrollerProperties::SnapPositionRatio, 0.5);
    prop.setScrollMetric(QScrollerProperties::SnapTime, 0.3);
    QScroller::scroller(scroll->viewport())->setScrollerProperties(prop);

    main->addWidget(scroll, 1, Qt::AlignHCenter);

    QHBoxLayout *closeRow = new QHBoxLayout();
    closeRow->addStretch();
    QPushButton *closeBtn = new QPushButton("❌");
    closeBtn->setFixedHeight(48);
    closeBtn->setStyleSheet(
        "QPushButton { background-color: #66000000; color: red; font-size: 30pt; padding: 6px 18px; border-radius: 8px; }"
        "QPushButton:hover { background-color: #282828; }");
    QObject::connect(closeBtn, &QPushButton::clicked, &window,
                     [&window]() { window.dismissWithAnimation(); });
    closeRow->addWidget(closeBtn);
    closeRow->addStretch();
    main->addLayout(closeRow);

    KeyFilter *kf = new KeyFilter();
    window.installEventFilter(kf);

    if (!usedAppCache) {
        // Cache work is done after the launcher has appeared. Even the first
        // run won't wait for the extra directory scan and atomic disk write.
        QTimer::singleShot(450, &window,
                           [appsCachePath, apps, pendingCacheFiles]() {
            writeDesktopCache(appsCachePath, apps, desktopDirectoryStamps(),
                              pendingCacheFiles);
        });
    }

    // Do not walk /proc or spawn PolicyKit agents before the first frame.
    // Application clicks still perform their own synchronous authentication
    // checks, so this deferred best-effort startup cannot bypass permission.
    QTimer::singleShot(650, &window, []() {
        if (polkitPamUsesAlternix() && !polkitAgentRunning())
            startPolkitAgent();
    });

    // ──────────────────────────────  Lazy icons, scheduled by the row cascade
    // The opening animation already discovers the real bottommost visible row
    // AFTER Qtile has settled fullscreen geometry. Reuse its chosen row order
    // rather than guessing how many rows are visible or loading index 0 first.
    // Each row is decoded in its own left-to-right tile order, with the lowest
    // visible row first. Scrolling later requests any newly visible rows.
    QTimer *iconTimer = new QTimer(&window);
    iconTimer->setInterval(8);
    QVector<int> pendingIcons;
    QVector<bool> iconRequested(tiles.size(), false);
    int nextPendingIcon = 0;

    QObject::connect(iconTimer, &QTimer::timeout, &window,
                     [&tiles, iconTimer, &pendingIcons, &nextPendingIcon]() {
        if (nextPendingIcon >= pendingIcons.size()) {
            iconTimer->stop();
            pendingIcons.clear();
            nextPendingIcon = 0;
            return;
        }
        const int index = pendingIcons.at(nextPendingIcon++);
        if (index >= 0 && index < tiles.size())
            tiles.at(index)->loadIcon();
        if (nextPendingIcon >= pendingIcons.size()) {
            iconTimer->stop();
            pendingIcons.clear();
            nextPendingIcon = 0;
        }
    });

    window.setIconLoader(iconTimer);
    window.setRowIconLoader([&tiles, &pendingIcons, &iconRequested,
                             &nextPendingIcon, iconTimer, cols](int row) {
        const int first = row * cols;
        const int last = qMin(first + cols, tiles.size());
        for (int index = first; index < last; ++index) {
            if (!iconRequested[index]) {
                iconRequested[index] = true;
                pendingIcons.append(index);
            }
        }
        if (nextPendingIcon < pendingIcons.size() && !iconTimer->isActive())
            iconTimer->start();
    });

    // Suppress the first fullscreen paint until the row effects are installed;
    // otherwise the entire launcher can flash for one frame before cascading.
    window.setUpdatesEnabled(false);
    window.showFullScreen();
    QVector<QWidget*> tileWidgets;
    tileWidgets.reserve(tiles.size());
    for (AppTile *tile : tiles) tileWidgets.append(tile);
    window.openWithAnimation(scroll, tileWidgets, cols, closeBtn);
    window.setUpdatesEnabled(true);

    // Observe touches and mouse drags anywhere in the scrollable app list.
    PullDownDismissFilter *pullFilter = new PullDownDismissFilter(&window, scroll, &window);
    a.installEventFilter(pullFilter);
    if (profileStartup)
        qInfo() << "osm-launcher startup: fullscreen requested in"
                << startupClock.elapsed() << "ms";

    return a.exec();
}
