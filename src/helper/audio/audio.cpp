#include "audio.hpp"
#include <core/global/globals.hpp>
#include <fancy.hpp>
#if defined(_WIN32)
#include <helper/misc/misc.hpp>
#endif

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace Soundux::Objects
{
#if defined(_WIN32)
    using Soundux::Helpers::widen;
#endif

#if defined(__linux__)
    constexpr const char *NULL_SINK_NAME = "soundux_sink";
#endif

    void Audio::setup()
    {
#if defined(__linux__)
        nullSink = std::nullopt;

        if (pipeWirePlayback)
        {
            pipeWirePlayback->destroy();
            pipeWirePlayback.reset();
        }
        nativePipeWire = false;

        if (Globals::gSettings.audioBackend == Enums::BackendType::PipeWire)
        {
            auto playback = std::make_unique<PipeWirePlayback>();
            if (playback->setup())
            {
                pipeWirePlayback = std::move(playback);
                nativePipeWire = true;

                defaultPlayback = AudioDevice{};
                defaultPlayback.name = "default";
                defaultPlayback.isDefault = true;

                nullSink = AudioDevice{};
                nullSink->name = NULL_SINK_NAME;
                nullSink->isDefault = false;

                Fancy::fancy.logTime().message() << "Using native pipewire playback" << std::endl;
                return;
            }

            Fancy::fancy.logTime().failure() << "Failed to setup native pipewire playback" << std::endl;
        }
#endif
        for (const auto &device : getAudioDevices())
        {
            if (device.isDefault)
            {
                defaultPlayback = device;
            }
#if defined(__linux__)
            if (device.name == NULL_SINK_NAME)
            {
                nullSink = device;
            }
#endif
        }
    }
    void Audio::destroy()
    {
        stopAll();

#if defined(__linux__)
        if (pipeWirePlayback)
        {
            pipeWirePlayback->destroy();
            pipeWirePlayback.reset();
        }
        nativePipeWire = false;
#endif
    }
    std::optional<PlayingSound> Audio::play(const Objects::Sound &sound,
                                            const std::optional<Objects::AudioDevice> &playbackDevice)
    {
        static std::atomic<std::uint64_t> id = 0;

        auto *decoder = new ma_decoder;
#if defined(_WIN32)
        auto res = ma_decoder_init_file_w(widen(sound.path).c_str(), nullptr, decoder);
#else
        auto res = ma_decoder_init_file(sound.path.c_str(), nullptr, decoder);
#endif

        if (res != MA_SUCCESS)
        {
            Fancy::fancy.logTime().failure() << "Failed to create decoder from file: " << sound.path << ", error: " >>
                res << std::endl;
            delete decoder;

            return std::nullopt;
        }

        ma_uint64 length_in_pcm_frames{};
        ma_decoder_get_length_in_pcm_frames(decoder, &length_in_pcm_frames);

        const auto volume = playbackDevice
                                ? (sound.remoteVolume ? *sound.remoteVolume : Globals::gSettings.remoteVolume)
                                : (sound.localVolume ? *sound.localVolume : Globals::gSettings.localVolume);

        auto pSound = std::make_shared<PlayingSound>();
        auto soundId = ++id;

        pSound->id = soundId;
        pSound->sound = sound;
        pSound->raw.decoder = decoder;
        pSound->length = length_in_pcm_frames;
        pSound->sampleRate = decoder->outputSampleRate;
        pSound->playbackDevice = playbackDevice ? *playbackDevice : defaultPlayback;
        pSound->lengthInMs = static_cast<std::uint64_t>(static_cast<double>(pSound->length) /
                                                        static_cast<double>(pSound->sampleRate) * 1000);

#if defined(__linux__)
        if (nativePipeWire)
        {
            auto *stream = pipeWirePlayback->start(pSound.get(), playbackDevice ? NULL_SINK_NAME : "",
                                                   static_cast<float>(volume) / 100.F);
            if (!stream)
            {
                Fancy::fancy.logTime().warning() << "Failed to play sound " << sound.path << std::endl;
                ma_decoder_uninit(decoder);
                delete decoder;

                return std::nullopt;
            }

            pSound->raw.pipeWireStream = stream;
            playingSounds->emplace(soundId, pSound);
            return *pSound;
        }
#endif

        auto *device = new ma_device;
        auto config = ma_device_config_init(ma_device_type_playback);

        config.dataCallback = data_callback;
        config.periodSizeInMilliseconds = 100;
        config.sampleRate = decoder->outputSampleRate;
        config.playback.format = decoder->outputFormat;
        config.playback.channels = decoder->outputChannels;
        config.pUserData = reinterpret_cast<void *>(static_cast<PlayingSound *>(pSound.get()));

        if (playbackDevice)
        {
            config.playback.pDeviceID = &playbackDevice->raw.id;
        }
        else
        {
            config.playback.pDeviceID = &defaultPlayback.raw.id;
        }

        if (ma_device_init(nullptr, &config, device) != MA_SUCCESS)
        {
            Fancy::fancy.logTime().failure() << "Failed to create device" << std::endl;
            ma_decoder_uninit(decoder);
            delete decoder;
            delete device;

            return std::nullopt;
        }

        device->masterVolumeFactor = static_cast<float>(volume) / 100.f;

        if (ma_device_start(device) != MA_SUCCESS)
        {
            ma_device_uninit(device);
            ma_decoder_uninit(decoder);

            delete device;
            delete decoder;

            Fancy::fancy.logTime().warning() << "Failed to play sound " << sound.path << std::endl;

            return std::nullopt;
        }

        pSound->raw.device = device;

        playingSounds->emplace(soundId, pSound);
        return *pSound;
    }
    void Audio::stopAll()
    {
        auto scoped = playingSounds.scoped();
        while (!scoped->empty())
        {
            auto &sound = scoped->begin()->second;
            destroyPlayback(*sound);

            scoped->erase(sound->id);
        }
    }
    bool Audio::stop(const std::uint32_t &soundId)
    {
        auto scoped = playingSounds.scoped();
        if (scoped->find(soundId) != scoped->end())
        {
            auto &sound = scoped->at(soundId);
            destroyPlayback(*sound);

            scoped->erase(sound->id);
            return true;
        }

        Fancy::fancy.logTime().warning() << "Failed to stop sound with id " << soundId << ", sound does not exist"
                                         << std::endl;
        return false;
    }
    std::optional<PlayingSound> Audio::pause(const std::uint32_t &soundId)
    {
        auto scoped = playingSounds.scoped();
        if (scoped->find(soundId) != scoped->end())
        {
            auto &sound = scoped->at(soundId);

            if (!sound->paused)
            {
                setPlaybackActive(*sound, false);
                sound->paused = true;
            }

            return *sound;
        }

        Fancy::fancy.logTime().warning() << "Failed to pause sound with id " << soundId << ", sound does not exist"
                                         << std::endl;
        return std::nullopt;
    }
    std::optional<PlayingSound> Audio::repeat(const std::uint32_t &soundId, bool shouldRepeat)
    {
        auto scoped = playingSounds.scoped();
        if (scoped->find(soundId) != scoped->end())
        {
            auto &sound = scoped->at(soundId);
            sound->repeat = shouldRepeat;

            return *sound;
        }

        Fancy::fancy.logTime().warning() << "Failed to set repeat for sound with id " << soundId
                                         << ", sound does not exist" << std::endl;
        return std::nullopt;
    }
    std::optional<PlayingSound> Audio::resume(const std::uint32_t &soundId)
    {
        auto scoped = playingSounds.scoped();
        if (scoped->find(soundId) != scoped->end())
        {
            auto &sound = scoped->at(soundId);

            if (sound->paused)
            {
                setPlaybackActive(*sound, true);
                sound->paused = false;
            }

            return *sound;
        }

        Fancy::fancy.logTime().warning() << "Failed to resume sound with id " << soundId << ", sound does not exist "
                                         << std::endl;
        return std::nullopt;
    }
    void Audio::onFinished(PlayingSound sound)
    {
        auto scoped = playingSounds.scoped();
        if (scoped->find(sound.id) != scoped->end())
        {
            destroyPlayback(sound);

            Globals::gGui->onSoundFinished(sound);
            scoped->erase(sound.id);
        }
        else
        {
            Fancy::fancy.logTime().warning() << "Sound finished but is not playing" << std::endl;
        }
    }
    void Audio::onSoundProgressed(PlayingSound *sound, std::uint64_t frames)
    {
        sound->readFrames += frames;
        sound->buffer += frames;

        if (sound->buffer > (sound->sampleRate / 2))
        {
            sound->readInMs = static_cast<std::uint64_t>(
                (static_cast<double>(sound->readFrames) / static_cast<double>(sound->length)) *
                static_cast<double>(sound->lengthInMs));

            Globals::gGui->onSoundProgressed(*sound);

            sound->buffer = 0;
        }
    }
    void Audio::onSoundSeeked(PlayingSound *sound, std::uint64_t frame)
    {
        sound->shouldSeek = false;
        sound->readFrames = frame;
        sound->readInMs = static_cast<std::uint64_t>((static_cast<double>(frame) / static_cast<double>(sound->length)) *
                                                     static_cast<double>(sound->lengthInMs));
    }
    std::optional<PlayingSound> Audio::seek(const std::uint32_t &soundId, std::uint64_t position)
    {
        auto scoped = playingSounds.scoped();
        if (scoped->find(soundId) != scoped->end())
        {
            auto &sound = scoped->at(soundId);
            sound->seekTo =
                static_cast<std::uint64_t>((static_cast<double>(position) / static_cast<double>(sound->lengthInMs)) *
                                           static_cast<double>(sound->length));
            sound->shouldSeek = true;

            auto rtn = *sound;
            rtn.readFrames = rtn.seekTo;
            rtn.readInMs =
                static_cast<std::uint64_t>((static_cast<double>(rtn.seekTo) / static_cast<double>(rtn.length)) *
                                           static_cast<double>(rtn.lengthInMs));

            return rtn;
        }

        Fancy::fancy.logTime().warning() << "Failed to seek sound with id " << soundId << ", sound does not exist"
                                         << std::endl;
        return std::nullopt;
    }
    void Audio::destroyPlayback(PlayingSound &sound)
    {
#if defined(__linux__)
        if (nativePipeWire)
        {
            if (sound.raw.pipeWireStream)
            {
                pipeWirePlayback->stop(static_cast<PipeWirePlayback::Stream *>(sound.raw.pipeWireStream.load()));
                sound.raw.pipeWireStream = nullptr;
            }

            if (sound.raw.decoder)
            {
                ma_decoder_uninit(sound.raw.decoder);
                sound.raw.decoder = nullptr;
            }
            return;
        }
#endif
        if (sound.raw.device)
        {
            ma_device_uninit(sound.raw.device);
            sound.raw.device = nullptr;
        }

        if (sound.raw.decoder)
        {
            ma_decoder_uninit(sound.raw.decoder);
            sound.raw.decoder = nullptr;
        }
    }
    void Audio::setPlaybackActive(PlayingSound &sound, bool active)
    {
#if defined(__linux__)
        if (nativePipeWire)
        {
            if (sound.raw.pipeWireStream)
            {
                pipeWirePlayback->setActive(static_cast<PipeWirePlayback::Stream *>(sound.raw.pipeWireStream.load()),
                                            active);
            }
            return;
        }
#endif
        if (!sound.raw.device)
        {
            return;
        }

        if (active)
        {
            if (ma_device_get_state(sound.raw.device) == ma_device_state_stopped)
            {
                ma_device_start(sound.raw.device);
            }
        }
        else if (ma_device_get_state(sound.raw.device) == ma_device_state_started)
        {
            ma_device_stop(sound.raw.device);
        }
    }
    void Audio::setVolume(const std::uint32_t &soundId, float volume)
    {
        auto scoped = playingSounds.scoped();
        if (scoped->find(soundId) == scoped->end())
        {
            return;
        }

        auto &sound = scoped->at(soundId);

#if defined(__linux__)
        if (nativePipeWire)
        {
            if (sound->raw.pipeWireStream)
            {
                pipeWirePlayback->setVolume(static_cast<PipeWirePlayback::Stream *>(sound->raw.pipeWireStream.load()),
                                            volume);
            }
            return;
        }
#endif
        if (sound->raw.device)
        {
            sound->raw.device.load()->masterVolumeFactor = volume;
        }
    }
    std::uint32_t Audio::pump(PlayingSound *sound, void *output, std::uint32_t frameCount)
    {
        if (!sound || !sound->raw.decoder)
        {
            return 0;
        }

        ma_uint64 readFrames{};
        ma_decoder_read_pcm_frames(sound->raw.decoder, output, frameCount, &readFrames);

        if (sound->shouldSeek)
        {
            ma_decoder_seek_to_pcm_frame(sound->raw.decoder, sound->seekTo);
            Globals::gAudio.onSoundSeeked(sound, sound->seekTo);
        }
        if (sound->playbackDevice.isDefault && readFrames > 0)
        {
            Globals::gAudio.onSoundProgressed(sound, readFrames);
        }

        if (readFrames <= 0 && sound->repeat)
        {
            ma_decoder_seek_to_pcm_frame(sound->raw.decoder, 0);
            Globals::gAudio.onSoundSeeked(sound, 0);
        }

        return static_cast<std::uint32_t>(readFrames);
    }
    void Audio::data_callback(ma_device *device, void *output, [[maybe_unused]] const void *input,
                              std::uint32_t frameCount)
    {
        auto *sound = reinterpret_cast<PlayingSound *>(device->pUserData);
        if (!sound || !sound->raw.decoder)
        {
            return;
        }

        if (Globals::gAudio.pump(sound, output, frameCount) == 0 && !sound->repeat)
        {
            Globals::gQueue.push_unique(reinterpret_cast<std::uintptr_t>(device),
                                        [sound = *sound] { Globals::gAudio.onFinished(sound); });
        }
    }
    std::vector<AudioDevice> Audio::getAudioDevices()
    {
        std::string defaultName;
        {
            ma_device device;
            ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
            ma_device_init(nullptr, &deviceConfig, &device);

            defaultName = device.playback.name;

            ma_device_uninit(&device);
        }

        ma_context context;

        if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS)
        {
            Fancy::fancy.logTime().failure() << "Failed to initialize context" << std::endl;
            return {};
        }

        ma_device_info *pPlayBackDeviceInfos{};
        ma_uint32 deviceCount{};

        ma_result result = ma_context_get_devices(&context, &pPlayBackDeviceInfos, &deviceCount, nullptr, nullptr);
        if (result != MA_SUCCESS)
        {
            Fancy::fancy.logTime().failure() << "Failed to get playback devices!" << std::endl;
            return {};
        }

        std::vector<AudioDevice> playBackDevices;
        for (unsigned int i = 0; deviceCount > i; i++)
        {
            auto &rawDevice = pPlayBackDeviceInfos[i];

            AudioDevice device;
            device.raw = rawDevice;
            device.name = rawDevice.name;
            device.isDefault = rawDevice.name == defaultName;

            playBackDevices.emplace_back(device);
        }

        ma_context_uninit(&context);

        for (auto it = playBackDevices.begin(); it != playBackDevices.end(); it++)
        {
            if (it->name.find("VB-Audio") != std::string::npos)
            {
                if (it != playBackDevices.begin())
                {
                    std::iter_swap(playBackDevices.begin(), it);
                }
            }
        }

        return playBackDevices;
    }
