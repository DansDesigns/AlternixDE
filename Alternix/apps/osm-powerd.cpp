#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <cstdlib>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>

static const int MAX_EVENTS = 32;

struct MonitoredDevice {
    int fd;
    std::string path;
    std::string name;
    bool grabbed;
};

// Read the input device name
std::string getDeviceName(int fd) {
    char name[256] = {0};
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
        return "";
    return std::string(name);
}

// Check if the device supports KEY_POWER
bool deviceHasPowerKey(int fd) {
    unsigned long bitmask[KEY_MAX / (8 * sizeof(long)) + 1];
    memset(bitmask, 0, sizeof(bitmask));

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bitmask)), bitmask) < 0)
        return false;

    int idx   = KEY_POWER / (8 * sizeof(long));
    int shift = KEY_POWER % (8 * sizeof(long));

    return (bitmask[idx] & (1UL << shift)) != 0;
}

// Trim trailing whitespace/nulls and lowercase, for robust name matching.
// Some drivers (older baytrail_button, OEM firmware variants) pad or vary
// the case of the device name string.
static std::string normalizeName(const std::string &raw) {
    std::string s = raw;
    while (!s.empty() && (s.back() == '\0' || isspace((unsigned char)s.back())))
        s.pop_back();
    for (char &c : s) c = tolower((unsigned char)c);
    return s;
}

// Devices known to falsely advertise KEY_POWER support without being a real
// physical power button — e.g. Intel's "Virtual Buttons" HID sensor device,
// which some platforms (including F10 handling on certain laptops) expose.
// DO NOT add "power button" variants here — this is a blocklist, not an
// allowlist, specifically so GPIO-based power buttons (soc_button_array /
// baytrail_button, used on Bay Trail / Cherry Trail tablets like the HP
// ElitePad, which don't use the classic ACPI PWRB device) are still grabbed
// even though their exact name string can vary by platform/firmware.
static bool isBlockedPowerDevice(const std::string &normalizedName) {
    static const std::vector<std::string> blocklist = {
        "virtual buttons",
        "video bus",
        "sleep button",
        "lid switch",
    };
    for (const auto &b : blocklist) {
        if (normalizedName.find(b) != std::string::npos)
            return true;
    }
    return false;
}

// A dedicated power/volume/rotation-lock button device (soc_button_array,
// ACPI PWRB, etc.) never supports ordinary alphabet keys — it has one or a
// handful of specific buttons and nothing else. A real keyboard obviously
// does support them, even if it also happens to declare KEY_POWER in its
// capability bitmask. Check a handful of common letters/space rather than
// counting total bits, since button-device key counts vary but "does it
// have letters" reliably doesn't.
static bool isLikelyFullKeyboard(int fd) {
    unsigned long bitmask[KEY_MAX / (8 * sizeof(long)) + 1];
    memset(bitmask, 0, sizeof(bitmask));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bitmask)), bitmask) < 0)
        return false;

    auto hasKey = [&](int code) {
        int idx   = code / (8 * sizeof(long));
        int shift = code % (8 * sizeof(long));
        return (bitmask[idx] & (1UL << shift)) != 0;
    };

    int letterHits = 0;
    for (int k : {KEY_A, KEY_E, KEY_S, KEY_T, KEY_SPACE}) {
        if (hasKey(k)) letterHits++;
    }
    return letterHits >= 3;
}

// Try to find an "active" logged-in user via /run/user/<uid>
uid_t findActiveUserUid() {
    DIR *dir = opendir("/run/user");
    if (!dir)
        return 0;

    uid_t best = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.')
            continue;

        bool numeric = true;
        for (char *p = ent->d_name; *p; ++p) {
            if (!isdigit(static_cast<unsigned char>(*p))) {
                numeric = false;
                break;
            }
        }
        if (!numeric)
            continue;

        char *end = nullptr;
        long val = strtol(ent->d_name, &end, 10);
        if (!end || *end != '\0')
            continue;

        if (val >= 1000 && (uid_t)val > best) {
            best = (uid_t)val;
        }
    }

    closedir(dir);
    return best;  // 0 if none found
}

