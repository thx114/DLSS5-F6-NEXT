#pragma once

#include <nvsdk_ngx_defs.h>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <cstdint>

class NgxFeatureRegistry
{
    std::mutex mutex;
    struct Entry { NVSDK_NGX_Feature feature; std::optional<int> flags; uint64_t generation; };
    std::unordered_map<unsigned int, Entry> features;
    uint64_t nextGeneration = 0;

  public:
    void Record(unsigned int handle, NVSDK_NGX_Feature feature, std::optional<int> flags = std::nullopt)
    {
        std::lock_guard lock(mutex);
        features[handle] = { feature, flags, ++nextGeneration };
    }

    void Forget(unsigned int handle)
    {
        std::lock_guard lock(mutex);
        features.erase(handle);
    }

    struct Snapshot
    {
        std::optional<NVSDK_NGX_Feature> feature;
        bool frameGenerationCreated = false;
        std::optional<int> flags;
        uint64_t generation = 0;
    };

    Snapshot Read(unsigned int handle)
    {
        std::lock_guard lock(mutex);
        Snapshot result;
        if (auto it = features.find(handle); it != features.end())
        {
            result.feature = it->second.feature;
            result.flags = it->second.flags;
            result.generation = it->second.generation;
        }
        for (const auto& [id, entry] : features)
            result.frameGenerationCreated |= entry.feature == NVSDK_NGX_Feature_FrameGeneration;
        return result;
    }
};
