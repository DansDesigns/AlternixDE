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
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QFile>
#include <QVector>
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
#include <grp.h>

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

// ---------------------------------------------------------
// GPS hardware detection
//
// Same shape as the rotation sensor check: work out whether the hardware is
// actually present, and if it is not usable say plainly why (REASON) and
// what to do about it (HINT). Silent failure is the thing to avoid - without
// this the page just quietly falls back to network location and the user has
// no idea whether the machine even has a GPS chip.
// ---------------------------------------------------------

static LocationInfo getLocationFromSerialNmea();

struct GpsProbe {
    bool talking          = false;   // a receiver is actually sending NMEA
    bool chipEvidence     = false;   // something GNSS-shaped exists
    bool permissionDenied = false;   // ports exist but we cannot open them
    QString device;
    QString reason;
    QString hint;
    QStringList detail;              // full report for the dialog
};

static QString readSysFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    QString v = QString::fromLatin1(f.readAll()).trimmed();
    f.close();
    return v;
}

static bool userInGroup(const char *name)
{
    struct group *g = ::getgrnam(name);
    if (!g)
        return false;
    gid_t target = g->gr_gid;
    if (::getegid() == target)
        return true;

    int n = ::getgroups(0, nullptr);
    if (n <= 0)
        return false;
    QVector<gid_t> list(n);
    if (::getgroups(n, list.data()) != n)
        return false;
    for (int i = 0; i < list.size(); ++i)
        if (list.at(i) == target)
            return true;
    return false;
}

// USB vendors worth calling out. The serial-bridge chips are also used by
// plenty of non-GPS hardware, so they are reported as "possible" only.
struct UsbId { const char *vid; const char *name; bool definite; };
static const UsbId USB_GNSS_IDS[] = {
    { "1546", "u-blox GNSS receiver",              true  },
    { "1163", "SiRF GNSS receiver",                true  },
    { "091e", "Garmin GNSS receiver",              true  },
    { "067b", "Prolific USB serial (often a GPS cable)", false },
    { "10c4", "Silicon Labs USB serial (often a GPS)",   false },
    { "0403", "FTDI USB serial (sometimes a GPS)",       false },
    { "1199", "Sierra Wireless modem (may provide GPS)", false },
    { "12d1", "Huawei modem (may provide GPS)",          false }
};
static const int USB_GNSS_COUNT = 8;