// Decide which user to run osm-power as; returns passwd* or nullptr
passwd* getTargetUserPw() {
    // 1) explicit override
    const char *envUser = std::getenv("OSM_USER");
    if (envUser && envUser[0] != '\0') {
        passwd *pw = getpwnam(envUser);
        if (pw) {
            std::cerr << "osm-powerd: using OSM_USER=" << envUser << "\n";
            return pw;
        }
        std::cerr << "osm-powerd: OSM_USER=" << envUser
                  << " not found in passwd, ignoring\n";
    }

    // 2) sudo user (when started with sudo)
    const char *sudoUser = std::getenv("SUDO_USER");
    if (sudoUser && sudoUser[0] != '\0') {
        passwd *pw = getpwnam(sudoUser);
        if (pw) {
            std::cerr << "osm-powerd: using SUDO_USER=" << sudoUser << "\n";
            return pw;
        }
    }

    // 3) try to infer from /run/user/<uid>
    uid_t active = findActiveUserUid();
    if (active != 0) {
        passwd *pw = getpwuid(active);
        if (pw) {
            std::cerr << "osm-powerd: using active /run/user uid="
                      << active << " (" << pw->pw_name << ")\n";
            return pw;
        }
    }

    // 4) fallback to current USER env
    const char *userEnv = std::getenv("USER");
    if (userEnv && userEnv[0] != '\0') {
        passwd *pw = getpwnam(userEnv);
        if (pw) {
            std::cerr << "osm-powerd: fallback USER=" << userEnv << "\n";
            return pw;
        }
    }

    // 5) last resort: root
    passwd *rootPw = getpwnam("root");
    if (!rootPw) {
        std::cerr << "osm-powerd: WARNING: cannot find any user, including root\n";
    } else {
        std::cerr << "osm-powerd: WARNING: falling back to root\n";
    }
    return rootPw;
}

// Run osm-power as the target user (drop privileges in the child)
void run_osm_power_as_user() {
    passwd *pw = getTargetUserPw();
    if (!pw) {
        std::cerr << "osm-powerd: no valid target user, not starting osm-power\n";
        _exit(1);
    }

    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;

    std::cerr << "osm-powerd: dropping to user "
              << pw->pw_name << " (uid=" << uid
              << ", gid=" << gid << ")\n";

    // Fix environment to look like that user
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);

    // XDG_RUNTIME_DIR usually /run/user/<uid>
    {
        std::string xdg = "/run/user/" + std::to_string(uid);
        setenv("XDG_RUNTIME_DIR", xdg.c_str(), 1);
    }

    // Drop privileges
    if (initgroups(pw->pw_name, gid) != 0) {
        perror("initgroups");
    }
    if (setgid(gid) != 0) {
        perror("setgid");
    }
    if (setuid(uid) != 0) {
        perror("setuid");
    }

    // At this point, getuid() should be the user; env has HOME/USER set.
    execlp("osm-power", "osm-power", (char*)nullptr);
    perror("execlp osm-power");
    _exit(1);
}

