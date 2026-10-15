#include <oxwald/android/remote_console.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/runtime/console.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>

namespace ox::android {

RemoteConsoleServer::~RemoteConsoleServer() { stop(); }

bool RemoteConsoleServer::start(u16 port) {
    stop();
    m_listenFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (m_listenFd < 0) {
        OX_LOG_WARN("android", "remote console: socket() failed: {}", std::strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // device-local only; reached through `adb forward`
    if (bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(m_listenFd, 1) != 0) {
        OX_LOG_WARN("android", "remote console: cannot listen on 127.0.0.1:{}: {}", port, std::strerror(errno));
        close(m_listenFd);
        m_listenFd = -1;
        return false;
    }
    m_port = port;
    m_stop = false;
    m_thread = std::thread([this] { acceptLoop(); });
    m_logSink = log::addSink([this](const log::Record& r) {
        if (r.level < log::Level::Info || r.category == "remote") return;
        send(std::format("# [{}] [{}] {}\n", log::levelName(r.level), r.category, r.message));
    });
    OX_LOG_INFO("android", "remote console listening on 127.0.0.1:{} (adb forward tcp:{} tcp:{})", port, port, port);
    return true;
}

void RemoteConsoleServer::stop() {
    if (m_logSink >= 0) log::removeSink(m_logSink);
    m_logSink = -1;
    m_stop = true;
    if (m_listenFd >= 0) shutdown(m_listenFd, SHUT_RDWR);
    {
        std::lock_guard lock(m_mutex);
        if (m_clientFd >= 0) shutdown(m_clientFd, SHUT_RDWR);
    }
    if (m_thread.joinable()) m_thread.join();
    if (m_listenFd >= 0) close(m_listenFd);
    m_listenFd = -1;
    std::lock_guard lock(m_mutex);
    if (m_clientFd >= 0) close(m_clientFd);
    m_clientFd = -1;
}

void RemoteConsoleServer::send(const std::string& text) {
    std::lock_guard lock(m_mutex);
    if (m_clientFd < 0) return;
    usize off = 0;
    while (off < text.size()) {
        const ssize_t n = ::send(m_clientFd, text.data() + off, text.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return;
        off += usize(n);
    }
}

void RemoteConsoleServer::acceptLoop() {
    while (!m_stop) {
        pollfd pfd{m_listenFd, POLLIN, 0};
        if (poll(&pfd, 1, 250) <= 0) continue;
        const int fd = accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        {
            std::lock_guard lock(m_mutex);
            m_clientFd = fd;
        }
        send("Oxwald remote console - engine commands and cvars (help, stats, r.ScreenPercentage 70, quit)\n> ");
        std::string pending;
        char buffer[512];
        while (!m_stop) {
            pollfd cfd{fd, POLLIN, 0};
            const int pr = poll(&cfd, 1, 250);
            if (pr == 0) continue;
            const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            pending.append(buffer, usize(n));
            usize nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::lock_guard lock(m_mutex);
                m_queue.push_back(std::move(line));
            }
        }
        std::lock_guard lock(m_mutex);
        close(m_clientFd);
        m_clientFd = -1;
    }
}

void RemoteConsoleServer::pump(Console& console) {
    std::deque<std::string> lines;
    {
        std::lock_guard lock(m_mutex);
        lines.swap(m_queue);
    }
    for (const std::string& line : lines) {
        if (line.empty()) {
            send("> ");
            continue;
        }
        OX_LOG_INFO("remote", "> {}", line);
        auto r = console.execute(line);
        send(r ? (r->empty() ? std::string() : *r + "\n") : "error: " + r.error().message + "\n");
        send("> ");
    }
}

} // namespace ox::android
