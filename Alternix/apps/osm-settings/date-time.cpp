// AlternixOS Date & Time settings module.
// Extracted from Location: clock, RTC, Chrony and manual time sync operate
// independently of GPS/location permissions and the Location on/off switch.
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QFrame>
#include <QScrollArea>
#include <QScroller>
#include <QProcess>
#include <QMessageBox>
#include <QFont>
#include <QApplication>
#include <QTimer>
#include <QRegularExpression>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDialog>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QTimeZone>
#include <QFile>
#include <QPalette>
#include <QSizePolicy>
#include <QColor>
#include <functional>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

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

// Clock helper and network-time waits update the Date & Time spinner while
// still processing Qt events. The Location page has its own independent tick.
static std::function<void()> g_locTick;


// ---------------------------------------------------------
// Privileged clock operations
//
// System sudo handles authentication through its PAM service. Applications
// must not probe sudo -n or implement a second password dialog.
// ---------------------------------------------------------
static QString runTimeHelper(const QString &action, const QString &argument,
                             const QString &marker, int timeoutMs, bool *reached)
{
    if (reached) *reached = false;
    const QString helper = "/usr/local/sbin/alternix-time-helper";
    if (!QFileInfo(helper).isExecutable())
        return "The Alternix clock helper is not installed on this system.";

    QStringList args = {"--", helper, action};
    if (!argument.isEmpty()) args << argument;
    QString program = "/usr/bin/sudo";
    if (::geteuid() == 0) {
        program = helper;
        args.removeFirst();
        args.removeFirst();
    }

    QProcess process;
    process.setStandardInputFile(QProcess::nullDevice());
    process.start(program, args, QIODevice::ReadOnly);
    if (!process.waitForStarted(2000))
        return "The Alternix clock action could not start.";
    // The operation timeout is separate from the user's time entering a pattern.
    const int totalTimeoutMs = timeoutMs + (::geteuid() == 0 ? 0 : 120000);
    QElapsedTimer timer;
    timer.start();
    while (!process.waitForFinished(80)) {
        if (timer.elapsed() > totalTimeoutMs) {
            process.kill();
            process.waitForFinished(500);
            return "The clock action timed out.";
        }
        if (g_locTick) g_locTick();
        QCoreApplication::processEvents();
    }
    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    if (process.exitStatus() == QProcess::NormalExit &&
        process.exitCode() == 0 && output.contains(marker)) {
        if (reached) *reached = true;
        return output.trimmed();
    }
    return "Clock action was cancelled, denied by sudo/PAM, or failed. "
           "Check Alternix authentication and the clock helper permissions.";
}

// ---------------------------------------------------------
// Built-in SNTP client (RFC 4330)
//
// Deliberately self-contained. ntpdate is not installed on a default Devuan
// system and this has to work on a fresh machine with no extra packages and
// no daemon running. Plain UDP also succeeds when the clock is so far out
// that TLS certificate checks fail, which is exactly the state a tablet with
// a flat or missing RTC boots into.
// ---------------------------------------------------------

static const qint64 NTP_UNIX_DELTA = 2208988800LL;   // 1900 -> 1970, in seconds

static bool sntpQuery(const QString &host, quint16 port, int timeoutMs,
                      qint64 *unixMsOut, QString *errOut)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    struct addrinfo *res = nullptr;
    QByteArray portStr = QByteArray::number(port);
    int rc = ::getaddrinfo(host.toLatin1().constData(), portStr.constData(),
                           &hints, &res);
    if (rc != 0 || !res) {
        if (errOut)
            *errOut = QString("%1: %2").arg(host,
                          QString::fromLocal8Bit(gai_strerror(rc)));
        return false;
    }

    bool ok = false;
    QString lastErr;

    for (struct addrinfo *ai = res; ai && !ok; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK,
                          ai->ai_protocol);
        if (fd < 0)
            continue;

        unsigned char pkt[48];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x1B;          // LI 0, version 3, mode 3 (client)

        QElapsedTimer rtt;
        rtt.start();

        if (::sendto(fd, pkt, sizeof(pkt), 0, ai->ai_addr, ai->ai_addrlen)
                != static_cast<ssize_t>(sizeof(pkt))) {
            lastErr = QString("%1: could not send (%2)")
                          .arg(host, QString::fromLocal8Bit(strerror(errno)));
            ::close(fd);
            continue;
        }

        while (rtt.elapsed() < timeoutMs) {
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(fd, &rf);

            struct timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 50000;

            int r = ::select(fd + 1, &rf, nullptr, nullptr, &tv);
            if (r > 0) {
                unsigned char resp[48];
                ssize_t n = ::recv(fd, resp, sizeof(resp), 0);
                if (n == 48) {
                    qint64 elapsed = rtt.elapsed();

                    int stratum = resp[1];
                    if (stratum == 0 || stratum > 15) {
                        lastErr = QString("%1: server is not synchronised").arg(host);
                        break;
                    }

                    // Transmit timestamp: seconds at byte 40, fraction at 44.
                    quint32 secs = (static_cast<quint32>(resp[40]) << 24)
                                 | (static_cast<quint32>(resp[41]) << 16)
                                 | (static_cast<quint32>(resp[42]) << 8)
                                 |  static_cast<quint32>(resp[43]);
                    quint32 frac = (static_cast<quint32>(resp[44]) << 24)
                                 | (static_cast<quint32>(resp[45]) << 16)
                                 | (static_cast<quint32>(resp[46]) << 8)
                                 |  static_cast<quint32>(resp[47]);
                    if (secs == 0) {
                        lastErr = QString("%1: empty timestamp").arg(host);
                        break;
                    }

                    qint64 ms = (static_cast<qint64>(secs) - NTP_UNIX_DELTA) * 1000LL
                              + static_cast<qint64>(static_cast<double>(frac)
                                                    * 1000.0 / 4294967296.0);

                    // Half the round trip is the usual estimate of one-way delay.
                    ms += elapsed / 2;

                    if (unixMsOut) *unixMsOut = ms;
                    ok = true;
                }
                break;
            }

            if (g_locTick) g_locTick();
            QCoreApplication::processEvents();
        }

        ::close(fd);
        if (!ok && lastErr.isEmpty())
            lastErr = QString("%1: no reply on UDP port %2").arg(host).arg(port);
    }

    ::freeaddrinfo(res);
    if (!ok && errOut)
        *errOut = lastErr;
    return ok;
}

