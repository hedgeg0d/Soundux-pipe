#if defined(__linux__)
#include "playback.hpp"
#include "forward.hpp"
#include <algorithm>
#include <core/global/globals.hpp>
#include <fancy.hpp>
#include <helper/audio/audio.hpp>

namespace Soundux::Objects
{
    namespace
    {
        spa_audio_format toSpaFormat(ma_format format)
        {
            switch (format)
            {
            case ma_format_u8:
                return SPA_AUDIO_FORMAT_U8;
            case ma_format_s16:
                return SPA_AUDIO_FORMAT_S16;
            case ma_format_s24:
                return SPA_AUDIO_FORMAT_S24;
            case ma_format_s32:
                return SPA_AUDIO_FORMAT_S32;
            case ma_format_f32:
            default:
                return SPA_AUDIO_FORMAT_F32;
            }
        }
    } // namespace

    PipeWirePlayback::Lock::Lock(pw_thread_loop *loop) : loop(loop)
    {
        if (loop)
        {
            PipeWireApi::thread_loop_lock(loop);
        }
    }
    PipeWirePlayback::Lock::~Lock()
    {
        if (loop)
        {
            PipeWireApi::thread_loop_unlock(loop);
        }
    }

    bool PipeWirePlayback::setup()
    {
        if (!PipeWireApi::setup())
        {
            return false;
        }

        PipeWireApi::init(nullptr, nullptr);

        loop = PipeWireApi::thread_loop_new("soundux-streams", nullptr);
        if (!loop)
        {
            Fancy::fancy.logTime().failure() << "Failed to create stream loop" << std::endl;
            return false;
        }

        PipeWireApi::thread_loop_start(loop);

        Lock lock(loop);

        context = PipeWireApi::context_new(PipeWireApi::thread_loop_get_loop(loop), nullptr, 0);
        if (!context)
        {
            Fancy::fancy.logTime().failure() << "Failed to create stream context" << std::endl;
            return false;
        }

        core = PipeWireApi::context_connect(context, nullptr, 0);
        if (!core)
        {
            Fancy::fancy.logTime().failure() << "Failed to connect stream context" << std::endl;
            return false;
        }

        coreEvents.version = PW_VERSION_CORE_EVENTS;
        coreEvents.error = []([[maybe_unused]] void *data, std::uint32_t id, int seq, int res,
                              const char *message) {
            Fancy::fancy.logTime() << "Stream core error - id " << id << " seq " << seq << " res " << res << ": "
                                   << message << std::endl;
        };
        pw_core_add_listener(core, &coreListener, &coreEvents, this);

        ready = true;
        return true;
    }

    void PipeWirePlayback::destroy()
    {
        if (!loop)
        {
            return;
        }

        {
            Lock lock(loop);
            ready = false;

            if (core)
            {
                PipeWireApi::core_disconnect(core);
                core = nullptr;
            }

            if (context)
            {
                PipeWireApi::context_destroy(context);
                context = nullptr;
            }
        }

        PipeWireApi::thread_loop_stop(loop);
        PipeWireApi::thread_loop_destroy(loop);
        loop = nullptr;
    }

    PipeWirePlayback::Stream *PipeWirePlayback::start(PlayingSound *sound, const std::string &target, float volume)
    {
        if (!ready || !sound || !sound->raw.decoder)
        {
            return nullptr;
        }

        Lock lock(loop);

        static const pw_stream_events events = [] {
            pw_stream_events value{};
            value.version = PW_VERSION_STREAM_EVENTS;
            value.process = onProcess;
            value.state_changed = onStateChanged;
            return value;
        }();

        auto *decoder = sound->raw.decoder.load();
        auto *state = new Stream();
        state->sound = sound;
        state->volume = volume;
        state->monaural = decoder->outputChannels == 1;

        const auto streamChannels = state->monaural ? 2 : decoder->outputChannels;

        pw_properties *props = PipeWireApi::properties_new(nullptr, nullptr);
        PipeWireApi::properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
        PipeWireApi::properties_set(props, PW_KEY_MEDIA_CATEGORY, "Playback");
        PipeWireApi::properties_set(props, PW_KEY_MEDIA_ROLE, "Music");
        PipeWireApi::properties_set(props, PW_KEY_APP_NAME, "Soundux");
        if (!target.empty())
        {
            PipeWireApi::properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());
            //* node.target is deprecated but needed for pipewire < 0.3.44
            PipeWireApi::properties_set(props, "node.target", target.c_str());
        }

        state->stream = PipeWireApi::stream_new_simple(PipeWireApi::thread_loop_get_loop(loop), "Soundux", props,
                                                       &events, state);
        if (!state->stream)
        {
            Fancy::fancy.logTime().warning() << "Failed to create stream" << std::endl;
            delete state;
            return nullptr;
        }

