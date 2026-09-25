#if defined(__linux__)
#include <thread>
#include "pipewire.hpp"
#include "forward.hpp"
#include <chrono>
#include <core/global/globals.hpp>
#include <cstring>
#include <ctime>
#include <fancy.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

namespace Soundux::Objects
{
    namespace
    {
        constexpr const char *SINK_NAME = "soundux_sink";
        constexpr const char *MIC_LOOPBACK_NAME = "soundux_mic";
        constexpr const char *VIRTUAL_SOURCE_NAME = "soundux_source";
        constexpr const char *INTERNAL_PREFIX = "soundux";

        int parseVersion(const std::string &version)
        {
            std::istringstream stream(version);
            std::string part;
            std::vector<int> parts;

            while (std::getline(stream, part, '.'))
            {
                try
                {
                    parts.emplace_back(std::stoi(part));
                }
                catch (const std::exception &)
                {
                    break;
                }
            }

            if (parts.size() >= 3)
            {
                return (parts[0] * 10000) + (parts[1] * 100) + parts[2];
            }

            return 0;
        }
    } // namespace

    Side Port::sideOf(const std::string &name)
    {
        if (name.empty())
        {
            return Side::UNDEFINED;
        }
        if (name.find("MONO") != std::string::npos)
        {
            return Side::MONO;
        }

        const auto position = name.rfind('_');
        const auto suffix = position == std::string::npos ? name : name.substr(position + 1);

        if (suffix == "FL" || suffix == "L" || suffix == "1")
        {
            return Side::LEFT;
        }
        if (suffix == "FR" || suffix == "R" || suffix == "2")
        {
            return Side::RIGHT;
        }

        return Side::UNDEFINED;
    }

    PipeWire::Lock::Lock(pw_thread_loop *loop) : loop(loop)
    {
        if (loop)
        {
            PipeWireApi::thread_loop_lock(loop);
        }
    }
    PipeWire::Lock::~Lock()
    {
        if (loop)
        {
            PipeWireApi::thread_loop_unlock(loop);
        }
    }

    void PipeWire::sync()
    {
        if (!core)
        {
            return;
        }

        //* Waits until all pending requests have been processed by the server.
        //* The loop keeps running in its own thread, so we only need to wait for the done event.
        int pending = 0;
        spa_hook listener{};
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.done = [](void *data, std::uint32_t id, int seq) {
            auto *info = static_cast<std::pair<PipeWire *, int *> *>(data);
            if (info && id == PW_ID_CORE && seq == *info->second)
            {
                *info->second = -1;
                PipeWireApi::thread_loop_signal(info->first->loop, false);
            }
        };
        events.error = [](void *data, std::uint32_t id, int seq, int res, const char *message) {
            auto *info = static_cast<std::pair<PipeWire *, int *> *>(data);
            if (info && id == PW_ID_CORE && seq == *info->second)
            {
                Fancy::fancy.logTime() << "Core failure - seq " << seq << " - res " << res << ": " << message
                                       << std::endl;
                *info->second = -1;
                PipeWireApi::thread_loop_signal(info->first->loop, false);
            }
        };

        auto data = std::make_pair(this, &pending);
        pw_core_add_listener(core, &listener, &events, &data);

        //* Never wait forever, the server might be gone
        const struct timespec timeout = {1, 0};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

        pending = pw_core_sync(core, PW_ID_CORE, 0);
        while (pending != -1)
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                Fancy::fancy.logTime().warning() << "Timed out while syncing with pipewire" << std::endl;
                break;
            }

