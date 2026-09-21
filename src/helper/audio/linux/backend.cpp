#if defined(__linux__)
#include "backend.hpp"
#include "pipewire/pipewire.hpp"
#include "pulseaudio/pulseaudio.hpp"
#include <core/enums/enums.hpp>
#include <core/global/globals.hpp>
#include <fancy.hpp>
#include <memory>

namespace Soundux::Objects
{
    std::shared_ptr<AudioBackend> AudioBackend::createInstance(Enums::BackendType backend)
    {
        if (backend == Enums::BackendType::None)
        {
            Fancy::fancy.logTime().message() << "Audio backend is disabled" << std::endl;
            return nullptr;
        }

        const auto createPipeWire = [&]() -> std::shared_ptr<AudioBackend> {
            auto pipeWireInstance = std::shared_ptr<PipeWire>(new PipeWire()); // NOLINT
            if (pipeWireInstance->setup())
            {
                return pipeWireInstance;
            }

            return nullptr;
        };

        const auto createPulseAudio = [&](bool &runningOnPipeWire) -> std::shared_ptr<AudioBackend> {
            auto pulseInstance = std::shared_ptr<PulseAudio>(new PulseAudio()); // NOLINT
            if (!pulseInstance->setup())
            {
                runningOnPipeWire = pulseInstance->isRunningPipeWire();
                return nullptr;
            }

            if (!pulseInstance->switchOnConnectPresent() && !pulseInstance->loadModules())
            {
                return nullptr;
            }

            //* The stuff we do is broken on pipewire-pulse, prefer the native backend there
            runningOnPipeWire = pulseInstance->isRunningPipeWire();
            return pulseInstance;
        };

        std::shared_ptr<AudioBackend> instance;
        bool runningOnPipeWire = false;

        if (backend == Enums::BackendType::PipeWire)
        {
            instance = createPipeWire();
            if (!instance)
            {
                Fancy::fancy.logTime().warning() << "Failed to use pipewire, falling back to pulseaudio" << std::endl;
                instance = createPulseAudio(runningOnPipeWire);
            }
        }
        else if (backend == Enums::BackendType::PulseAudio)
        {
            instance = createPulseAudio(runningOnPipeWire);

            if (!instance || runningOnPipeWire)
            {
                Fancy::fancy.logTime().message() << "Using the native pipewire backend" << std::endl;
                instance = createPipeWire();
            }
        }

        if (instance)
        {
            Globals::gSettings.audioBackend = std::dynamic_pointer_cast<PipeWire>(instance)
                                                  ? Enums::BackendType::PipeWire
                                                  : Enums::BackendType::PulseAudio;
            return instance;
        }

        Fancy::fancy.logTime().failure() << "Failed to create AudioBackend instance" << std::endl;
        Globals::gSettings.audioBackend = Enums::BackendType::None;
        return nullptr;
    }
} // namespace Soundux::Objects
#endif
