#include "MidiInput.h"

#include <QStringList>

#include <algorithm>
#include <array>

#ifdef AWESOME_STUDIO_HAS_ALSA
#include <alsa/asoundlib.h>

#include <cstdio>
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

    Impl()
    {
        nrpnMsb.fill(-1);
        nrpnLsb.fill(-1);
        nrpnDataMsb.fill(0);
        nrpnDataLsb.fill(0);
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
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_APPLICATION);
    if (impl->localPort < 0)
    {
        impl->error = QString::fromLocal8Bit(snd_strerror(impl->localPort));
        disconnect();
        if (error != nullptr)
            *error = impl->error;
        return false;
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
}

bool MidiInput::isConnected() const
{
#ifdef AWESOME_STUDIO_HAS_ALSA
    return impl->sequencer != nullptr;
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

        if (event->type == SND_SEQ_EVENT_NOTEON || event->type == SND_SEQ_EVENT_NOTEOFF)
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
    return events;
}

QString MidiInput::errorMessage() const
{
    return impl->error;
}
