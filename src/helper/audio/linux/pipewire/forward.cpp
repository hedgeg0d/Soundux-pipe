#if defined(__linux__)
#include "forward.hpp"
#include <dlfcn.h>
#include <exception>
#include <fancy.hpp>
#include <stdexcept>

template <typename T> void loadFunc(void *so, T &function, const std::string &name)
{
    function = reinterpret_cast<T>(dlsym(so, name.c_str()));

    if (function == nullptr)
    {
        throw std::runtime_error("Failed to load function " + name);
    }
}

bool Soundux::PipeWireApi::setup()
{
    auto *libpipewire = dlopen("libpipewire-0.3.so.0", RTLD_LAZY);
    if (libpipewire)
    {
        try
        {
#define stringify(what) #what
#define load(name) loadFunc(libpipewire, name, stringify(pw_##name))
            load(init);
            load(context_new);
            load(proxy_destroy);
            load(properties_new);
            load(properties_set);
            load(context_connect);
            load(properties_setf);
            load(context_destroy);
            load(properties_free);
            load(core_disconnect);
            load(proxy_add_listener);

            //* Thread loop
            load(thread_loop_new);
            load(thread_loop_get_loop);
            load(thread_loop_start);
            load(thread_loop_stop);
            load(thread_loop_destroy);
            load(thread_loop_lock);
            load(thread_loop_unlock);
            load(thread_loop_wait);
            load(thread_loop_signal);

            //* Streams
            load(stream_new_simple);
            load(stream_destroy);
            load(stream_add_listener);
            load(stream_connect);
            load(stream_disconnect);
            load(stream_set_active);
            load(stream_dequeue_buffer);
            load(stream_queue_buffer);
            load(stream_get_time_n);
            load(stream_set_control);
            load(stream_flush);
            load(stream_update_params);

            //* Modules
            load(context_load_module);
            load(impl_module_destroy);
            return true;
        }
        catch (std::exception &e)
        {
            Fancy::fancy.logTime().failure() << "Loading Functions failed: " << e.what() << std::endl;
        }
    }

    Fancy::fancy.logTime().failure() << "Failed to load pipewire" << std::endl;
    return false;
}

#endif
