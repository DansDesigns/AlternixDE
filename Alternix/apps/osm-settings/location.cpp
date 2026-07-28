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
#include <QSpacerItem>
#include <QApplication>
#include <QTimer>
#include <QRegularExpression>
#include <QSettings>
#include <QDir>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QDialog>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QTimeZone>
#include <QProcessEnvironment>
#include <QFile>
#include <QPair>
#include <functional>

// Direct serial access for GNSS receivers that speak plain NMEA.
// Deliberately POSIX rather than QtSerialPort: no extra Qt module and no
// extra -dev package needed at build time.
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

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
    QString place;    // estimated town / country, when one is known
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

// ---------------------------------------------------------
// Runtime options (mirrored from osm-settings.conf so the 2s refresh
// loop does not re-read the INI file on every tick)
// ---------------------------------------------------------

static bool g_netLocEnabled    = false;   // Location/network_enabled
static bool g_serialGpsEnabled = true;    // Location/serial_gps_enabled

// ---------------------------------------------------------
// Root command runner
//
// Fast path is `sudo -n`, which succeeds because install-alternix_devuan.sh
// grants NOPASSWD. If that path produces no marker line the request never
// reached the script (sudo refused), so we retry through SUDO_ASKPASS
// (osm-sudo). Waits are polled rather than a single fixed waitForFinished
// so the caller's spinner keeps animating and long syncs are not truncated.
// ---------------------------------------------------------

static QString askpassHelperPath()
{
    QString fromEnv = qEnvironmentVariable("SUDO_ASKPASS");
    if (!fromEnv.isEmpty() && QFile::exists(fromEnv))
        return fromEnv;

    const QStringList candidates = {
        "/usr/bin/osm-sudo",
        "/usr/local/bin/osm-sudo",
        "/usr/lib/alternix/osm-sudo"
    };
    for (const QString &c : candidates) {
        if (QFile::exists(c))
            return c;
    }
    return QString();
}

