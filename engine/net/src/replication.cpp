#include <oxwald/core/assert.hpp>
#include <oxwald/core/log.hpp>
#include <oxwald/net/replication.hpp>

#include <algorithm>
#include <glm/geometric.hpp>

namespace ox::net {

// ---------------------------------------------------------------- NetObject

NetObject::NetObject(std::string_view typeName) : m_typeName(typeName), m_typeId(netHash(typeName)) {}
NetObject::~NetObject() = default;

bool NetObject::isRelevantTo(PeerId client, const glm::vec3* viewer) const {
    if (isOwnedBy(client)) {
        return true;
    }
    switch (m_relevancy) {
    case Relevancy::Always: return true;
    case Relevancy::OwnerOnly: return false;
    case Relevancy::Distance: {
        if (!viewer || !m_position) {
            return true;
        }
        const glm::vec3 d = m_position() - *viewer;
        return glm::dot(d, d) <= m_relevancyRadius * m_relevancyRadius;
    }
    case Relevancy::Custom: return m_filter ? m_filter(client, viewer) : true;
    }
    return true;
}

NetState NetObject::captureState() const {
    NetState state;
    state.reserve(m_properties.size());
    for (const auto& p : m_properties) {
        state.push_back(p->capture());
    }
    return state;
}

// ---------------------------------------------------------------- ReplicationServer

struct ReplicationServer::ClientRecord {
    bool acked = false; // spawn acknowledged; deltas allowed from here on
    u32 ackedSeq = 0;
    NetStatePtr ackedState;
    u32 minSeq = 0; // acks of packets older than this record's (re)spawn are ignored
    f32 accumulator = 0.f;
};

struct ReplicationServer::ClientState {
    struct Sent {
        u32 seq = 0;
        bool processed = false;
        std::vector<std::pair<NetId, NetStatePtr>> objects;
        std::vector<NetId> despawns;
    };

    PeerId peer = kInvalidPeer;
    bool hasViewer = false;
    glm::vec3 viewer{0.f};
    u32 nextSeq = 1;
    std::unordered_map<NetId, ClientRecord> records;
    std::vector<NetId> pendingDespawns;
    std::deque<Sent> sent;

    Sent* findSent(u32 seq) {
        if (sent.empty() || seq < sent.front().seq || seq > sent.back().seq) {
            return nullptr;
        }
        return &sent[seq - sent.front().seq];
    }

