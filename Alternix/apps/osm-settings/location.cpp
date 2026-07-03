#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QFrame>
#include <QProcess>
#include <QMessageBox>
#include <QFont>
#include <QSpacerItem>
#include <QApplication>
#include <QTimer>
#include <QRegularExpression>
#include <QSettings>
#include <QDir>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <functional>

// ---------------------------------------------------------
// Helpers (button + command runner)
// ---------------------------------------------------------

static QPushButton* smallBtnBT(const QString &txt) {
    QPushButton *b = new QPushButton(txt);
    b->setFixedSize(180, 60);
    b->setStyleSheet(
        "QPushButton {"
        " background:#444444;"
        " color:white;"
        " border:1px solid #222222;"
        " border-radius:16px;"
        " font-size:26px;"
        " font-weight:bold;"
        " padding:10px 24px;"
        "}"
        "QPushButton:hover { background:#555555; }"
        "QPushButton:pressed { background:#333333; }"
    );
    return b;
}

// Spinner frames (Braille pattern rotation — present in DejaVu Sans).
static const char* const SPIN_FRAMES_LOC[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"
};
static const int SPIN_COUNT_LOC = 10;

// Optional UI tick, called during command waits. The manual Refresh handler
// points this at its button-spinner update so the animation runs through
// the entire backend chain (mmcli → gpsd → AT) without threading each
// backend individually.
static std::function<void()> g_locTick;

static QString runCommand(const QString &program, const QStringList &args)
{
    QProcess proc;
    proc.start(program, args);

    // Sliced wait: keeps the UI responsive (gpspipe -w -n 10 alone can take
    // several seconds) and lets g_locTick animate during it.
    QElapsedTimer t;
    t.start();
    while (!proc.waitForFinished(80)) {
        if (t.elapsed() > 15000) {
            proc.kill();
            proc.waitForFinished(500);
            break;
        }
        if (g_locTick) g_locTick();
        QCoreApplication::processEvents();
    }

    QString out = proc.readAllStandardOutput();
    out += proc.readAllStandardError();
    return out;
}

// ---------------------------------------------------------
// Location info struct
// ---------------------------------------------------------

struct LocationInfo {
    bool hasFix = false;
    double lat = 0.0;
    double lon = 0.0;

    bool hasHeading = false;
    double heading = 0.0;

    bool hasSat = false;
    int satellites = 0;

    QString source;   // "ModemManager", "gpsd", "AT"
    QString error;
};

// ---------------------------------------------------------
// Backends
// ---------------------------------------------------------

static LocationInfo getLocationFromMmcli()
{
    LocationInfo info;

    QString listOut = runCommand("mmcli", {"-L"});
    if (listOut.isEmpty() || listOut.contains("error", Qt::CaseInsensitive))
        return info;

    QRegularExpression modemRe("/Modem/(\\d+)");
    QRegularExpressionMatch m = modemRe.match(listOut);
    if (!m.hasMatch())
        return info;

    QString modemId = m.captured(1);
    QString locOut = runCommand("mmcli", {"-m", modemId, "--location-get"});
    if (locOut.isEmpty() || locOut.contains("error", Qt::CaseInsensitive))
        return info;

    info.source = "ModemManager";

    QStringList lines = locOut.split('\n');
    QRegularExpression numRe("(-?\\d+\\.\\d+)");
    for (const QString &lineRaw : lines) {
        QString line = lineRaw.trimmed();

        if (line.contains("latitude", Qt::CaseInsensitive)) {
            QRegularExpressionMatch nm = numRe.match(line);
            if (nm.hasMatch()) {
                info.lat = nm.captured(1).toDouble();
            }
        } else if (line.contains("longitude", Qt::CaseInsensitive)) {
            QRegularExpressionMatch nm = numRe.match(line);
            if (nm.hasMatch()) {
                info.lon = nm.captured(1).toDouble();
            }
        } else if (line.contains("heading", Qt::CaseInsensitive) ||
                   line.contains("track", Qt::CaseInsensitive)) {
            QRegularExpressionMatch nm = numRe.match(line);
            if (nm.hasMatch()) {
                info.heading = nm.captured(1).toDouble();
                info.hasHeading = true;
            }
        } else if (line.contains("satellites", Qt::CaseInsensitive)) {
            // First integer on the line
            QRegularExpression intRe("(\\d+)");
            QRegularExpressionMatch im = intRe.match(line);
            if (im.hasMatch()) {
                info.satellites = im.captured(1).toInt();
                info.hasSat = true;
            }
        }
    }

    if (info.lat != 0.0 || info.lon != 0.0)
        info.hasFix = true;

    return info;
}