int main() {
    std::vector<MonitoredDevice> devices;

    // Scan event0..event39
    for (int i = 0; i < 40; i++) {
        std::string path = "/dev/input/event" + std::to_string(i);
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;

        std::string name = getDeviceName(fd);
        std::string normName = normalizeName(name);
        bool hasPowerKey = deviceHasPowerKey(fd);
        bool isPowerName = (normName.find("power button") != std::string::npos);

        // DIAGNOSTIC — DO NOT REMOVE. Prints every input device scanned,
        // not just the ones we decide to monitor, so a mismatch on new
        // hardware (e.g. a differently-named GPIO power button) is visible
        // in the console/log instead of failing completely silently.
        std::cerr << "osm-powerd: scanned " << path
                   << " name=\"" << name << "\""
                   << " hasPowerKey=" << (hasPowerKey ? "yes" : "no")
                   << "\n";

        bool monitor = false;
        bool grab    = false;

        // Any device with KEY_POWER or "Power Button" in name is interesting,
        // BUT we only *trigger* osm-power from a device we've grabbed.
        if (hasPowerKey || isPowerName) {
            monitor = true;
        }

        // GRAB CRITERION — DO NOT REVERT TO NAME-ONLY MATCH, AND DO NOT
        // DROP THE isLikelyFullKeyboard() GUARD BELOW.
        // Previously this only grabbed devices with the exact substring
        // "Power Button" in their name. That works for the classic ACPI
        // PWRB device, but tablets built to Microsoft's "Windows ACPI
        // Design Guide for SoC Platforms" (Bay Trail / Cherry Trail —
        // e.g. HP ElitePad) expose their power button via GPIO
        // (soc_button_array / baytrail_button), and the exact name string
        // can vary by firmware/driver version. Relying on the name alone
        // meant those devices were "monitored" (logged) but never
        // "grabbed" — and the trigger in main() only fires for grabbed
        // devices — so presses were silently ignored. Grabbing based on
        // real KEY_POWER capability (minus a small known-bad blocklist)
        // is the reliable signal across platforms.
        //
        // BUT: many real keyboards (e.g. the Miix 520's detachable
        // keyboard) also declare KEY_POWER in their capability bitmask
        // even though they never send it, as part of a broad firmware
        // key-set declaration. Grabbing one of those via EVIOCGRAB
        // exclusively locks the whole keyboard away from X11/qtile until
        // it's physically unplugged and replugged (which creates a fresh,
        // ungrabbed device node — matching exactly that symptom). So:
        // never grab anything that looks like an actual keyboard,
        // regardless of what else it claims to support.
        if (hasPowerKey && !isBlockedPowerDevice(normName) &&
            !isLikelyFullKeyboard(fd)) {
            grab = true;
        }

        if (!monitor) {
            close(fd);
            continue;
        }

        if (grab) {
            if (ioctl(fd, EVIOCGRAB, 1) < 0) {
                perror("EVIOCGRAB failed");
                grab = false;
            } else {
                std::cout << "Exclusively grabbing: " << path
                          << " (" << name << ")\n";
            }
        } else {
            std::cout << "Listening (no grab): " << path
                      << " (" << name << ")\n";
        }

        devices.push_back({fd, path, name, grab});
    }

    if (devices.empty()) {
        std::cerr << "No POWER BUTTON devices detected.\n";
        return 1;
    }

    // Setup epoll
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("epoll_create1");
        return 1;
    }

    for (const auto &dev : devices) {
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = dev.fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, dev.fd, &ev) < 0) {
            perror("epoll_ctl");
        }
    }

    struct epoll_event events[MAX_EVENTS];

    while (true) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0)
            continue;

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            struct input_event ev;

            while (read(fd, &ev, sizeof(ev)) > 0) {
                if (ev.type == EV_KEY &&
                    ev.code == KEY_POWER &&
                    ev.value != 0) {  // press or repeat

                    // Find which device this fd belongs to
                    const MonitoredDevice *src = nullptr;
                    for (const auto &d : devices) {
                        if (d.fd == fd) {
                            src = &d;
                            break;
                        }
                    }

                    if (src) {
                        std::cout << "POWER BUTTON PRESSED from "
                                  << src->path << " (" << src->name << ")"
                                  << (src->grabbed ? " [grabbed]" : " [no grab]")
                                  << std::endl;
                    } else {
                        std::cout << "POWER BUTTON PRESSED from unknown fd "
                                  << fd << std::endl;
                    }

                    // ⚠ Only trigger osm-power if this event came from the
                    // grabbed real "Power Button" device. This prevents Intel
                    // Virtual Buttons / F10 from acting as a power key.
                    if (src && src->grabbed) {
                        if (fork() == 0) {
                            run_osm_power_as_user();
                        }
                    }
                }
            }
        }
    }

    return 0;
}
