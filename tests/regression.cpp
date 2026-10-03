#include <core/global/globals.hpp>
#include <filesystem>
#include <fstream>
#include <helper/audio/linux/pipewire/pipewire.hpp>
#include <helper/json/bindings.hpp>
#include <iostream>
#include <stdexcept>

using namespace Soundux;
using namespace Soundux::Objects;
using namespace Soundux::Globals;

namespace
{
    void check(bool result, const char *message)
    {
        if (!result)
        {
            throw std::runtime_error(message);
        }
    }

    nlohmann::json readConfig()
    {
        std::ifstream stream(Config::path);
        return nlohmann::json::parse(stream);
    }

    class TestWindow : public Window
    {
      public:
        using Window::changeSettings;
        using Window::removeTab;
        using Window::setCustomLocalVolume;
        using Window::setCustomRemoteVolume;
        using Window::setHotkey;
        std::vector<Enums::ErrorCode> errors;
        void show() override {}
        void mainLoop() override {}
        void onAdminRequired() override {}
        void onSettingsChanged() override {}
        void onSwitchOnConnectDetected(bool) override {}
        void onError(const Enums::ErrorCode &error) override
        {
            errors.push_back(error);
        }
        void onSoundProgressed(const PlayingSound &) override {}
        void onDownloadProgressed(float, const std::string &) override {}
    };

    class TestBackend : public AudioBackend
    {
      public:
        bool defaultResult = true;
        bool revertResult = true;
        bool setup() override
        {
            return true;
        }
        void destroy() override {}
        bool useAsDefault() override
        {
            return defaultResult;
        }
        bool revertDefault() override
        {
            return revertResult;
        }
        bool muteInput(bool) override
        {
            return true;
        }
        std::set<std::string> currentlyInputApps() override
        {
            return {};
        }
        std::set<std::string> currentlyPassedThrough() override
        {
            return {};
        }
        bool stopAllPassthrough() override
        {
            return true;
        }
        bool stopPassthrough(const std::string &) override
        {
            return true;
        }
        bool passthroughFrom(std::shared_ptr<PlaybackApp>) override
        {
            return true;
        }
        bool stopSoundInput() override
        {
            return true;
        }
        bool inputSoundTo(std::shared_ptr<RecordingApp>) override
        {
            return true;
        }
        std::shared_ptr<PlaybackApp> getPlaybackApp(const std::string &) override
        {
            return nullptr;
        }
        std::shared_ptr<RecordingApp> getRecordingApp(const std::string &) override
        {
            return nullptr;
        }
        std::vector<std::shared_ptr<PlaybackApp>> getPlaybackApps() override
        {
            return {};
        }
        std::vector<std::shared_ptr<RecordingApp>> getRecordingApps() override
        {
            return {};
        }
    };

