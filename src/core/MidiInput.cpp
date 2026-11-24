#include "MidiInput.h"

#include <QByteArray>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#ifdef AWESOME_STUDIO_HAS_ALSA
#include <alsa/asoundlib.h>

#include <cstdio>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

namespace
{
QString coreMidiStringProperty(MIDIObjectRef object, CFStringRef property)
{
    CFStringRef value = nullptr;
    if (MIDIObjectGetStringProperty(object, property, &value) != noErr || value == nullptr)
        return {};

    const CFIndex maximumLength =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    QByteArray utf8(static_cast<qsizetype>(maximumLength), '\0');
    const bool converted = CFStringGetCString(
        value, utf8.data(), static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8);
    CFRelease(value);
    return converted ? QString::fromUtf8(utf8.constData()) : QString();
}
}
#endif

struct MidiInput::Impl
{
#ifdef AWESOME_STUDIO_HAS_ALSA
    snd_seq_t* sequencer = nullptr;
    int localPort = -1;
    std::array<int, 16> nrpnMsb;
    std::array<int, 16> nrpnLsb;
    std::array<int, 16> nrpnDataMsb{};
    std::array<int, 16> nrpnDataLsb{};

#endif

#ifdef _WIN32
    HMIDIIN winMmHandle = nullptr;
#endif

#ifdef __APPLE__
    MIDIClientRef coreMidiClient = 0;
    MIDIPortRef coreMidiInputPort = 0;
    MIDIEndpointRef coreMidiSource = 0;
    Byte coreMidiRunningStatus = 0;
    Byte coreMidiPendingStatus = 0;
    Byte coreMidiExpectedData = 0;
    Byte coreMidiReceivedData = 0;
    Byte coreMidiFirstData = 0;
    bool coreMidiInSysEx = false;
#endif

#if defined(_WIN32) || defined(__APPLE__)
    std::mutex platformMidiMutex;
    std::vector<MidiInputEvent> platformMidiEvents;
    std::array<int, 16> nrpnMsb;
    std::array<int, 16> nrpnLsb;
    std::array<int, 16> nrpnDataMsb{};
    std::array<int, 16> nrpnDataLsb{};
#endif

    Impl()
    {
#ifdef AWESOME_STUDIO_HAS_ALSA
        nrpnMsb.fill(-1);
        nrpnLsb.fill(-1);
        nrpnDataMsb.fill(0);
        nrpnDataLsb.fill(0);
#endif
#if defined(_WIN32) || defined(__APPLE__)
        nrpnMsb.fill(-1);
        nrpnLsb.fill(-1);
        nrpnDataMsb.fill(0);
        nrpnDataLsb.fill(0);
#endif
    }

#if defined(_WIN32) || defined(__APPLE__)
    void queuePackedMessage(std::uint32_t packedMessage)
    {
        const int status = static_cast<int>(packedMessage & 0xff);
        const int messageType = status & 0xf0;
        const int channel = status & 0x0f;
        const int data1 = static_cast<int>((packedMessage >> 8) & 0x7f);
        const int data2 = static_cast<int>((packedMessage >> 16) & 0x7f);

        if (messageType == 0x80 || messageType == 0x90)
        {
            MidiInputEvent event;
            event.channel = channel;
            event.pitch = data1;
            event.velocity = data2;
            event.noteOn = messageType == 0x90 && data2 > 0;
            platformMidiEvents.push_back(event);
        }
        else if (messageType == 0xb0)
        {
            MidiInputEvent control;
            control.channel = channel;
            control.controller = data1;
            control.value = data2;
            const size_t channelIndex = static_cast<size_t>(channel);
            if (control.controller == 101 || control.controller == 100)
            {
                nrpnMsb[channelIndex] = -1;
                nrpnLsb[channelIndex] = -1;
                nrpnDataMsb[channelIndex] = 0;
                nrpnDataLsb[channelIndex] = 0;
            }
            else if (control.controller == 99)
            {
                nrpnMsb[channelIndex] = control.value;
                nrpnLsb[channelIndex] = -1;
                nrpnDataMsb[channelIndex] = 0;
                nrpnDataLsb[channelIndex] = 0;
            }
            else if (control.controller == 98)
            {
                nrpnLsb[channelIndex] = control.value;
                nrpnDataMsb[channelIndex] = 0;
                nrpnDataLsb[channelIndex] = 0;
            }
            else if ((control.controller == 6 || control.controller == 38) &&
                     nrpnMsb[channelIndex] >= 0 && nrpnLsb[channelIndex] >= 0)
            {
                if (control.controller == 6)
                    nrpnDataMsb[channelIndex] = control.value;
                else
                    nrpnDataLsb[channelIndex] = control.value;
                control.registeredParameter = true;
                control.parameterNumber = (nrpnMsb[channelIndex] << 7) |
                                          nrpnLsb[channelIndex];
                control.parameterValue = (nrpnDataMsb[channelIndex] << 7) |
                                         nrpnDataLsb[channelIndex];
                platformMidiEvents.push_back(control);
            }
            else
            {
                control.controlChange = true;
                platformMidiEvents.push_back(control);
            }
        }
        else if (messageType == 0xe0)
        {
            MidiInputEvent bend;
            bend.channel = channel;
            bend.pitchBend = true;
            bend.value = data1 | (data2 << 7);
            platformMidiEvents.push_back(bend);
        }
    }