#if defined(_WIN32)
    std::optional<AudioDevice> Audio::getAudioDevice(const std::string &name)
    {
        for (const auto &device : getAudioDevices())
        {
            if (device.name == name)
            {
                return device;
            }
        }
        return std::nullopt;
    }
#endif
    std::vector<PlayingSound> Audio::getPlayingSounds()
    {
        auto scoped = playingSounds.scoped();

        std::vector<PlayingSound> rtn;
        for (const auto &sound : *scoped)
        {
            rtn.emplace_back(*sound.second);
        }

        return rtn;
    }
    PlayingSound::PlayingSound(const PlayingSound &other)
    {
        if (&other == this)
        {
            return;
        }

        length = other.length;
        lengthInMs = other.lengthInMs;
        readFrames = other.readFrames;
        sampleRate = other.sampleRate;

        id = other.id;
        sound = other.sound;
        buffer = other.buffer;

        seekTo.store(other.seekTo);
        paused.store(other.paused);
        repeat.store(other.repeat);
        readInMs.store(other.readInMs);
        shouldSeek.store(other.shouldSeek);

        raw.device.store(other.raw.device);
        raw.decoder.store(other.raw.decoder);
#if defined(__linux__)
        raw.pipeWireStream.store(other.raw.pipeWireStream);
#endif
        playbackDevice = other.playbackDevice;
    }
    PlayingSound &PlayingSound::operator=(const PlayingSound &other)
    {
        if (&other == this)
        {
            return *this;
        }

        length = other.length;
        lengthInMs = other.lengthInMs;
        readFrames = other.readFrames;
        sampleRate = other.sampleRate;

        id = other.id;
        sound = other.sound;
        buffer = other.buffer;

        seekTo.store(other.seekTo);
        paused.store(other.paused);
        repeat.store(other.repeat);
        readInMs.store(other.readInMs);
        shouldSeek.store(other.shouldSeek);

        raw.device.store(other.raw.device);
        raw.decoder.store(other.raw.decoder);
#if defined(__linux__)
        raw.pipeWireStream.store(other.raw.pipeWireStream);
#endif
        playbackDevice = other.playbackDevice;

        return *this;
    }
} // namespace Soundux::Objects