static GpsProbe probeGpsHardware(bool deepScan)
{
    GpsProbe p;

    // --- 1. Kernel GNSS subsystem (the clearest possible evidence) --------
    QDir gnssDir("/sys/class/gnss");
    if (gnssDir.exists()) {
        const QStringList nodes = gnssDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &n : nodes) {
            p.chipEvidence = true;
            p.detail << QString("Kernel GNSS device: /dev/%1").arg(n);
        }
    }
    if (!p.chipEvidence)
        p.detail << "Kernel GNSS subsystem: nothing registered";

    // --- 2. ACPI / platform GNSS identifiers ------------------------------
    // Broadcom GNSS on Bay Trail and Cherry Trail tablets shows up here.
    const QStringList acpiRoots = { "/sys/bus/acpi/devices", "/sys/bus/platform/devices" };
    const QStringList acpiIds   = { "BCM4752", "BCM4750", "BCM4751", "GNSS", "UBLX" };
    bool acpiHit = false;
    for (const QString &root : acpiRoots) {
        QDir d(root);
        if (!d.exists())
            continue;
        const QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &e : entries) {
            for (const QString &id : acpiIds) {
                if (e.contains(id, Qt::CaseInsensitive)) {
                    p.chipEvidence = true;
                    acpiHit = true;
                    p.detail << QString("Firmware reports a GNSS device: %1/%2").arg(root, e);
                }
            }
        }
    }
    if (!acpiHit)
        p.detail << "Firmware device list: no GNSS chip declared";

    // --- 3. USB receivers and serial bridges ------------------------------
    QDir usbDir("/sys/bus/usb/devices");
    bool usbHit = false;
    if (usbDir.exists()) {
        const QStringList devs = usbDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &d : devs) {
            QString vid = readSysFile(usbDir.filePath(d) + "/idVendor");
            if (vid.isEmpty())
                continue;
            for (int i = 0; i < USB_GNSS_COUNT; ++i) {
                if (vid.compare(USB_GNSS_IDS[i].vid, Qt::CaseInsensitive) != 0)
                    continue;
                usbHit = true;
                if (USB_GNSS_IDS[i].definite)
                    p.chipEvidence = true;
                QString pid = readSysFile(usbDir.filePath(d) + "/idProduct");
                p.detail << QString("USB %1:%2 - %3")
                                .arg(vid, pid, QString::fromLatin1(USB_GNSS_IDS[i].name));
            }
        }
    }
    if (!usbHit)
        p.detail << "USB devices: no GNSS receiver or GPS serial cable attached";

    // --- 4. Serial ports and whether we are allowed to open them ----------
    const QStringList ports = nmeaCandidatePorts();
    if (ports.isEmpty()) {
        p.detail << "Serial ports: none present at all";
    } else {
        p.detail << QString("Serial ports found: %1").arg(ports.join(", "));
        bool anyOpenable = false;
        QStringList blocked;
        for (const QString &port : ports) {
            int fd = ::open(port.toLocal8Bit().constData(),
                            O_RDONLY | O_NOCTTY | O_NONBLOCK);
            if (fd >= 0) {
                anyOpenable = true;
                ::close(fd);
            } else if (errno == EACCES || errno == EPERM) {
                blocked << port;
            }
        }
        if (!blocked.isEmpty()) {
            p.permissionDenied = true;
            p.detail << QString("Cannot be opened (permission denied): %1")
                            .arg(blocked.join(", "));
        }
        if (!anyOpenable && !blocked.isEmpty())
            p.detail << "No serial port could be opened at all";
    }
    p.detail << QString("This user is %1a member of the 'dialout' group")
                    .arg(userInGroup("dialout") ? "" : "NOT ");

    // --- 5. gpsd -----------------------------------------------------------
    bool gpsdInstalled = QFile::exists("/usr/sbin/gpsd") || QFile::exists("/usr/bin/gpsd");
    bool gpsdSocket    = QFile::exists("/var/run/gpsd.sock") ||
                         QFile::exists("/run/gpsd.sock");
    p.detail << QString("gpsd: %1, socket %2")
                    .arg(gpsdInstalled ? "installed" : "not installed",
                         gpsdSocket ? "present" : "absent");

    // --- 6. Live listen on the serial ports --------------------------------
    bool savedToggle = g_serialGpsEnabled;
    g_serialGpsEnabled = true;      // the check listens regardless of the toggle
    g_nmeaPort.clear();             // force a real hunt, not the cached port
    g_nmeaProbed = false;
    LocationInfo live = getLocationFromSerialNmea();
    g_serialGpsEnabled = savedToggle;
    if (!live.source.isEmpty()) {
        p.talking = true;
        p.chipEvidence = true;
        p.device = live.source;
        p.detail << QString("Listening: %1 is sending valid NMEA data").arg(live.source);
        if (live.hasFix)
            p.detail << QString("Position: %1, %2")
                            .arg(live.lat, 0, 'f', 6).arg(live.lon, 0, 'f', 6);
        else
            p.detail << "Position: receiver is running but has no fix yet";
    } else {
        p.detail << "Listening: no serial port sent any recognisable GPS data";
    }

    // --- 7. Slow checks, only for the explicit Check button -----------------
    if (deepScan) {
        QString modems = runCommand("mmcli", {"-L"});
        if (modems.contains("/Modem/")) {
            QRegularExpression re("/Modem/(\\d+)");
            QRegularExpressionMatch m = re.match(modems);
            if (m.hasMatch()) {
                QString st = runCommand("mmcli", {"-m", m.captured(1), "--location-status"});
                bool gpsCapable = st.contains("gps", Qt::CaseInsensitive);
                if (gpsCapable)
                    p.chipEvidence = true;
                p.detail << QString("Mobile broadband modem %1: %2")
                                .arg(m.captured(1),
                                     gpsCapable ? "reports GPS capability"
                                                : "present but reports no GPS");
            }
        } else {
            p.detail << "Mobile broadband: no modem present (ModemManager found nothing)";
        }

        QString dm = runCommand("/bin/sh",
            {"-c", "dmesg 2>/dev/null | grep -iE 'gnss|u-blox|ublox|bcm475|nmea' | head -n 8"});
        if (!dm.trimmed().isEmpty())
            p.detail << "Kernel messages:\n" + dm.trimmed();
        else
            p.detail << "Kernel messages: nothing about GNSS in the log";
    }

    // --- Conclusion: REASON and HINT ---------------------------------------
    if (p.talking) {
        if (live.hasFix) {
            p.reason = QString("Working: %1").arg(p.device);
            p.hint.clear();
        } else {
            p.reason = QString("GPS chip found on %1, waiting for a fix").arg(p.device);
            p.hint = "A receiver needs a clear view of the sky. Indoors it can "
                     "take several minutes, or never get a fix at all.";
        }
    } else if (p.permissionDenied) {
        p.reason = "A serial port was found but cannot be opened";
        p.hint = "This user is not in the 'dialout' group, which is what grants "
                 "access to serial ports. Add the user to that group and sign "
                 "out and back in.";
    } else if (p.chipEvidence) {
        p.reason = "GPS hardware is present but is not sending anything";
        p.hint = "The chip may need its driver loaded or a power-on command. "
                 "The full report below shows what was found.";
    } else {
        p.reason = "No GPS chip found in this machine";
        p.hint = "Most x86 tablets have no GNSS receiver at all. Either plug in "
                 "a USB GPS receiver, or use Network location, which estimates "
                 "your position from your internet address.";
    }

    return p;
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
        // page's text rendering grey. The palette is applied to this page
        // only; other Settings modules keep their own palette.
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
        // Card 5: Location sources
        // -------------------------------------------------
        QFrame *srcFrame = new QFrame(this);
        srcFrame->setStyleSheet(
            "QFrame {"
            " background:#3a3a3a;"
            " border-radius:40px;"
            "}"
        );
        srcFrame->setFixedHeight(600);

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

        // What the GPS check found, always on screen.
        gpsStatusLabel = new QLabel("GPS chip: not checked yet", srcFrame);
        gpsStatusLabel->setAlignment(Qt::AlignCenter);
        gpsStatusLabel->setWordWrap(true);
        gpsStatusLabel->setMinimumWidth(1);
        gpsStatusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        gpsStatusLabel->setStyleSheet("QLabel { font-size:22px; color:#BBBBBB; }");
        srcLayout->addWidget(gpsStatusLabel);

        QHBoxLayout *checkRow = new QHBoxLayout();
        checkRow->setAlignment(Qt::AlignHCenter);
        checkGpsButton = smallBtnBT("Check GPS");
        // WIDTH FIX - DO NOT REMOVE: stays under the 340px card inner width.
        checkGpsButton->setFixedSize(220, 60);
        applyBtnColour(checkGpsButton, "white", 22);
        checkRow->addWidget(checkGpsButton);
        srcLayout->addLayout(checkRow);

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

        connect(serialGpsButton, &QPushButton::clicked,
                this, &LocationPage::toggleSerialGps);
        connect(netLocButton, &QPushButton::clicked,
                this, &LocationPage::toggleNetworkLocation);
        connect(checkGpsButton, &QPushButton::clicked,
                this, &LocationPage::runGpsCheck);

        // Timer: 2 second refresh while on and page visible
        refreshTimer = new QTimer(this);
        refreshTimer->setInterval(2000);
        connect(refreshTimer, &QTimer::timeout,
                this, &LocationPage::refreshDataOnce);

        // osm-notify modifies the same persistent Location switch; watch it
        // even while Location is turned off (refreshTimer is stopped then).
        stateMonitorTimer = new QTimer(this);
        stateMonitorTimer->setInterval(900);
        connect(stateMonitorTimer, &QTimer::timeout,
                this, &LocationPage::syncExternalLocationSettings);
        stateMonitorTimer->start();

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
        g_netLocEnabled    = settings.value("Location/network_enabled", false).toBool();
        g_serialGpsEnabled = settings.value("Location/serial_gps_enabled", true).toBool();

        updatePowerButton();
        updateSourceButtons();

        if (locationEnabled) {
            if (!stackedWidget || stackedWidget->currentWidget() == this)
                refreshTimer->start();
            // Deferred: the backend chain (mmcli → gpspipe waiting on 10
            // messages → AT probe) previously ran before the page could
            // paint, freezing the hub for seconds. Show the page first.
            QTimer::singleShot(50, this, [this]() { refreshDataOnce(); });
            // Deferred as well: the sysfs walk is quick but the page should
            // paint first.
            QTimer::singleShot(120, this, [this]() { quickGpsCheck(); });
        } else {
            gpsLabel->setText("Location is turned off");
            satLabel->setText("Visible satellites\n\nLocation is turned off");
            setSourceReadoutMessage("Location is turned off");
        }
    }