    void despawn(NetId id) {
        if (records.erase(id) > 0 && std::find(pendingDespawns.begin(), pendingDespawns.end(), id) ==
                                         pendingDespawns.end()) {
            pendingDespawns.push_back(id);
        }
    }
};

ReplicationServer::ReplicationServer(ReplicationConfig config) : m_config(config) {}
ReplicationServer::~ReplicationServer() = default;

NetId ReplicationServer::add(std::shared_ptr<NetObject> object) {
    OX_ASSERT(object && object->m_netId == kInvalidNetId, "object is null or already replicated");
    const NetId id = m_nextId++;
    object->m_netId = id;
    m_objects.emplace(id, std::move(object));
    m_order.push_back(id);
    return id;
}

void ReplicationServer::remove(NetId id) {
    if (m_objects.erase(id) == 0) {
        return;
    }
    std::erase(m_order, id);
    m_current.erase(id);
    for (auto& [peer, cs] : m_clients) {
        cs->despawn(id);
    }
}

NetObject* ReplicationServer::find(NetId id) const {
    auto it = m_objects.find(id);
    return it == m_objects.end() ? nullptr : it->second.get();
}

void ReplicationServer::addClient(PeerId client) {
    auto cs = std::make_unique<ClientState>();
    cs->peer = client;
    m_clients[client] = std::move(cs);
}

void ReplicationServer::removeClient(PeerId client) { m_clients.erase(client); }

void ReplicationServer::setViewerPosition(PeerId client, const glm::vec3& position) {
    if (auto it = m_clients.find(client); it != m_clients.end()) {
        it->second->hasViewer = true;
        it->second->viewer = position;
    }
}

void ReplicationServer::beginTick() {
    for (NetId id : m_order) {
        m_current[id] = std::make_shared<const NetState>(m_objects[id]->captureState());
    }
}

bool ReplicationServer::isSpawnedOn(PeerId client, NetId id) const {
    auto it = m_clients.find(client);
    if (it == m_clients.end()) {
        return false;
    }
    auto rec = it->second->records.find(id);
    return rec != it->second->records.end() && rec->second.acked;
}

SnapshotStats ReplicationServer::writeSnapshot(PeerId client, BitWriter& w, u32 serverTick, f64 serverTime) {
    SnapshotStats stats;
    auto csIt = m_clients.find(client);
    if (csIt == m_clients.end()) {
        return stats;
    }
    ClientState& cs = *csIt->second;
    const u32 seq = cs.nextSeq++;
    const glm::vec3* viewer = cs.hasViewer ? &cs.viewer : nullptr;
    const usize startBytes = w.bytesWritten();

    // Relevancy changes become spawns/despawns.
    for (NetId id : m_order) {
        const bool relevant = m_objects[id]->isRelevantTo(client, viewer);
        const bool known = cs.records.contains(id);
        if (relevant && !known) {
            cs.records[id].minSeq = seq;
            std::erase(cs.pendingDespawns, id);
        } else if (!relevant && known) {
            cs.despawn(id);
        }
    }

    ClientState::Sent sent;
    sent.seq = seq;

    w.writeVarU32(seq);
    w.writeVarU32(serverTick);
    w.writeF64(serverTime);
    const u32 despawnCount = std::min<u32>(static_cast<u32>(cs.pendingDespawns.size()), m_config.maxDespawnsPerPacket);
    w.writeVarU32(despawnCount);
    for (u32 i = 0; i < despawnCount; ++i) {
        w.writeVarU32(cs.pendingDespawns[i]);
        sent.despawns.push_back(cs.pendingDespawns[i]);
    }
    stats.despawns = despawnCount;

    struct Candidate {
        NetId id;
        ClientRecord* rec;
        NetObject* obj;
        NetStatePtr state;
        bool useBaseline;
        std::vector<bool> changed;
    };
    std::vector<Candidate> candidates;
    for (NetId id : m_order) {
        auto recIt = cs.records.find(id);
        if (recIt == cs.records.end()) {
            continue;
        }
        NetObject* obj = m_objects[id].get();
        NetStatePtr& state = m_current[id];
        if (!state) {
            state = std::make_shared<const NetState>(obj->captureState()); // added after beginTick()
        }
        ClientRecord& rec = recIt->second;
        Candidate c{id, &rec, obj, state, rec.acked && seq - rec.ackedSeq <= m_config.maxBaselineAge, {}};
        if (c.useBaseline) {
            const auto& props = obj->properties();
            c.changed.resize(props.size());
            bool any = false;
            for (usize i = 0; i < props.size(); ++i) {
                c.changed[i] = !props[i]->equal((*state)[i].get(), (*rec.ackedState)[i].get());
                any = any || c.changed[i];
            }
            if (!any) {
                continue;
            }
        }
        f32 factor = 1.f;
        if (viewer && obj->hasPosition()) {
            const f32 d = glm::length(obj->position() - *viewer);
            factor = obj->relevancy() == Relevancy::Distance && obj->m_relevancyRadius > 0.f
                         ? std::clamp(1.f - d / obj->m_relevancyRadius, 0.1f, 1.f)
                         : 1.f / (1.f + d * 0.05f);
        }
        if (!rec.acked) {
            factor *= 4.f; // pending spawns go first
        }
        rec.accumulator += obj->priority() * factor;
        candidates.push_back(std::move(c));
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.rec->accumulator > b.rec->accumulator; });