static LocationInfo getLocationFromGpsd()
{
    LocationInfo info;

    QString out = runCommand("gpspipe", {"-w", "-n", "10"});
    if (out.isEmpty())
        return info;

    info.source = "gpsd";

    // Parse TPV block for lat/lon/track
    QRegularExpression latRe("\"lat\"\\s*:\\s*([-0-9\\.]+)");
    QRegularExpression lonRe("\"lon\"\\s*:\\s*([-0-9\\.]+)");
    QRegularExpression trackRe("\"track\"\\s*:\\s*([-0-9\\.]+)");

    QRegularExpressionMatch latM = latRe.match(out);
    QRegularExpressionMatch lonM = lonRe.match(out);
    if (latM.hasMatch() && lonM.hasMatch()) {
        info.lat = latM.captured(1).toDouble();
        info.lon = lonM.captured(1).toDouble();
        info.hasFix = true;
    }

    QRegularExpressionMatch trM = trackRe.match(out);
    if (trM.hasMatch()) {
        info.heading = trM.captured(1).toDouble();
        info.hasHeading = true;
    }

    // Satellites: approximate by counting occurrences of `"PRN":`
    int satCount = out.count("\"PRN\"");
    if (satCount > 0) {
        info.satellites = satCount;
        info.hasSat = true;
    }

    return info;
}

static LocationInfo getLocationFromAT()
{
    LocationInfo info;

    // Use microcom if available to talk to first USB/ACM serial that exists.
    QString script =
        "if command -v microcom >/dev/null 2>&1; then "
        "for p in /dev/ttyUSB* /dev/ttyACM*; do "
        "  if [ -e \"$p\" ]; then "
        "    echo -e 'AT+CGPSINFO\\r' | microcom -t 2000 -s 115200 -p \"$p\"; "
        "    break; "
        "  fi; "
        "done; "
        "fi";

    QString out = runCommand("bash", {"-c", script});
    if (out.isEmpty())
        return info;

    info.source = "AT";

    // Expect something like: +CGPSINFO: 4914.1234,N,12308.5678,W,...
    QRegularExpression lineRe("\\+CGPSINFO:(.*)");
    QRegularExpressionMatch lm = lineRe.match(out);
    if (!lm.hasMatch())
        return info;

    QString fields = lm.captured(1).trimmed();
    QStringList parts = fields.split(',', Qt::KeepEmptyParts);
    if (parts.size() < 4)
        return info;

    // Convert from DDMM.MMMM format to decimal degrees.
    auto convertCoord = [](const QString &coord, const QString &hem) -> double {
        if (coord.isEmpty())
            return 0.0;
        bool ok = false;
        double val = coord.toDouble(&ok);
        if (!ok || val == 0.0)
            return 0.0;
        int degrees = static_cast<int>(val / 100.0);
        double minutes = val - degrees * 100.0;
        double dec = degrees + minutes / 60.0;
        if (hem == "S" || hem == "W")
            dec = -dec;
        return dec;
    };

    double lat = convertCoord(parts.value(0).trimmed(), parts.value(1).trimmed());
    double lon = convertCoord(parts.value(2).trimmed(), parts.value(3).trimmed());

    if (lat != 0.0 || lon != 0.0) {
        info.lat = lat;
        info.lon = lon;
        info.hasFix = true;
    }

    return info;
}

static LocationInfo getBestLocation()
{
    LocationInfo info;

    info = getLocationFromMmcli();
    if (info.hasFix || info.hasSat)
        return info;

    info = getLocationFromGpsd();
    if (info.hasFix || info.hasSat)
        return info;

    info = getLocationFromAT();
    if (info.hasFix || info.hasSat)
        return info;

    info.error = "Adaptor or libraires no found";
    return info;
}

