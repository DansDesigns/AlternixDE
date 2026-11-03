#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QFrame>
#include <QSlider>
#include <QScrollArea>
#include <QScroller>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QTimer>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QQueue>
#include <QSet>
#include <QHash>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QWaitCondition>
#include <QThread>
#include <QElapsedTimer>
#include <QVector>
#include <QByteArray>
#include <QtGlobal>
#include <thread>
#include <chrono>

// Audio controls use pactl, which works with both PulseAudio and pipewire-pulse.
// No root privileges, shell command interpolation, or additional link libraries
// are necessary. Main Volume controls the OUTPUT SINK; the other audio sliders
// control streams, not the physical output device.

static QString sliderStyle() {
    return
        "QSlider { background:transparent; }"
        "QSlider::groove:horizontal { background:#666666; height:14px;"
        " border-radius:7px; margin:0px; }"
        "QSlider::sub-page:horizontal { background:#4aa3ff; border-radius:7px; }"
        "QSlider::handle:horizontal { background:white; border-radius:16px;"
        " width:32px; height:32px; margin:-9px 0; }"
        "QSlider::handle:horizontal:pressed { background:#e0e0e0; }"
        "QSlider:disabled::handle:horizontal { background:#888888; }"
        "QSlider:disabled::sub-page:horizontal { background:#555555; }";
}

static QFrame *makeSliderCard(const QString &title, QSlider **outSlider) {
    QFrame *card = new QFrame();
    card->setStyleSheet("QFrame { background:#3a3a3a; border-radius:30px; }");
    card->setFixedHeight(170);

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(20, 20, 20, 20);
    lay->setSpacing(15);

    QLabel *lbl = new QLabel(title, card);
    lbl->setStyleSheet("font-size:30px; color:white; font-weight:bold;");
    lbl->setAlignment(Qt::AlignCenter);

    QSlider *slider = new QSlider(Qt::Horizontal, card);
    slider->setRange(0, 100);
    slider->setValue(50);
    slider->setStyleSheet(sliderStyle());
    slider->setFixedHeight(40);

    *outSlider = slider;
    lay->addWidget(lbl);
    lay->addWidget(slider);
    return card;
}

// The native PulseAudio stream-restore extension can write the volume policy
// for an audio role even when there are NO streams currently playing.
// pipewire-pulse implements the same protocol where stream-restore is enabled.
// Use QLibrary so the existing osm-settings build needs no new -lpulse flag
// and doesn't require libpulse development headers to compile.
// These structures have the C ABI of pa_channel_map, pa_cvolume and
// pa_ext_stream_restore_info (PA_CHANNELS_MAX = 32).
struct PulseRoleChannelMap {
    quint8 channels = 0;
    qint32 map[32] = {};
};
struct PulseRoleVolume {
    quint8 channels = 0;
    quint32 values[32] = {};
};
struct PulseRoleInfo {
    const char *name = nullptr;
    PulseRoleChannelMap channelMap;
    PulseRoleVolume volume;
    const char *device = nullptr;
    int mute = 0;
};

static_assert(sizeof(PulseRoleChannelMap) == 132, "PulseAudio channel map ABI mismatch");
static_assert(sizeof(PulseRoleVolume) == 132, "PulseAudio volume ABI mismatch");

struct ExistingRole {
    QByteArray device;
    PulseRoleChannelMap channelMap;
    PulseRoleVolume volume;
    int mute = 0;
};
struct ExistingRolesResult {
    QHash<QString, ExistingRole> entries;
    bool done = false;
};
static void pulseRoleReadEntry(void *, const PulseRoleInfo *info, int eol, void *data) {
    ExistingRolesResult *result = static_cast<ExistingRolesResult *>(data);
    if (eol != 0) {
        result->done = true;
    } else if (info && info->name) {
        ExistingRole entry;
        if (info->device) entry.device = QByteArray(info->device);
        entry.channelMap = info->channelMap;
        entry.volume = info->volume;
        entry.mute = info->mute;
        result->entries.insert(QString::fromUtf8(info->name), entry);
    }
}

