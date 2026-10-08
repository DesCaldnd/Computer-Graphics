#include <oxwald/ai/behavior_tree.hpp>
#include <oxwald/core/log.hpp>

#include <fstream>
#include <utility>

namespace ox::ai {

const char* toString(BTStatus s) {
    switch (s) {
    case BTStatus::Idle: return "Idle";
    case BTStatus::Running: return "Running";
    case BTStatus::Success: return "Success";
    case BTStatus::Failure: return "Failure";
    }
    return "?";
}

// ---- BTNode ---------------------------------------------------------------------------------------------

BTStatus BTNode::tick(BTContext& ctx) {
    if (m_status != BTStatus::Running) {
        onEnter(ctx);
    }
    const BTStatus s = onTick(ctx);
    m_status = s;
    m_lastTick = ctx.tree.m_tick;
    if (s != BTStatus::Running) {
        onExit(ctx, s);
    }
    if (ctx.tree.m_traceFn) {
        ctx.tree.m_traceFn(*this, s);
    }
    return s;
}

void BTNode::abort(BTContext& ctx) {
    if (m_status != BTStatus::Running) {
        return;
    }
    onAbort(ctx);
    m_status = BTStatus::Failure;
    onExit(ctx, BTStatus::Failure);
}

void BTNode::onAbort(BTContext& ctx) {
    for (const auto& c : children()) {
        c->abort(ctx);
    }
}

void BTNode::resetStatus() {
    m_status = BTStatus::Idle;
    for (const auto& c : children()) {
        c->resetStatus();
    }
}

// ---- composites ------------------------------------------------------------------------------------------

BTNode* BTComposite::addChild(std::unique_ptr<BTNode> child) {
    m_children.push_back(std::move(child));
    return m_children.back().get();
}

BTStatus BTSequence::onTick(BTContext& ctx) {
    while (m_index < m_children.size()) {
        const BTStatus s = m_children[m_index]->tick(ctx);
        if (s == BTStatus::Running) {
            return s;
        }
        if (s == BTStatus::Failure) {
            return BTStatus::Failure;
        }
        ++m_index;
    }
    return BTStatus::Success;
}

BTStatus BTSelector::onTick(BTContext& ctx) {
    // Lower-priority abort: a higher-priority guarded branch whose condition became true pre-empts the
    // running branch.
    if (m_index < m_children.size() && m_children[m_index]->running()) {
        for (usize i = 0; i < m_index; ++i) {
            auto* cond = dynamic_cast<BTBlackboardCondition*>(m_children[i].get());
            if (cond != nullptr && cond->abortsLowerPriority() && cond->consumeDirty() && cond->evaluate(ctx.blackboard)) {
                m_children[m_index]->abort(ctx);
                m_index = i;
                break;
            }
        }
    }
    while (m_index < m_children.size()) {
        const BTStatus s = m_children[m_index]->tick(ctx);
        if (s == BTStatus::Running) {
            return s;
        }
        if (s == BTStatus::Success) {
            return BTStatus::Success;
        }
        ++m_index;
    }
    return BTStatus::Failure;
}

void BTParallel::onEnter(BTContext&) { m_results.assign(m_children.size(), BTStatus::Idle); }

BTStatus BTParallel::onTick(BTContext& ctx) {
    usize successes = 0, failures = 0;
    for (usize i = 0; i < m_children.size(); ++i) {
        if (m_results[i] == BTStatus::Idle || m_results[i] == BTStatus::Running) {
            m_results[i] = m_children[i]->tick(ctx);
        }
        successes += m_results[i] == BTStatus::Success ? 1 : 0;
        failures += m_results[i] == BTStatus::Failure ? 1 : 0;
    }
    const usize n = m_children.size();
    BTStatus result = BTStatus::Running;
    if ((m_failure == Policy::RequireOne && failures > 0) || (m_failure == Policy::RequireAll && failures == n)) {
        result = BTStatus::Failure;
    } else if ((m_success == Policy::RequireOne && successes > 0) || (m_success == Policy::RequireAll && successes == n)) {
        result = BTStatus::Success;
    } else if (successes + failures == n) {
        result = BTStatus::Failure; // everything finished without satisfying the success policy
    }
    if (result != BTStatus::Running) {
        for (const auto& c : m_children) {
            c->abort(ctx);
        }
    }
    return result;
}

void BTParallel::saveParams(nlohmann::json& j) const {
    j["success"] = m_success == Policy::RequireAll ? "RequireAll" : "RequireOne";
    j["failure"] = m_failure == Policy::RequireAll ? "RequireAll" : "RequireOne";
}

// ---- decorators ------------------------------------------------------------------------------------------

void BTDecorator::setChild(std::unique_ptr<BTNode> child) {
    m_child.clear();
    if (child) {
        m_child.push_back(std::move(child));
    }
}

BTStatus BTInverter::onTick(BTContext& ctx) {
    if (child() == nullptr) {
        return BTStatus::Failure;
    }
    const BTStatus s = child()->tick(ctx);
    if (s == BTStatus::Success) {
        return BTStatus::Failure;
    }
    if (s == BTStatus::Failure) {
        return BTStatus::Success;
    }
    return s;
}

BTStatus BTSucceeder::onTick(BTContext& ctx) {
    if (child() == nullptr) {
        return BTStatus::Success;
    }
    const BTStatus s = child()->tick(ctx);
    return s == BTStatus::Running ? s : BTStatus::Success;
}

BTStatus BTRepeater::onTick(BTContext& ctx) {
    if (child() == nullptr) {
        return BTStatus::Failure;
    }
    const BTStatus s = child()->tick(ctx);
    if (s == BTStatus::Running) {
        return s;
    }
    if (s == BTStatus::Failure && !m_ignoreFailure) {
        return BTStatus::Failure;
    }
    ++m_done;
    if (m_count >= 0 && m_done >= m_count) {
        return BTStatus::Success;
    }
    return BTStatus::Running;
}

void BTRepeater::saveParams(nlohmann::json& j) const {
    j["count"] = m_count;
    j["ignoreFailure"] = m_ignoreFailure;
}

BTStatus BTCooldown::onTick(BTContext& ctx) {
    if (child() == nullptr) {
        return BTStatus::Failure;
    }
    if (!child()->running() && ctx.time < m_readyAt) {
        return BTStatus::Failure;
    }
    const BTStatus s = child()->tick(ctx);
    if (s != BTStatus::Running) {
        m_readyAt = ctx.time + static_cast<f64>(m_seconds);
    }
    return s;
}

void BTCooldown::saveParams(nlohmann::json& j) const { j["seconds"] = m_seconds; }

BTStatus BTTimeout::onTick(BTContext& ctx) {
    if (child() == nullptr) {
        return BTStatus::Failure;
    }
    m_elapsed += static_cast<f64>(ctx.dt);
    if (m_elapsed >= static_cast<f64>(m_seconds) - 1e-6) {
        child()->abort(ctx);
        return BTStatus::Failure;
    }
    return child()->tick(ctx);
}

void BTTimeout::saveParams(nlohmann::json& j) const { j["seconds"] = m_seconds; }

namespace {

const char* opName(BBOp op) {
    switch (op) {
    case BBOp::IsSet: return "IsSet";
    case BBOp::IsNotSet: return "IsNotSet";
    case BBOp::Equals: return "Equals";
    case BBOp::NotEquals: return "NotEquals";
    case BBOp::Less: return "Less";
    case BBOp::LessOrEqual: return "LessOrEqual";
    case BBOp::Greater: return "Greater";
    case BBOp::GreaterOrEqual: return "GreaterOrEqual";
    }
    return "IsSet";
}

BBOp opFromName(const std::string& s) {
    for (BBOp op : {BBOp::IsSet, BBOp::IsNotSet, BBOp::Equals, BBOp::NotEquals, BBOp::Less, BBOp::LessOrEqual, BBOp::Greater,
                    BBOp::GreaterOrEqual}) {
        if (s == opName(op)) {
            return op;
        }
    }
    OX_LOG_WARN("ai", "BT: unknown blackboard op '{}'", s);
    return BBOp::IsSet;
}

const char* abortName(BTAbortMode m) {
    switch (m) {
    case BTAbortMode::None: return "None";
    case BTAbortMode::Self: return "Self";
    case BTAbortMode::LowerPriority: return "LowerPriority";
    case BTAbortMode::Both: return "Both";
    }
    return "None";
}

BTAbortMode abortFromName(const std::string& s) {
    for (BTAbortMode m : {BTAbortMode::None, BTAbortMode::Self, BTAbortMode::LowerPriority, BTAbortMode::Both}) {
        if (s == abortName(m)) {
            return m;
        }
    }
    return BTAbortMode::None;
}

std::optional<f64> asNumber(const BlackboardValue& v) {
    if (auto* i = std::get_if<i32>(&v)) return static_cast<f64>(*i);
    if (auto* f = std::get_if<f32>(&v)) return static_cast<f64>(*f);
    if (auto* u = std::get_if<u64>(&v)) return static_cast<f64>(*u);
    if (auto* b = std::get_if<bool>(&v)) return *b ? 1.0 : 0.0;
    return std::nullopt;
}

} // namespace

BTBlackboardCondition::BTBlackboardCondition(std::string key, BBOp op, BlackboardValue value, BTAbortMode abort,
                                             std::string name)
    : BTDecorator(name.empty() ? "BB:" + key : std::move(name)), m_key(std::move(key)), m_op(op), m_value(std::move(value)),
      m_abort(abort) {}

bool BTBlackboardCondition::evaluate(const Blackboard& bb) const {
    const BlackboardValue* v = bb.find(m_key);
    const bool set = v != nullptr && !std::holds_alternative<std::monostate>(*v);
    switch (m_op) {
    case BBOp::IsSet: return set;
    case BBOp::IsNotSet: return !set;
    default: break;
    }
    if (!set) {
        return m_op == BBOp::NotEquals;
    }
    const auto a = asNumber(*v), b = asNumber(m_value);
    switch (m_op) {
    case BBOp::Equals:
        return (a && b) ? *a == *b : *v == m_value;
    case BBOp::NotEquals:
        return (a && b) ? *a != *b : !(*v == m_value);
    case BBOp::Less: return a && b && *a < *b;
    case BBOp::LessOrEqual: return a && b && *a <= *b;
    case BBOp::Greater: return a && b && *a > *b;
    case BBOp::GreaterOrEqual: return a && b && *a >= *b;
    default: return false;
    }
}

BTStatus BTBlackboardCondition::onTick(BTContext& ctx) {
    BTNode* c = child();
    if (c != nullptr && c->running()) {
        if (abortsSelf() && consumeDirty() && !evaluate(ctx.blackboard)) {
            c->abort(ctx);
            return BTStatus::Failure;
        }
        return c->tick(ctx);
    }
    m_dirty = false;
    if (!evaluate(ctx.blackboard)) {
        return BTStatus::Failure;
    }
    return c != nullptr ? c->tick(ctx) : BTStatus::Success;
}

void BTBlackboardCondition::saveParams(nlohmann::json& j) const {
    j["key"] = m_key;
    j["op"] = opName(m_op);
    if (!std::holds_alternative<std::monostate>(m_value)) {
        toJson(j["value"], m_value);
    }
    if (m_abort != BTAbortMode::None) {
        j["abort"] = abortName(m_abort);
    }
}

// ---- leaves ----------------------------------------------------------------------------------------------

BTStatus BTSetBlackboard::onTick(BTContext& ctx) {
    ctx.blackboard.setValue(m_key, m_value);
    return BTStatus::Success;
}

void BTSetBlackboard::saveParams(nlohmann::json& j) const {
    j["key"] = m_key;
    toJson(j["value"], m_value);
}

BTSubTree::BTSubTree(std::string treeName, std::unique_ptr<BTNode> root) : BTDecorator(treeName), m_treeName(std::move(treeName)) {
    setChild(std::move(root));
}

BTStatus BTSubTree::onTick(BTContext& ctx) { return child() != nullptr ? child()->tick(ctx) : BTStatus::Failure; }

// ---- tree ------------------------------------------------------------------------------------------------

BehaviorTree::BehaviorTree(std::unique_ptr<BTNode> root, std::shared_ptr<Blackboard> blackboard)
    : m_root(std::move(root)), m_blackboard(blackboard ? std::move(blackboard) : std::make_shared<Blackboard>()) {
    u32 nextId = 0;
    std::function<void(BTNode&, BTNode*)> visit = [&](BTNode& n, BTNode* parent) {
        n.m_id = nextId++;
        n.m_parent = parent;
        if (auto* cond = dynamic_cast<BTBlackboardCondition*>(&n); cond != nullptr && cond->abortMode() != BTAbortMode::None) {
            m_observers.push_back(m_blackboard->observe(cond->key(), [cond](const std::string&, const BlackboardValue&,
                                                                            const BlackboardValue&) { cond->markDirty(); }));
        }
        for (const auto& c : n.children()) {
            visit(*c, &n);
        }
    };
    if (m_root) {
        visit(*m_root, nullptr);
    }
}

BehaviorTree::~BehaviorTree() {
    for (auto id : m_observers) {
        m_blackboard->unobserve(id);
    }
}

BTStatus BehaviorTree::tick(f32 dt) {
    if (!m_root) {
        return BTStatus::Failure;
    }
    ++m_tick;
    m_time += static_cast<f64>(dt);
    BTContext ctx{*m_blackboard, dt, m_time, *this, m_user};
    return m_root->tick(ctx);
}

void BehaviorTree::abort() {
    if (m_root) {
        BTContext ctx{*m_blackboard, 0.f, m_time, *this, m_user};
        m_root->abort(ctx);
    }
}

void BehaviorTree::reset() {
    abort();
    if (m_root) {
        m_root->resetStatus();
    }
}

void BehaviorTree::forEachNode(const std::function<void(BTNode&, u32)>& fn) const {
    std::function<void(BTNode&, u32)> visit = [&](BTNode& n, u32 depth) {
        fn(n, depth);
        for (const auto& c : n.children()) {
            visit(*c, depth + 1);
        }
    };
    if (m_root) {
        visit(*m_root, 0);
    }
}

BTNode* BehaviorTree::findNode(std::string_view name) const {
    BTNode* found = nullptr;
    forEachNode([&](BTNode& n, u32) {
        if (found == nullptr && n.name() == name) {
            found = &n;
        }
    });
    return found;
}

std::vector<BehaviorTree::TraceEntry> BehaviorTree::trace() const {
    std::vector<TraceEntry> out;
    forEachNode([&](BTNode& n, u32 depth) {
        out.push_back({n.id(), n.parent() != nullptr ? n.parent()->id() : n.id(), depth, n.name(), n.typeName(), n.status(),
                       n.lastTick(), n.lastTick() == m_tick && m_tick != 0});
    });
    return out;
}

nlohmann::json BehaviorTree::toJson() const { return m_root ? BTFactory::save(*m_root) : nlohmann::json{}; }

// ---- builder ---------------------------------------------------------------------------------------------

void BTBuilder::push(std::unique_ptr<BTNode> node, bool leaf) {
    BTNode* raw = node.get();
    const bool container = !leaf && (dynamic_cast<BTComposite*>(raw) != nullptr ||
                                     (dynamic_cast<BTDecorator*>(raw) != nullptr && dynamic_cast<BTSubTree*>(raw) == nullptr));
    if (m_stack.empty()) {
        if (m_root) {
            OX_LOG_ERROR("ai", "BTBuilder: more than one root node");
            return;
        }
        m_root = std::move(node);
    } else if (auto* comp = dynamic_cast<BTComposite*>(m_stack.back())) {
        comp->addChild(std::move(node));
    } else if (auto* deco = dynamic_cast<BTDecorator*>(m_stack.back())) {
        deco->setChild(std::move(node));
    }
    if (container) {
        m_stack.push_back(raw);
    } else {
        closeDecorators();
    }
}

void BTBuilder::closeDecorators() {
    while (!m_stack.empty()) {
        auto* deco = dynamic_cast<BTDecorator*>(m_stack.back());
        if (deco == nullptr || deco->child() == nullptr) {
            break;
        }
        m_stack.pop_back();
    }
}

BTBuilder& BTBuilder::sequence(std::string name) { push(std::make_unique<BTSequence>(std::move(name))); return *this; }
BTBuilder& BTBuilder::selector(std::string name) { push(std::make_unique<BTSelector>(std::move(name))); return *this; }
BTBuilder& BTBuilder::parallel(BTParallel::Policy s, BTParallel::Policy f, std::string name) {
    push(std::make_unique<BTParallel>(s, f, std::move(name)));
    return *this;
}
BTBuilder& BTBuilder::inverter(std::string name) { push(std::make_unique<BTInverter>(std::move(name))); return *this; }
BTBuilder& BTBuilder::succeeder(std::string name) { push(std::make_unique<BTSucceeder>(std::move(name))); return *this; }
BTBuilder& BTBuilder::repeater(i32 count, bool ignoreFailure, std::string name) {
    push(std::make_unique<BTRepeater>(count, ignoreFailure, std::move(name)));
    return *this;
}
BTBuilder& BTBuilder::cooldown(f32 seconds, std::string name) { push(std::make_unique<BTCooldown>(seconds, std::move(name))); return *this; }
BTBuilder& BTBuilder::timeout(f32 seconds, std::string name) { push(std::make_unique<BTTimeout>(seconds, std::move(name))); return *this; }
BTBuilder& BTBuilder::blackboardCondition(std::string key, BBOp op, BlackboardValue value, BTAbortMode abort, std::string name) {
    push(std::make_unique<BTBlackboardCondition>(std::move(key), op, std::move(value), abort, std::move(name)));
    return *this;
}
BTBuilder& BTBuilder::blackboardCheck(std::string key, BBOp op, BlackboardValue value, std::string name) {
    push(std::make_unique<BTBlackboardCondition>(std::move(key), op, std::move(value), BTAbortMode::None, std::move(name)), true);
    return *this;
}
BTBuilder& BTBuilder::condition(std::string name, BTCondition::Fn fn) {
    push(std::make_unique<BTCondition>(std::move(name), std::move(fn)));
    return *this;
}
BTBuilder& BTBuilder::action(std::string name, BTAction::Fn fn, BTAction::AbortFn onAbort) {
    push(std::make_unique<BTAction>(std::move(name), std::move(fn), std::move(onAbort)));
    return *this;
}
BTBuilder& BTBuilder::wait(f32 seconds, std::string name) { push(std::make_unique<BTWait>(seconds, std::move(name))); return *this; }
BTBuilder& BTBuilder::setBlackboard(std::string key, BlackboardValue value) {
    push(std::make_unique<BTSetBlackboard>(std::move(key), std::move(value)));
    return *this;
}
BTBuilder& BTBuilder::subTree(std::string treeName, std::unique_ptr<BTNode> root) {
    push(std::make_unique<BTSubTree>(std::move(treeName), std::move(root)));
    return *this;
}
BTBuilder& BTBuilder::node(std::unique_ptr<BTNode> n) { push(std::move(n)); return *this; }

BTBuilder& BTBuilder::end() {
    // Pop the innermost open composite (and any decorators wrapping it, which are now complete).
    while (!m_stack.empty() && dynamic_cast<BTComposite*>(m_stack.back()) == nullptr) {
        m_stack.pop_back();
    }
    if (!m_stack.empty()) {
        m_stack.pop_back();
    }
    closeDecorators();
    return *this;
}

std::unique_ptr<BTNode> BTBuilder::buildRoot() {
    m_stack.clear();
    return std::move(m_root);
}

std::unique_ptr<BehaviorTree> BTBuilder::build(std::shared_ptr<Blackboard> blackboard) {
    return std::make_unique<BehaviorTree>(buildRoot(), std::move(blackboard));
}

// ---- JSON factory ----------------------------------------------------------------------------------------

BTFactory::BTFactory() {
    auto name = [](const nlohmann::json& j, const char* def) { return j.value("name", std::string(def)); };
    registerNode("Sequence", [=](const nlohmann::json& j, const BTFactory&) { return std::make_unique<BTSequence>(name(j, "Sequence")); });
    registerNode("Selector", [=](const nlohmann::json& j, const BTFactory&) { return std::make_unique<BTSelector>(name(j, "Selector")); });
    registerNode("Parallel", [=](const nlohmann::json& j, const BTFactory&) {
        auto pol = [](const std::string& s) { return s == "RequireOne" ? BTParallel::Policy::RequireOne : BTParallel::Policy::RequireAll; };
        return std::make_unique<BTParallel>(pol(j.value("success", "RequireAll")),
                                            j.value("failure", "RequireOne") == "RequireAll" ? BTParallel::Policy::RequireAll
                                                                                              : BTParallel::Policy::RequireOne,
                                            name(j, "Parallel"));
    });
    registerNode("Inverter", [=](const nlohmann::json& j, const BTFactory&) { return std::make_unique<BTInverter>(name(j, "Inverter")); });
    registerNode("Succeeder", [=](const nlohmann::json& j, const BTFactory&) { return std::make_unique<BTSucceeder>(name(j, "Succeeder")); });
    registerNode("Repeater", [=](const nlohmann::json& j, const BTFactory&) {
        return std::make_unique<BTRepeater>(j.value("count", -1), j.value("ignoreFailure", false), name(j, "Repeater"));
    });
    registerNode("Cooldown", [=](const nlohmann::json& j, const BTFactory&) {
        return std::make_unique<BTCooldown>(j.value("seconds", 1.f), name(j, "Cooldown"));
    });
    registerNode("Timeout", [=](const nlohmann::json& j, const BTFactory&) {
        return std::make_unique<BTTimeout>(j.value("seconds", 1.f), name(j, "Timeout"));
    });
    registerNode("Wait", [=](const nlohmann::json& j, const BTFactory&) { return std::make_unique<BTWait>(j.value("seconds", 1.f), name(j, "Wait")); });
    registerNode("BlackboardCondition", [](const nlohmann::json& j, const BTFactory&) {
        return std::make_unique<BTBlackboardCondition>(
            j.value("key", std::string{}), opFromName(j.value("op", "IsSet")),
            j.contains("value") ? blackboardValueFromJson(j["value"]) : BlackboardValue{},
            abortFromName(j.value("abort", "None")), j.value("name", std::string{}));
    });
    registerNode("SetBlackboard", [=](const nlohmann::json& j, const BTFactory&) {
        return std::make_unique<BTSetBlackboard>(j.value("key", std::string{}),
                                                 j.contains("value") ? blackboardValueFromJson(j["value"]) : BlackboardValue{},
                                                 name(j, "SetBlackboard"));
    });
    registerNode("Action", [](const nlohmann::json& j, const BTFactory& f) -> std::unique_ptr<BTNode> {
        const std::string key = j.value("action", j.value("name", std::string{}));
        auto it = f.m_actions.find(key);
        if (it == f.m_actions.end()) {
            OX_LOG_ERROR("ai", "BT: action '{}' is not registered", key);
            return nullptr;
        }
        return std::make_unique<BTAction>(key, it->second.first, it->second.second);
    });
    registerNode("Condition", [](const nlohmann::json& j, const BTFactory& f) -> std::unique_ptr<BTNode> {
        const std::string key = j.value("condition", j.value("name", std::string{}));
        auto it = f.m_conditions.find(key);
        if (it == f.m_conditions.end()) {
            OX_LOG_ERROR("ai", "BT: condition '{}' is not registered", key);
            return nullptr;
        }
        return std::make_unique<BTCondition>(key, it->second);
    });
    registerNode("SubTree", [](const nlohmann::json& j, const BTFactory& f) -> std::unique_ptr<BTNode> {
        const std::string tree = j.value("tree", std::string{});
        auto it = f.m_trees.find(tree);
        if (it == f.m_trees.end()) {
            OX_LOG_ERROR("ai", "BT: subtree '{}' is not registered", tree);
            return nullptr;
        }
        auto root = f.createNode(it->second.contains("root") ? it->second["root"] : it->second);
        if (!root) {
            return nullptr;
        }
        return std::make_unique<BTSubTree>(tree, std::move(root));
    });
}

void BTFactory::registerNode(const std::string& type, NodeCreator creator) { m_creators[type] = std::move(creator); }

void BTFactory::registerAction(const std::string& name, BTAction::Fn fn, BTAction::AbortFn onAbort) {
    m_actions[name] = {std::move(fn), std::move(onAbort)};
}

void BTFactory::registerCondition(const std::string& name, BTCondition::Fn fn) { m_conditions[name] = std::move(fn); }

void BTFactory::registerTree(const std::string& name, nlohmann::json definition) { m_trees[name] = std::move(definition); }

const BTAction::Fn* BTFactory::findAction(const std::string& name) const {
    auto it = m_actions.find(name);
    return it != m_actions.end() ? &it->second.first : nullptr;
}

const BTCondition::Fn* BTFactory::findCondition(const std::string& name) const {
    auto it = m_conditions.find(name);
    return it != m_conditions.end() ? &it->second : nullptr;
}

std::unique_ptr<BTNode> BTFactory::createNode(const nlohmann::json& j) const {
    if (!j.is_object() || !j.contains("type")) {
        OX_LOG_ERROR("ai", "BT: node without \"type\"");
        return nullptr;
    }
    const std::string type = j["type"].get<std::string>();
    auto it = m_creators.find(type);
    if (it == m_creators.end()) {
        OX_LOG_ERROR("ai", "BT: unknown node type '{}'", type);
        return nullptr;
    }
    std::unique_ptr<BTNode> node = it->second(j, *this);
    if (!node) {
        return nullptr;
    }
    if (auto* comp = dynamic_cast<BTComposite*>(node.get()); comp != nullptr && j.contains("children")) {
        for (const auto& c : j["children"]) {
            auto child = createNode(c);
            if (!child) {
                return nullptr;
            }
            comp->addChild(std::move(child));
        }
    } else if (auto* deco = dynamic_cast<BTDecorator*>(node.get());
               deco != nullptr && dynamic_cast<BTSubTree*>(deco) == nullptr && j.contains("child")) {
        auto child = createNode(j["child"]);
        if (!child) {
            return nullptr;
        }
        deco->setChild(std::move(child));
    }
    return node;
}

std::unique_ptr<BehaviorTree> BTFactory::load(const nlohmann::json& j, std::shared_ptr<Blackboard> bb) const {
    try {
        const nlohmann::json& rootJson = j.contains("root") ? j["root"] : j;
        auto root = createNode(rootJson);
        if (!root) {
            return nullptr;
        }
        auto tree = std::make_unique<BehaviorTree>(std::move(root), std::move(bb));
        if (j.contains("blackboard")) {
            tree->blackboard().loadJson(j["blackboard"]);
        }
        return tree;
    } catch (const std::exception& e) {
        OX_LOG_ERROR("ai", "BT: invalid JSON: {}", e.what());
        return nullptr;
    }
}

std::unique_ptr<BehaviorTree> BTFactory::loadFile(const std::filesystem::path& path, std::shared_ptr<Blackboard> bb) const {
    std::ifstream f(path);
    if (!f) {
        OX_LOG_ERROR("ai", "BT: cannot open '{}'", path.string());
        return nullptr;
    }
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded()) {
        OX_LOG_ERROR("ai", "BT: '{}' is not valid JSON", path.string());
        return nullptr;
    }
    return load(j, std::move(bb));
}

nlohmann::json BTFactory::save(const BTNode& node) {
    nlohmann::json j;
    j["type"] = node.typeName();
    j["name"] = node.name();
    node.saveParams(j);
    if (dynamic_cast<const BTComposite*>(&node) != nullptr) {
        j["children"] = nlohmann::json::array();
        for (const auto& c : node.children()) {
            j["children"].push_back(save(*c));
        }
    } else if (dynamic_cast<const BTDecorator*>(&node) != nullptr && dynamic_cast<const BTSubTree*>(&node) == nullptr &&
               !node.children().empty()) {
        j["child"] = save(*node.children().front());
    }
    return j;
}

bool BTFactory::saveFile(const BTNode& root, const std::filesystem::path& path) {
    std::ofstream f(path);
    if (!f) {
        return false;
    }
    f << save(root).dump(2);
    return static_cast<bool>(f);
}

} // namespace ox::ai
