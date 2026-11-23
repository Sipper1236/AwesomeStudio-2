#pragma once

#include "Project.h"

class AudioFile
{
public:
    static bool loadWav(const QString& path, std::shared_ptr<const AudioAsset>& asset,
                        QString* error = nullptr);
};
