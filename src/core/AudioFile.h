#pragma once

#include "Project.h"

class AudioFile
{
public:
    static constexpr quint64 maximumProjectDecodedBytes = 512ULL * 1024 * 1024;

    static bool loadWav(const QString& path, std::shared_ptr<const AudioAsset>& asset,
                        QString* error = nullptr);
};