    void handleShortMessage(std::uint32_t packedMessage)
    {
        std::lock_guard<std::mutex> lock(platformMidiMutex);
        queuePackedMessage(packedMessage);
    }
#endif

#ifdef _WIN32
    static void CALLBACK midiInputCallback(HMIDIIN, UINT message, DWORD_PTR instance,
                                           DWORD_PTR parameter1, DWORD_PTR)
    {
        if (message != MIM_DATA || instance == 0)
            return;
        reinterpret_cast<Impl*>(instance)->handleShortMessage(static_cast<std::uint32_t>(parameter1));
    }
#endif

#ifdef __APPLE__
    static void coreMidiReadCallback(const MIDIPacketList* packetList, void* readProcRefCon,
                                     void*)
    {
        if (packetList == nullptr || readProcRefCon == nullptr)
            return;

        auto* self = static_cast<Impl*>(readProcRefCon);
        const MIDIPacket* packet = &packetList->packet[0];
        for (UInt32 packetIndex = 0; packetIndex < packetList->numPackets; ++packetIndex)
        {
            self->handleMidiBytes(packet->data, packet->length);
            packet = MIDIPacketNext(packet);
        }
    }

    void handleMidiBytes(const Byte* bytes, UInt16 length)
    {
        std::lock_guard<std::mutex> lock(platformMidiMutex);
        for (UInt16 index = 0; index < length; ++index)
        {
            const Byte value = bytes[index];
            if (value >= 0xf8)
                continue;

            if ((value & 0x80) != 0)
            {
                if (coreMidiInSysEx)
                {
                    if (value == 0xf7)
                    {
                        coreMidiInSysEx = false;
                        continue;
                    }
                    coreMidiInSysEx = false;
                }

                coreMidiReceivedData = 0;
                if (value == 0xf0)
                {
                    coreMidiInSysEx = true;
                    coreMidiRunningStatus = 0;
                    coreMidiPendingStatus = 0;
                    coreMidiExpectedData = 0;
                }
                else if (value >= 0xf0)
                {
                    coreMidiRunningStatus = 0;
                    coreMidiPendingStatus = value;
                    coreMidiExpectedData = value == 0xf1 || value == 0xf3 ? 1
                        : value == 0xf2 ? 2 : 0;
                    if (coreMidiExpectedData == 0)
                        coreMidiPendingStatus = 0;
                }
                else
                {
                    coreMidiRunningStatus = value;
                    coreMidiPendingStatus = value;
                    const int messageType = value & 0xf0;
                    coreMidiExpectedData = messageType == 0xc0 || messageType == 0xd0 ? 1 : 2;
                }
                continue;
            }

            if (coreMidiInSysEx)
                continue;
            if (coreMidiPendingStatus == 0)
            {
                if (coreMidiRunningStatus == 0)
                    continue;
                coreMidiPendingStatus = coreMidiRunningStatus;
                const int messageType = coreMidiRunningStatus & 0xf0;
                coreMidiExpectedData = messageType == 0xc0 || messageType == 0xd0 ? 1 : 2;
                coreMidiReceivedData = 0;
            }

            if (coreMidiReceivedData == 0)
            {
                coreMidiFirstData = value;
                coreMidiReceivedData = 1;
                if (coreMidiExpectedData == 1)
                {
                    queuePackedMessage(static_cast<std::uint32_t>(coreMidiPendingStatus) |
                                       (static_cast<std::uint32_t>(coreMidiFirstData) << 8));
                    coreMidiPendingStatus = coreMidiPendingStatus < 0xf0
                        ? coreMidiRunningStatus : 0;
                    coreMidiReceivedData = 0;
                }
            }
            else
            {
                queuePackedMessage(static_cast<std::uint32_t>(coreMidiPendingStatus) |
                                   (static_cast<std::uint32_t>(coreMidiFirstData) << 8) |
                                   (static_cast<std::uint32_t>(value) << 16));
                coreMidiPendingStatus = coreMidiPendingStatus < 0xf0
                    ? coreMidiRunningStatus : 0;
                coreMidiReceivedData = 0;
            }
        }
    }
#endif

