#pragma once

#include <QList>
#include <QString>
#include <QVector>

#include <memory>

struct MidiInputPort
{
    QString id;
    QString name;
};

struct MidiInputEvent
{
    int channel = 0;
    int pitch = 0;
    int velocity = 0;
    bool noteOn = false;
    bool controlChange = false;
    int controller = 0;
    int value = 0;
    bool pitchBend = false;
};

class MidiInput
{
public:
    MidiInput();
    ~MidiInput();

    static bool isSupported();
    static QList<MidiInputPort> availablePorts(QString* error = nullptr);

    bool connectTo(const QString& portId, QString* error = nullptr);
    void disconnect();
    bool isConnected() const;
    QVector<MidiInputEvent> readAvailable();
    QString errorMessage() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