// Try the public pools in turn. Returns the first server that answers.
static bool sntpBestTime(qint64 *unixMsOut, QString *serverOut, QString *logOut)
{
    const QStringList servers = {
        "pool.ntp.org",
        "0.debian.pool.ntp.org",
        "time.cloudflare.com",
        "time.google.com"
    };

    for (const QString &s : servers) {
        QString err;
        if (sntpQuery(s, 123, 2500, unixMsOut, &err)) {
            if (serverOut) *serverOut = s;
            return true;
        }
        if (logOut)
            *logOut += "\n" + (err.isEmpty() ? s + ": no reply" : err);
    }
    return false;
}

// ---------------------------------------------------------
// Hardware clock (RTC) write-back after a manual clock change.
// RTC presence, RTC read access and RTC write access are DIFFERENT states.
// A failed hwclock command must never be reported as "no clock chip".
// Preserve the configured RTC time scale (LOCAL or UTC); never blindly
// rewrite a LOCAL/dual-boot RTC as UTC just because the first attempt fails.
// Normal continuous synchronisation is handled by chrony, not this helper.
// ---------------------------------------------------------

// RTC write operations are implemented by the root-owned, fixed-action
// alternix-time-helper installed by AlternixOS.



static QString readSysFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QString v = QString::fromLatin1(f.readAll()).trimmed();
    f.close();
    return v;
}


