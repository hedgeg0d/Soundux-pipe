#pragma once
#if defined(__linux__)
#include "../backend.hpp"
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <var_guard.hpp>

#include <pipewire/extensions/metadata.h>
#include <pipewire/impl-module.h>
#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

namespace Soundux
{
    namespace Objects
    {
        enum class Side
        {
            UNDEFINED,
            LEFT,
            RIGHT,
            MONO,
        };

        struct Port
        {
            std::uint32_t id = 0;
            std::string name;
            spa_direction direction = SPA_DIRECTION_INPUT;
            Side side = Side::UNDEFINED;
            std::uint32_t parentNode = 0;

            static Side sideOf(const std::string &name);
        };

        struct Node
        {
            std::uint32_t id = 0;
            std::string name;
            std::uint32_t pid = 0;
            std::string rawName;
            bool isMonitor = false;
            std::string mediaClass;
            std::string applicationBinary;
            std::uint32_t clientId = 0;
        };

        //* Application properties (like application.process.binary) are only available on the bound client object
        struct Client
        {
            std::string binary;
            bool resolved = false;
        };

        struct PipeWirePlaybackApp : public PlaybackApp
        {
            std::uint32_t pid = 0;
            std::uint32_t nodeId = 0;
            ~PipeWirePlaybackApp() override = default;
        };
        struct PipeWireRecordingApp : public RecordingApp
        {
            std::uint32_t pid = 0;
            std::uint32_t nodeId = 0;
            ~PipeWireRecordingApp() override = default;
        };

        class PipeWire : public AudioBackend
        {
            friend class AudioBackend;

          private:
            //* RAII wrapper around the thread loop lock, every public method has to hold it.
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
            pw_registry *registry = nullptr;
            std::uint32_t version = 0;

            spa_hook registryListener{};
            pw_registry_events registryEvents{};
            spa_hook coreListener{};
            pw_core_events coreEvents{};
            spa_hook metadataListener{};
            pw_metadata_events metadataEvents{};
            pw_metadata *defaultMetadata = nullptr;
            std::uint32_t metadataId = 0;

            //* Name of the source we capture the microphone from and the current/saved values of the default metadata
            std::string defaultSource;
            std::string defaultSourceValue;
            std::string configuredSourceValue;
            std::string savedSourceValue;

            pw_proxy *sinkProxy = nullptr;

            //* Our virtual devices
            pw_impl_module *micLoopback = nullptr;
            pw_impl_module *virtualSource = nullptr;
            bool usingAsDefault = false;

            sxl::var_guard<std::map<std::uint32_t, Node>> nodes;
            sxl::var_guard<std::map<std::uint32_t, Port>> ports;
            sxl::var_guard<std::map<std::uint32_t, Client>> clients;

            //* Links we created, so that we can clean them up again
            std::map<std::uint32_t, pw_proxy *> linkProxies;
            std::map<std::string, std::vector<std::uint32_t>> passthroughLinks;

            //* Nodes that we moved to our sink, per application
            std::map<std::string, std::vector<std::uint32_t>> soundInputNodes;

          private:
            //* All of these expect the lock to be held
            void sync();
            bool createNullSink();
            bool createMicLoopback();
            bool createVirtualSource();
            void removeVirtualSource();
            bool setMetadataValue(const std::string &key, const std::string &value);
            bool removeMetadataValue(const std::string &key);
            //* Moves a node to the given target by setting its metadata, this is what the pulse backend did with
            //* its move calls. The session manager then relinks the node (including format conversion) for us.
            bool setNodeTarget(std::uint32_t nodeId, const std::string &target);
            bool clearNodeTarget(std::uint32_t nodeId);
            bool setNodeMute(std::uint32_t nodeId, bool mute);
            bool deleteLink(std::uint32_t id);
            std::optional<std::uint32_t> linkPorts(std::uint32_t outputPort, std::uint32_t inputPort);
            std::vector<Port> portsOf(std::uint32_t nodeId, std::optional<spa_direction> direction);
            std::optional<std::uint32_t> findNodeByName(const std::string &name);
            std::string firstRealSource();
            std::string applicationOf(const Node &node);
            bool createLinksFor(const std::string &application);
            bool routeToSoundInput(const std::string &application);
            static bool sidesMatch(Side a, Side b);
            static bool isInternalNode(const Node &node);

            void onCoreInfo(const pw_core_info *);

            static int onMetadataProperty(void *, std::uint32_t, const char *, const char *, const char *);
            static void onGlobalRemoved(void *, std::uint32_t);
            static void onGlobalAdded(void *, std::uint32_t, std::uint32_t, const char *, std::uint32_t,
                                      const spa_dict *);

          protected:
            bool setup() override;

          public:
            PipeWire() = default;
            void destroy() override;

            bool useAsDefault() override;
            bool revertDefault() override;
            bool muteInput(bool state) override;

            std::set<std::string> currentlyInputApps() override;
            std::set<std::string> currentlyPassedThrough() override;

            bool stopAllPassthrough() override;
            bool stopPassthrough(const std::string &app) override;
            bool passthroughFrom(std::shared_ptr<PlaybackApp> app) override;

            bool stopSoundInput() override;
            bool inputSoundTo(std::shared_ptr<RecordingApp> app) override;

            std::shared_ptr<PlaybackApp> getPlaybackApp(const std::string &app) override;
            std::shared_ptr<RecordingApp> getRecordingApp(const std::string &app) override;

            std::vector<std::shared_ptr<PlaybackApp>> getPlaybackApps() override;
            std::vector<std::shared_ptr<RecordingApp>> getRecordingApps() override;
        };
    } // namespace Objects
} // namespace Soundux
#endif