// Returns combined stdout+stderr. `reachedScript` reports whether the
// script itself ran (marker seen), as opposed to sudo blocking us.
static QString runAsRootShell(const QString &script,
                              const QString &marker,
                              int timeoutMs,
                              bool *reachedScript)
{
    if (reachedScript) *reachedScript = false;

    auto attempt = [&](bool useAskpass) -> QString {
        QProcess proc;

        if (useAskpass) {
            QString helper = askpassHelperPath();
            if (helper.isEmpty())
                return QStringLiteral("osm-sudo askpass helper not found");
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("SUDO_ASKPASS", helper);
            proc.setProcessEnvironment(env);
            proc.start("sudo", {"-A", "/bin/sh", "-c", script});
        } else {
            proc.start("sudo", {"-n", "/bin/sh", "-c", script});
        }

        QElapsedTimer t;
        t.start();
        while (!proc.waitForFinished(80)) {
            if (proc.error() == QProcess::FailedToStart)
                return QStringLiteral("sudo could not be started");
            if (t.elapsed() > timeoutMs) {
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
    };

    QString out = attempt(false);
    if (out.contains(marker)) {
        if (reachedScript) *reachedScript = true;
        return out;
    }

    QString fallback = attempt(true);
    if (fallback.contains(marker)) {
        if (reachedScript) *reachedScript = true;
        return fallback;
    }

    return out + "\n" + fallback;
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
// Battery-backed clock (RTC)
//
// `hwclock --systohc` alone is not enough on these tablets: the plain call
// can fail while writing through an explicit /dev/rtc node or the direct ISA
// path still works. The earlier version sent hwclock's error to /dev/null,
// so a failure was reported with no reason attached. This keeps the reason.
// ---------------------------------------------------------

static const char *RTC_WRITE_SH = R"SH(
RTCLOG=""

# Read the clock chip back through sysfs and see whether it now agrees with
# the system clock. This does not depend on hwclock's exit status, which can
# be non-zero for a warning even when the write actually landed.
RTC_SEEN=0
rtc_readback_ok() {
  for r in /sys/class/rtc/rtc0 /sys/class/rtc/rtc1; do
    [ -r "$r/date" ] && [ -r "$r/time" ] || continue
    RTC_SEEN=1
    RD=$(cat "$r/date" 2>/dev/null)
    RT=$(cat "$r/time" 2>/dev/null)
    [ -n "$RD" ] && [ -n "$RT" ] || continue
    RE=$(date -u -d "$RD $RT" +%s 2>/dev/null) || continue
    NOW=$(date -u +%s)
    D=$((RE - NOW))
    [ "$D" -lt 0 ] && D=$((0 - D))
    if [ "$D" -le 5 ]; then return 0; fi
    RTCLOG="$RTCLOG
$r reads $RD $RT UTC, which is ${D}s away from the system clock"
  done
  return 1
}

rtc_write() {
  HW=""
  for c in /sbin/hwclock /usr/sbin/hwclock /bin/hwclock /usr/bin/hwclock; do
    if [ -x "$c" ]; then HW="$c"; break; fi
  done

  if [ -z "$HW" ]; then
    RTCLOG="hwclock is not installed (it comes from the util-linux package)"
    return 1
  fi

  if O=$("$HW" --systohc 2>&1); then return 0; fi
  RTCLOG="$HW --systohc: $O"

  for d in /dev/rtc /dev/rtc0 /dev/rtc1; do
    [ -e "$d" ] || continue
    if O=$("$HW" --systohc -f "$d" 2>&1); then return 0; fi
    RTCLOG="$RTCLOG
$HW --systohc -f $d: $O"
  done

  if O=$("$HW" --systohc --directisa 2>&1); then return 0; fi
  RTCLOG="$RTCLOG
$HW --systohc --directisa: $O"

  # hwclock may have complained while still setting the chip correctly.
  if rtc_readback_ok; then
    RTCLOG="$RTCLOG

hwclock reported an error, but the clock chip reads back correctly, so the
time has in fact been stored."
    return 0
  fi

  if [ "$RTC_SEEN" = 0 ] && [ ! -e /dev/rtc0 ] && [ ! -e /dev/rtc ] && [ ! -e /dev/rtc1 ]; then
    RTCLOG="$RTCLOG

There is no /dev/rtc0 on this machine, so the kernel has not registered a
battery-backed clock at all. The time will have to be synced again after
every power off."
  fi
  return 1
}
)SH";

// ---------------------------------------------------------
// Direct serial NMEA backend (offline GNSS chips)
//
// Covers receivers exposed as a plain serial port: u-blox over USB CDC
// (/dev/ttyACM*), USB dongles (/dev/ttyUSB*) and on-board GNSS wired to a
// legacy UART (/dev/ttyS*). Works with no daemon at all, so it functions
// when gpsd is absent and there is no network.
//
// The port and baud rate are cached once found: the page auto-refreshes
// every 2 seconds and a blind re-probe of every tty at every baud rate on
// each tick would stall the UI completely.
// ---------------------------------------------------------

static QString g_nmeaPort;                 // cached working device node
static speed_t g_nmeaBaud = B9600;         // cached working baud
static QElapsedTimer g_nmeaProbeAge;       // time since last full probe
static bool g_nmeaProbed = false;
static QString g_nmeaError;                // last open() failure, if any

static const speed_t NMEA_BAUDS[] = { B9600, B115200, B38400, B4800 };
static const int NMEA_BAUD_COUNT = 4;

static const char* nmeaBaudName(speed_t b)
{
    if (b == B9600)   return "9600";
    if (b == B115200) return "115200";
    if (b == B38400)  return "38400";
    if (b == B4800)   return "4800";
    return "?";
}

static bool nmeaChecksumOk(const QString &sentence)
{
    int star = sentence.lastIndexOf('*');
    if (star < 1 || star + 2 >= sentence.size())
        return false;

    unsigned char sum = 0;
    for (int i = 1; i < star; ++i)
        sum ^= static_cast<unsigned char>(sentence.at(i).toLatin1());

    bool ok = false;
    unsigned int given = sentence.mid(star + 1, 2).toUInt(&ok, 16);
    return ok && given == sum;
}

// Read raw bytes from one port at one baud rate for up to msTimeout.
static QByteArray readNmeaRaw(const QString &dev, speed_t baud, int msTimeout)
{
    QByteArray buf;

    int fd = ::open(dev.toLocal8Bit().constData(),
                    O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        if (errno == EACCES || errno == EPERM) {
            g_nmeaError = QString("%1: permission denied "
                                  "(user is not in the 'dialout' group)")
                              .arg(dev);
        }
        return buf;
    }

    struct termios tio;
    if (::tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        return buf;
    }

    ::cfmakeraw(&tio);
    ::cfsetispeed(&tio, baud);
    ::cfsetospeed(&tio, baud);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    ::tcsetattr(fd, TCSANOW, &tio);
    ::tcflush(fd, TCIFLUSH);

    QElapsedTimer t;
    t.start();
    while (t.elapsed() < msTimeout) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 60000;

        int r = ::select(fd + 1, &rf, nullptr, nullptr, &tv);
        if (r > 0) {
            char tmp[512];
            ssize_t n = ::read(fd, tmp, sizeof(tmp));
            if (n > 0)
                buf.append(tmp, static_cast<int>(n));
        }

        if (g_locTick) g_locTick();
        QCoreApplication::processEvents();

        // Enough for a full position + satellite picture: stop early.
        if (buf.contains("RMC") && buf.contains("GGA") && buf.size() > 512)
            break;
    }

    ::close(fd);
    return buf;
}

// Does this blob contain at least one structurally valid NMEA sentence?
static bool looksLikeNmea(const QByteArray &raw)
{
    const QStringList lines = QString::fromLatin1(raw).split('\n');
    for (const QString &lineRaw : lines) {
        QString line = lineRaw.trimmed();
        if (line.startsWith('$') && line.contains('*') && nmeaChecksumOk(line))
            return true;
    }
    return false;
}

static QStringList nmeaCandidatePorts()
{
    QStringList ports;

    // Named GNSS nodes first (udev rules, gpsd symlinks).
    const QStringList named = { "/dev/gps0", "/dev/gnss0", "/dev/ttyGPS0" };
    for (const QString &p : named) {
        if (QFile::exists(p))
            ports << p;
    }

    // USB CDC and USB serial dongles.
    for (int i = 0; i < 4; ++i) {
        QString p = QString("/dev/ttyACM%1").arg(i);
        if (QFile::exists(p)) ports << p;
    }
    for (int i = 0; i < 4; ++i) {
        QString p = QString("/dev/ttyUSB%1").arg(i);
        if (QFile::exists(p)) ports << p;
    }

    // Legacy UARTs last. On x86 tablets ttyS0-3 always exist whether or not
    // anything is wired to them, so probing these is the slow case.
    for (int i = 0; i < 6; ++i) {
        QString p = QString("/dev/ttyS%1").arg(i);
        if (QFile::exists(p)) ports << p;
    }

    return ports;
}

// DDMM.MMMM -> decimal degrees
static double nmeaToDecimal(const QString &coord, const QString &hem)
{
    if (coord.isEmpty())
        return 0.0;
    bool ok = false;
    double val = coord.toDouble(&ok);
    if (!ok)
        return 0.0;

    int degrees   = static_cast<int>(val / 100.0);
    double minutes = val - degrees * 100.0;
    double dec = degrees + minutes / 60.0;

    if (hem == "S" || hem == "W")
        dec = -dec;
    return dec;
}

static void parseNmea(const QByteArray &raw, LocationInfo &info)
{
    const QStringList lines = QString::fromLatin1(raw).split('\n');

    int gsvInView = -1;

    for (const QString &lineRaw : lines) {
        QString line = lineRaw.trimmed();
        if (!line.startsWith('$') || !line.contains('*'))
            continue;
        if (!nmeaChecksumOk(line))
            continue;

        QString body = line.left(line.lastIndexOf('*'));
        QStringList f = body.split(',', Qt::KeepEmptyParts);
        if (f.isEmpty())
            continue;

        // Talker ID varies by constellation: GP/GN/GL/GA/BD/QZ.
        QString type = f.value(0);
        if (type.size() < 6)
            continue;
        QString kind = type.right(3);

        if (kind == "GGA" && f.size() >= 8) {
            // 1 time, 2 lat, 3 N/S, 4 lon, 5 E/W, 6 fix quality, 7 sats used
            int quality = f.value(6).toInt();
            if (quality > 0) {
                double lat = nmeaToDecimal(f.value(2), f.value(3));
                double lon = nmeaToDecimal(f.value(4), f.value(5));
                if (lat != 0.0 || lon != 0.0) {
                    info.lat = lat;
                    info.lon = lon;
                    info.hasFix = true;
                }
            }
            bool ok = false;
            int used = f.value(7).toInt(&ok);
            if (ok && used > 0) {
                info.satellites = used;
                info.hasSat = true;
            }
        } else if (kind == "RMC" && f.size() >= 9) {
            // 1 time, 2 status, 3 lat, 4 N/S, 5 lon, 6 E/W, 7 speed, 8 course
            if (f.value(2) == "A") {
                double lat = nmeaToDecimal(f.value(3), f.value(4));
                double lon = nmeaToDecimal(f.value(5), f.value(6));
                if (lat != 0.0 || lon != 0.0) {
                    info.lat = lat;
                    info.lon = lon;
                    info.hasFix = true;
                }
                bool ok = false;
                double course = f.value(8).toDouble(&ok);
                if (ok && !f.value(8).isEmpty()) {
                    info.heading = course;
                    info.hasHeading = true;
                }
            }
        } else if (kind == "GSV" && f.size() >= 4) {
            // 3 = satellites in view for this constellation
            bool ok = false;
            int inView = f.value(3).toInt(&ok);
            if (ok) {
                if (gsvInView < 0) gsvInView = 0;
                gsvInView = qMax(gsvInView, inView);
            }
        }
    }

    // Satellites in view is the more useful "can it see the sky" number;
    // prefer it over the smaller "used in fix" count from GGA.
    if (gsvInView > 0) {
        info.satellites = gsvInView;
        info.hasSat = true;
    }
}

static LocationInfo getLocationFromSerialNmea()
{
    LocationInfo info;
    if (!g_serialGpsEnabled)
        return info;

    g_nmeaError.clear();

    // Cached port: read it directly, no probing.
    if (!g_nmeaPort.isEmpty() && QFile::exists(g_nmeaPort)) {
        QByteArray raw = readNmeaRaw(g_nmeaPort, g_nmeaBaud, 1200);
        if (looksLikeNmea(raw)) {
            info.source = QString("GPS %1 @ %2")
                              .arg(g_nmeaPort, nmeaBaudName(g_nmeaBaud));
            parseNmea(raw, info);
            // A receiver with no sky view still streams valid sentences.
            if (!info.hasFix && !info.hasSat) {
                info.hasSat = true;
                info.satellites = 0;
            }
            return info;
        }
        // Port went away or stopped talking: drop the cache and re-probe.
        g_nmeaPort.clear();
    }

    // Rate-limit full probes. Without this the 2s refresh would spend
    // several seconds walking every tty on every tick.
    if (g_nmeaProbed && g_nmeaProbeAge.isValid() &&
        g_nmeaProbeAge.elapsed() < 30000) {
        return info;
    }

    const QStringList ports = nmeaCandidatePorts();
    for (const QString &port : ports) {
        for (int b = 0; b < NMEA_BAUD_COUNT; ++b) {
            QByteArray raw = readNmeaRaw(port, NMEA_BAUDS[b], 350);
            if (raw.isEmpty())
                break;                       // dead port, skip other bauds
            if (!looksLikeNmea(raw))
                continue;                    // bytes but garbage: try next baud

            g_nmeaPort = port;
            g_nmeaBaud = NMEA_BAUDS[b];

            QByteArray full = readNmeaRaw(port, NMEA_BAUDS[b], 1200);
            info.source = QString("GPS %1 @ %2")
                              .arg(port, nmeaBaudName(NMEA_BAUDS[b]));
            parseNmea(full, info);
            if (!info.hasFix && !info.hasSat) {
                info.hasSat = true;
                info.satellites = 0;
            }

            g_nmeaProbed = true;
            g_nmeaProbeAge.restart();
            return info;
        }
    }

    g_nmeaProbed = true;
    g_nmeaProbeAge.restart();
    return info;
}

// ---------------------------------------------------------
// Network geolocation ("location services")
//
// Off by default: this sends a request to an outside service and reveals
// the device's public IP address to it. Accuracy is town/city at best, so
// it only ever runs after every GPS backend has failed.
// ---------------------------------------------------------

static LocationInfo getLocationFromNetwork()
{
    LocationInfo info;
    if (!g_netLocEnabled)
        return info;

    QString script = R"SH(
FETCH=""
if command -v curl >/dev/null 2>&1; then FETCH="curl -s -m 8"
elif command -v wget >/dev/null 2>&1; then FETCH="wget -q -T 8 -O -"
else exit 1
fi
for u in "https://ipwho.is/" "http://ip-api.com/json/?fields=status,lat,lon,city,country"; do
  OUT=$($FETCH "$u" 2>/dev/null)
  case "$OUT" in
    *latitude*|*\"lat\"*) printf '%s' "$OUT"; exit 0 ;;
  esac
done
exit 1
)SH";

    QString out = runCommand("/bin/sh", {"-c", script});
    if (out.trimmed().isEmpty())
        return info;

    // ipwho.is uses latitude/longitude, ip-api.com uses lat/lon.
    QRegularExpression latRe("\"lat(?:itude)?\"\\s*:\\s*([-0-9\\.]+)");
    QRegularExpression lonRe("\"lon(?:gitude)?\"\\s*:\\s*([-0-9\\.]+)");
    QRegularExpression cityRe("\"city\"\\s*:\\s*\"([^\"]*)\"");
    QRegularExpression countryRe("\"country\"\\s*:\\s*\"([^\"]*)\"");

    QRegularExpressionMatch latM = latRe.match(out);
    QRegularExpressionMatch lonM = lonRe.match(out);
    if (!latM.hasMatch() || !lonM.hasMatch())
        return info;

    info.lat = latM.captured(1).toDouble();
    info.lon = lonM.captured(1).toDouble();
    info.hasFix = true;

    QString place;
    QRegularExpressionMatch cityM = cityRe.match(out);
    QRegularExpressionMatch countryM = countryRe.match(out);
    if (cityM.hasMatch())
        place = cityM.captured(1);
    if (countryM.hasMatch())
        place += (place.isEmpty() ? QString() : QString(", ")) + countryM.captured(1);

    info.place = place;
    info.source = place.isEmpty()
                      ? QString("Network - approximate only")
                      : QString("Network - approximate only (%1)").arg(place);

    return info;
}