class DateTimePage : public QWidget
{
public:
    explicit DateTimePage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), stackedWidget(stack)
    {
        // -----------------------------
        // Global font override
        // -----------------------------
        QFont f = QApplication::font();
        f.setPixelSize(26);          // global size
        setFont(f);   // page-local: do NOT change the app-wide font

        // Global white text + dark bg
        //
        // GREY NUMBERS FIX - DO NOT REMOVE
        // This used to call qApp->setPalette(), which mutated the palette of
        // the whole application from inside a plugin and left every other
        // page's numbers rendering grey. The palette is now applied to this
        // page only, exactly like the page-local font above, and the
        // stylesheet uses explicit class selectors as wifi.cpp does. An
        // unqualified "color:white" does not reliably reach the internal
        // child widgets of QDateTimeEdit / QMessageBox, which is what made
        // the digits come out grey.
        QPalette pal = palette();
        pal.setColor(QPalette::WindowText, Qt::white);
        pal.setColor(QPalette::Text, Qt::white);
        pal.setColor(QPalette::ButtonText, Qt::white);
        setPalette(pal);

        setStyleSheet(
            "QScrollArea { background:#282828; font-family:Sans; border:none; }"
            "QWidget { background:#282828; font-family:Sans; }"
            "QLabel { color:white; font-family:Sans; }"
            "QMessageBox QLabel { color:white; font-family:Sans; }"
        );

        QVBoxLayout *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(40, 40, 40, 40);
        rootLayout->setSpacing(20);

        // Title
        QLabel *titleLabel = new QLabel("Date & Time", this);
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setStyleSheet(
            "QLabel { font-size:42px; font-weight:bold; color:white; }"
        );
        rootLayout->addWidget(titleLabel);

        // -------------------------------------------------
        // Scrollable middle: cards scroll, title stays pinned
        // -------------------------------------------------
        QScrollArea *midScroll = new QScrollArea(this);
        midScroll->setWidgetResizable(true);
        midScroll->setFrameShape(QFrame::NoFrame);
        midScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        midScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(midScroll->viewport(), QScroller::LeftMouseButtonGesture);

        QWidget *midContainer = new QWidget(midScroll);
        QVBoxLayout *midLayout = new QVBoxLayout(midContainer);
        midLayout->setContentsMargins(0, 0, 0, 0);
        midLayout->setSpacing(20);

        // -------------------------------------------------
        // Date & time controls
        // -------------------------------------------------
        QFrame *timeFrame = new QFrame(this);
        timeFrame->setStyleSheet(
            "QFrame {"
            " background:#3a3a3a;"
            " border-radius:40px;"
            "}"
        );
        timeFrame->setMinimumHeight(650);

        QVBoxLayout *timeLayout = new QVBoxLayout(timeFrame);
        timeLayout->setContentsMargins(30, 30, 30, 30);
        timeLayout->setSpacing(14);

        QLabel *timeHeading = new QLabel("Date & time", timeFrame);
        timeHeading->setAlignment(Qt::AlignCenter);
        timeHeading->setStyleSheet(
            "QLabel { font-size:30px; font-weight:bold; color:white; }"
        );
        timeLayout->addWidget(timeHeading);

        clockLabel = new QLabel("--", timeFrame);
        clockLabel->setAlignment(Qt::AlignCenter);
        clockLabel->setWordWrap(true);
        clockLabel->setMinimumWidth(1);
        clockLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        clockLabel->setStyleSheet("QLabel { font-size:34px; color:white; }");
        timeLayout->addWidget(clockLabel);

        tzInfoLabel = new QLabel("", timeFrame);
        tzInfoLabel->setAlignment(Qt::AlignCenter);
        tzInfoLabel->setWordWrap(true);
        tzInfoLabel->setMinimumWidth(1);
        tzInfoLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        tzInfoLabel->setStyleSheet("QLabel { font-size:22px; color:#BBBBBB; }");
        timeLayout->addWidget(tzInfoLabel);

        rtcStatusLabel = new QLabel("Hardware clock: checking...", timeFrame);
        rtcStatusLabel->setAlignment(Qt::AlignCenter);
        rtcStatusLabel->setWordWrap(true);
        rtcStatusLabel->setMinimumWidth(1);
        rtcStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        rtcStatusLabel->setStyleSheet("QLabel { font-size:20px; color:#BBBBBB; }");
        timeLayout->addWidget(rtcStatusLabel);

        autoSyncLabel = new QLabel("Automatic sync: checking...", timeFrame);
        autoSyncLabel->setAlignment(Qt::AlignCenter);
        autoSyncLabel->setWordWrap(true);
        autoSyncLabel->setMinimumWidth(1);
        autoSyncLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        autoSyncLabel->setStyleSheet("QLabel { font-size:20px; color:#BBBBBB; }");
        timeLayout->addWidget(autoSyncLabel);

        // Zone selector: Local / UTC / GMT
        QHBoxLayout *zoneRow = new QHBoxLayout();
        zoneRow->setSpacing(14);
        zoneRow->setAlignment(Qt::AlignHCenter);

        localZoneButton = smallBtnBT("Local");
        utcZoneButton   = smallBtnBT("UTC");
        gmtZoneButton   = smallBtnBT("GMT");
        // WIDTH FIX - DO NOT REMOVE
        // 3*104 + 2*14 = 340, which is the card's inner width at 720px. Any
        // wider and this row becomes the widest thing on the page and drags
        // the whole settings window past the screen edge.
        localZoneButton->setFixedSize(104, 56);
        utcZoneButton->setFixedSize(104, 56);
        gmtZoneButton->setFixedSize(104, 56);

        zoneRow->addWidget(localZoneButton);
        zoneRow->addWidget(utcZoneButton);
        zoneRow->addWidget(gmtZoneButton);
        timeLayout->addLayout(zoneRow);

        // Actions: change the clock by hand, or fetch it from the internet
        QHBoxLayout *timeActionRow = new QHBoxLayout();
        timeActionRow->setSpacing(20);
        timeActionRow->setAlignment(Qt::AlignHCenter);

        setTimeButton  = smallBtnBT("Change");
        syncTimeButton = smallBtnBT("Sync now");
        // WIDTH FIX - DO NOT REMOVE: 2*160 + 20 = 340
        setTimeButton->setFixedSize(160, 62);
        syncTimeButton->setFixedSize(160, 62);
        applyBtnColour(setTimeButton,  "white", 22);
        applyBtnColour(syncTimeButton, "white", 22);

        timeActionRow->addWidget(setTimeButton);
        timeActionRow->addWidget(syncTimeButton);
        timeLayout->addLayout(timeActionRow);

        timeStatusLabel = new QLabel("", timeFrame);
        timeStatusLabel->setAlignment(Qt::AlignCenter);
        timeStatusLabel->setWordWrap(true);
        timeStatusLabel->setMinimumWidth(1);
        timeStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        timeStatusLabel->setStyleSheet("QLabel { font-size:22px; color:#BBBBBB; }");
        timeLayout->addWidget(timeStatusLabel);

        midLayout->addWidget(timeFrame);

        midLayout->addStretch();

        midScroll->setWidget(midContainer);
        // Keep the title and back button visible on narrow screens while the
        // date/time controls scroll independently.
        rootLayout->addWidget(midScroll, 1);

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

        connect(backButton, &QPushButton::clicked, this, [this]() {
            if (stackedWidget) stackedWidget->setCurrentIndex(0);
        });
        connect(localZoneButton, &QPushButton::clicked,
                this, [this]() { setClockMode(0); });
        connect(utcZoneButton, &QPushButton::clicked,
                this, [this]() { setClockMode(1); });
        connect(gmtZoneButton, &QPushButton::clicked,
                this, [this]() { setClockMode(2); });
        connect(setTimeButton, &QPushButton::clicked,
                this, &DateTimePage::openSetTimeDialog);
        connect(syncTimeButton, &QPushButton::clicked,
                this, &DateTimePage::syncTimeFromInternet);

        clockTimer = new QTimer(this);
        clockTimer->setInterval(1000);
        connect(clockTimer, &QTimer::timeout,
                this, &DateTimePage::updateClock);
        if (stackedWidget) {
            connect(stackedWidget, &QStackedWidget::currentChanged,
                    this, &DateTimePage::onStackIndexChanged);
        }

        QSettings settings(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                           QSettings::IniFormat);
        clockMode = settings.value("DateTime/clock_mode", 0).toInt();
        if (clockMode < 0 || clockMode > 2) clockMode = 0;
        updateZoneButtons();
        updateClock();
        if (!stackedWidget || stackedWidget->currentWidget() == this)
            clockTimer->start();
    }

