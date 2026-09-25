#pragma once
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Soundux
{
    namespace Objects
    {
        class Hotkeys
        {
            std::thread listener;
            std::atomic<bool> kill = false;
            std::atomic<bool> notify = false;
            std::atomic<bool> windowEvents = false;

            mutable std::mutex keyNamesMutex;
            std::map<int, std::string> keyNames;

            std::vector<int> pressedKeys;
            std::vector<int> keysToPress;
#if defined(_WIN32)
            std::thread keyPressThread;
            std::atomic<bool> shouldPressKeys = false;
#endif

          private:
            void listen();

          public:
            void init();
            void stop();
            void shouldNotify(bool);

            /// \returns Whether the interface currently expects keys to be recorded
            bool isNotifying() const;
            /// \returns Whether keys have to be taken from the window instead of a global grab
            bool usesWindowEvents() const;
            /// \effects Marks the global listener as unavailable so window events are used instead
            void setWindowEventsRequired();
            /// \effects Remembers the name of the given key, used when the global listener is unavailable
            void learnKeyName(int, const std::string &);
            /// \returns The remembered name of the given key, or a fallback name
            std::string lookupKeyName(int) const;

            void onKeyUp(int);
            void onKeyDown(int);

            void pressKeys(const std::vector<int> &);
            void releaseKeys(const std::vector<int> &);

            std::string getKeyName(const int &);
            std::string getKeySequence(const std::vector<int> &);
        };
    } // namespace Objects
} // namespace Soundux