// ---------------------------------------------------------
// Reverse geocoding: turn a GPS fix into a town and country name.
//
// This needs the network, so it sits behind the same Network toggle as IP
// geolocation. It is rate limited hard on purpose: the page refreshes every
// two seconds and OpenStreetMap's public service allows roughly one request
// per second, so a naive call here would get the whole project blocked.
// ---------------------------------------------------------

static QString g_placeName;
static double g_placeLat = 0.0;
static double g_placeLon = 0.0;
static QElapsedTimer g_placeAge;
static bool g_placeQueried = false;

static QString reverseGeocode(double lat, double lon)
{
    if (!g_netLocEnabled)
        return QString();

    // About a kilometre. Below this the town name will not have changed.
    bool moved = (qAbs(lat - g_placeLat) > 0.01) || (qAbs(lon - g_placeLon) > 0.01);

    if (!g_placeName.isEmpty() && !moved)
        return g_placeName;
    if (g_placeQueried && !moved &&
        g_placeAge.isValid() && g_placeAge.elapsed() < 60000)
        return g_placeName;

    g_placeQueried = true;
    g_placeAge.restart();
    g_placeLat = lat;
    g_placeLon = lon;

    QString url = QString("https://nominatim.openstreetmap.org/reverse"
                          "?format=jsonv2&zoom=10&addressdetails=1&lat=%1&lon=%2")
                      .arg(lat, 0, 'f', 5)
                      .arg(lon, 0, 'f', 5);

    // A descriptive User-Agent is required by the service's terms of use.
    QString ua = "Alternix-osm-settings/1.0 (+https://github.com/DansDesigns/Alternix)";

    QString script = QString(
        "if command -v curl >/dev/null 2>&1; then\n"
        "  curl -s -m 8 -A \"%2\" \"%1\" && exit 0\n"
        "fi\n"
        "if command -v wget >/dev/null 2>&1; then\n"
        "  wget -q -T 8 -O - --user-agent=\"%2\" \"%1\" && exit 0\n"
        "fi\n"
        "exit 1\n").arg(url, ua);

    QString out = runCommand("/bin/sh", {"-c", script});
    if (out.trimmed().isEmpty())
        return g_placeName;

    QRegularExpression townRe(
        "\"(?:city|town|village|hamlet|municipality|suburb)\"\\s*:\\s*\"([^\"]*)\"");
    QRegularExpression countyRe("\"county\"\\s*:\\s*\"([^\"]*)\"");
    QRegularExpression countryRe("\"country\"\\s*:\\s*\"([^\"]*)\"");

    QString town;
    QRegularExpressionMatch tm = townRe.match(out);
    if (tm.hasMatch())
        town = tm.captured(1);
    else {
        QRegularExpressionMatch cm = countyRe.match(out);
        if (cm.hasMatch())
            town = cm.captured(1);
    }

    QString country;
    QRegularExpressionMatch com = countryRe.match(out);
    if (com.hasMatch())
        country = com.captured(1);

    QString place = town;
    if (!country.isEmpty())
        place += (place.isEmpty() ? QString() : QString(", ")) + country;

    if (!place.isEmpty())
        g_placeName = place;

    return g_placeName;
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

    info = getLocationFromSerialNmea();
    if (info.hasFix || info.hasSat)
        return info;

    info = getLocationFromAT();
    if (info.hasFix || info.hasSat)
        return info;

    info = getLocationFromNetwork();
    if (info.hasFix || info.hasSat)
        return info;

    info.error = "Adaptor or libraires no found";
    if (!g_nmeaError.isEmpty())
        info.error += "\n\n" + g_nmeaError;
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
        QLabel *titleLabel = new QLabel("Location", this);
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

        midLayout->addWidget(gpsFrame);

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

        midLayout->addWidget(satFrame);

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

        midLayout->addWidget(mapFrame);

        // -------------------------------------------------
        // Card 4: Date & time
        // -------------------------------------------------
        QFrame *timeFrame = new QFrame(this);
        timeFrame->setStyleSheet(
            "QFrame {"
            " background:#3a3a3a;"
            " border-radius:40px;"
            "}"
        );
        timeFrame->setFixedHeight(520);

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

        // -------------------------------------------------
        // Card 5: Location sources
        // -------------------------------------------------
        QFrame *srcFrame = new QFrame(this);
        srcFrame->setStyleSheet(
            "QFrame {"
            " background:#3a3a3a;"
            " border-radius:40px;"
            "}"
        );
        srcFrame->setFixedHeight(460);

        QVBoxLayout *srcLayout = new QVBoxLayout(srcFrame);
        srcLayout->setContentsMargins(30, 30, 30, 30);
        srcLayout->setSpacing(14);

        QLabel *srcHeading = new QLabel("Location sources", srcFrame);
        srcHeading->setAlignment(Qt::AlignCenter);
        srcHeading->setStyleSheet(
            "QLabel { font-size:30px; font-weight:bold; color:white; }"
        );
        srcLayout->addWidget(srcHeading);

        QLabel *srcInfo = new QLabel(
            "GPS works with no internet and is used first. "
            "Network location estimates your position from your internet "
            "address. It is only accurate to about your town, and it asks "
            "an outside company where you are.",
            srcFrame);
        srcInfo->setAlignment(Qt::AlignCenter);
        srcInfo->setWordWrap(true);
        // WIDTH FIX - DO NOT REMOVE
        // A wrapping QLabel reports a minimum width wide enough for its
        // longest unbreakable run, which pushed the whole page past 720px
        // and produced horizontal scrolling. Letting it shrink to 1px means
        // the label wraps to whatever width the card actually has.
        srcInfo->setMinimumWidth(1);
        srcInfo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        srcInfo->setStyleSheet("QLabel { font-size:22px; color:#BBBBBB; }");
        srcLayout->addWidget(srcInfo);

        QHBoxLayout *srcRow = new QHBoxLayout();
        srcRow->setSpacing(10);
        srcRow->setAlignment(Qt::AlignHCenter);

        serialGpsButton = smallBtnBT("GPS chip");
        netLocButton    = smallBtnBT("Network");
        // WIDTH FIX - DO NOT REMOVE: 2*165 + 10 = 340. The state goes on a
        // second line so the button stays narrow without abbreviating.
        serialGpsButton->setFixedSize(165, 76);
        netLocButton->setFixedSize(165, 76);

        srcRow->addWidget(serialGpsButton);
        srcRow->addWidget(netLocButton);
        srcLayout->addLayout(srcRow);

        // Readout: where the device currently thinks it is.
        srcCoordLabel = new QLabel("Coordinates: not known yet", srcFrame);
        srcCoordLabel->setAlignment(Qt::AlignCenter);
        srcCoordLabel->setWordWrap(true);
        srcCoordLabel->setMinimumWidth(1);
        srcCoordLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        srcCoordLabel->setStyleSheet("QLabel { font-size:24px; color:white; }");
        srcLayout->addWidget(srcCoordLabel);

        srcPlaceLabel = new QLabel("Estimated place: not known yet", srcFrame);
        srcPlaceLabel->setAlignment(Qt::AlignCenter);
        srcPlaceLabel->setWordWrap(true);
        srcPlaceLabel->setMinimumWidth(1);
        srcPlaceLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        srcPlaceLabel->setStyleSheet("QLabel { font-size:24px; color:white; }");
        srcLayout->addWidget(srcPlaceLabel);

        midLayout->addWidget(srcFrame);

        midLayout->addStretch();

        midScroll->setWidget(midContainer);
        // Stretch 1 pins the title at the top and the buttons/back at the
        // bottom; the cards scroll in between if the screen is short.
        rootLayout->addWidget(midScroll, 1);

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

        connect(localZoneButton, &QPushButton::clicked,
                this, [this]() { setClockMode(0); });
        connect(utcZoneButton, &QPushButton::clicked,
                this, [this]() { setClockMode(1); });
        connect(gmtZoneButton, &QPushButton::clicked,
                this, [this]() { setClockMode(2); });

        connect(setTimeButton, &QPushButton::clicked,
                this, &LocationPage::openSetTimeDialog);
        connect(syncTimeButton, &QPushButton::clicked,
                this, &LocationPage::syncTimeFromInternet);

        connect(serialGpsButton, &QPushButton::clicked,
                this, &LocationPage::toggleSerialGps);
        connect(netLocButton, &QPushButton::clicked,
                this, &LocationPage::toggleNetworkLocation);

        // Clock ticks once a second, independently of the location refresh:
        // turning Location off must not stop the clock.
        clockTimer = new QTimer(this);
        clockTimer->setInterval(1000);
        connect(clockTimer, &QTimer::timeout,
                this, &LocationPage::updateClock);

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
        clockMode          = settings.value("DateTime/clock_mode", 0).toInt();
        if (clockMode < 0 || clockMode > 2)
            clockMode = 0;
        g_netLocEnabled    = settings.value("Location/network_enabled", false).toBool();
        g_serialGpsEnabled = settings.value("Location/serial_gps_enabled", true).toBool();

        updatePowerButton();
        updateZoneButtons();
        updateSourceButtons();
        updateClock();

        if (!stackedWidget || stackedWidget->currentWidget() == this)
            clockTimer->start();

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
            setSourceReadoutMessage("Location is turned off");
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

    // Date & time
    QLabel *clockLabel      = nullptr;
    QLabel *tzInfoLabel     = nullptr;
    QLabel *timeStatusLabel = nullptr;

    QPushButton *localZoneButton = nullptr;
    QPushButton *utcZoneButton   = nullptr;
    QPushButton *gmtZoneButton   = nullptr;
    QPushButton *setTimeButton   = nullptr;
    QPushButton *syncTimeButton  = nullptr;

    QTimer *clockTimer = nullptr;
    int clockMode = 0;              // 0 = Local, 1 = UTC, 2 = GMT

    // Location sources
    QPushButton *serialGpsButton = nullptr;
    QPushButton *netLocButton    = nullptr;
    QLabel *srcCoordLabel = nullptr;
    QLabel *srcPlaceLabel = nullptr;

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

    // Animate a button's spinner across a long blocking call, and keep the
    // 2s location refresh from interleaving with it.
    void runWithSpinner(QPushButton *btn, const QString &restoreText,
                        const std::function<void()> &work)
    {
        if (!btn || !btn->isEnabled())
            return;

        bool wasRefreshing = refreshTimer && refreshTimer->isActive();
        if (refreshTimer) refreshTimer->stop();

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
        if (wasRefreshing && refreshTimer) refreshTimer->start();
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

        // The dialog pumps events; do not let the location refresh run under it.
        bool wasRefreshing = refreshTimer && refreshTimer->isActive();
        if (refreshTimer) refreshTimer->stop();
        int result = dlg.exec();
        if (wasRefreshing && refreshTimer) refreshTimer->start();

        if (result != QDialog::Accepted)
            return;

        applySystemDateTime(edit->dateTime().toUTC());
    }

    void applySystemDateTime(const QDateTime &utc)
    {
        QString stamp = utc.toString("yyyy-MM-dd HH:mm:ss");

        QString script = QString::fromLatin1(RTC_WRITE_SH) + QString(
            "\n"
            "if ! O=$(date -u -s \"%1\" 2>&1); then\n"
            "  echo OSMSET_FAIL\n"
            "  echo \"date -u -s: $O\"\n"
            "  exit 1\n"
            "fi\n"
            "RTC=ok\n"
            "rtc_write || RTC=failed\n"
            "echo \"OSMSET_OK:$RTC\"\n"
            "echo \"$RTCLOG\"\n"
            "exit 0\n"
        ).arg(stamp);

        bool reached = false;
        QString out;
        runWithSpinner(setTimeButton, "Change", [&]() {
            out = runAsRootShell(script, "OSMSET_", 30000, &reached);
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

        updateClock();

        if (out.contains("OSMSET_OK:failed")) {
            setTimeStatus("Clock changed, but the battery-backed clock could not be "
                          "updated. The time may be wrong again after a shutdown.",
                          "#FFC24B");
            showDetail("Battery-backed clock",
                       "The time on screen is now correct, but it could not be "
                       "written to the clock chip that keeps time while the "
                       "machine is switched off. The reason is below.",
                       out, QMessageBox::Warning);
        } else {
            setTimeStatus("Clock changed by hand at " +
                          QDateTime::currentDateTime().toString("HH:mm:ss") + ".",
                          "#7CFC00");
        }
    }

    void syncTimeFromInternet()
    {
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

            QString script;
            if (haveSntp) {
                QString epoch = QString("%1.%2")
                                    .arg(unixMs / 1000)
                                    .arg(unixMs % 1000, 3, 10, QChar('0'));
                script = QString::fromLatin1(RTC_WRITE_SH) + QString(
                    "\n"
                    "if ! O=$(date -u -s \"@%1\" 2>&1); then\n"
                    "  echo OSMSYNC_FAIL\n"
                    "  echo \"date -u -s: $O\"\n"
                    "  exit 1\n"
                    "fi\n"
                    "RTC=ok\n"
                    "rtc_write || RTC=failed\n"
                    "echo \"OSMSYNC_OK:$RTC:%2\"\n"
                    "echo \"$RTCLOG\"\n"
                    "exit 0\n"
                ).arg(epoch, server);
            } else {
                // Step 2: fall back to the Date header of a web server. Less
                // precise (whole seconds) but survives networks that block
                // outgoing UDP port 123.
                script = QString::fromLatin1(RTC_WRITE_SH) + R"SH(
LOG=""
SRC=""

HAVE_CURL=0; command -v curl >/dev/null 2>&1 && HAVE_CURL=1
HAVE_WGET=0; command -v wget >/dev/null 2>&1 && HAVE_WGET=1

if [ "$HAVE_CURL" = 0 ] && [ "$HAVE_WGET" = 0 ]; then
  LOG="$LOG
neither curl nor wget is installed, so there is no way to fetch the time"
fi

for url in http://deb.devuan.org/ https://deb.devuan.org/ http://www.google.com/; do
  D=""
  if [ "$HAVE_CURL" = 1 ]; then
    D=$(curl -sI -m 8 "$url" 2>/dev/null | grep -i '^date:' | head -n1 | sed 's/^[Dd][Aa][Tt][Ee]:[ ]*//' | tr -d '\r')
  fi
  if [ -z "$D" ] && [ "$HAVE_WGET" = 1 ]; then
    D=$(wget -S --spider -T 8 -q -O /dev/null "$url" 2>&1 | grep -i '^ *date:' | head -n1 | sed 's/^ *[Dd][Aa][Tt][Ee]:[ ]*//' | tr -d '\r')
  fi
  if [ -n "$D" ]; then
    if date -u -s "$D" >/dev/null 2>&1; then SRC="the web server at $url"; break; fi
    LOG="$LOG
could not apply the date from $url: $D"
  else
    LOG="$LOG
no date sent by $url"
  fi
done

if [ -z "$SRC" ]; then
  echo "OSMSYNC_FAIL"
  echo "$LOG"
  exit 1
fi

RTC=ok
rtc_write || RTC=failed
echo "OSMSYNC_OK:$RTC:$SRC"
echo "$LOG"
echo "$RTCLOG"
exit 0
)SH";
            }

            out = runAsRootShell(script, "OSMSYNC_", 90000, &reached);
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

        updateClock();

        QString source;
        QRegularExpression okRe("OSMSYNC_OK:([^:]*):(.*)");
        QRegularExpressionMatch okM = okRe.match(out);
        bool rtcFailed = false;
        if (okM.hasMatch()) {
            rtcFailed = (okM.captured(1).trimmed() == "failed");
            source = okM.captured(2).trimmed();
        }

        QString when = QDateTime::currentDateTime().toString("HH:mm:ss");
        if (rtcFailed) {
            setTimeStatus(QString("Clock synced at %1, but the battery-backed clock "
                                  "could not be updated. The time may be wrong again "
                                  "after a shutdown.").arg(when),
                          "#FFC24B");
            showDetail("Battery-backed clock",
                       "The time was fetched successfully and the clock on screen "
                       "is now correct. It could not be written to the clock chip "
                       "that keeps time while the machine is switched off, so it "
                       "may be wrong again after a reboot. The reason is below.",
                       detail, QMessageBox::Warning);
        } else if (!source.isEmpty()) {
            setTimeStatus(QString("Clock synced at %1 from %2.").arg(when, source),
                          "#7CFC00");
        } else {
            setTimeStatus(QString("Clock synced at %1.").arg(when), "#7CFC00");
        }
    }

    // -------------------------------------------------
    // Location sources
    // -------------------------------------------------

    void updateSourceReadout(const LocationInfo &info)
    {
        if (srcCoordLabel) {
            if (info.hasFix) {
                srcCoordLabel->setText(QString("Coordinates: %1, %2")
                                           .arg(info.lat, 0, 'f', 6)
                                           .arg(info.lon, 0, 'f', 6));
            } else {
                srcCoordLabel->setText("Coordinates: not known yet");
            }
        }

        if (!srcPlaceLabel)
            return;

        QString place = info.place;
        if (place.isEmpty() && info.hasFix)
            place = reverseGeocode(info.lat, info.lon);

        if (!place.isEmpty()) {
            srcPlaceLabel->setText("Estimated place: " + place);
        } else if (!info.hasFix) {
            srcPlaceLabel->setText("Estimated place: not known yet");
        } else if (!g_netLocEnabled) {
            srcPlaceLabel->setText("Estimated place: turn Network on to look up "
                                   "a town name");
        } else {
            srcPlaceLabel->setText("Estimated place: could not be looked up");
        }
    }

    void setSourceReadoutMessage(const QString &msg)
    {
        if (srcCoordLabel) srcCoordLabel->setText("Coordinates: " + msg);
        if (srcPlaceLabel) srcPlaceLabel->setText("Estimated place: " + msg);
    }

    void updateSourceButtons()
    {
        if (serialGpsButton) {
            serialGpsButton->setText(g_serialGpsEnabled ? "GPS chip\nOn"
                                                        : "GPS chip\nOff");
            applyBtnColour(serialGpsButton,
                           g_serialGpsEnabled ? "#7CFC00" : "#CC6666", 22);
        }
        if (netLocButton) {
            netLocButton->setText(g_netLocEnabled ? "Network\nOn" : "Network\nOff");
            applyBtnColour(netLocButton,
                           g_netLocEnabled ? "#7CFC00" : "#CC6666", 22);
        }
    }

    void toggleSerialGps()
    {
        g_serialGpsEnabled = !g_serialGpsEnabled;
        saveSetting("Location/serial_gps_enabled", g_serialGpsEnabled);

        // Force a fresh port hunt on the next refresh.
        g_nmeaPort.clear();
        g_nmeaProbed = false;

        updateSourceButtons();
        if (locationEnabled)
            refreshDataOnce();
    }

    void toggleNetworkLocation()
    {
        g_netLocEnabled = !g_netLocEnabled;
        saveSetting("Location/network_enabled", g_netLocEnabled);
        updateSourceButtons();
        if (locationEnabled)
            refreshDataOnce();
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
            setSourceReadoutMessage("Location is turned off");
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

        bool visible = (stackedWidget->widget(idx) == this);

        if (visible && locationEnabled) {
            refreshTimer->start();
        } else {
            refreshTimer->stop();
        }

        // The clock is not part of Location, so it follows visibility only.
        if (clockTimer) {
            if (visible) {
                updateClock();
                clockTimer->start();
            } else {
                clockTimer->stop();
            }
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
            setSourceReadoutMessage("not known");
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

        updateSourceReadout(info);
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