private:
    QStackedWidget *stackedWidget = nullptr;

    QLabel *gpsLabel = nullptr;
    QLabel *satLabel = nullptr;

    QPushButton *powerButton = nullptr;
    QPushButton *refreshButton = nullptr;

    QTimer *refreshTimer = nullptr;
    QTimer *stateMonitorTimer = nullptr;
    bool locationEnabled = true;

    // Location sources
    QPushButton *serialGpsButton = nullptr;
    QPushButton *netLocButton    = nullptr;
    QLabel *srcCoordLabel = nullptr;
    QLabel *srcPlaceLabel = nullptr;
    QLabel *gpsStatusLabel = nullptr;
    QPushButton *checkGpsButton = nullptr;

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

    // Animate the GPS check without interleaving automatic refreshes.
    void runWithSpinner(QPushButton *btn, const QString &restoreText,
                        const std::function<void()> &work)
    {
        if (!btn || !btn->isEnabled())
            return;

        bool wasRefreshing = refreshTimer && refreshTimer->isActive();
        if (refreshTimer) refreshTimer->stop();

        if (checkGpsButton) checkGpsButton->setEnabled(false);

        int frame = 0;
        btn->setText(QString::fromUtf8(SPIN_FRAMES_LOC[0]));
        g_locTick = [btn, &frame]() {
            frame = (frame + 1) % SPIN_COUNT_LOC;
            btn->setText(QString::fromUtf8(SPIN_FRAMES_LOC[frame]));
        };

        work();

        g_locTick = nullptr;
        btn->setText(restoreText);
        if (checkGpsButton) checkGpsButton->setEnabled(true);
        if (wasRefreshing && refreshTimer) refreshTimer->start();
    }

    // -------------------------------------------------
    // Location sources
    // -------------------------------------------------

    void applyGpsProbe(const GpsProbe &p)
    {
        if (!gpsStatusLabel)
            return;

        QString colour = p.talking ? "#7CFC00" : "#FFC24B";
        if (!p.talking && !p.chipEvidence && !p.permissionDenied)
            colour = "#BBBBBB";          // nothing wrong, there is just no chip

        QString text = "GPS chip: " + p.reason;
        if (!p.hint.isEmpty())
            text += "\n" + p.hint;

        gpsStatusLabel->setText(text);
        gpsStatusLabel->setStyleSheet(
            QString("QLabel { font-size:22px; color:%1; }").arg(colour));
    }

    // Quick check on the way in: sysfs only, no slow mmcli or dmesg.
    void quickGpsCheck()
    {
        applyGpsProbe(probeGpsHardware(false));
    }

    // The Check GPS button: everything, with the full report shown.
    void runGpsCheck()
    {
        GpsProbe p;
        runWithSpinner(checkGpsButton, "Check GPS", [&]() {
            p = probeGpsHardware(true);
        });

        applyGpsProbe(p);

        QString body = p.reason;
        if (!p.hint.isEmpty())
            body += "\n\n" + p.hint;

        showDetail("GPS chip check", body, p.detail.join("\n"),
                   p.talking ? QMessageBox::Information : QMessageBox::Warning);
    }

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
            const bool active = locationEnabled && g_serialGpsEnabled;
            serialGpsButton->setText(!locationEnabled ? "GPS chip\nPaused" :
                                     (g_serialGpsEnabled ? "GPS chip\nOn" : "GPS chip\nOff"));
            applyBtnColour(serialGpsButton,
                           active ? "#7CFC00" : "#CC6666", 22);
            serialGpsButton->setToolTip(!locationEnabled ?
                "Location is switched off; the GPS source is paused." : QString());
        }
        if (netLocButton) {
            netLocButton->setText(g_netLocEnabled ? "Network\nOn" : "Network\nOff");
            applyBtnColour(netLocButton,
                           g_netLocEnabled ? "#7CFC00" : "#CC6666", 22);
        }
    }

    void syncExternalLocationSettings()
    {
        if (stackedWidget && stackedWidget->currentWidget() != this) return;
        QSettings saved(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                        QSettings::IniFormat);
        const bool enabled = saved.value("Location/enabled", true).toBool();
        const bool gps = saved.value("Location/serial_gps_enabled", true).toBool();
        const bool net = saved.value("Location/network_enabled", false).toBool();
        const bool masterChanged = enabled != locationEnabled;
        const bool sourceChanged = gps != g_serialGpsEnabled || net != g_netLocEnabled;
        if (!masterChanged && !sourceChanged) return;
        locationEnabled = enabled;
        if (gps != g_serialGpsEnabled) {
            g_nmeaPort.clear();
            g_nmeaProbed = false;
        }
        g_serialGpsEnabled = gps;
        g_netLocEnabled = net;
        updatePowerButton();
        updateSourceButtons();
        if (!locationEnabled) {
            refreshTimer->stop();
            gpsLabel->setText("Location is turned off");
            satLabel->setText("Visible satellites\n\nLocation is turned off");
            setSourceReadoutMessage("Location is turned off");
        } else {
            if (!refreshTimer->isActive()) refreshTimer->start();
            // Only use the expensive GPS backend on a meaningful state change.
            QTimer::singleShot(0, this, [this]() { refreshDataOnce(); });
        }
    }

    void toggleSerialGps()
    {
        QSettings current(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                          QSettings::IniFormat);
        g_serialGpsEnabled = !current.value("Location/serial_gps_enabled", true).toBool();
        saveSetting("Location/serial_gps_enabled", g_serialGpsEnabled);

        // Force a fresh port hunt on the next refresh.
        g_nmeaPort.clear();
        g_nmeaProbed = false;

        updateSourceButtons();
        if (g_serialGpsEnabled)
            quickGpsCheck();
        else if (gpsStatusLabel)
            gpsStatusLabel->setText("GPS chip: switched off");
        if (locationEnabled)
            refreshDataOnce();
    }

    void toggleNetworkLocation()
    {
        QSettings current(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                          QSettings::IniFormat);
        g_netLocEnabled = !current.value("Location/network_enabled", false).toBool();
        saveSetting("Location/network_enabled", g_netLocEnabled);
        updateSourceButtons();
        if (locationEnabled)
            refreshDataOnce();
    }

    void togglePower()
    {
        // Read the latest saved state: osm-notify may have changed it between
        // the last 900ms poll and this click.
        QSettings settings(QDir::homePath() + "/.config/Alternix/osm-settings.conf",
                           QSettings::IniFormat);
        locationEnabled = !settings.value("Location/enabled", true).toBool();
        settings.setValue("Location/enabled", locationEnabled);
        settings.sync();

        updatePowerButton();

        if (!locationEnabled) {
            refreshTimer->stop();
            gpsLabel->setText("Location is turned off");
            satLabel->setText("Visible satellites\n\nLocation is turned off");
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