    for (Candidate& c : candidates) {
        const u32 mark = w.bitPosition();
        const auto& props = c.obj->properties();
        const bool spawn = !c.rec->acked;
        w.writeBool(true);
        w.writeVarU32(c.id);
        w.writeBool(spawn);
        if (spawn) {
            w.writeU32(c.obj->typeId());
            w.writeVarU32(c.obj->owner());
        } else {
            w.writeBool(c.useBaseline);
            if (c.useBaseline) {
                w.writeVarU32(seq - c.rec->ackedSeq);
            }
        }
        w.writeVarU32(static_cast<u32>(props.size()));
        const bool full = spawn || !c.useBaseline;
        for (usize i = 0; i < props.size(); ++i) {
            if (!full) {
                w.writeBool(c.changed[i]);
                if (!c.changed[i]) {
                    continue;
                }
            }
            props[i]->write(w, (*c.state)[i].get());
        }
        // +1 byte reserve for the terminator bit and padding.
        if (stats.objectsWritten > 0 && w.bytesWritten() - startBytes + 1 > m_config.bytesPerTick) {
            w.rewind(mark);
            ++stats.objectsDeferred;
            continue;
        }
        c.rec->accumulator = 0.f;
        sent.objects.emplace_back(c.id, c.state);
        ++stats.objectsWritten;
        stats.spawns += spawn ? 1 : 0;
    }
    w.writeBool(false);

    cs.sent.push_back(std::move(sent));
    while (cs.sent.size() > m_config.sentHistory) {
        cs.sent.pop_front();
    }
    stats.bytes = static_cast<u32>(w.bytesWritten() - startBytes);
    return stats;
}

void ReplicationServer::processAck(PeerId client, u32 latestSeq, u32 ackBits) {
    auto csIt = m_clients.find(client);
    if (csIt == m_clients.end()) {
        return;
    }
    ClientState& cs = *csIt->second;
    for (u32 i = 0; i <= 32 && i < latestSeq; ++i) {
        if (i > 0 && (ackBits & (1u << (i - 1))) == 0) {
            continue;
        }
        const u32 seq = latestSeq - i;
        ClientState::Sent* sent = cs.findSent(seq);
        if (!sent || sent->processed) {
            continue;
        }
        sent->processed = true;
        for (auto& [id, state] : sent->objects) {
            auto rec = cs.records.find(id);
            if (rec == cs.records.end() || seq < rec->second.minSeq) {
                continue;
            }
            if (!rec->second.acked || seq > rec->second.ackedSeq) {
                rec->second.acked = true;
                rec->second.ackedSeq = seq;
                rec->second.ackedState = state;
            }
        }
        for (NetId id : sent->despawns) {
            std::erase(cs.pendingDespawns, id);
        }
    }
}

// ---------------------------------------------------------------- NetObjectFactory

void NetObjectFactory::registerType(std::string_view typeName, CreateFn fn) { m_types[netHash(typeName)] = std::move(fn); }

std::shared_ptr<NetObject> NetObjectFactory::create(NetTypeId type, NetId id, PeerId owner) const {
    auto it = m_types.find(type);
    return it == m_types.end() ? nullptr : it->second(type, id, owner);
}

// ---------------------------------------------------------------- ReplicationClient

namespace {
constexpr usize kClientHistory = 64;
}

ReplicationClient::ReplicationClient() = default;
ReplicationClient::~ReplicationClient() { clear(); }

NetObject* ReplicationClient::find(NetId id) const {
    auto it = m_objects.find(id);
    return it == m_objects.end() ? nullptr : it->second.get();
}

void ReplicationClient::despawn(NetId id) {
    auto it = m_objects.find(id);
    if (it == m_objects.end()) {
        return;
    }
    std::shared_ptr<NetObject> obj = it->second;
    m_objects.erase(it);
    m_history.erase(id);
    obj->onNetDespawn();
    if (onDespawn) {
        onDespawn(*obj);
    }
}

void ReplicationClient::clear() {
    std::vector<NetId> ids;
    for (auto& [id, obj] : m_objects) {
        ids.push_back(id);
    }
    for (NetId id : ids) {
        despawn(id);
    }
    m_latestSeq = 0;
    m_ackBits = 0;
    m_ackDirty = false;
}