// ---------------------------------------------------------
// LocationPage widget
// ---------------------------------------------------------

class LocationPage : public QWidget
{
public:
    explicit LocationPage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), stackedWidget(stack)
    {
        // -----------------------------
        // Global font override
        // -----------------------------
        QFont f = QApplication::font();
        f.setPixelSize(26);          // global size
        setFont(f);   // page-local: do NOT change the app-wide font

        // Global white text + dark bg
        QPalette pal = qApp->palette();
        pal.setColor(QPalette::WindowText, Qt::white);
        pal.setColor(QPalette::Text, Qt::white);
        pal.setColor(QPalette::ButtonText, Qt::white);
        qApp->setPalette(pal);

        setStyleSheet("background:#282828; color:white;");

        QVBoxLayout *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(40, 40, 40, 40);
        rootLayout->setSpacing(20);

        // Title
        QLabel *titleLabel = new QLabel("Location", this);
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setStyleSheet(
            "QLabel { font-size:42px; font-weight:bold; color:white; }"
        );
        rootLayout->addWidget(titleLabel);

        // -------------------------------------------------
        // Card 1: GPS coordinates + Compass heading
        // -------------------------------------------------
        QFrame *gpsFrame = new QFrame(this);
        gpsFrame->setStyleSheet(
            "QFrame {"
            " background:#3a3a3a;"
            " border-radius:40px;"
            "}"
        );
        gpsFrame->setFixedHeight(220);

        QVBoxLayout *gpsLayout = new QVBoxLayout(gpsFrame);
        gpsLayout->setContentsMargins(30, 30, 30, 30);
        gpsLayout->setSpacing(12);

        gpsLabel = new QLabel("Gps coordinates\nCompass heading", gpsFrame);
        gpsLabel->setAlignment(Qt::AlignCenter);
        gpsLabel->setWordWrap(true);
        gpsLabel->setStyleSheet("QLabel { font-size:28px; }");
        gpsLayout->addWidget(gpsLabel);

        rootLayout->addWidget(gpsFrame);

        // -------------------------------------------------
        // Card 2: Visible satellites
        // -------------------------------------------------
        QFrame *satFrame = new QFrame(this);
        satFrame->setStyleSheet(
            "QFrame {"
            " background:#3a3a3a;"
            " border-radius:40px;"
            "}"
        );
        satFrame->setFixedHeight(190);

        QVBoxLayout *satLayout = new QVBoxLayout(satFrame);
        satLayout->setContentsMargins(30, 30, 30, 30);
        satLayout->setSpacing(12);

        satLabel = new QLabel("Visible satellites", satFrame);
        satLabel->setAlignment(Qt::AlignCenter);
        satLabel->setWordWrap(true);
        satLabel->setStyleSheet("QLabel { font-size:28px; }");
        satLayout->addWidget(satLabel);

        rootLayout->addWidget(satFrame);

        // -------------------------------------------------
        // Card 3: Mini map of local area (text placeholder)
        // -------------------------------------------------
        QFrame *mapFrame = new QFrame(this);
        mapFrame->setStyleSheet(
            "QFrame {"
            " background:#3a3a3a;"
            " border-radius:40px;"
            "}"
        );
        mapFrame->setFixedHeight(260);

        QVBoxLayout *mapLayout = new QVBoxLayout(mapFrame);
        mapLayout->setContentsMargins(30, 30, 30, 30);
        mapLayout->setSpacing(12);

        mapLabel = new QLabel("Mini map\nof local area", mapFrame);
        mapLabel->setAlignment(Qt::AlignCenter);
        mapLabel->setWordWrap(true);
        mapLabel->setStyleSheet("QLabel { font-size:28px; }");
        mapLayout->addWidget(mapLabel);

        rootLayout->addWidget(mapFrame);

        // Add stretch so buttons/back sit at bottom
        rootLayout->addStretch(1);

        // -------------------------------------------------
        // Bottom buttons: On/Off + Refresh
        // -------------------------------------------------
        QHBoxLayout *bottomButtonsLayout = new QHBoxLayout();
        bottomButtonsLayout->setSpacing(40);
        bottomButtonsLayout->setAlignment(Qt::AlignHCenter);

        powerButton   = smallBtnBT("On");
        refreshButton = smallBtnBT("Refresh");

        bottomButtonsLayout->addWidget(powerButton);
        bottomButtonsLayout->addWidget(refreshButton);

        rootLayout->addLayout(bottomButtonsLayout);

        // -------------------------------------------------
        // Back button pinned to bottom
        // -------------------------------------------------
        QPushButton *backButton = new QPushButton(QStringLiteral("❮"), this);
        backButton->setFixedSize(140, 60);
        backButton->setStyleSheet(
            "QPushButton {"
            " background:#444444;"
            " color:white;"
            " border:1px solid #222222;"
            " border-radius:16px;"
            " font-size:34px;"
            " font-family:'DejaVu Sans';"
            "}"
            "QPushButton:hover { background:#555555; }"
            "QPushButton:pressed { background:#333333; }"
        );

        QHBoxLayout *backLayout = new QHBoxLayout();
        backLayout->addWidget(backButton, 0, Qt::AlignHCenter);
        rootLayout->addLayout(backLayout);

        // -------------------------------------------------
        // Connections
        // -------------------------------------------------
        connect(powerButton, &QPushButton::clicked,
                this, &LocationPage::togglePower);
        connect(refreshButton, &QPushButton::clicked,
                this, &LocationPage::manualRefresh);
        connect(backButton, &QPushButton::clicked, this, [this]() {
            if (stackedWidget) {
                stackedWidget->setCurrentIndex(0);
            }
        });

        // Timer: 2 second refresh while on and page visible
        refreshTimer = new QTimer(this);
        refreshTimer->setInterval(2000);
        connect(refreshTimer, &QTimer::timeout,
                this, &LocationPage::refreshDataOnce);

        if (stackedWidget) {
            connect(stackedWidget, &QStackedWidget::currentChanged,
                    this, &LocationPage::onStackIndexChanged);
        }

        // -------------------------------------------------
        // Load persisted state
        // -------------------------------------------------
        QSettings settings(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                           QSettings::IniFormat);
        locationEnabled = settings.value("Location/enabled", true).toBool();
        updatePowerButton();

        if (locationEnabled) {
            if (!stackedWidget || stackedWidget->currentWidget() == this)
                refreshTimer->start();
            // Deferred: the backend chain (mmcli → gpspipe waiting on 10
            // messages → AT probe) previously ran before the page could
            // paint, freezing the hub for seconds. Show the page first.
            QTimer::singleShot(50, this, [this]() { refreshDataOnce(); });
        } else {
            gpsLabel->setText("Location is turned off");
            satLabel->setText("Visible satellites\n\nLocation is turned off");
            mapLabel->setText("Mini map of local area\n\nLocation is turned off");
        }
    }

