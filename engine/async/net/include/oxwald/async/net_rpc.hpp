#pragma once

// Request/response RPCs on top of the net module's fire-and-forget RPCs (target Oxwald::async_net).
//
// Wire format: the request is the RPC `name` with (u32 requestId, args...); the reply is the RPC "<name>#reply"
// with (u32 requestId, bool ok, Resp value, std::string error). Both go over ReliableOrdered.
//
//   // server
//   ox::net::serveRequest<Inventory>(server, "inv.fetch", [&](PeerId from, u32 slot) { return db.load(from, slot); });
//   // client (keep the RpcCall alive as long as requests may be pending, e.g. as a member)
//   ox::net::RpcCall<Inventory, u32> fetchInventory(client, "inv.fetch");
//   std::optional<Inventory> inv = co_await ox::realTimeout(fetchInventory(slot), 3.0);
//
// Exceptions thrown by the server handler come back as ox::AsyncError on the client. Replies are delivered from
// NetClient::poll() (main thread), so awaiting coroutines resume on the next scheduler tick.

#include <oxwald/async/future.hpp>
#include <oxwald/net/net_client.hpp>
#include <oxwald/net/net_server.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace ox::net {

inline std::string rpcReplyName(std::string_view name) { return std::string(name) + "#reply"; }

// Binds `name` on the server. handler: Resp(PeerId from, Args...). The reply is sent automatically.
template <class Resp, class... Args, class F>
void serveRequest(NetServer& server, std::string_view name, F handler) {
    server.rpcs().bind(name, [&server, reply = rpcReplyName(name), handler = std::move(handler)](
                                 PeerId from, u32 requestId, Args... args) {
        try {
            Resp value = handler(from, args...);
            server.callClient(from, reply, requestId, true, value, std::string{});
        } catch (const std::exception& e) {
            server.callClient(from, reply, requestId, false, Resp{}, std::string(e.what()));
        }
    });
}

// Client side of one request type. Not thread-safe (use from the thread that polls the NetClient).
template <class Resp, class... Args>
class RpcCall {
public:
    RpcCall(NetClient& client, std::string name)
        : m_client(client), m_name(std::move(name)), m_reply(rpcReplyName(m_name)), m_shared(std::make_shared<Shared>()) {
        std::weak_ptr<Shared> weak = m_shared;
        m_client.rpcs().bind(m_reply, [weak](PeerId, u32 requestId, bool ok, Resp value, std::string error) {
            auto shared = weak.lock();
            if (!shared) {
                return;
            }
            auto it = shared->pending.find(requestId);
            if (it == shared->pending.end()) {
                return; // unknown/duplicate reply
            }
            Promise<Resp> promise = std::move(it->second);
            shared->pending.erase(it);
            if (ok) {
                promise.setValue(std::move(value));
            } else {
                promise.setError(std::move(error));
            }
        });
    }
    ~RpcCall() {
        m_client.rpcs().unbind(m_reply);
        failAll("rpc '" + m_name + "' destroyed");
    }
    RpcCall(const RpcCall&) = delete;
    RpcCall& operator=(const RpcCall&) = delete;

    Future<Resp> operator()(const Args&... args) {
        u32 id = 0;
        return call(id, args...);
    }
    // Same, also returning the request id for cancel() (0 when the request was not sent).
    Future<Resp> call(u32& requestId, const Args&... args) {
        requestId = 0;
        if (!m_client.connected()) {
            return makeErrorFuture<Resp>("rpc '" + m_name + "': not connected");
        }
        const u32 id = ++m_shared->nextId;
        Promise<Resp>& promise = m_shared->pending[id];
        Future<Resp> future = promise.future();
        m_client.callServer(m_name, id, args...);
        requestId = id;
        return future;
    }

    // Fails one pending request (e.g. after ox::timeout gave up on it) and forgets it: a late reply is ignored.
    // Returns false when the request already completed or is unknown.
    bool cancel(u32 requestId, const std::string& reason = "cancelled") {
        auto it = m_shared->pending.find(requestId);
        if (it == m_shared->pending.end()) {
            return false;
        }
        Promise<Resp> promise = std::move(it->second);
        m_shared->pending.erase(it);
        promise.setError("rpc '" + m_name + "': " + reason);
        return true;
    }

    // Fails every pending request (call on disconnect).
    void failAll(const std::string& reason) {
        auto pending = std::move(m_shared->pending);
        m_shared->pending.clear();
        for (auto& [id, promise] : pending) {
            promise.setError(reason);
        }
    }
    [[nodiscard]] usize pending() const { return m_shared->pending.size(); }
    [[nodiscard]] const std::string& name() const { return m_name; }

private:
    struct Shared {
        std::unordered_map<u32, Promise<Resp>> pending;
        u32 nextId = 0;
    };
    NetClient& m_client;
    std::string m_name;
    std::string m_reply;
    std::shared_ptr<Shared> m_shared;
};

} // namespace ox::net