            PipeWireApi::thread_loop_wait(loop, &timeout);
        }

        spa_hook_remove(&listener);
    }

    void PipeWire::onCoreInfo(const pw_core_info *info)
    {
        if (!info || !info->name || !info->version || version)
        {
            return;
        }

        Fancy::fancy.logTime().message() << "Connected to PipeWire (" << info->name << ") on version " << info->version
                                         << std::endl;

        version = parseVersion(info->version);
        if (version && version < 326)
        {
            Fancy::fancy.logTime().warning() << "Your PipeWire version is below the minimum required (0.3.26), "
                                                "you may experience bugs or crashes"
                                             << std::endl;
        }
    }

    int PipeWire::onMetadataProperty(void *data, [[maybe_unused]] std::uint32_t id, const char *key,
                                     [[maybe_unused]] const char *type, const char *value)
    {
        auto *thiz = static_cast<PipeWire *>(data);
        if (!thiz || !key)
        {
            return 0;
        }

        //* The configured source is the one that wireplumber persists, writing it is the same as `wpctl set-default`
        if (std::strcmp(key, "default.configured.audio.source") == 0)
        {
            thiz->configuredSourceValue = value ? value : "";
            return 0;
        }

        if (std::strcmp(key, "default.audio.source") != 0)
        {
            return 0;
        }

        if (!value)
        {
            thiz->defaultSourceValue.clear();
            return 0;
        }

        thiz->defaultSourceValue = value;

        auto parsed = nlohmann::json::parse(value, nullptr, false);
        if (parsed.is_discarded() || !parsed.count("name"))
        {
            return 0;
        }

        auto name = parsed["name"].get<std::string>();
        if (name == VIRTUAL_SOURCE_NAME)
        {
            //* That is our own virtual source, keep the original microphone around
            return 0;
        }

        if (thiz->defaultSource != name)
        {
            thiz->defaultSource = name;
            Fancy::fancy.logTime().message() << "Found default device: " << name << std::endl;
        }

        return 0;
    }

    void PipeWire::onGlobalAdded(void *data, std::uint32_t id, [[maybe_unused]] std::uint32_t perms, const char *type,
                                 [[maybe_unused]] std::uint32_t globalVersion, const spa_dict *props)
    {
        auto *thiz = static_cast<PipeWire *>(data);
        if (!thiz || !type || !props)
        {
            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0)
        {
            if (thiz->defaultMetadata)
            {
                return;
            }

            const auto *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            if (!name || std::strcmp(name, "default") != 0)
            {
                return;
            }

            auto *bound =
                reinterpret_cast<pw_metadata *>(pw_registry_bind(thiz->registry, id, type, PW_VERSION_METADATA, 0));
            if (!bound)
            {
                return;
            }

            thiz->metadataEvents.version = PW_VERSION_METADATA_EVENTS;
            thiz->metadataEvents.property = onMetadataProperty;
            pw_metadata_add_listener(bound, &thiz->metadataListener, &thiz->metadataEvents, thiz);
            thiz->defaultMetadata = bound;
            thiz->metadataId = id;
            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0)
        {
            //* Only the id is tracked here, the properties are fetched on demand (see applicationOf)
            thiz->clients->emplace(id, Client{});
            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
        {
            Node node;
            node.id = id;

            if (const auto *rawName = spa_dict_lookup(props, PW_KEY_NODE_NAME); rawName)
            {
                node.rawName = rawName;
            }
            if (const auto *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS); mediaClass)
            {
                node.mediaClass = mediaClass;
            }
            //* Yes this is swapped. (For compatibility reasons)
            if (const auto *appName = spa_dict_lookup(props, "application.name"); appName)
            {
                node.name = appName;
            }
            if (const auto *binary = spa_dict_lookup(props, "application.process.binary"); binary)
            {
                node.applicationBinary = binary;
            }
            if (const auto *clientId = spa_dict_lookup(props, "client.id"); clientId)
            {
                try
                {
                    node.clientId = static_cast<std::uint32_t>(std::stoul(clientId));
                }
                catch (const std::exception &)
                {
                }
            }
            if (const auto *pid = spa_dict_lookup(props, "application.process.id"); pid)
            {
                try
                {
                    node.pid = static_cast<std::uint32_t>(std::stoul(pid));
                }
                catch (const std::exception &)
                {
                }
            }
            if (spa_dict_lookup(props, "stream.monitor"))
            {
                node.isMonitor = true;
            }

            thiz->nodes->emplace(id, node);

            //* The microphone might show up later, for example when a headset gets connected
            if (!thiz->micLoopback && node.mediaClass == "Audio/Source" && !node.isMonitor && !isInternalNode(node))
            {
                //* Loading a module from the loop callback deadlocks the loop, do it elsewhere
                std::thread([thiz] {
                    Lock lock(thiz->loop);
                    thiz->createMicLoopback();
                }).detach();
            }

            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0)
        {
            Port port;
            port.id = id;

            if (const auto *nodeId = spa_dict_lookup(props, PW_KEY_NODE_ID); nodeId)
            {
                try
                {
                    port.parentNode = static_cast<std::uint32_t>(std::stoul(nodeId));
                }
                catch (const std::exception &)
                {
                }
            }
            if (const auto *portName = spa_dict_lookup(props, PW_KEY_PORT_NAME); portName)
            {
                port.name = portName;
                port.side = Port::sideOf(port.name);
            }
            if (const auto *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION); direction)
            {
                port.direction = std::strcmp(direction, "out") == 0 ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
            }

            thiz->ports->emplace(id, port);
            return;
        }
    }

    void PipeWire::onGlobalRemoved(void *data, std::uint32_t id)
    {
        auto *thiz = static_cast<PipeWire *>(data);
        if (!thiz)
        {
            return;
        }

        if (thiz->metadataId == id && thiz->defaultMetadata)
        {
            spa_hook_remove(&thiz->metadataListener);
            thiz->defaultMetadata = nullptr;
            thiz->metadataId = 0;
            thiz->defaultSourceValue.clear();
        }

        {
            auto scopedNodes = thiz->nodes.scoped();
            if (auto node = scopedNodes->find(id); node != scopedNodes->end())
            {
                if (node->second.rawName == SINK_NAME)
                {
                    //* The sink is gone for good, the proxy was already removed
                    thiz->sinkProxy = nullptr;
                }
                scopedNodes->erase(node);
            }
        }

        thiz->ports->erase(id);
        thiz->clients->erase(id);

        if (auto link = thiz->linkProxies.find(id); link != thiz->linkProxies.end())
        {
            //* The link was removed elsewhere, the proxy is already gone
            thiz->linkProxies.erase(link);
        }
    }

    bool PipeWire::createNullSink()
    {
        pw_properties *props = PipeWireApi::properties_new(nullptr, nullptr);

        PipeWireApi::properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
        PipeWireApi::properties_set(props, PW_KEY_NODE_NAME, SINK_NAME);
        PipeWireApi::properties_set(props, PW_KEY_NODE_DESCRIPTION, "Soundux");
        PipeWireApi::properties_set(props, PW_KEY_FACTORY_NAME, "support.null-audio-sink");

        auto *proxy = reinterpret_cast<pw_proxy *>(
            pw_core_create_object(core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0));

        if (!proxy)
        {
            Fancy::fancy.logTime().failure() << "Failed to create null sink node" << std::endl;
            PipeWireApi::properties_free(props);
            return false;
        }

        spa_hook listener{};
        bool success = false;

        pw_proxy_events events{};
        events.version = PW_VERSION_PROXY_EVENTS;
        events.bound = [](void *data, [[maybe_unused]] std::uint32_t id) { *static_cast<bool *>(data) = true; };
        events.error = [](void *data, [[maybe_unused]] int seq, [[maybe_unused]] int res, const char *message) {
            Fancy::fancy.logTime().failure() << "Failed to create null sink: " << message << std::endl;
            *static_cast<bool *>(data) = false;
        };

        PipeWireApi::proxy_add_listener(proxy, &listener, &events, &success);
        sync();
        spa_hook_remove(&listener);
        PipeWireApi::properties_free(props);

        if (!success)
        {
            PipeWireApi::proxy_destroy(proxy);
            return false;
        }

        sinkProxy = proxy;
        return true;
    }

    bool PipeWire::createMicLoopback()
    {
        if (micLoopback)
        {
            return true;
        }

        //* The capture side has to be pinned to the actual microphone, otherwise it would follow the default
        //* source, which is us once the user enabled useAsDefault, resulting in a feedback loop.
        auto source = defaultSource;
        if (source.empty() || source == VIRTUAL_SOURCE_NAME)
        {
            source = firstRealSource();
        }

        if (source.empty())
        {
            Fancy::fancy.logTime().warning()
                << "Waiting for a microphone, the loopback is created once one shows up" << std::endl;
            return false;
        }

        const std::string args =
            "capture.props = { node.name = \"" + std::string(MIC_LOOPBACK_NAME) +
            "\" media.class = \"Stream/Input/Audio\" target.object = \"" + source + "\" node.target = \"" + source +
            "\" } "
            "playback.props = { node.name = \"soundux_mic_playback\" media.class = "
            "\"Stream/Output/Audio\" target.object = \"" +
            std::string(SINK_NAME) + "\" node.target = \"" + std::string(SINK_NAME) + "\" }";

        micLoopback = PipeWireApi::context_load_module(context, "libpipewire-module-loopback", args.c_str(), nullptr);
        if (!micLoopback)
        {
            Fancy::fancy.logTime().failure() << "Failed to create microphone loopback" << std::endl;
            return false;
        }

        Fancy::fancy.logTime().message() << "Microphone loopback uses " << source << std::endl;

        return true;
    }

    std::string PipeWire::firstRealSource()
    {
        for (const auto &[id, node] : nodes.copy())
        {
            if (isInternalNode(node) || node.isMonitor || node.mediaClass != "Audio/Source")
            {
                continue;
            }

            return node.name;
        }

        return {};
    }

    bool PipeWire::createVirtualSource()
    {
        if (virtualSource)
        {
            return true;
        }

        const std::string args =
            "capture.props = { node.name = \"soundux_source_capture\" media.class = \"Stream/Input/Audio\" "
            "stream.capture.sink = true target.object = \"" +
            std::string(SINK_NAME) + "\" node.target = \"" + std::string(SINK_NAME) + "\" } " +
            "playback.props = { node.name = \"" + std::string(VIRTUAL_SOURCE_NAME) +
            "\" node.description = \"Soundux\" media.class = \"Audio/Source\" "
            "node.passive = true }";

        virtualSource = PipeWireApi::context_load_module(context, "libpipewire-module-loopback", args.c_str(), nullptr);
        if (!virtualSource)
        {
            Fancy::fancy.logTime().failure() << "Failed to create virtual source" << std::endl;
            return false;
        }

        return true;
    }

    void PipeWire::removeVirtualSource()
    {
        if (!virtualSource)
        {
            return;
        }

        PipeWireApi::impl_module_destroy(virtualSource);
        virtualSource = nullptr;
        sync();
    }

    bool PipeWire::setNodeTarget(std::uint32_t nodeId, const std::string &target)
    {
        if (!defaultMetadata)
        {
            return false;
        }

        const auto targetNode = findNodeByName(target);
        if (!targetNode)
        {
            Fancy::fancy.logTime().warning() << "Could not find " << target << " to move the node to" << std::endl;
            return false;
        }

        //* This is how pipewire-pulse relocates streams for its move calls, the value is a node id (Spa:Id).
        //* The session manager then links the node to that target including any needed format conversion.
        const auto value = std::to_string(*targetNode);
        return pw_metadata_set_property(defaultMetadata, nodeId, "target.node", "Spa:Id", value.c_str()) >= 0;
    }

    bool PipeWire::clearNodeTarget(std::uint32_t nodeId)
    {
        if (!defaultMetadata)
        {
            return false;
        }

        return pw_metadata_set_property(defaultMetadata, nodeId, "target.node", nullptr, nullptr) >= 0;
    }

    bool PipeWire::setMetadataValue(const std::string &key, const std::string &value)
    {
        if (!defaultMetadata)
        {
            return false;
        }

        return pw_metadata_set_property(defaultMetadata, 0, key.c_str(), "Spa:String:JSON", value.c_str()) >= 0;
    }

    bool PipeWire::removeMetadataValue(const std::string &key)
    {
        if (!defaultMetadata)
        {
            return false;
        }

        return pw_metadata_set_property(defaultMetadata, 0, key.c_str(), nullptr, nullptr) >= 0;
    }

    bool PipeWire::setNodeMute(std::uint32_t nodeId, bool mute)
    {
        char buffer[1024];
        spa_pod_builder builder;
        spa_pod_builder_init(&builder, buffer, sizeof(buffer));

        spa_pod_frame frame[1];
        spa_pod_builder_push_object(&builder, &frame[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
        spa_pod_builder_add(&builder, SPA_PROP_mute, SPA_POD_Bool(mute), 0);
        auto *param = static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &frame[0]));

        auto *bound =
            reinterpret_cast<pw_node *>(pw_registry_bind(registry, nodeId, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (!bound)
        {
            return false;
        }

        const auto result = pw_node_set_param(bound, SPA_PARAM_Props, 0, param) >= 0;
        sync();
        PipeWireApi::proxy_destroy(reinterpret_cast<pw_proxy *>(bound));

        return result;
    }

    bool PipeWire::deleteLink(std::uint32_t id)
    {
        if (auto link = linkProxies.find(id); link != linkProxies.end())
        {
            PipeWireApi::proxy_destroy(link->second);
            linkProxies.erase(link);
        }
        else
        {
            pw_registry_destroy(registry, id);
        }

        sync();
        return true;
    }

    std::optional<std::uint32_t> PipeWire::linkPorts(std::uint32_t outputPort, std::uint32_t inputPort)
    {
        pw_properties *props = PipeWireApi::properties_new(nullptr, nullptr);

        PipeWireApi::properties_set(props, PW_KEY_APP_NAME, "soundux");
        PipeWireApi::properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", outputPort);
        PipeWireApi::properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", inputPort);

        auto *proxy = reinterpret_cast<pw_proxy *>(
            pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));

        if (!proxy)
        {
            Fancy::fancy.logTime().warning() << "Failed to create link from " << outputPort << " to " << inputPort
                                             << std::endl;
            PipeWireApi::properties_free(props);
            return std::nullopt;
        }

        spa_hook listener{};
        std::optional<std::uint32_t> result;

        pw_proxy_events events{};
        events.version = PW_VERSION_PROXY_EVENTS;
        events.bound = [](void *data, std::uint32_t id) {
            *static_cast<std::optional<std::uint32_t> *>(data) = id;
        };
        events.error = [](void *data, [[maybe_unused]] int seq, [[maybe_unused]] int res, const char *message) {
            Fancy::fancy.logTime().warning() << "Failed to create link: " << message << std::endl;
            *static_cast<std::optional<std::uint32_t> *>(data) = std::nullopt;
        };

        PipeWireApi::proxy_add_listener(proxy, &listener, &events, &result);
        sync();
        spa_hook_remove(&listener);
        PipeWireApi::properties_free(props);

        if (result)
        {
            linkProxies.emplace(*result, proxy);
        }
        else
        {
            PipeWireApi::proxy_destroy(proxy);
        }

        return result;
    }

    std::optional<std::uint32_t> PipeWire::findNodeByName(const std::string &name)
    {
        for (const auto &[nodeId, node] : nodes.copy())
        {
            if (node.rawName == name)
            {
                return nodeId;
            }
        }

        return std::nullopt;
    }

    std::string PipeWire::applicationOf(const Node &node)
    {
        //* PulseAudio clients carry the binary on the node itself, binding the client and waiting
        //* for the server for every application freezes the interface while the list is built
        if (!node.applicationBinary.empty())
        {
            return node.applicationBinary;
        }

        Fancy::fancy.logTime().message() << "Looking up the application of node " << node.clientId
                                        << ", this waits for the server" << std::endl;

        if (!node.clientId)
        {
            return node.applicationBinary;
        }

        {
            const auto knownClients = clients.copy();
            if (auto client = knownClients.find(node.clientId);
                client != knownClients.end() && client->second.resolved)
            {
                return client->second.binary;
            }
        }

        //* application.process.binary is not part of the registry globals, we have to bind the client to get it.
        //* The value is cached afterwards, clients rarely change their properties.
        std::string binary;
        auto *client = reinterpret_cast<pw_client *>(
            pw_registry_bind(registry, node.clientId, PW_TYPE_INTERFACE_Client, PW_VERSION_CLIENT, 0));
        if (client)
        {
            spa_hook hook{};
            pw_client_events events{};
            events.version = PW_VERSION_CLIENT_EVENTS;
            events.info = [](void *data, const pw_client_info *info) {
                if (info && info->props)
                {
                    if (const auto *value = spa_dict_lookup(info->props, "application.process.binary"); value)
                    {
                        *static_cast<std::string *>(data) = value;
                    }
                }
            };
            pw_client_add_listener(client, &hook, &events, &binary);
            sync();
            spa_hook_remove(&hook);
            PipeWireApi::proxy_destroy(reinterpret_cast<pw_proxy *>(client));
        }

        auto scopedClients = clients.scoped();
        auto &entry = (*scopedClients)[node.clientId];
        entry.resolved = true;
        entry.binary = binary;

        return binary;
    }

    std::vector<Port> PipeWire::portsOf(std::uint32_t nodeId, std::optional<spa_direction> direction)
    {
        std::vector<Port> rtn;

        for (const auto &[portId, port] : ports.copy())
        {
            if (port.parentNode != nodeId)
            {
                continue;
            }
            if (direction && port.direction != *direction)
            {
                continue;
            }

            rtn.emplace_back(port);
        }

        return rtn;
    }

    bool PipeWire::sidesMatch(Side a, Side b)
    {
        if (a == Side::UNDEFINED || b == Side::UNDEFINED)
        {
            return false;
        }

        return a == b || a == Side::MONO || b == Side::MONO;
    }

    bool PipeWire::isInternalNode(const Node &node)
    {
        return node.rawName.rfind(INTERNAL_PREFIX, 0) == 0;
    }

    bool PipeWire::createLinksFor(const std::string &application)
    {
        const auto sink = findNodeByName(SINK_NAME);
        if (!sink)
        {
            Fancy::fancy.logTime().warning() << "Could not find " << SINK_NAME << std::endl;
            return false;
        }

        const auto sinkPorts = portsOf(*sink, SPA_DIRECTION_INPUT);
        if (sinkPorts.empty())
        {
            Fancy::fancy.logTime().warning() << "Could not find ports of " << SINK_NAME << std::endl;
            return false;
        }

        std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs;

        for (const auto &[nodeId, node] : nodes.copy())
        {
            if (applicationOf(node) != application || isInternalNode(node) || node.isMonitor)
            {
                continue;
            }

            //* Recording streams also have monitor ports, only use playback streams here
            if (node.mediaClass.rfind("Stream/Output", 0) != 0)
            {
                continue;
            }

            for (const auto &appPort : portsOf(nodeId, SPA_DIRECTION_OUTPUT))
            {
                for (const auto &sinkPort : sinkPorts)
                {
                    if (!sidesMatch(appPort.side, sinkPort.side))
                    {
                        continue;
                    }

                    pairs.emplace_back(appPort.id, sinkPort.id);
                }
            }
        }

        if (pairs.empty())
        {
            Fancy::fancy.logTime().warning() << "Could not find ports for app " << application << std::endl;
            return false;
        }

        bool success = false;
        for (const auto &[output, input] : pairs)
        {
            if (auto link = linkPorts(output, input))
            {
                passthroughLinks[application].emplace_back(*link);
                success = true;
            }
        }

        return success;
    }

    bool PipeWire::routeToSoundInput(const std::string &application)
    {
        if (!findNodeByName(SINK_NAME))
        {
            Fancy::fancy.logTime().warning() << "Could not find " << SINK_NAME << std::endl;
            return false;
        }

        auto &targets = soundInputNodes[application];
        bool success = !targets.empty();

        for (const auto &[nodeId, node] : nodes.copy())
        {
            if (node.name.empty() || node.isMonitor || isInternalNode(node) || applicationOf(node) != application)
            {
                continue;
            }

            //* Move the capture stream of the app to our sink, the session manager links it to our sink's monitor
            if (node.mediaClass.rfind("Stream/Input", 0) != 0)
            {
                continue;
            }

            if (std::find(targets.begin(), targets.end(), nodeId) != targets.end())
            {
                success = true;
                continue;
            }

            if (setNodeTarget(nodeId, SINK_NAME))
            {
                Fancy::fancy.logTime().message() << "Moved " << application << " to " << SINK_NAME << std::endl;
                targets.emplace_back(nodeId);
                success = true;
            }
        }

        if (!success)
        {
            Fancy::fancy.logTime().warning() << "Could not find capture streams of app " << application << std::endl;
        }

        return success;
    }

    namespace
    {
        //* Reports the calls that keep the interface waiting, they should be instant
        class SlowCall
        {
          public:
            explicit SlowCall(const char *name)
                : name(name), start(std::chrono::steady_clock::now())
            {
            }

            ~SlowCall()
            {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                     start)
                                    .count();
                if (ms > 200)
                {
                    Fancy::fancy.logTime().warning() << name << " waited " << ms << " ms for the server" << std::endl;
                }
            }

          private:
            const char *name;
            std::chrono::steady_clock::time_point start;
        };
    } // namespace

    bool PipeWire::setup()
    {
        if (!PipeWireApi::setup())
        {
            return false;
        }

        PipeWireApi::init(nullptr, nullptr);

        loop = PipeWireApi::thread_loop_new("soundux", nullptr);
        if (!loop)
        {
            Fancy::fancy.logTime().failure() << "Failed to create thread loop" << std::endl;
            return false;
        }

        PipeWireApi::thread_loop_start(loop);

        Lock lock(loop);

        context = PipeWireApi::context_new(PipeWireApi::thread_loop_get_loop(loop), nullptr, 0);
        if (!context)
        {
            Fancy::fancy.logTime().failure() << "Failed to create context" << std::endl;
            return false;
        }

        core = PipeWireApi::context_connect(context, nullptr, 0);
        if (!core)
        {
            Fancy::fancy.logTime().failure() << "Failed to connect context" << std::endl;
            return false;
        }

        coreEvents.version = PW_VERSION_CORE_EVENTS;
        coreEvents.info = [](void *data, const pw_core_info *info) {
            auto *thiz = static_cast<PipeWire *>(data);
            if (thiz)
            {
                thiz->onCoreInfo(info);
            }
        };
        coreEvents.error = []([[maybe_unused]] void *data, std::uint32_t id, int seq, int res, const char *message) {
            Fancy::fancy.logTime() << "Core error - id " << id << " seq " << seq << " res " << res << ": " << message
                                   << std::endl;
        };
        pw_core_add_listener(core, &coreListener, &coreEvents, this);

        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (!registry)
        {
            Fancy::fancy.logTime().failure() << "Failed to get registry" << std::endl;
            return false;
        }

        registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
        registryEvents.global = onGlobalAdded;
        registryEvents.global_remove = onGlobalRemoved;
        pw_registry_add_listener(registry, &registryListener, &registryEvents, this);

        //* The first sync populates the registry (and binds the metadata), the second one makes sure
        //* that we already received the initial metadata values like default.audio.source
        sync();
        sync();

        if (defaultSource.empty())
        {
            Fancy::fancy.logTime().warning() << "Failed to retrieve default microphone" << std::endl;
        }

        if (!createNullSink())
        {
            return false;
        }
        sync();

        //* Not fatal, the soundboard works without a microphone, the loopback is retried once one appears
        createMicLoopback();
        sync();

        return true;
    }

    void PipeWire::destroy()
    {
        if (!loop)
        {
            return;
        }

        {
            Lock lock(loop);

            stopSoundInput();
            stopAllPassthrough();
            revertDefault();

            if (micLoopback)
            {
                PipeWireApi::impl_module_destroy(micLoopback);
                micLoopback = nullptr;
            }

            if (sinkProxy)
            {
                PipeWireApi::proxy_destroy(sinkProxy);
                sinkProxy = nullptr;
            }

            linkProxies.clear();

            if (defaultMetadata)
            {
                spa_hook_remove(&metadataListener);
                PipeWireApi::proxy_destroy(reinterpret_cast<pw_proxy *>(defaultMetadata));
                defaultMetadata = nullptr;
            }

            if (registry)
            {
                PipeWireApi::proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
                registry = nullptr;
            }

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

    bool PipeWire::useAsDefault()
    {
        Lock lock(loop);

        if (usingAsDefault)
        {
            return true;
        }

        if (defaultSource.empty())
        {
            Fancy::fancy.logTime().failure()
                << "Could not set default source because original default source is unknown" << std::endl;
            return false;
        }

        if (savedSourceValue.empty() && configuredSourceValue.find(VIRTUAL_SOURCE_NAME) == std::string::npos)
        {
            savedSourceValue = configuredSourceValue;
        }

        //* Setting the configured source is the same as `wpctl set-default`, wireplumber will update the
        //* effective default.audio.source for us afterwards. The effective key is written as well for session
        //* managers that do not know about the configured one.
        const std::string value = std::string("{\"name\":\"") + VIRTUAL_SOURCE_NAME + "\"}";

        if (!createVirtualSource() || !setMetadataValue("default.configured.audio.source", value) ||
            !setMetadataValue("default.audio.source", value))
        {
            removeVirtualSource();
            Fancy::fancy.logTime().failure() << "Failed to set default source to soundux" << std::endl;
            return false;
        }

        usingAsDefault = true;
        return true;
    }

    bool PipeWire::revertDefault()
    {
        Lock lock(loop);

        if (!usingAsDefault)
        {
            return true;
        }

        bool success = true;
        if (savedSourceValue.empty())
        {
            success = removeMetadataValue("default.configured.audio.source") &&
                      removeMetadataValue("default.audio.source");
        }
        else
        {
            success = setMetadataValue("default.configured.audio.source", savedSourceValue) &&
                      setMetadataValue("default.audio.source", savedSourceValue);
        }

        if (!success)
        {
            Fancy::fancy.logTime().failure() << "Failed to reset default device" << std::endl;
        }

        removeVirtualSource();
        savedSourceValue.clear();
        usingAsDefault = false;

        return success;
    }

    bool PipeWire::muteInput(bool state)
    {
        Lock lock(loop);

        const auto mic = findNodeByName(MIC_LOOPBACK_NAME);
        if (!mic)
        {
            Fancy::fancy.logTime().warning() << "Could not find microphone loopback" << std::endl;
            return false;
        }

        if (!setNodeMute(*mic, state))
        {
            Fancy::fancy.logTime().failure() << "Failed to mute microphone" << std::endl;
            return false;
        }

        return true;
    }

    std::set<std::string> PipeWire::currentlyInputApps()
    {
        Lock lock(loop);

        std::set<std::string> rtn;
        for (const auto &[app, nodes] : soundInputNodes)
        {
            rtn.emplace(app);
        }

        return rtn;
    }

    std::set<std::string> PipeWire::currentlyPassedThrough()
    {
        Lock lock(loop);

        std::set<std::string> rtn;
        for (const auto &[app, links] : passthroughLinks)
        {
            rtn.emplace(app);
        }

        return rtn;
    }

    bool PipeWire::stopSoundInput()
    {
        Lock lock(loop);

        for (const auto &[application, nodeIds] : soundInputNodes)
        {
            for (const auto &nodeId : nodeIds)
            {
                clearNodeTarget(nodeId);
            }
        }
        soundInputNodes.clear();

        return true;
    }

    bool PipeWire::stopAllPassthrough()
    {
        Lock lock(loop);

        for (const auto &[appBinary, links] : passthroughLinks)
        {
            for (const auto &id : links)
            {
                deleteLink(id);
            }
        }
        passthroughLinks.clear();

        return true;
    }

    bool PipeWire::stopPassthrough(const std::string &app)
    {
        Lock lock(loop);

        if (passthroughLinks.find(app) == passthroughLinks.end())
        {
            Fancy::fancy.logTime().warning() << "Could not find links for application " << app << std::endl;
            return false;
        }

        for (const auto &id : passthroughLinks.at(app))
        {
            deleteLink(id);
        }
        passthroughLinks.erase(app);

        return true;
    }

    bool PipeWire::passthroughFrom(std::shared_ptr<PlaybackApp> app)
    {
        SlowCall slow("passthroughFrom");
        Lock lock(loop);

        if (!app)
        {
            Fancy::fancy.logTime().warning() << "Invalid app" << std::endl;
            return false;
        }

        if (const auto existing = passthroughLinks.find(app->application); existing != passthroughLinks.end())
        {
            const bool alive = std::all_of(existing->second.begin(), existing->second.end(),
                                           [this](std::uint32_t id) { return linkProxies.count(id) != 0; });
            if (alive)
            {
                Fancy::fancy.logTime().message()
                    << "Ignoring sound passthrough request because requested app is already moved" << std::endl;
                return true;
            }

            //* The app recreated its streams (for example after switching devices), link the new ones
            passthroughLinks.erase(existing);
        }


        if (!createLinksFor(app->application))
        {
            Fancy::fancy.logTime().failure() << "Could not pass " << app->application << " through" << std::endl;
            return false;
        }

        Fancy::fancy.logTime().message() << "Passing " << app->application << " through" << std::endl;

        return true;
    }

    bool PipeWire::inputSoundTo(std::shared_ptr<RecordingApp> app)
    {
        SlowCall slow("inputSoundTo");
        Lock lock(loop);

        if (!app)
        {
            Fancy::fancy.logTime().warning() << "Invalid app" << std::endl;
            return false;
        }

        if (const auto existing = soundInputNodes.find(app->application); existing != soundInputNodes.end())
        {
            auto scopedNodes = nodes.scoped();
            const bool alive = std::all_of(existing->second.begin(), existing->second.end(),
                                           [&scopedNodes](std::uint32_t id) { return scopedNodes->count(id) != 0; });

            if (alive)
            {
                return true;
            }

            //* The app recreated its streams (for example after switching devices), move the new ones
            soundInputNodes.erase(existing);
        }


        return routeToSoundInput(app->application);
    }

    std::shared_ptr<PlaybackApp> PipeWire::getPlaybackApp(const std::string &app)
    {
        for (const auto &candidate : getPlaybackApps())
        {
            if (candidate->application == app)
            {
                return candidate;
            }
        }

        return nullptr;
    }

    std::shared_ptr<RecordingApp> PipeWire::getRecordingApp(const std::string &app)
    {
        for (const auto &candidate : getRecordingApps())
        {
            if (candidate->application == app)
            {
                return candidate;
            }
        }

        return nullptr;
    }

    std::vector<std::shared_ptr<PlaybackApp>> PipeWire::getPlaybackApps()
    {
        SlowCall slow("getPlaybackApps");
        //* Called from the interface thread, waiting for the server here would freeze the window
        Lock lock(loop);

        std::vector<std::shared_ptr<PlaybackApp>> rtn;

        for (const auto &[nodeId, node] : nodes.copy())
        {
            if (node.name.empty() || node.isMonitor || isInternalNode(node))
            {
                continue;
            }

            //* Only actual playback streams are of interest, recording streams also have monitor ports
            if (node.mediaClass.rfind("Stream/Output", 0) != 0)
            {
                continue;
            }

            PipeWirePlaybackApp app;
            app.pid = node.pid;
            app.nodeId = nodeId;
            app.name = node.name;
            app.application = applicationOf(node);
            rtn.emplace_back(std::make_shared<PipeWirePlaybackApp>(app));
        }

        return rtn;
    }

    std::vector<std::shared_ptr<RecordingApp>> PipeWire::getRecordingApps()
    {
        SlowCall slow("getRecordingApps");
        //* Called from the interface thread, waiting for the server here would freeze the window
        Lock lock(loop);

        std::vector<std::shared_ptr<RecordingApp>> rtn;

        for (const auto &[nodeId, node] : nodes.copy())
        {
            if (node.name.empty() || node.isMonitor || isInternalNode(node))
            {
                continue;
            }

            if (node.mediaClass.rfind("Stream/Input", 0) != 0)
            {
                continue;
            }

            PipeWireRecordingApp app;
            app.pid = node.pid;
            app.nodeId = nodeId;
            app.name = node.name;
            app.application = applicationOf(node);
            rtn.emplace_back(std::make_shared<PipeWireRecordingApp>(app));
        }

        return rtn;
    }
} // namespace Soundux::Objects
#endif