private:
    QStackedWidget *stackedWidget = nullptr;

    QLabel *gpsLabel = nullptr;
    QLabel *satLabel = nullptr;
    QLabel *mapLabel = nullptr;

    QPushButton *powerButton = nullptr;
    QPushButton *refreshButton = nullptr;

    QTimer *refreshTimer = nullptr;
    bool locationEnabled = true;

    // -------------------------------------------------
    // UI helpers
    // -------------------------------------------------
    void updatePowerButton()
    {
        if (locationEnabled) {
            powerButton->setText("On");
            powerButton->setStyleSheet(
                "QPushButton {"
                " background:#444444;"
                " color:#7CFC00;"      // bright green
                " border:1px solid #222222;"
                " border-radius:16px;"
                " font-size:26px;"
                " font-weight:bold;"
                " padding:10px 24px;"
                "}"
                "QPushButton:hover { background:#555555; }"
                "QPushButton:pressed { background:#333333; }"
            );
        } else {
            powerButton->setText("Off");
            powerButton->setStyleSheet(
                "QPushButton {"
                " background:#444444;"
                " color:#CC6666;"      // dim red
                " border:1px solid #222222;"
                " border-radius:16px;"
                " font-size:26px;"
                " font-weight:bold;"
                " padding:10px 24px;"
                "}"
                "QPushButton:hover { background:#555555; }"
                "QPushButton:pressed { background:#333333; }"
            );
        }
    }

    void togglePower()
    {
        locationEnabled = !locationEnabled;

        // Save state
        QSettings settings(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                           QSettings::IniFormat);
        settings.setValue("Location/enabled", locationEnabled);
        settings.sync();

        updatePowerButton();

        if (!locationEnabled) {
            refreshTimer->stop();
            gpsLabel->setText("Location is turned off");
            satLabel->setText("Visible satellites\n\nLocation is turned off");
            mapLabel->setText("Mini map of local area\n\nLocation is turned off");
            return;
        }

        // If page visible, resume auto-refresh
        if (!stackedWidget || stackedWidget->currentWidget() == this)
            refreshTimer->start();

        refreshDataOnce();
    }

    void onStackIndexChanged(int idx)
    {
        if (!stackedWidget)
            return;

        if (stackedWidget->widget(idx) == this && locationEnabled) {
            refreshTimer->start();
        } else {
            refreshTimer->stop();
        }
    }

    // -------------------------------------------------
    // Data refresh
    // -------------------------------------------------
    void refreshDataOnce()
    {
        if (!locationEnabled) {
            return;
        }

        // Reentrancy guard: the sliced command waits pump events, so the 2s
        // auto-refresh timer can fire again mid-refresh; without this the
        // refreshes would nest and stack up.
        static bool busy = false;
        if (busy) return;
        busy = true;

        LocationInfo info = getBestLocation();

        if (!info.error.isEmpty()) {
            gpsLabel->setText(info.error);
            satLabel->setText(info.error);
            mapLabel->setText("Mini map of local area\n\n" + info.error);
            busy = false;
            return;
        }

        // GPS + heading text
        QString gpsText;
        if (info.hasFix) {
            gpsText = QString("Latitude: %1\nLongitude: %2")
                          .arg(info.lat, 0, 'f', 6)
                          .arg(info.lon, 0, 'f', 6);
        } else {
            gpsText = "No GPS fix";
        }

        if (info.hasHeading) {
            gpsText += QString("\nCompass heading: %1°")
                           .arg(info.heading, 0, 'f', 1);
        } else {
            gpsText += "\nCompass heading: unknown";
        }

        gpsLabel->setText(gpsText);

        // Satellites text
        QString satText;
        if (info.hasSat) {
            satText = QString("Visible satellites: %1").arg(info.satellites);
        } else {
            satText = "Visible satellites: unknown";
        }
        if (!info.source.isEmpty()) {
            satText += QString("\n\nSource: %1").arg(info.source);
        }

        satLabel->setText(satText);

        // Mini map placeholder text
        QString mapText = "Mini map of local area";
        if (info.hasFix) {
            mapText += QString("\n\nLat: %1\nLon: %2")
                           .arg(info.lat, 0, 'f', 6)
                           .arg(info.lon, 0, 'f', 6);
        } else {
            mapText += "\n\nNo position fix";
        }
        mapText += "\n(Use external map app for full view)";

        mapLabel->setText(mapText);
        busy = false;
    }

    // Manual refresh (button press): animates the Refresh button spinner
    // through the whole backend chain, unlike the silent auto-refresh.
    void manualRefresh()
    {
        if (!locationEnabled || !refreshButton->isEnabled()) return;

        refreshButton->setEnabled(false);
        int frame = 0;
        refreshButton->setText(QString::fromUtf8(SPIN_FRAMES_LOC[0]));

        g_locTick = [this, &frame]() {
            frame = (frame + 1) % SPIN_COUNT_LOC;
            refreshButton->setText(QString::fromUtf8(SPIN_FRAMES_LOC[frame]));
        };

        refreshDataOnce();

        g_locTick = nullptr;
        refreshButton->setText("Refresh");
        refreshButton->setEnabled(true);
    }
};

// ---------------------------------------------------------
// Factory function for osm-settings plugin
// ---------------------------------------------------------

extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new LocationPage(stack);
}