    void regression()
    {
        TestWindow window;
        auto backend = std::make_shared<TestBackend>();
        gAudioBackend = backend;
        gSettings = Settings{};
        Sound sound;
        sound.id = 42;
        sound.name = "Regression";
        sound.path = "/nonexistent/regression.wav";
        Tab tab;
        tab.name = "Regression";
        tab.path = "/nonexistent";
        tab.sounds.push_back(sound);
        gData.setTabs({tab});
        gData.soundIdCounter = 42;

        auto *liveSound = &gData.getSound(42)->get();
        gData.markFavorite(42, true);
        check(gConfig.saveCurrent(), "initial save failed");
        check(&gData.getSound(42)->get() == liveSound, "snapshot redirected live sound index");
        check(&gFavorites->at(42).get() == liveSound, "snapshot redirected favorites index");

        check(window.setHotkey(42, {50, 38}).has_value(), "hotkey change failed");
        check(readConfig()["data"]["tabs"][0]["sounds"][0]["hotkeys"] == nlohmann::json({50, 38}),
              "first hotkey change was not saved");
        check(window.setHotkey(42, {37, 39}).has_value(), "second hotkey change failed");
        check(gData.getTabs()[0].sounds[0].hotkeys == std::vector<int>({37, 39}), "hotkey did not reach live tab");
        check(window.setCustomLocalVolume(42, 23).has_value(), "local volume change failed");
        check(window.setCustomRemoteVolume(42, 67).has_value(), "remote volume change failed");
        check(window.setCustomLocalVolume(42, std::nullopt).has_value(), "volume reset failed");

        auto settings = gSettings;
        settings.localVolume = 31;
        settings.remoteVolume = 72;
        settings.stopHotkey = {9};
        settings.pushToTalkKeys = {65};
        window.changeSettings(settings);
        Config loaded;
        check(loaded.load(), "round-trip load failed");
        check(&gData.getSound(42)->get() == liveSound, "loading snapshot redirected live index");
        check(loaded.settings.localVolume == 31 && loaded.settings.remoteVolume == 72, "global volumes lost");
        check(loaded.settings.stopHotkey == std::vector<int>({9}), "stop hotkey lost");
        check(loaded.settings.pushToTalkKeys == std::vector<int>({65}), "push-to-talk keys lost");
        const auto restored = loaded.data.getTabs()[0].sounds[0];
        check(restored.hotkeys == std::vector<int>({37, 39}), "sound hotkey lost");
        check(!restored.localVolume && restored.remoteVolume == 67 && restored.isFavorite, "sound settings lost");

        settings = gSettings;
        settings.outputs = {"one", "two"};
        window.changeSettings(settings);
        check(gSettings.outputs == std::vector<std::string>({"one"}), "multiple outputs not normalized");
        check(readConfig()["settings"]["outputs"] == nlohmann::json({"one"}), "pre-normalized outputs saved");
        settings = gSettings;
        settings.useAsDefaultDevice = true;
        backend->defaultResult = false;
        check(!window.changeSettings(settings).useAsDefaultDevice, "failed default switch left checkbox enabled");
        check(!readConfig()["settings"]["useAsDefaultDevice"].get<bool>(), "failed default switch persisted");
        backend->defaultResult = true;
        check(window.changeSettings(settings).useAsDefaultDevice, "successful default switch not applied");
        check(gSettings.outputs.empty() && readConfig()["settings"]["outputs"].empty(),
              "default mode retained outputs");
        backend->revertResult = false;
        settings = gSettings;
        settings.useAsDefaultDevice = false;
        check(window.changeSettings(settings).useAsDefaultDevice, "failed revert discarded default state");
        backend->revertResult = true;
        check(!window.changeSettings(settings).useAsDefaultDevice, "default mode did not turn off");

        auto legacy = readConfig();
        legacy["data"]["tabs"][0]["sounds"][0].erase("modifiedDate");
        legacy["settings"].erase("audioBackend");
        legacy["settings"]["stopHotkey"] = {9, "invalid"};
        legacy["settings"]["remoteVolume"] = "invalid";
        legacy["settings"]["theme"] = 99;
        legacy["settings"]["viewMode"] = 99;
        {
            std::ofstream stream(Config::path);
            stream << legacy;
        }
        check(loaded.load(), "legacy config rejected");
        check(loaded.settings.audioBackend == Enums::BackendType::PipeWire, "legacy backend default wrong");
        check(loaded.settings.stopHotkey.empty() && loaded.settings.remoteVolume == 100,
              "malformed fields not isolated");
        check(loaded.settings.theme == Enums::Theme::System && loaded.settings.viewMode == Enums::ViewMode::List,
              "invalid settings enums accepted");
        check(loaded.data.getTabs()[0].sounds[0].hotkeys == std::vector<int>({37, 39}), "legacy sound metadata lost");

        const auto preserved = readConfig();
        const std::filesystem::path held = Config::path + ".held";
        std::filesystem::rename(Config::path, held);
        std::filesystem::create_directory(Config::path);
        check(!gConfig.saveCurrent(), "failed replacement reported success");
        check(std::filesystem::is_directory(Config::path), "failed write removed destination");
        std::filesystem::remove(Config::path);
        std::filesystem::rename(held, Config::path);
        check(readConfig() == preserved, "failed write damaged previous config");

        {
            std::ofstream stream(Config::path);
            stream << "{broken";
        }
        Config recovered;
        check(!recovered.load(), "corrupt config accepted");
        bool backupFound = false;
        for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::path(Config::path).parent_path()))
        {
            if (entry.path().filename().string().find("config_old_") == 0)
            {
                std::ifstream stream(entry.path());
                const std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                backupFound = backupFound || content == "{broken";
            }
        }
        check(backupFound, "corrupt config not preserved");
        check(recovered.save(), "fresh config cannot be saved after recovery");
        check(window.removeTab(0).empty() && readConfig()["data"]["tabs"].empty(), "tab removal not saved");
        gAudioBackend.reset();
    }
} // namespace

int main(int argc, char **argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--pipewire")
        {
            auto backend = AudioBackend::createInstance(Enums::BackendType::PipeWire);
            check(std::dynamic_pointer_cast<PipeWire>(backend) != nullptr, "native PipeWire setup failed");
            std::cout << "TEST_READY" << std::endl;
            for (std::string command; std::getline(std::cin, command);)
            {
                if (command == "default")
                {
                    check(backend->useAsDefault(), "native default switch failed");
                }
                else if (command == "revert")
                {
                    check(backend->revertDefault(), "native default restore failed");
                }
                else if (command == "destroy")
                {
                    backend->destroy();
                    std::cout << "TEST_DONE" << std::endl;
                    return 0;
                }
                else
                {
                    throw std::runtime_error("Unknown test command");
                }
                std::cout << "TEST_DONE" << std::endl;
            }
            return 0;
        }
        regression();
        std::cout << "Config, hotkeys, volumes, indexes and settings transitions passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
