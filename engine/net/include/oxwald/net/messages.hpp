#pragma once

#include <oxwald/net/serialize.hpp>
#include <oxwald/net/transport.hpp>

#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace ox::net {

// A typed message:
//   struct Chat {
//       static constexpr std::string_view kNetName = "game.chat";
//       std::string text;
//       void serialize(BitWriter& w) const { w.writeString(text); }
//       void deserialize(BitReader& r) { text = r.readString(); }
//   };
// The id on the wire is netHash(kNetName).
template <class M>
concept NetMessage = std::is_default_constructible_v<M> && requires(const M& cm, M& m, BitWriter& w, BitReader& r) {
    { M::kNetName } -> std::convertible_to<std::string_view>;
    cm.serialize(w);
    m.deserialize(r);
};

template <NetMessage M>
constexpr u32 messageId() {
    return netHash(M::kNetName);
}

class MessageRegistry {
public:
    using RawHandler = std::function<void(PeerId from, BitReader& reader)>;

    template <NetMessage M, class F>
    void on(F&& handler) {
        onRaw(messageId<M>(), std::string(M::kNetName),
              [h = std::function<void(PeerId, const M&)>(std::forward<F>(handler))](PeerId from, BitReader& r) {
                  M msg{};
                  msg.deserialize(r);
                  if (r.ok()) {
                      h(from, msg);
                  }
              });
    }

    void onRaw(u32 id, std::string name, RawHandler handler);
    void remove(u32 id);
    bool contains(u32 id) const { return m_handlers.contains(id); }
    // Returns false (and logs) for unknown ids or malformed payloads.
    bool dispatch(PeerId from, u32 id, BitReader& reader) const;

    template <NetMessage M>
    static void encode(BitWriter& w, const M& msg) {
        w.writeU32(messageId<M>());
        msg.serialize(w);
    }

private:
    struct Entry {
        std::string name;
        RawHandler handler;
    };
    std::unordered_map<u32, Entry> m_handlers;
};

namespace detail {
template <class T>
struct CallableTraits : CallableTraits<decltype(&T::operator())> {};
template <class C, class R, class... A>
struct CallableTraits<R (C::*)(A...) const> {
    using Args = std::tuple<A...>;
};
template <class C, class R, class... A>
struct CallableTraits<R (C::*)(A...)> {
    using Args = std::tuple<A...>;
};
template <class R, class... A>
struct CallableTraits<R (*)(A...)> {
    using Args = std::tuple<A...>;
};

template <class Tuple>
struct DropFirst;
template <class First, class... Rest>
struct DropFirst<std::tuple<First, Rest...>> {
    using Type = std::tuple<std::decay_t<Rest>...>;
};
} // namespace detail

// RPCs by name hash. Arguments use NetCodec<T> (specialise it for custom types).
//   rpcs.bind("player.hit", [](PeerId from, f32 damage, glm::vec3 at) { ... });
//   client.callServer("player.hit", 10.f, glm::vec3{...});
// The handler's first parameter is always the sender PeerId (on clients it is the server peer).
class RpcRegistry {
public:
    template <class F>
    void bind(std::string_view name, F&& fn) {
        using Args = typename detail::DropFirst<typename detail::CallableTraits<std::decay_t<F>>::Args>::Type;
        m_messages.onRaw(netHash(name), std::string(name),
                         [f = std::forward<F>(fn)](PeerId from, BitReader& r) mutable {
                             Args args{};
                             std::apply([&r](auto&... a) { (netRead(r, a), ...); }, args);
                             if (r.ok()) {
                                 std::apply([&](auto&... a) { f(from, a...); }, args);
                             }
                         });
    }

    void unbind(std::string_view name) { m_messages.remove(netHash(name)); }
    bool dispatch(PeerId from, u32 id, BitReader& reader) const { return m_messages.dispatch(from, id, reader); }

    template <class... Args>
    static void encode(BitWriter& w, std::string_view name, const Args&... args) {
        w.writeU32(netHash(name));
        (netWrite(w, args), ...);
    }

private:
    MessageRegistry m_messages;
};

} // namespace ox::net