    QString error;
};

MidiInput::MidiInput() : impl(std::make_unique<Impl>())
{
}

MidiInput::~MidiInput()
{
    disconnect();
}

bool MidiInput::isSupported()
{
#ifdef AWESOME_STUDIO_HAS_ALSA
    return true;
#elif defined(_WIN32)
    return true;
#elif defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

QList<MidiInputPort> MidiInput::availablePorts(QString* error)
{
    QList<MidiInputPort> ports;
#ifdef AWESOME_STUDIO_HAS_ALSA
    snd_seq_t* sequencer = nullptr;
    const int openResult = snd_seq_open(&sequencer, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
    if (openResult < 0)
    {
        if (error != nullptr)
            *error = QString::fromLocal8Bit(snd_strerror(openResult));
        return ports;
    }

    snd_seq_client_info_t* clientInfo = nullptr;
    snd_seq_port_info_t* portInfo = nullptr;
    snd_seq_client_info_alloca(&clientInfo);
    snd_seq_port_info_alloca(&portInfo);
    snd_seq_client_info_set_client(clientInfo, -1);
    while (snd_seq_query_next_client(sequencer, clientInfo) >= 0)
    {
        const int client = snd_seq_client_info_get_client(clientInfo);
        const QString clientName = QString::fromLocal8Bit(snd_seq_client_info_get_name(clientInfo));
        snd_seq_port_info_set_client(portInfo, client);
        snd_seq_port_info_set_port(portInfo, -1);
        while (snd_seq_query_next_port(sequencer, portInfo) >= 0)
        {
            const unsigned int capabilities = snd_seq_port_info_get_capability(portInfo);
            if ((capabilities & SND_SEQ_PORT_CAP_READ) == 0 ||
                (capabilities & SND_SEQ_PORT_CAP_SUBS_READ) == 0)
                continue;

            const int port = snd_seq_port_info_get_port(portInfo);
            const QString portName = QString::fromLocal8Bit(snd_seq_port_info_get_name(portInfo));
            ports.push_back({QStringLiteral("%1:%2").arg(client).arg(port),
                             QStringLiteral("%1 · %2").arg(clientName, portName)});
        }
    }
    snd_seq_close(sequencer);
#elif defined(_WIN32)
    const UINT deviceCount = midiInGetNumDevs();
    for (UINT device = 0; device < deviceCount; ++device)
    {
        MIDIINCAPSW capabilities{};
        const MMRESULT result = midiInGetDevCapsW(
            static_cast<UINT_PTR>(device), &capabilities, sizeof(capabilities));
        if (result != MMSYSERR_NOERROR)
            continue;
        ports.push_back({QStringLiteral("winmm:%1").arg(device),
                         QString::fromWCharArray(capabilities.szPname)});
    }
#elif defined(__APPLE__)
    const ItemCount sourceCount = MIDIGetNumberOfSources();
    for (ItemCount index = 0; index < sourceCount; ++index)
    {
        const MIDIEndpointRef source = MIDIGetSource(index);
        if (source == 0)
            continue;

        MIDIUniqueID uniqueId = 0;
        if (MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &uniqueId) != noErr)
            continue;
        QString name = coreMidiStringProperty(source, kMIDIPropertyDisplayName);
        if (name.isEmpty())
            name = coreMidiStringProperty(source, kMIDIPropertyName);
        if (name.isEmpty())
            name = QStringLiteral("MIDI input %1").arg(uniqueId);
        ports.push_back({QStringLiteral("coremidi:%1").arg(uniqueId), name});
    }
#else
    if (error != nullptr)
        *error = QStringLiteral("MIDI input is unavailable in this build.");
#endif
    return ports;
}

bool MidiInput::connectTo(const QString& portId, QString* error)
{
    disconnect();
#ifdef AWESOME_STUDIO_HAS_ALSA
    const QStringList address = portId.split(QLatin1Char(':'));
    bool clientOk = false;
    bool portOk = false;
    const int client = address.size() == 2 ? address[0].toInt(&clientOk) : -1;
    const int port = address.size() == 2 ? address[1].toInt(&portOk) : -1;
    if (!clientOk || !portOk || client < 0 || port < 0)
    {
        impl->error = QStringLiteral("The selected MIDI input port is invalid.");
        if (error != nullptr)
            *error = impl->error;
        return false;
    }

    int result = snd_seq_open(&impl->sequencer, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
    if (result < 0)
    {
        impl->error = QString::fromLocal8Bit(snd_strerror(result));
        if (error != nullptr)
            *error = impl->error;
        return false;
    }
    snd_seq_set_client_name(impl->sequencer, "Awesome Studio");
    impl->localPort = snd_seq_create_simple_port(
        impl->sequencer, "MIDI Input",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE |
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_APPLICATION);
    if (impl->localPort < 0)
    {
        impl->error = QString::fromLocal8Bit(snd_strerror(impl->localPort));
        disconnect();
        if (error != nullptr)
            *error = impl->error;
        return false;
    }
    snd_seq_port_info_t* localPortInfo = nullptr;
    if (snd_seq_port_info_malloc(&localPortInfo) >= 0)
    {
        if (snd_seq_get_port_info(impl->sequencer, impl->localPort, localPortInfo) >= 0)
        {
            snd_seq_port_info_set_timestamping(localPortInfo, 1);
            snd_seq_port_info_set_timestamp_real(localPortInfo, 1);
            snd_seq_port_info_set_timestamp_queue(localPortInfo, SND_SEQ_QUEUE_DIRECT);
            snd_seq_set_port_info(impl->sequencer, impl->localPort, localPortInfo);
        }
        snd_seq_port_info_free(localPortInfo);
    }

    result = snd_seq_connect_from(impl->sequencer, impl->localPort, client, port);
    if (result < 0)
    {
        impl->error = QString::fromLocal8Bit(snd_strerror(result));
        disconnect();
        if (error != nullptr)
            *error = impl->error;
        return false;
    }
    impl->error.clear();
    return true;
#elif defined(_WIN32)
    if (!portId.startsWith(QStringLiteral("winmm:")))
    {
        impl->error = QStringLiteral("The selected MIDI input port is invalid.");
        if (error != nullptr)
            *error = impl->error;
        return false;
    }

    bool deviceOk = false;
    const uint device = portId.mid(6).toUInt(&deviceOk);
    if (!deviceOk || device >= midiInGetNumDevs())
    {
        impl->error = QStringLiteral("The selected MIDI input device is no longer available.");
        if (error != nullptr)
            *error = impl->error;
        return false;
    }

    MMRESULT result = midiInOpen(
        &impl->winMmHandle, static_cast<UINT>(device),
        reinterpret_cast<DWORD_PTR>(&Impl::midiInputCallback),
        reinterpret_cast<DWORD_PTR>(impl.get()), CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR)
    {
        wchar_t message[MAXERRORLENGTH]{};
        impl->winMmHandle = nullptr;
        impl->error = midiInGetErrorTextW(result, message, MAXERRORLENGTH) == MMSYSERR_NOERROR
            ? QString::fromWCharArray(message)
            : QStringLiteral("WinMM error %1").arg(static_cast<uint>(result));
        if (error != nullptr)
            *error = impl->error;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl->platformMidiMutex);
        impl->platformMidiEvents.clear();
        impl->nrpnMsb.fill(-1);
        impl->nrpnLsb.fill(-1);
        impl->nrpnDataMsb.fill(0);
        impl->nrpnDataLsb.fill(0);
    }
    result = midiInStart(impl->winMmHandle);
    if (result != MMSYSERR_NOERROR)
    {
        wchar_t message[MAXERRORLENGTH]{};
        impl->error = midiInGetErrorTextW(result, message, MAXERRORLENGTH) == MMSYSERR_NOERROR
            ? QString::fromWCharArray(message)
            : QStringLiteral("WinMM error %1").arg(static_cast<uint>(result));
        disconnect();
        if (error != nullptr)
            *error = impl->error;
        return false;
    }
    impl->error.clear();
    return true;
#elif defined(__APPLE__)
    if (!portId.startsWith(QStringLiteral("coremidi:")))
    {
        impl->error = QStringLiteral("The selected MIDI input port is invalid.");
        if (error != nullptr)
            *error = impl->error;
        return false;
    }

    bool uniqueIdOk = false;
    const qlonglong requestedId = portId.mid(9).toLongLong(&uniqueIdOk);
    if (!uniqueIdOk)
    {
        impl->error = QStringLiteral("The selected MIDI input port is invalid.");
        if (error != nullptr)
            *error = impl->error;
        return false;
    }

    const ItemCount sourceCount = MIDIGetNumberOfSources();
    for (ItemCount index = 0; index < sourceCount; ++index)
    {
        const MIDIEndpointRef source = MIDIGetSource(index);
        MIDIUniqueID uniqueId = 0;
        if (source != 0 &&
            MIDIObjectGetIntegerProperty(source, kMIDIPropertyUniqueID, &uniqueId) == noErr &&
            static_cast<qlonglong>(uniqueId) == requestedId)
        {
            impl->coreMidiSource = source;
            break;
        }
    }
    if (impl->coreMidiSource == 0)
    {
        impl->error = QStringLiteral("The selected CoreMIDI source is no longer available.");
        if (error != nullptr)
            *error = impl->error;
        return false;
    }

    OSStatus result = MIDIClientCreate(CFSTR("Awesome Studio"), nullptr, nullptr,
                                       &impl->coreMidiClient);
    if (result != noErr)
    {
        impl->coreMidiSource = 0;
        impl->error = QStringLiteral("Could not create a CoreMIDI client (error %1).")
                          .arg(static_cast<int>(result));
        if (error != nullptr)
            *error = impl->error;
        return false;
    }

    result = MIDIInputPortCreate(impl->coreMidiClient, CFSTR("MIDI Input"),
                                 &Impl::coreMidiReadCallback, impl.get(),
                                 &impl->coreMidiInputPort);
    if (result != noErr)
    {
        impl->error = QStringLiteral("Could not create a CoreMIDI input port (error %1).")
                          .arg(static_cast<int>(result));
        disconnect();
        if (error != nullptr)
            *error = impl->error;
        return false;
    }

    result = MIDIPortConnectSource(impl->coreMidiInputPort, impl->coreMidiSource, nullptr);
    if (result != noErr)
    {
        impl->error = QStringLiteral("Could not connect to the CoreMIDI source (error %1).")
                          .arg(static_cast<int>(result));
        disconnect();
        if (error != nullptr)
            *error = impl->error;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl->platformMidiMutex);
        impl->platformMidiEvents.clear();
        impl->nrpnMsb.fill(-1);
        impl->nrpnLsb.fill(-1);
        impl->nrpnDataMsb.fill(0);
        impl->nrpnDataLsb.fill(0);
        impl->coreMidiRunningStatus = 0;
        impl->coreMidiPendingStatus = 0;
        impl->coreMidiExpectedData = 0;
        impl->coreMidiReceivedData = 0;
        impl->coreMidiInSysEx = false;
    }
    impl->error.clear();
    return true;
#else
    Q_UNUSED(portId)
    impl->error = QStringLiteral("MIDI input is unavailable in this build.");
    if (error != nullptr)
        *error = impl->error;
    return false;
#endif
}