private:
    QStackedWidget *stackedWidget = nullptr;
    QLabel *clockLabel      = nullptr;
    QLabel *tzInfoLabel     = nullptr;
    QLabel *timeStatusLabel = nullptr;
    QLabel *rtcStatusLabel  = nullptr;
    QLabel *autoSyncLabel   = nullptr;
    QElapsedTimer timeHealthAge;

    QPushButton *localZoneButton = nullptr;
    QPushButton *utcZoneButton   = nullptr;
    QPushButton *gmtZoneButton   = nullptr;
    QPushButton *setTimeButton   = nullptr;
    QPushButton *syncTimeButton  = nullptr;

    QTimer *clockTimer = nullptr;
    int clockMode = 0; // 0 = Local, 1 = UTC, 2 = GMT

    void applyBtnColour(QPushButton *b, const QString &colour, int fontPx = 26)
    {
        if (!b) return;
        b->setStyleSheet(QString(
            "QPushButton {"
            " background:#444444;"
            " color:%1;"
            " border:1px solid #222222;"
            " border-radius:16px;"
            " font-size:%2px;"
            " font-weight:bold;"
            " padding:6px 12px;"
            "}"
            "QPushButton:hover { background:#555555; }"
            "QPushButton:pressed { background:#333333; }"
            "QPushButton:disabled { color:#888888; }"
        ).arg(colour).arg(fontPx));
    }

    void saveSetting(const QString &key, const QVariant &value)
    {
        QSettings settings(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                           QSettings::IniFormat);
        settings.setValue(key, value);
        settings.sync();
    }

    void showDetail(const QString &title, const QString &text, const QString &detail,
                    QMessageBox::Icon icon = QMessageBox::Critical)
    {
        QMessageBox box(this);
        box.setIcon(icon);
        box.setWindowTitle(title);
        box.setText(text);
        if (!detail.trimmed().isEmpty())
            box.setDetailedText(detail);
        box.setStyleSheet("background:#282828; color:white; font-size:24px;");
        box.exec();
    }

    // Animate the clock buttons during time sync without affecting GPS.
    void runWithSpinner(QPushButton *btn, const QString &restoreText,
                        const std::function<void()> &work)
    {
        if (!btn || !btn->isEnabled())
            return;

        if (setTimeButton)  setTimeButton->setEnabled(false);
        if (syncTimeButton) syncTimeButton->setEnabled(false);

        int frame = 0;
        btn->setText(QString::fromUtf8(SPIN_FRAMES_LOC[0]));
        g_locTick = [btn, &frame]() {
            frame = (frame + 1) % SPIN_COUNT_LOC;
            btn->setText(QString::fromUtf8(SPIN_FRAMES_LOC[frame]));
        };

        work();

        g_locTick = nullptr;
        btn->setText(restoreText);
        if (setTimeButton)  setTimeButton->setEnabled(true);
        if (syncTimeButton) syncTimeButton->setEnabled(true);
    }

    // -------------------------------------------------
    // Date & time
    // -------------------------------------------------

    // Note: GMT and UTC are the same offset. GMT is offered because that is
    // what the label says on most clocks; it is strictly UTC+0 here and does
    // not follow British Summer Time.
    QDateTime nowInMode() const
    {
        QDateTime n = QDateTime::currentDateTime();
        if (clockMode == 0)
            return n;
        return n.toUTC();
    }

    QString zoneSuffix() const
    {
        if (clockMode == 1) return QStringLiteral("UTC");
        if (clockMode == 2) return QStringLiteral("GMT");
        return QDateTime::currentDateTime().timeZoneAbbreviation();
    }

    void updateClock()
    {
        if (!clockLabel)
            return;

        QDateTime n = nowInMode();
        clockLabel->setText(n.toString("dddd d MMMM yyyy") + "\n" +
                            n.toString("HH:mm:ss") + " " + zoneSuffix());

        if (tzInfoLabel) {
            QTimeZone tz = QTimeZone::systemTimeZone();
            int offs = tz.offsetFromUtc(QDateTime::currentDateTime());
            QChar sign = (offs < 0) ? QChar('-') : QChar('+');
            int a = qAbs(offs);
            QString offStr = QString("%1%2:%3")
                                 .arg(sign)
                                 .arg(a / 3600, 2, 10, QChar('0'))
                                 .arg((a % 3600) / 60, 2, 10, QChar('0'));
            tzInfoLabel->setText(QString("System time zone: %1  (UTC%2)")
                                     .arg(QString::fromUtf8(tz.id()), offStr));
        }
        // Clock readout is every second; hardware/service probes are not.
        if (!timeHealthAge.isValid() || timeHealthAge.elapsed() >= 20000) {
            timeHealthAge.restart();
            updateTimeHealth();
        }
    }

    void updateTimeHealth()
    {
        if (rtcStatusLabel) {
            QDir d("/sys/class/rtc");
            QStringList nodes = d.entryList(QStringList() << "rtc*",
                QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
            QStringList found;
            for (const QString &node : nodes) {
                QString name = readSysFile(d.filePath(node + "/name"));
                const bool readable = !readSysFile(d.filePath(node + "/time")).isEmpty();
                QString description = node;
                if (!name.isEmpty()) description += " (" + name + ")";
                if (!readable) description += " [not readable]";
                found << description;
            }
            if (found.isEmpty()) {
                for (const QString &node : QStringList({"/dev/rtc", "/dev/rtc0", "/dev/rtc1"})) {
                    if (QFile::exists(node)) found << node;
                }
            }
            QString hwclock = QStandardPaths::findExecutable("hwclock");
            if (hwclock.isEmpty()) {
                // X sessions often omit /usr/sbin from PATH even when the
                // util-linux-extra package and hwclock are both installed.
                const QStringList paths = {
                    "/usr/sbin/hwclock", "/sbin/hwclock",
                    "/usr/bin/hwclock", "/bin/hwclock",
                    "/usr/local/sbin/hwclock", "/usr/local/bin/hwclock"
                };
                for (const QString &candidate : paths) {
                    QFileInfo fi(candidate);
                    if (fi.isFile() && fi.isExecutable()) {
                        hwclock = candidate;
                        break;
                    }
                }
            }
            QString status = found.isEmpty()
                ? "Hardware clock: not exposed to Linux (BIOS RTC may still exist)"
                : "Hardware clock detected: " + found.join(", ");
            if (hwclock.isEmpty()) {
                // A missing executable is NOT evidence that the package was
                // never installed or that this computer has no BIOS clock.
                QProcess pkg;
                pkg.start("dpkg-query", {"-W", "-f=${Status}", "util-linux-extra"});
                bool installed = pkg.waitForStarted(300) &&
                    pkg.waitForFinished(1000) && pkg.exitCode() == 0 &&
                    QString::fromLocal8Bit(pkg.readAllStandardOutput())
                        .contains("install ok installed");
                if (pkg.state() != QProcess::NotRunning) {
                    pkg.kill(); pkg.waitForFinished(150);
                }
                status += installed
                    ? "\nhwclock command is unavailable, but util-linux-extra IS installed"
                    : "\nhwclock command unavailable (check util-linux-extra installation)";
            }
            rtcStatusLabel->setText(status);
            rtcStatusLabel->setStyleSheet(QString("QLabel { font-size:20px; color:%1; }")
                .arg(found.isEmpty() ? "#FFC24B" : "#7CFC00"));
        }

        if (autoSyncLabel) {
            QString text;
            QString colour = "#FFC24B";
            if (!QFile::exists("/usr/bin/chronyc") && !QFile::exists("/bin/chronyc")) {
                text = "Automatic sync: Chrony not installed (Sync now is manual only)";
            } else {
                QProcess probe;
                probe.start("chronyc", {"-n", "tracking"});
                if (probe.waitForStarted(300) && probe.waitForFinished(1200) &&
                    probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0) {
                    QString report = QString::fromLocal8Bit(probe.readAllStandardOutput());
                    if (report.contains(QRegularExpression("Leap status\\s*:\\s*Normal"))) {
                        text = "Automatic sync: Chrony synchronised";
                        colour = "#7CFC00";
                    } else {
                        text = "Automatic sync: Chrony running, waiting for a valid time source";
                    }
                } else {
                    if (probe.state() != QProcess::NotRunning) {
                        probe.kill();
                        probe.waitForFinished(150);
                    }
                    text = "Automatic sync: Chrony not responding";
                }
            }
            autoSyncLabel->setText(text);
            autoSyncLabel->setStyleSheet(QString("QLabel { font-size:20px; color:%1; }")
                .arg(colour));
        }
    }

    void updateZoneButtons()
    {
        applyBtnColour(localZoneButton, clockMode == 0 ? "#7CFC00" : "white", 24);
        applyBtnColour(utcZoneButton,   clockMode == 1 ? "#7CFC00" : "white", 24);
        applyBtnColour(gmtZoneButton,   clockMode == 2 ? "#7CFC00" : "white", 24);
    }

    void setClockMode(int mode)
    {
        clockMode = mode;
        saveSetting("DateTime/clock_mode", mode);
        updateZoneButtons();
        updateClock();
    }

    void setTimeStatus(const QString &text, const QString &colour)
    {
        if (!timeStatusLabel)
            return;
        timeStatusLabel->setText(text);
        timeStatusLabel->setStyleSheet(
            QString("QLabel { font-size:22px; color:%1; }").arg(colour));
    }

    void openSetTimeDialog()
    {
        QDialog dlg(this);
        dlg.setWindowTitle("Change date & time");
        dlg.setModal(true);
        // Explicit selectors, not a bare "color:white": see the grey numbers
        // note in the constructor.
        dlg.setStyleSheet(
            "QWidget { background:#282828; font-family:Sans; }"
            "QLabel { color:white; font-family:Sans; }"
        );
        // WIDTH FIX - DO NOT REMOVE
        // Must stay inside a 720px wide screen.
        dlg.setMaximumWidth(700);
        dlg.resize(640, 470);

        QVBoxLayout *lay = new QVBoxLayout(&dlg);
        lay->setContentsMargins(30, 30, 30, 30);
        lay->setSpacing(18);

        QLabel *head = new QLabel(
            clockMode == 0
                ? QString("Enter the time shown on your local clock")
                : QString("Enter the time in %1").arg(zoneSuffix()),
            &dlg);
        head->setAlignment(Qt::AlignCenter);
        head->setWordWrap(true);
        head->setMinimumWidth(1);
        head->setStyleSheet("QLabel { font-size:28px; font-weight:bold; color:white; }");
        lay->addWidget(head);

        QDateTimeEdit *edit = new QDateTimeEdit(&dlg);
        edit->setTimeSpec(clockMode == 0 ? Qt::LocalTime : Qt::UTC);
        edit->setDisplayFormat("dd MMM yyyy  HH:mm:ss");
        edit->setCalendarPopup(false);
        edit->setButtonSymbols(QAbstractSpinBox::NoButtons);
        edit->setAlignment(Qt::AlignCenter);
        edit->setMinimumHeight(96);
        edit->setMinimumWidth(1);
        edit->setDateTimeRange(QDateTime(QDate(2000, 1, 1), QTime(0, 0, 0)),
                               QDateTime(QDate(2099, 12, 31), QTime(23, 59, 59)));
        edit->setDateTime(nowInMode());

        // GREY NUMBERS FIX - DO NOT REMOVE
        // QDateTimeEdit draws its digits through an internal QLineEdit, which
        // takes its colour from the palette rather than from a stylesheet rule
        // on the QDateTimeEdit itself. Setting both is what keeps the digits
        // white instead of grey. The palette is set on this widget only.
        QPalette ep = edit->palette();
        ep.setColor(QPalette::Text,            Qt::white);
        ep.setColor(QPalette::WindowText,      Qt::white);
        ep.setColor(QPalette::ButtonText,      Qt::white);
        ep.setColor(QPalette::HighlightedText, QColor("#222222"));
        ep.setColor(QPalette::Highlight,       QColor("#7CFC00"));
        ep.setColor(QPalette::Base,            QColor("#3a3a3a"));
        ep.setColor(QPalette::Button,          QColor("#3a3a3a"));
        edit->setPalette(ep);

        edit->setStyleSheet(
            "QDateTimeEdit {"
            " background:#3a3a3a;"
            " color:white;"
            " border:1px solid #222222;"
            " border-radius:16px;"
            " font-size:34px;"
            " font-family:Sans;"
            " padding:8px 14px;"
            "}"
            "QDateTimeEdit QLineEdit {"
            " background:transparent;"
            " color:white;"
            " border:none;"
            "}"
            "QDateTimeEdit::section:selected { background:#7CFC00; color:#222222; }"
        );
        lay->addWidget(edit);

        QLabel *hint = new QLabel("Tap a number, then use the arrows below", &dlg);
        hint->setAlignment(Qt::AlignCenter);
        hint->setStyleSheet("QLabel { font-size:22px; color:#BBBBBB; }");
        lay->addWidget(hint);

        // Explicit large step buttons: the spin box's own arrows are far too
        // small for a finger on a tablet.
        QHBoxLayout *stepRow = new QHBoxLayout();
        stepRow->setSpacing(30);
        stepRow->setAlignment(Qt::AlignHCenter);

        QPushButton *downBtn = smallBtnBT(QStringLiteral("\u25BC"));
        QPushButton *upBtn   = smallBtnBT(QStringLiteral("\u25B2"));
        downBtn->setFixedSize(160, 70);
        upBtn->setFixedSize(160, 70);
        stepRow->addWidget(downBtn);
        stepRow->addWidget(upBtn);
        lay->addLayout(stepRow);

        connect(downBtn, &QPushButton::clicked, edit, &QDateTimeEdit::stepDown);
        connect(upBtn,   &QPushButton::clicked, edit, &QDateTimeEdit::stepUp);

        QHBoxLayout *actionRow = new QHBoxLayout();
        actionRow->setSpacing(30);
        actionRow->setAlignment(Qt::AlignHCenter);

        QPushButton *cancelBtn = smallBtnBT("Cancel");
        QPushButton *applyBtn  = smallBtnBT("Set");
        cancelBtn->setFixedSize(190, 66);
        applyBtn->setFixedSize(190, 66);
        actionRow->addWidget(cancelBtn);
        actionRow->addWidget(applyBtn);
        lay->addLayout(actionRow);

        connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
        connect(applyBtn,  &QPushButton::clicked, &dlg, &QDialog::accept);

        // Date & Time runs independently of the Location page.
        int result = dlg.exec();

        if (result != QDialog::Accepted)
            return;

        applySystemDateTime(edit->dateTime().toUTC());
    }

    void applySystemDateTime(const QDateTime &utc)
    {
        // The helper only accepts a UTC epoch; never pass a shell script.
        const QString epoch = QString::number(utc.toSecsSinceEpoch());
        bool reached = false;
        QString out;
        runWithSpinner(setTimeButton, "Change", [&]() {
            out = runTimeHelper("set", epoch, "OSMSET_", 30000, &reached);
        });

        if (!reached) {
            setTimeStatus("The clock could not be changed: permission was refused.",
                          "#FF6B6B");
            showDetail("Change date & time",
                       "The system would not allow the clock to be changed.",
                       out);
            return;
        }

        if (out.contains("OSMSET_FAIL")) {
            setTimeStatus("The clock could not be changed: the date command failed.",
                          "#FF6B6B");
            showDetail("Change date & time",
                       "The date could not be applied to the system clock.",
                       out);
            return;
        }

        timeHealthAge.invalidate();
        updateClock();

        if (out.contains("OSMSET_OK:fake")) {
            setTimeStatus("Clock changed. Linux could not access an RTC; an approximate "
                          "boot timestamp was saved to disk.", "#FFC24B");
        } else if (out.contains("OSMSET_OK:unwritable") ||
                   out.contains("OSMSET_OK:unavailable")) {
            setTimeStatus("Clock changed; RTC write-back was unavailable. "
                          "See the hardware-clock diagnostics.", "#FFC24B");
            showDetail("Hardware-clock write problem",
                       "Linux could not write the hardware clock. This does NOT mean "
                       "that the BIOS clock is absent. See the diagnostics below.",
                       out, QMessageBox::Warning);
        } else {
            setTimeStatus("Clock changed by hand at " +
                          QDateTime::currentDateTime().toString("HH:mm:ss") + ".",
                          "#7CFC00");
        }
    }

    // If Chrony is running, ask IT to refresh rather than force-setting the
    // system clock behind the synchronisation daemon's back.
    bool syncViaChrony()
    {
        if (!QFile::exists("/usr/bin/chronyc") && !QFile::exists("/bin/chronyc"))
            return false;
        QProcess probe;
        probe.start("chronyc", {"-n", "tracking"});
        if (!probe.waitForStarted(250) || !probe.waitForFinished(900) ||
            probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0) {
            if (probe.state() != QProcess::NotRunning) {
                probe.kill(); probe.waitForFinished(150);
            }
            return false;
        }

        // Chrony is already synchronising the clock in the background.
        // Merely checking its status never needs root or an osm-sudo pattern.
        // In particular, do not attempt the privileged helper for a clock
        // that Chrony reports as synchronised: this is not a forced sync button.
        const QString tracking = QString::fromLocal8Bit(probe.readAllStandardOutput());
        if (tracking.contains(QRegularExpression(
                "Leap status\\s*:\\s*Normal", QRegularExpression::CaseInsensitiveOption))) {
            setTimeStatus("Clock already synchronised by Chrony. "
                          "Automatic corrections are active; no manual clock change needed.",
                          "#7CFC00");
            timeHealthAge.invalidate();
            updateClock();
            return true;
        }

        // Only an unsynchronised Chrony needs a privileged intervention.
        bool reached = false;
        QString out;
        runWithSpinner(syncTimeButton, "Sync now", [&]() {
            out = runTimeHelper("chrony", QString(), "CHRONY_REACHED", 30000, &reached);
        });
        if (reached && out.contains("CHRONY_SYNC_OK")) {
            setTimeStatus("Chrony has refreshed the automatic network time sync.", "#7CFC00");
        } else {
            setTimeStatus("Chrony could not confirm a network time source. "
                          "It will retry when connectivity returns.", "#FFC24B");
            showDetail("Automatic time sync", "Chrony did not synchronise yet.",
                       out, QMessageBox::Warning);
        }
        timeHealthAge.invalidate();
        updateClock();
        return true;
    }

    void syncTimeFromInternet()
    {
        if (syncViaChrony()) return;
        qint64 unixMs = 0;
        QString server;
        QString sntpLog;
        bool haveSntp = false;
        bool reached = false;
        QString out;

        runWithSpinner(syncTimeButton, "Sync now", [&]() {
            // Step 1: ask an NTP server directly. No package needed, and it
            // works even when the clock is far enough out to break TLS.
            haveSntp = sntpBestTime(&unixMs, &server, &sntpLog);

            if (haveSntp) {
                QString epoch = QString("%1.%2")
                                    .arg(unixMs / 1000)
                                    .arg(unixMs % 1000, 3, 10, QChar('0'));
                out = runTimeHelper("network-set", epoch, "OSMSYNC_", 30000, &reached);
            } else {
                // When UDP/123 is blocked, retain the existing HTTP Date
                // fallback in the fixed-action root helper.
                out = runTimeHelper("http-sync", QString(), "OSMSYNC_", 90000, &reached);
            }
        });

        // Every diagnostic goes in front of the user, not to /dev/null.
        QString detail = out;
        if (!sntpLog.trimmed().isEmpty())
            detail += "\n\nTime servers tried:" + sntpLog;

        if (!reached) {
            setTimeStatus("The clock could not be synced: permission was refused.",
                          "#FF6B6B");
            showDetail("Sync date & time",
                       "The system would not allow the clock to be changed.",
                       detail);
            return;
        }

        if (out.contains("OSMSYNC_FAIL")) {
            setTimeStatus("The clock could not be synced. Check that you are "
                          "connected to a network, then try again.",
                          "#FF6B6B");
            showDetail("Sync date & time",
                       "No time source could be reached.",
                       detail);
            return;
        }

        timeHealthAge.invalidate();
        updateClock();

        QString source;
        QRegularExpression okRe("OSMSYNC_OK:([^:]*):(.*)");
        QRegularExpressionMatch okM = okRe.match(out);
        bool rtcFailed = false;
        bool rtcFake = false;
        if (okM.hasMatch()) {
            QString state = okM.captured(1).trimmed();
            rtcFailed = (state == "unwritable" || state == "unavailable");
            rtcFake   = (state == "fake");
            source = okM.captured(2).trimmed();
        }

        QString when = QDateTime::currentDateTime().toString("HH:mm:ss");
        if (rtcFake) {
            setTimeStatus(QString("Clock synced at %1. No RTC was accessible to "
                                  "Linux; an approximate boot time was saved to disk.").arg(when),
                          "#FFC24B");
        } else if (rtcFailed) {
            setTimeStatus(QString("Clock synced at %1, but Linux could not write "
                                  "the RTC. The hardware may still be present.").arg(when),
                          "#FFC24B");
            showDetail("Hardware-clock write problem",
                       "The system time was corrected, but Linux could not write "
                       "the RTC. This does not establish that the BIOS clock "
                       "is missing. The actual error follows.",
                       detail, QMessageBox::Warning);
        } else if (!source.isEmpty()) {
            setTimeStatus(QString("Clock synced at %1 from %2.").arg(when, source),
                          "#7CFC00");
        } else {
            setTimeStatus(QString("Clock synced at %1.").arg(when), "#7CFC00");
        }
    }

    void onStackIndexChanged(int index)
    {
        if (!stackedWidget || !clockTimer) return;
        const bool visible = stackedWidget->widget(index) == this;
        if (visible) {
            timeHealthAge.invalidate();
            updateClock();
            clockTimer->start();
        } else {
            clockTimer->stop();
        }
    }
};

extern "C" QWidget* make_page(QStackedWidget *stack)
{
    return new DateTimePage(stack);
}
