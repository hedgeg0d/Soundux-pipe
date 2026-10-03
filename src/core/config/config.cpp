#include "config.hpp"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fancy.hpp>
#include <filesystem>
#include <fstream>
#include <helper/json/bindings.hpp>
#include <mutex>
#include <stdexcept>
#include <system_error>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace Soundux::Objects
{
    namespace
    {
        std::mutex configMutex;

        // Write beside the destination, then replace it atomically. A failed write leaves the old config intact.
        void writeAtomic(const std::filesystem::path &path, const std::string &content)
        {
            std::filesystem::create_directories(path.parent_path());
            std::filesystem::path temporary;
#if defined(__linux__)
            std::string name = path.string() + ".XXXXXX";
            const int fd = mkstemp(name.data());
            if (fd < 0)
            {
                throw std::system_error(errno, std::generic_category(), "Creating config temporary file");
            }
            temporary = name;
            FILE *file = fdopen(fd, "wb");
            if (!file)
            {
                const int error = errno;
                close(fd);
                std::filesystem::remove(temporary);
                throw std::system_error(error, std::generic_category(), "Opening config temporary file");
            }
#else
            temporary = path.string() + "." +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp";
#endif
            try
            {
#if defined(__linux__)
                bool success = fwrite(content.data(), 1, content.size(), file) == content.size();
                success = fflush(file) == 0 && success;
                success = fsync(fd) == 0 && success;
                const int error = errno;
                success = fclose(file) == 0 && success;
                if (!success)
                {
                    throw std::system_error(error ? error : EIO, std::generic_category(), "Writing config");
                }
                std::filesystem::rename(temporary, path);
                // Persist the rename too, so a power loss cannot bring back a truncated file.
                const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
                if (directory >= 0)
                {
                    fsync(directory);
                    close(directory);
                }
#else
                std::ofstream file;
                file.exceptions(std::ios::failbit | std::ios::badbit);
                file.open(temporary, std::ios::binary | std::ios::trunc);
                file << content;
                file.close();
                if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                {
                    throw std::system_error(GetLastError(), std::system_category(), "Replacing config");
                }
#endif
            }
            catch (...)
            {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                throw;
            }
        }
    } // namespace

    const std::string Config::path = []() -> std::string {
#if defined(__linux__)
        const auto *configPath = std::getenv("XDG_CONFIG_HOME"); // NOLINT
        if (configPath && *configPath && std::filesystem::path(configPath).is_absolute())
        {
            return std::string(configPath) + "/Soundux/config.json";
        }
        const auto *home = std::getenv("HOME"); // NOLINT
        if (home && *home)
        {
            return std::string(home) + "/.config/Soundux/config.json";
        }
#elif defined(_WIN32)
        const auto *appData = std::getenv("APPDATA"); // NOLINT
        if (appData && *appData)
        {
            return std::string(appData) + "\\Soundux\\config.json";
        }
#endif
        return {};
    }();

    bool Config::save() const
    {
        std::lock_guard<std::mutex> lock(configMutex);
        return write();
    }

    bool Config::write() const
    {
        try
        {
            if (path.empty() || !writeAllowed)
            {
                throw std::runtime_error("Config path unavailable or unreadable config not backed up");
            }
            writeAtomic(path, nlohmann::json(*this).dump());
            Fancy::fancy.logTime().success() << "Config written" << std::endl;
            return true;
        }
        catch (const std::exception &e)
        {
            Fancy::fancy.logTime().failure() << "Failed to write config: " >> e.what() << std::endl;
            return false;
        }
    }

    bool Config::saveCurrent()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        // set() is only for the live Data: calling it on a snapshot redirects gSounds/gFavorites into the copy.
        data = Globals::gData;
        settings = Globals::gSettings;
        return write();
    }

    bool Config::load()
    {
        std::lock_guard<std::mutex> lock(configMutex);
        try
        {
            if (path.empty())
            {
                throw std::runtime_error("Config path unavailable");
            }
            if (!std::filesystem::exists(path))
            {
                writeAllowed = true;
                Fancy::fancy.logTime().warning() << "Config not found" << std::endl;
                return false;
            }

            writeAllowed = false;
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                throw std::runtime_error("Cannot open config");
            }
            stream.exceptions(std::ios::badbit);
            const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            stream.close();

            try
            {
                auto conf = nlohmann::json::parse(content).get<Config>();
                data = conf.data;
                settings = conf.settings;
                writeAllowed = true;
                Fancy::fancy.logTime().success() << "Config read" << std::endl;
                return true;
            }
            catch (const nlohmann::json::exception &e)
            {
                // Preserve corrupt/unsupported input before allowing a fresh config to replace it.
                const auto backup =
                    std::filesystem::path(path).parent_path() /
                    ("config_old_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
                     ".json");
                std::filesystem::rename(path, backup);
                writeAllowed = true;
                Fancy::fancy.logTime().warning()
                    << "Invalid config backed up to " << backup.string() << ": " << e.what() << std::endl;
            }
        }
        catch (const std::exception &e)
        {
            Fancy::fancy.logTime().failure() << "Failed to read config: " << e.what() << std::endl;
        }
        return false;
    }
} // namespace Soundux::Objects
