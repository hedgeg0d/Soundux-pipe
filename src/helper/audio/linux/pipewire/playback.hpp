#pragma once
#if defined(__linux__)
#include <cstdint>
#include <string>
#include <vector>

#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

namespace Soundux
{
    namespace Objects
    {
        struct PlayingSound;

        //* Plays sounds through their own pipewire stream, decoding is still done by miniaudio
        class PipeWirePlayback
        {
          public:
            struct Stream
            {
                pw_stream *stream = nullptr;
                PlayingSound *sound = nullptr;
                float volume = 1.F;
                bool volumeApplied = false;
                //* Monaural files are played as stereo, pipewire would only use the front left channel otherwise
                bool monaural = false;
                std::vector<std::uint8_t> scratch;
            };

          private:
            //* RAII wrapper around the thread loop lock
            class Lock
            {
                pw_thread_loop *loop;

              public:
                explicit Lock(pw_thread_loop *loop);
                ~Lock();
                Lock(const Lock &) = delete;
                Lock &operator=(const Lock &) = delete;
            };

            pw_thread_loop *loop = nullptr;
            pw_context *context = nullptr;
            pw_core *core = nullptr;
            spa_hook coreListener{};
            pw_core_events coreEvents{};
            bool ready = false;

            static void onProcess(void *data);
            static void onStateChanged(void *data, pw_stream_state old, pw_stream_state state, const char *error);

          public:
            bool setup();
            void destroy();
            bool isReady() const { return ready; }

            //* Creates a stream for the sound, target is the node the stream should be connected to (empty = default)
            Stream *start(PlayingSound *sound, const std::string &target, float volume);
            void stop(Stream *stream);
            void setVolume(Stream *stream, float volume);
            void setActive(Stream *stream, bool active);
        };
    } // namespace Objects
} // namespace Soundux
#endif