bool ReplicationClient::readSnapshot(BitReader& r) {
    const u32 seq = r.readVarU32();
    const u32 tick = r.readVarU32();
    const f64 serverTime = r.readF64();
    if (!r.ok() || seq == 0) {
        return false;
    }
    if (m_latestSeq != 0 && seq <= m_latestSeq) {
        return false; // stale or duplicate: not applied, not acked
    }

    const u32 despawnCount = r.readVarU32();
    for (u32 i = 0; i < despawnCount && r.ok(); ++i) {
        despawn(r.readVarU32());
    }

    while (r.ok() && r.readBool()) {
        const NetId id = r.readVarU32();
        const bool spawn = r.readBool();
        NetTypeId type = 0;
        PeerId owner = kServerOwner;
        bool useBaseline = false;
        u32 baseSeq = 0;
        if (spawn) {
            type = r.readU32();
            owner = r.readVarU32();
        } else {
            useBaseline = r.readBool();
            if (useBaseline) {
                baseSeq = seq - r.readVarU32();
            }
        }
        const u32 propCount = r.readVarU32();
        if (!r.ok()) {
            break;
        }

        std::shared_ptr<NetObject> obj;
        if (auto it = m_objects.find(id); it != m_objects.end()) {
            obj = it->second;
        }
        bool created = false;
        if (!obj) {
            if (!spawn) {
                OX_LOG_ERROR("net", "snapshot {}: delta for unknown object {}", seq, id);
                return false;
            }
            obj = m_factory.create(type, id, owner);
            if (!obj) {
                OX_LOG_ERROR("net", "snapshot {}: no factory for type {:#x} (object {})", seq, type, id);
                return false;
            }
            obj->m_netId = id;
            created = true;
        }
        if (spawn) {
            obj->m_owner = owner;
        }
        const auto& props = obj->properties();
        if (propCount != props.size()) {
            OX_LOG_ERROR("net", "object {} ({}) has {} properties locally, server sent {}", id, obj->typeName(),
                         props.size(), propCount);
            return false;
        }

        History& hist = m_history[id];
        const NetStatePtr prev = hist.states.empty() ? nullptr : hist.states.back().second;
        NetStatePtr base = prev;
        if (useBaseline) {
            auto found = std::find_if(hist.states.begin(), hist.states.end(),
                                      [baseSeq](const auto& e) { return e.first == baseSeq; });
            if (found != hist.states.end()) {
                base = found->second;
            } else {
                OX_LOG_WARN("net", "object {}: baseline {} not in history, using latest", id, baseSeq);
            }
        }
        const bool full = spawn || !useBaseline;
        auto next = std::make_shared<NetState>(props.size());
        for (usize i = 0; i < props.size(); ++i) {
            if (full || r.readBool()) {
                (*next)[i] = props[i]->read(r);
            } else {
                (*next)[i] = base ? (*base)[i] : props[i]->capture();
            }
        }
        if (!r.ok()) {
            OX_LOG_ERROR("net", "snapshot {}: malformed entry for object {}", seq, id);
            return false;
        }
        for (usize i = 0; i < props.size(); ++i) {
            if (!prev || !props[i]->equal((*prev)[i].get(), (*next)[i].get())) {
                props[i]->apply((*next)[i].get());
            }
        }
        hist.states.emplace_back(seq, std::move(next));
        while (hist.states.size() > kClientHistory) {
            hist.states.pop_front();
        }
        if (created) {
            m_objects[id] = obj;
            obj->onNetSpawn();
            if (onSpawn) {
                onSpawn(*obj);
            }
        }
        obj->onSnapshotApplied(serverTime);
    }
    if (!r.ok()) {
        return false;
    }

    if (m_latestSeq == 0) {
        m_ackBits = 0;
    } else {
        const u32 shift = seq - m_latestSeq;
        m_ackBits = shift > 32 ? 0 : ((shift == 32 ? 0u : m_ackBits << shift) | (1u << (shift - 1)));
    }
    m_latestSeq = seq;
    m_ackDirty = true;
    m_lastTick = tick;
    m_lastServerTime = serverTime;
    return true;
}

void ReplicationClient::writeAck(BitWriter& w) const {
    w.writeVarU32(m_latestSeq);
    w.writeU32(m_ackBits);
}

} // namespace ox::net
