#include <oxwald/core/log.hpp>
#include <oxwald/net/messages.hpp>

namespace ox::net {

void MessageRegistry::onRaw(u32 id, std::string name, RawHandler handler) {
    auto [it, inserted] = m_handlers.try_emplace(id);
    if (!inserted && it->second.name != name) {
        OX_LOG_ERROR("net", "message id collision: '{}' and '{}' hash to {:#x}", it->second.name, name, id);
    }
    it->second = Entry{std::move(name), std::move(handler)};
}

void MessageRegistry::remove(u32 id) { m_handlers.erase(id); }

bool MessageRegistry::dispatch(PeerId from, u32 id, BitReader& reader) const {
    auto it = m_handlers.find(id);
    if (it == m_handlers.end()) {
        OX_LOG_WARN("net", "unknown message/rpc id {:#x} from peer {}", id, from);
        return false;
    }
    it->second.handler(from, reader);
    if (!reader.ok()) {
        OX_LOG_WARN("net", "malformed '{}' from peer {}", it->second.name, from);
        return false;
    }
    return true;
}

} // namespace ox::net
