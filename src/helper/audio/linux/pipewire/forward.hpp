#pragma once
#if defined(__linux__)
#include <cstdint>
#include <pipewire/impl-module.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <pipewire/thread-loop.h>

namespace Soundux
{
    namespace PipeWireApi
    {
        bool setup();

        //* We declare function pointers here so that we can use dlsym to assign them later.
        //* Functions that are defined inline inside the pipewire headers are called directly and are not listed here.
        inline pw_core *(*context_connect)(pw_context *, pw_properties *, std::size_t);
        inline pw_context *(*context_new)(pw_loop *, pw_properties *, std::size_t);

        inline void (*proxy_add_listener)(pw_proxy *, spa_hook *, pw_proxy_events *, void *);
        inline int (*properties_setf)(pw_properties *, const char *, const char *, ...);
        inline int (*properties_set)(pw_properties *, const char *, const char *);
        inline pw_properties *(*properties_new)(const char *, ...);
        inline void (*properties_free)(pw_properties *);
        inline void (*context_destroy)(pw_context *);
        inline int (*core_disconnect)(pw_core *);
        inline void (*proxy_destroy)(pw_proxy *);
        inline void (*init)(int *, char **);

        //* Thread loop
        inline pw_thread_loop *(*thread_loop_new)(const char *, const spa_dict *);
        inline pw_loop *(*thread_loop_get_loop)(pw_thread_loop *);
        inline int (*thread_loop_start)(pw_thread_loop *);
        inline int (*thread_loop_stop)(pw_thread_loop *);
        inline void (*thread_loop_destroy)(pw_thread_loop *);
        inline int (*thread_loop_lock)(pw_thread_loop *);
        inline int (*thread_loop_unlock)(pw_thread_loop *);
        inline int (*thread_loop_wait)(pw_thread_loop *, const struct timespec *);
        inline int (*thread_loop_signal)(pw_thread_loop *, bool);

        //* Streams
        inline pw_stream *(*stream_new_simple)(pw_loop *, const char *, pw_properties *, const pw_stream_events *,
                                               void *);
        inline void (*stream_destroy)(pw_stream *);
        inline int (*stream_add_listener)(pw_stream *, spa_hook *, const pw_stream_events *, void *);
        inline int (*stream_connect)(pw_stream *, pw_direction, std::uint32_t, pw_stream_flags, const spa_pod *const *,
                                     std::uint32_t);
        inline int (*stream_disconnect)(pw_stream *);
        inline int (*stream_set_active)(pw_stream *, bool);
        inline pw_buffer *(*stream_dequeue_buffer)(pw_stream *);
        inline int (*stream_queue_buffer)(pw_stream *, pw_buffer *);
        inline int (*stream_get_time_n)(pw_stream *, pw_time *, std::size_t);
        inline int (*stream_set_control)(pw_stream *, std::uint32_t, std::uint32_t, float *, ...);
        inline int (*stream_flush)(pw_stream *, bool);
        inline int (*stream_update_params)(pw_stream *, const spa_pod *const *, std::uint32_t);

        //* Modules
        inline pw_impl_module *(*context_load_module)(pw_context *, const char *, const char *, pw_properties *);
        inline void (*impl_module_destroy)(pw_impl_module *);
    } // namespace PipeWireApi
} // namespace Soundux
#endif
