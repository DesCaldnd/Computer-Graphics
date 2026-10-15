#pragma once

// Engine console over TCP for devices: listens on 127.0.0.1:<port> inside the app; the host reaches it through
//   adb forward tcp:27015 tcp:27015      then   tools/android/console.ps1  (or any line-based TCP client)
// Each received line is executed by ox::Console on the game thread (pump()) and the output (or error) is sent
// back followed by a "> " prompt. Log records at Info and above are streamed to the connected client as well
// ("# [warn] [render] ..."), so the console doubles as a filtered log view. One client at a time.
#include <oxwald/core/types.hpp>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace ox {
class Console;
}

namespace ox::android {

class RemoteConsoleServer {
public:
    RemoteConsoleServer() = default;
    ~RemoteConsoleServer();
    RemoteConsoleServer(const RemoteConsoleServer&) = delete;
    RemoteConsoleServer& operator=(const RemoteConsoleServer&) = delete;

    bool start(u16 port);
    void stop();
    // Game thread: executes queued commands.
    void pump(Console& console);
    [[nodiscard]] bool running() const { return m_listenFd >= 0; }
    [[nodiscard]] u16 port() const { return m_port; }

private:
    void acceptLoop();
    void send(const std::string& text);

    int m_listenFd = -1;
    int m_clientFd = -1;
    u16 m_port = 0;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
    std::mutex m_mutex; // m_clientFd writes + queue
    std::deque<std::string> m_queue;
    int m_logSink = -1;
};

} // namespace ox::android
