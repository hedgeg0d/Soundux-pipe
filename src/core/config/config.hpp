#pragma once
#include <core/objects/data.hpp>
#include <core/objects/settings.hpp>
#include <string>

namespace Soundux
{
    namespace Objects
    {
        struct Config
        {
            Data data;
            Settings settings;

            bool save() const;
            bool saveCurrent();
            bool load();
            static const std::string path;

          private:
            bool writeAllowed = true;
            bool write() const;
        };
    } // namespace Objects
} // namespace Soundux