        char buffer[1024];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, buffer, sizeof(buffer));

        spa_pod_frame frame[1];
        spa_pod_builder_push_object(&builder, &frame[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio), SPA_FORMAT_mediaSubtype,
                            SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_AUDIO_format,
                            SPA_POD_Id(toSpaFormat(decoder->outputFormat)), SPA_FORMAT_AUDIO_rate,
                            SPA_POD_Int(static_cast<int>(decoder->outputSampleRate)), SPA_FORMAT_AUDIO_channels,
                            SPA_POD_Int(streamChannels), 0);
        const spa_pod *params[1] = {static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &frame[0]))};

        if (PipeWireApi::stream_connect(state->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                                     PW_STREAM_FLAG_MAP_BUFFERS),
                                        params, 1) < 0)
        {
            Fancy::fancy.logTime().warning() << "Failed to connect stream" << std::endl;
            PipeWireApi::stream_destroy(state->stream);
            delete state;
            return nullptr;
        }

        setVolume(state, volume);

        return state;
    }

    void PipeWirePlayback::stop(Stream *stream)
    {
        if (!stream)
        {
            return;
        }

        Lock lock(loop);

        if (stream->stream)
        {
            PipeWireApi::stream_destroy(stream->stream);
            stream->stream = nullptr;
        }

        delete stream;
    }

    void PipeWirePlayback::setVolume(Stream *stream, float volume)
    {
        if (!stream || !stream->stream)
        {
            return;
        }

        Lock lock(loop);

        stream->volume = volume;
        PipeWireApi::stream_set_control(stream->stream, SPA_PROP_volume, 1, &stream->volume);
    }

    void PipeWirePlayback::setActive(Stream *stream, bool active)
    {
        if (!stream || !stream->stream)
        {
            return;
        }

        Lock lock(loop);
        PipeWireApi::stream_set_active(stream->stream, active);
    }

    void PipeWirePlayback::onProcess(void *data)
    {
        auto *state = static_cast<Stream *>(data);
        if (!state || !state->stream || !state->sound)
        {
            return;
        }

        auto *buffer = PipeWireApi::stream_dequeue_buffer(state->stream);
        if (!buffer || !buffer->buffer || buffer->buffer->n_datas == 0)
        {
            return;
        }

        auto *decoder = state->sound->raw.decoder.load();
        if (!decoder)
        {
            PipeWireApi::stream_queue_buffer(state->stream, buffer);
            return;
        }

        auto *spaBuffer = buffer->buffer;
        auto *spaData = &spaBuffer->datas[0];

        const auto sampleSize = ma_get_bytes_per_sample(decoder->outputFormat);
        const auto streamChannels = state->monaural ? 2U : decoder->outputChannels;
        const auto bytesPerFrame = sampleSize * streamChannels;

        auto frames = static_cast<std::uint32_t>(spaData->maxsize / bytesPerFrame);
        if (buffer->requested > 0)
        {
            frames = std::min(frames, static_cast<std::uint32_t>(buffer->requested));
        }

        std::uint32_t read = 0;
        if (state->monaural)
        {
            //* Decode into a scratch buffer and spread the samples over both channels
            if (state->scratch.size() < frames * sampleSize)
            {
                state->scratch.resize(frames * sampleSize);
            }

            read = Globals::gAudio.pump(state->sound, state->scratch.data(), frames);

            auto *output = static_cast<std::uint8_t *>(spaData->data);
            for (std::uint32_t frame = 0; frame < read; frame++)
            {
                const auto *sample = state->scratch.data() + (frame * sampleSize);
                std::memcpy(output + (frame * bytesPerFrame), sample, sampleSize);
                std::memcpy(output + (frame * bytesPerFrame) + sampleSize, sample, sampleSize);
            }
        }
        else
        {
            read = Globals::gAudio.pump(state->sound, spaData->data, frames);
        }

        spaData->chunk->offset = 0;
        spaData->chunk->stride = static_cast<std::int32_t>(bytesPerFrame);
        spaData->chunk->size = read * bytesPerFrame;

        PipeWireApi::stream_queue_buffer(state->stream, buffer);

        if (read == 0 && !state->sound->repeat)
        {
            Globals::gQueue.push_unique(
                reinterpret_cast<std::uintptr_t>(state->stream),
                [sound = *state->sound] { Globals::gAudio.onFinished(sound); });
        }
    }

    void PipeWirePlayback::onStateChanged(void *data, [[maybe_unused]] pw_stream_state old, pw_stream_state state,
                                          const char *error)
    {
        auto *stream = static_cast<Stream *>(data);
        if (!stream)
        {
            return;
        }

        if (state == PW_STREAM_STATE_ERROR)
        {
            Fancy::fancy.logTime().warning() << "Stream error: " << (error ? error : "unknown") << std::endl;
            return;
        }

        if ((state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) && !stream->volumeApplied &&
            stream->stream)
        {
            stream->volumeApplied = true;
            PipeWireApi::stream_set_control(stream->stream, SPA_PROP_volume, 1, &stream->volume);
        }
    }
} // namespace Soundux::Objects
#endif
