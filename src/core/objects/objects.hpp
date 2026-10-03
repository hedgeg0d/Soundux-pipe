#pragma once
#include <core/enums/enums.hpp>
#include <optional>
#include <string>
#include <vector>

namespace Soundux
{
    namespace Objects
    {
        struct AudioDevice;

        struct Sound
        {
            std::uint32_t id = 0;
            std::string name;
            std::string path;
            bool isFavorite = false;

            std::vector<int> hotkeys;
            std::uint64_t modifiedDate = 0;

            std::optional<int> localVolume;
            std::optional<int> remoteVolume;
        };

        struct Tab
        {
            std::uint32_t id = 0; //* Equal to index
            std::string name;
            std::string path;

            std::vector<Sound> sounds;
            Enums::SortMode sortMode = Enums::SortMode::ModifiedDate_Descending;
        };
    } // namespace Objects
} // namespace Soundux