static void pulseRoleWriteFinished(void *, int success, void *data) {
    // Callback is invoked on the worker's own mainloop thread.
    int *result = static_cast<int *>(data);
    *result = success ? 1 : -1;
}

static bool writePulseRoleVolumes(const QHash<QString, int> &changes) {
    if (changes.isEmpty()) return true;
    QLibrary lib(QStringLiteral("libpulse.so.0"));
    if (!lib.load()) return false;

    using MainloopNew = void *(*)();
    using MainloopFree = void (*)(void *);
    using MainloopGetApi = void *(*)(void *);
    using MainloopIterate = int (*)(void *, int, int *);
    using ContextNew = void *(*)(void *, const char *);
    using ContextConnect = int (*)(void *, const char *, int, const void *);
    using ContextGetState = int (*)(const void *);
    using ContextDisconnect = void (*)(void *);
    using ContextUnref = void (*)(void *);
    using RoleWrite = void *(*)(void *, int, const PulseRoleInfo *, unsigned,
                               int, void (*)(void *, int, void *), void *);
    using RoleRead = void *(*)(void *, void (*)(void *, const PulseRoleInfo *, int, void *), void *);
    using OperationUnref = void (*)(void *);

    const auto mainloopNew = reinterpret_cast<MainloopNew>(lib.resolve("pa_mainloop_new"));
    const auto mainloopFree = reinterpret_cast<MainloopFree>(lib.resolve("pa_mainloop_free"));
    const auto mainloopApi = reinterpret_cast<MainloopGetApi>(lib.resolve("pa_mainloop_get_api"));
    const auto iterate = reinterpret_cast<MainloopIterate>(lib.resolve("pa_mainloop_iterate"));
    const auto contextNew = reinterpret_cast<ContextNew>(lib.resolve("pa_context_new"));
    const auto contextConnect = reinterpret_cast<ContextConnect>(lib.resolve("pa_context_connect"));
    const auto contextState = reinterpret_cast<ContextGetState>(lib.resolve("pa_context_get_state"));
    const auto contextDisconnect = reinterpret_cast<ContextDisconnect>(lib.resolve("pa_context_disconnect"));
    const auto contextUnref = reinterpret_cast<ContextUnref>(lib.resolve("pa_context_unref"));
    const auto roleWrite = reinterpret_cast<RoleWrite>(lib.resolve("pa_ext_stream_restore_write"));
    const auto roleRead = reinterpret_cast<RoleRead>(lib.resolve("pa_ext_stream_restore_read"));
    const auto opUnref = reinterpret_cast<OperationUnref>(lib.resolve("pa_operation_unref"));

    if (!mainloopNew || !mainloopFree || !mainloopApi || !iterate || !contextNew ||
        !contextConnect || !contextState || !contextDisconnect || !contextUnref ||
        !roleWrite || !roleRead || !opUnref) return false;

    void *loop = mainloopNew();
    if (!loop) return false;
    void *context = contextNew(mainloopApi(loop), "AlternixOS Sound Settings");
    if (!context) {
        mainloopFree(loop);
        return false;
    }

    bool success = false;
    if (contextConnect(context, nullptr, 0, nullptr) >= 0) {
        QElapsedTimer timer;
        timer.start();
        // PA_CONTEXT_READY=4, FAILED=5, TERMINATED=6.
        while (timer.elapsed() < 1500 && contextState(context) < 4) {
            if (iterate(loop, 0, nullptr) < 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (contextState(context) == 4) {
            // Read existing role entries first so an edit doesn't erase a
            // previously stored output-device choice or stereo balance.
            ExistingRolesResult existing;
            void *readOp = roleRead(context, pulseRoleReadEntry, &existing);
            if (readOp) {
                timer.restart();
                while (!existing.done && timer.elapsed() < 1200) {
                    if (iterate(loop, 0, nullptr) < 0) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                opUnref(readOp);
            }

            QVector<QByteArray> nameBytes;
            QVector<QByteArray> deviceBytes;
            QVector<PulseRoleInfo> entries;
            nameBytes.reserve(changes.size());
            deviceBytes.reserve(changes.size());
            entries.reserve(changes.size());
            for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
                const int percent = qBound(0, it.value(), 100);
                const quint32 requested = static_cast<quint32>((percent * 65536 + 50) / 100);
                nameBytes.append(it.key().toUtf8());
                PulseRoleInfo info;
                if (existing.entries.contains(it.key())) {
                    const ExistingRole old = existing.entries.value(it.key());
                    info.channelMap = old.channelMap;
                    info.volume = old.volume;
                    deviceBytes.append(old.device);
                    // Preserve relative per-channel balance, where available.
                    quint32 greatest = 0;
                    for (int ch = 0; ch < info.volume.channels && ch < 32; ++ch)
                        greatest = qMax(greatest, info.volume.values[ch]);
                    for (int ch = 0; ch < info.volume.channels && ch < 32; ++ch)
                        info.volume.values[ch] = greatest
                            ? static_cast<quint32>((static_cast<quint64>(requested) *
                                info.volume.values[ch]) / greatest) : requested;
                } else {
                    deviceBytes.append(QByteArray());
                }
                if (info.channelMap.channels == 0 || info.volume.channels == 0 ||
                    info.channelMap.channels != info.volume.channels ||
                    info.volume.channels > 32) {
                    info.channelMap = PulseRoleChannelMap{};
                    info.channelMap.channels = 1;
                    info.channelMap.map[0] = 0; // PA_CHANNEL_POSITION_MONO
                    info.volume = PulseRoleVolume{};
                    info.volume.channels = 1;
                    info.volume.values[0] = requested;
                }
                info.mute = percent == 0 ? 1 : 0;
                entries.append(info);
            }
            for (int i = 0; i < entries.size(); ++i) {
                entries[i].name = nameBytes[i].constData();
                if (!deviceBytes[i].isEmpty()) entries[i].device = deviceBytes[i].constData();
            }

            int completed = 0;
            // PA_UPDATE_REPLACE=2: update only these entries, keeping the rest
            // of the user's per-application stream restore database intact.
            void *op = roleWrite(context, 2, entries.constData(),
                                 static_cast<unsigned>(entries.size()), 1,
                                 pulseRoleWriteFinished, &completed);
            if (op) {
                timer.restart();
                while (!completed && timer.elapsed() < 1500) {
                    if (iterate(loop, 0, nullptr) < 0) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                success = (completed == 1);
                opUnref(op);
            }
        }
    }
    contextDisconnect(context);
    contextUnref(context);
    mainloopFree(loop);
    return success;
}

// Single background writer coalesces changes and preserves request order.
// The UI never waits for a PulseAudio connection during slider movement.
class RoleRestoreWorker : public QThread {
public:
    explicit RoleRestoreWorker(QObject *parent = nullptr) : QThread(parent) { start(); }

    ~RoleRestoreWorker() override {
        {
            QMutexLocker lock(&mutex);
            stopping = true;
            changed.wakeAll();
        }
        wait();
    }

    void schedule(const QStringList &roleIds, int volume) {
        QMutexLocker lock(&mutex);
        for (const QString &id : roleIds) pending.insert(id, volume);
        changed.wakeOne();
    }

protected:
    void run() override {
        for (;;) {
            QHash<QString, int> tasks;
            {
                QMutexLocker lock(&mutex);
                while (!stopping && pending.isEmpty()) changed.wait(&mutex);
                if (stopping && pending.isEmpty()) return;
                tasks.swap(pending);
            }
            writePulseRoleVolumes(tasks);
        }
    }

private:
    QMutex mutex;
    QWaitCondition changed;
    QHash<QString, int> pending;
    bool stopping = false;
};

// All names below are PulseAudio media roles, not application names. An app
// must set media.role for its streams to take advantage of role restore.
enum class VolumeGroup { Notifications, Media, Calls, Alarms };

static QStringList restoreRoles(VolumeGroup group) {
    QStringList roles;
    switch (group) {
    case VolumeGroup::Notifications: roles = {"event", "notification"}; break;
    case VolumeGroup::Media: roles = {"music", "video", "movie", "game",
                                      "animation", "production"}; break;
    case VolumeGroup::Calls: roles = {"phone", "communication"}; break;
    case VolumeGroup::Alarms: roles = {"alarm"}; break;
    }
    QStringList ids;
    for (const QString &role : roles)
        ids << QStringLiteral("sink-input-by-media-role:") + role;
    return ids;
}

static VolumeGroup classifyRole(const QString &role) {
    const QString r = role.trimmed().toLower();
    if (r == "event" || r == "notification") return VolumeGroup::Notifications;
    if (r == "alarm") return VolumeGroup::Alarms;
    if (r == "phone" || r == "communication") return VolumeGroup::Calls;
    // Unknown/untagged streams belong to Media, not Notifications.
    return VolumeGroup::Media;
}

struct PlaybackStream {
    int index = -1;
    VolumeGroup group = VolumeGroup::Media;
    QString restoreId;
};

static QVector<PlaybackStream> parsePlaybackStreams(const QString &output) {
    QVector<PlaybackStream> result;
    static const QRegularExpression heading("^Sink Input #(\\d+)\\s*$");
    static const QRegularExpression roleProperty("^media\\.role\\s*=\\s*\"([^\"]*)\"");
    static const QRegularExpression restoreProperty(
        "^module-stream-restore\\.id\\s*=\\s*\"([^\"]*)\"");
    PlaybackStream current;
    bool found = false;
    for (const QString &rawLine : output.split('\n')) {
        const QString line = rawLine.trimmed();
        const auto head = heading.match(line);
        if (head.hasMatch()) {
            if (found) result.append(current);
            current = PlaybackStream{};
            current.index = head.captured(1).toInt();
            found = true;
            continue;
        }
        if (!found) continue;
        const auto role = roleProperty.match(line);
        if (role.hasMatch()) current.group = classifyRole(role.captured(1));
        const auto restore = restoreProperty.match(line);
        if (restore.hasMatch()) current.restoreId = restore.captured(1);
    }
    if (found) result.append(current);
    return result;
}

// ---------------------------------------------------------
// Sound settings page
// ---------------------------------------------------------
class SoundPage : public QWidget {
public:
    explicit SoundPage(QStackedWidget *stack, QWidget *parent = nullptr)
        : QWidget(parent), stackedWidget(stack), restoreWorker(new RoleRestoreWorker(this)) {
        setStyleSheet("background:#282828;");

        const QString confPath = QDir::homePath() + "/.config/Alternix/osm-settings.conf";
        QDir().mkpath(QFileInfo(confPath).absolutePath());
        settings = new QSettings(confPath, QSettings::IniFormat, this);

        QVBoxLayout *outer = new QVBoxLayout(this);
        outer->setContentsMargins(40, 40, 40, 40);
        outer->setSpacing(20);

        QLabel *titleLabel = new QLabel("Sound", this);
        titleLabel->setStyleSheet("font-size:42px; color:white; font-weight:bold;");
        titleLabel->setAlignment(Qt::AlignCenter);
        outer->addWidget(titleLabel);

        QScrollArea *scroll = new QScrollArea(this);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QScroller::grabGesture(scroll->viewport(), QScroller::LeftMouseButtonGesture);

        QWidget *scrollContainer = new QWidget(scroll);
        scrollContainer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        QVBoxLayout *scrollLayout = new QVBoxLayout(scrollContainer);
        scrollLayout->setContentsMargins(0, 0, 0, 0);
        scrollLayout->setSpacing(30);

        scrollLayout->addWidget(makeSliderCard("Main Volume", &mainSlider));
        scrollLayout->addWidget(makeSliderCard("Notifications", &notifSlider));
        scrollLayout->addWidget(makeSliderCard("Media", &mediaSlider));
        scrollLayout->addWidget(makeSliderCard("In-Call", &callSlider));
        scrollLayout->addWidget(makeSliderCard("Alarms", &alarmSlider));
        scrollLayout->addWidget(makeSliderCard("Vibration Strength", &vibSlider));
        scrollLayout->addStretch();
        scroll->setWidget(scrollContainer);
        outer->addWidget(scroll, 1);

        // A generic Linux audio API does not control a vibration motor. Do not
        // show a working-looking slider that only changes a number in a file.
        // It can be enabled later when a supported haptic driver is integrated.
        vibSlider->setValue(settings->value("Sound/VibrationStrength", 50).toInt());
        vibSlider->setEnabled(false);
        vibSlider->setToolTip("Vibration strength needs a supported haptic driver; "
                              "this device has no configured vibration backend.");

        notifSlider->setValue(settings->value("Sound/Notifications", 50).toInt());
        mediaSlider->setValue(settings->value("Sound/Media", 100).toInt());
        callSlider->setValue(settings->value("Sound/InCall", 100).toInt());
        alarmSlider->setValue(settings->value("Sound/Alarms", 100).toInt());
        mainSlider->setValue(settings->value("Sound/MainVolume", 50).toInt());

        statusLabel = new QLabel(this);
        statusLabel->setStyleSheet("color:#bbbbbb; font-size:14px;");
        statusLabel->setAlignment(Qt::AlignCenter);
        statusLabel->setWordWrap(true);
        statusLabel->setText("Category volumes apply to apps identifying their sound type; "
                             "other playback uses Media. Vibration control requires hardware support.");
        outer->addWidget(statusLabel);

        // Run pactl commands asynchronously, and debounce slider movements.
        controlProcess = new QProcess(this);
        connect(controlProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus status) {
            if (code != 0 || status != QProcess::NormalExit)
                reportAudioError();
            runNextCommand();
        });
        connect(controlProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            reportAudioError();
            runNextCommand();
        });

        sinkRefresh = new QProcess(this);
        connect(sinkRefresh, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus) {
            if (code != 0) return;
            static const QRegularExpression volumeRe("(\\d+)%");
            const QString output = QString::fromUtf8(sinkRefresh->readAllStandardOutput());
            const auto match = volumeRe.match(output);
            if (match.hasMatch() && !mainSlider->isSliderDown() &&
                !(mainTimer && mainTimer->isActive())) {
                QSignalBlocker blocker(mainSlider);
                mainSlider->setValue(qBound(0, match.captured(1).toInt(), 100));
            }
        });

        streamScan = new QProcess(this);
        connect(streamScan, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int code, QProcess::ExitStatus) {
            if (code != 0) return;
            const QString output = QString::fromUtf8(streamScan->readAllStandardOutput());
            const QVector<PlaybackStream> streams = parsePlaybackStreams(output);
            QSet<int> seen;
            for (const PlaybackStream &stream : streams) {
                if (stream.index < 0) continue;
                seen.insert(stream.index);
                if (forceStreamUpdate || !configuredStreams.contains(stream.index)) {
                    // Only apply category levels the user has intentionally
                    // changed. Old saved positions were previously placeholders.
                    if (isConfigured(stream.group))
                        queueCommand({"set-sink-input-volume", QString::number(stream.index),
                                      QString::number(groupValue(stream.group)) + "%"});
                }
            }
            configuredStreams = seen;
            forceStreamUpdate = false;
            if (scanAgain) {
                scanAgain = false;
                scanStreams();
            }
        });

        subscribeProcess = new QProcess(this);
        connect(subscribeProcess, &QProcess::readyReadStandardOutput, this, [this]() {
            subscribeBuffer.append(subscribeProcess->readAllStandardOutput());
            while (true) {
                const int newline = subscribeBuffer.indexOf('\n');
                if (newline < 0) break;
                const QByteArray line = subscribeBuffer.left(newline);
                subscribeBuffer.remove(0, newline + 1);
                if (line.contains("sink-input")) scanStreams();
                if (line.contains(" on sink ") || line.contains(" on server ")) refreshSinkVolume();
            }
            if (subscribeBuffer.size() > 8192) subscribeBuffer.clear();
        });

        mainTimer = new QTimer(this);
        mainTimer->setSingleShot(true);
        connect(mainTimer, &QTimer::timeout, this, [this]() {
            queueCommand({"set-sink-volume", "@DEFAULT_SINK@",
                          QString::number(mainSlider->value()) + "%"});
            if (mainSlider->value() > 0)
                queueCommand({"set-sink-mute", "@DEFAULT_SINK@", "0"});
        });
        connect(mainSlider, &QSlider::valueChanged, this, [this](int value) {
            settings->setValue("Sound/MainVolume", value);
            mainTimer->start(65);
        });
        connect(mainSlider, &QSlider::sliderReleased, this, [this]() {
            mainTimer->stop();
            mainTimer->start(1);
        });

        // Category values are immediately applied to playing streams and saved
        // through the PulseAudio/PipeWire stream-restore extension for later.
        connectGroupSlider(notifSlider, VolumeGroup::Notifications, "Sound/Notifications");
        connectGroupSlider(mediaSlider, VolumeGroup::Media, "Sound/Media");
        connectGroupSlider(callSlider, VolumeGroup::Calls, "Sound/InCall");
        connectGroupSlider(alarmSlider, VolumeGroup::Alarms, "Sound/Alarms");

        QPushButton *backButton = new QPushButton(QStringLiteral("❮"), this);
        backButton->setFixedSize(140, 60);
        backButton->setStyleSheet(
            "QPushButton { background:#444444; color:white; border:1px solid #222222; "
            "border-radius:16px; font-size:34px; }"
            "QPushButton:hover { background:#555555; }"
            "QPushButton:pressed { background:#333333; }");
        QHBoxLayout *backLay = new QHBoxLayout();
        backLay->addWidget(backButton, 0, Qt::AlignHCenter);
        outer->addLayout(backLay);
        connect(backButton, &QPushButton::clicked, this, [this]() {
            if (stackedWidget) stackedWidget->setCurrentIndex(0);
        });

        pactlPath = QStandardPaths::findExecutable("pactl");
        if (pactlPath.isEmpty()) {
            statusLabel->setText("Audio controls unavailable: pactl is not installed.");
            for (QSlider *s : {mainSlider, notifSlider, mediaSlider, callSlider, alarmSlider})
                s->setEnabled(false);
        } else {
            refreshSinkVolume();
            for (VolumeGroup group : {VolumeGroup::Notifications, VolumeGroup::Media,
                                      VolumeGroup::Calls, VolumeGroup::Alarms}) {
                if (isConfigured(group))
                    restoreWorker->schedule(restoreRoles(group), groupValue(group));
            }
            scanStreams();
            startSubscription();
        }
    }

    ~SoundPage() override {
        // Flush any last role-volume updates when leaving the settings app.
        for (auto it = roleTimers.begin(); it != roleTimers.end(); ++it) {
            it.value()->stop();
            if (isConfigured(it.key()))
                restoreWorker->schedule(restoreRoles(it.key()), groupValue(it.key()));
        }
        if (subscribeProcess && subscribeProcess->state() != QProcess::NotRunning) {
            subscribeProcess->terminate();
            if (!subscribeProcess->waitForFinished(300)) {
                subscribeProcess->kill();
                subscribeProcess->waitForFinished(300);
            }
        }
    }

