#include "vr_hand_manifest.h"

#include <string>

std::string BuildVrHandsActionManifestPath(const char* originalManifestPath)
{
    if (!originalManifestPath || !*originalManifestPath)
        return {};

    return originalManifestPath;
}