void MidiInput::disconnect()
{
#ifdef AWESOME_STUDIO_HAS_ALSA
    if (impl->sequencer != nullptr)
    {
        snd_seq_close(impl->sequencer);
        impl->sequencer = nullptr;
        impl->localPort = -1;
    }
    impl->nrpnMsb.fill(-1);
    impl->nrpnLsb.fill(-1);
    impl->nrpnDataMsb.fill(0);
    impl->nrpnDataLsb.fill(0);
#endif
#ifdef _WIN32
    if (impl->winMmHandle != nullptr)
    {
        midiInStop(impl->winMmHandle);
        midiInReset(impl->winMmHandle);
        midiInClose(impl->winMmHandle);
        impl->winMmHandle = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(impl->platformMidiMutex);
        impl->platformMidiEvents.clear();
        impl->nrpnMsb.fill(-1);
        impl->nrpnLsb.fill(-1);
        impl->nrpnDataMsb.fill(0);
        impl->nrpnDataLsb.fill(0);
    }
#endif
#ifdef __APPLE__
    if (impl->coreMidiInputPort != 0 && impl->coreMidiSource != 0)
        MIDIPortDisconnectSource(impl->coreMidiInputPort, impl->coreMidiSource);
    if (impl->coreMidiInputPort != 0)
    {
        MIDIPortDispose(impl->coreMidiInputPort);
        impl->coreMidiInputPort = 0;
    }
    if (impl->coreMidiClient != 0)
    {
        MIDIClientDispose(impl->coreMidiClient);
        impl->coreMidiClient = 0;
    }
    impl->coreMidiSource = 0;
    {
        std::lock_guard<std::mutex> lock(impl->platformMidiMutex);
        impl->platformMidiEvents.clear();
        impl->nrpnMsb.fill(-1);
        impl->nrpnLsb.fill(-1);
        impl->nrpnDataMsb.fill(0);
        impl->nrpnDataLsb.fill(0);
        impl->coreMidiRunningStatus = 0;
        impl->coreMidiPendingStatus = 0;
        impl->coreMidiExpectedData = 0;
        impl->coreMidiReceivedData = 0;
        impl->coreMidiInSysEx = false;
    }
#endif
}