private:
    void startProcess(QProcess *process, const QStringList &args) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("LC_ALL", "C");
        process->setProcessEnvironment(env);
        process->start(pactlPath, args);
    }

    void queueCommand(const QStringList &args) {
        if (pactlPath.isEmpty()) return;
        commandQueue.enqueue(args);
        runNextCommand();
    }

    void runNextCommand() {
        if (controlProcess->state() != QProcess::NotRunning || commandQueue.isEmpty()) return;
        startProcess(controlProcess, commandQueue.dequeue());
    }

    void refreshSinkVolume() {
        if (pactlPath.isEmpty() || sinkRefresh->state() != QProcess::NotRunning) return;
        startProcess(sinkRefresh, {"get-sink-volume", "@DEFAULT_SINK@"});
    }

    void scanStreams() {
        if (pactlPath.isEmpty() || streamScan->state() != QProcess::NotRunning) {
            scanAgain = true;
            return;
        }
        startProcess(streamScan, {"list", "sink-inputs"});
    }

    void startSubscription() {
        if (pactlPath.isEmpty() || subscribeProcess->state() != QProcess::NotRunning) return;
        startProcess(subscribeProcess, {"subscribe"});
    }

    void reportAudioError() {
        statusLabel->setText("Could not access the sound server. Check that PulseAudio or "
                             "PipeWire is running for your user session.");
    }

    bool isConfigured(VolumeGroup group) const {
        switch (group) {
        case VolumeGroup::Notifications: return settings->value("Sound/ConfiguredNotifications", false).toBool();
        case VolumeGroup::Media: return settings->value("Sound/ConfiguredMedia", false).toBool();
        case VolumeGroup::Calls: return settings->value("Sound/ConfiguredInCall", false).toBool();
        case VolumeGroup::Alarms: return settings->value("Sound/ConfiguredAlarms", false).toBool();
        }
        return false;
    }

    void markConfigured(VolumeGroup group) {
        switch (group) {
        case VolumeGroup::Notifications: settings->setValue("Sound/ConfiguredNotifications", true); break;
        case VolumeGroup::Media: settings->setValue("Sound/ConfiguredMedia", true); break;
        case VolumeGroup::Calls: settings->setValue("Sound/ConfiguredInCall", true); break;
        case VolumeGroup::Alarms: settings->setValue("Sound/ConfiguredAlarms", true); break;
        }
    }

    int groupValue(VolumeGroup group) const {
        switch (group) {
        case VolumeGroup::Notifications: return notifSlider->value();
        case VolumeGroup::Media: return mediaSlider->value();
        case VolumeGroup::Calls: return callSlider->value();
        case VolumeGroup::Alarms: return alarmSlider->value();
        }
        return 100;
    }

    void connectGroupSlider(QSlider *slider, VolumeGroup group, const QString &settingKey) {
        QTimer *streamTimer = new QTimer(this);
        streamTimer->setSingleShot(true);
        connect(streamTimer, &QTimer::timeout, this, [this]() {
            forceStreamUpdate = true;
            scanStreams();
        });
        QTimer *roleTimer = new QTimer(this);
        roleTimer->setSingleShot(true);
        roleTimers.insert(group, roleTimer);
        connect(roleTimer, &QTimer::timeout, this, [this, group]() {
            restoreWorker->schedule(restoreRoles(group), groupValue(group));
        });
        connect(slider, &QSlider::valueChanged, this,
                [this, group, settingKey, streamTimer, roleTimer](int value) {
            settings->setValue(settingKey, value);
            markConfigured(group);
            streamTimer->start(75);
            roleTimer->start(250);
        });
        connect(slider, &QSlider::sliderReleased, this,
                [streamTimer, roleTimer]() {
            streamTimer->start(1);
            roleTimer->start(1);
        });
    }

    QStackedWidget *stackedWidget = nullptr;
    QSettings *settings = nullptr;
    RoleRestoreWorker *restoreWorker = nullptr;
    QLabel *statusLabel = nullptr;
    QSlider *mainSlider = nullptr;
    QSlider *notifSlider = nullptr;
    QSlider *mediaSlider = nullptr;
    QSlider *callSlider = nullptr;
    QSlider *alarmSlider = nullptr;
    QSlider *vibSlider = nullptr;

    QString pactlPath;
    QQueue<QStringList> commandQueue;
    QProcess *controlProcess = nullptr;
    QProcess *sinkRefresh = nullptr;
    QProcess *streamScan = nullptr;
    QProcess *subscribeProcess = nullptr;
    QTimer *mainTimer = nullptr;
    QMap<VolumeGroup, QTimer *> roleTimers;
    QByteArray subscribeBuffer;
    QSet<int> configuredStreams;
    bool forceStreamUpdate = false;
    bool scanAgain = false;
};

extern "C" QWidget *make_page(QStackedWidget *stack) {
    return new SoundPage(stack);
}