bool MidiInput::isConnected() const
{
#ifdef AWESOME_STUDIO_HAS_ALSA
    return impl->sequencer != nullptr;
#elif defined(_WIN32)
    return impl->winMmHandle != nullptr;
#elif defined(__APPLE__)
    return impl->coreMidiInputPort != 0 && impl->coreMidiSource != 0;
#else
    return false;
#endif
}

QVector<MidiInputEvent> MidiInput::readAvailable()
{
    QVector<MidiInputEvent> events;
#ifdef AWESOME_STUDIO_HAS_ALSA
    if (impl->sequencer == nullptr)
        return events;

    while (snd_seq_event_input_pending(impl->sequencer, 1) > 0)
    {
        snd_seq_event_t* event = nullptr;
        if (snd_seq_event_input(impl->sequencer, &event) < 0 || event == nullptr)
            break;

        if (event->type == SND_SEQ_EVENT_CLOCK || event->type == SND_SEQ_EVENT_TICK)
        {
            MidiInputEvent clock;
            clock.clockTick = true;
            if (event->flags & SND_SEQ_TIME_STAMP_REAL)
                clock.timestampMicroseconds = static_cast<qint64>(event->time.time.tv_sec) * 1000000LL +
                                              event->time.time.tv_nsec / 1000;
            events.push_back(clock);
        }
        else if (event->type == SND_SEQ_EVENT_START || event->type == SND_SEQ_EVENT_CONTINUE ||
                 event->type == SND_SEQ_EVENT_STOP)
        {
            MidiInputEvent transport;
            transport.transportStart = event->type == SND_SEQ_EVENT_START;
            transport.transportContinue = event->type == SND_SEQ_EVENT_CONTINUE;
            transport.transportStop = event->type == SND_SEQ_EVENT_STOP;
            if (event->flags & SND_SEQ_TIME_STAMP_REAL)
                transport.timestampMicroseconds = static_cast<qint64>(event->time.time.tv_sec) * 1000000LL +
                                                  event->time.time.tv_nsec / 1000;
            events.push_back(transport);
        }
        else if (event->type == SND_SEQ_EVENT_NOTEON || event->type == SND_SEQ_EVENT_NOTEOFF)
        {
            const bool noteOn = event->type == SND_SEQ_EVENT_NOTEON && event->data.note.velocity > 0;
            events.push_back({event->data.note.channel,
                              event->data.note.note,
                              event->data.note.velocity,
                              noteOn});
        }
        else if (event->type == SND_SEQ_EVENT_CONTROLLER)
        {
            MidiInputEvent control;
            control.channel = std::clamp(static_cast<int>(event->data.control.channel), 0, 15);
            control.controller = std::clamp(static_cast<int>(event->data.control.param), 0, 127);
            control.value = std::clamp(event->data.control.value, 0, 127);
            const size_t channel = static_cast<size_t>(control.channel);
            if (control.controller == 101 || control.controller == 100)
            {
                impl->nrpnMsb[channel] = -1;
                impl->nrpnLsb[channel] = -1;
                impl->nrpnDataMsb[channel] = 0;
                impl->nrpnDataLsb[channel] = 0;
            }
            else if (control.controller == 99)
            {
                impl->nrpnMsb[channel] = control.value;
                impl->nrpnLsb[channel] = -1;
                impl->nrpnDataMsb[channel] = 0;
                impl->nrpnDataLsb[channel] = 0;
            }
            else if (control.controller == 98)
            {
                impl->nrpnLsb[channel] = control.value;
                impl->nrpnDataMsb[channel] = 0;
                impl->nrpnDataLsb[channel] = 0;
            }
            else if ((control.controller == 6 || control.controller == 38) &&
                     impl->nrpnMsb[channel] >= 0 && impl->nrpnLsb[channel] >= 0)
            {
                if (control.controller == 6)
                    impl->nrpnDataMsb[channel] = control.value;
                else
                    impl->nrpnDataLsb[channel] = control.value;
                control.registeredParameter = true;
                control.parameterNumber = (impl->nrpnMsb[channel] << 7) |
                                          impl->nrpnLsb[channel];
                control.parameterValue = (impl->nrpnDataMsb[channel] << 7) |
                                         impl->nrpnDataLsb[channel];
                events.push_back(control);
            }
            else
            {
                control.controlChange = true;
                events.push_back(control);
            }
        }
        else if (event->type == SND_SEQ_EVENT_PITCHBEND)
        {
            MidiInputEvent bend;
            bend.channel = event->data.control.channel;
            bend.pitchBend = true;
            bend.value = std::clamp(event->data.control.value + 8192, 0, 16383);
            events.push_back(bend);
        }
        snd_seq_free_event(event);
    }
#endif
#if defined(_WIN32) || defined(__APPLE__)
    std::vector<MidiInputEvent> pendingEvents;
    {
        std::lock_guard<std::mutex> lock(impl->platformMidiMutex);
        pendingEvents.swap(impl->platformMidiEvents);
    }
    for (const MidiInputEvent& event : pendingEvents)
        events.push_back(event);
#endif
    return events;
}

QString MidiInput::errorMessage() const
{
    return impl->error;
}
