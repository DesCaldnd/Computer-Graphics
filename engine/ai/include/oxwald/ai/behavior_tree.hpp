#pragma once

#include <oxwald/ai/blackboard.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ox::ai {

enum class BTStatus : u8 { Idle, Running, Success, Failure };
[[nodiscard]] const char* toString(BTStatus s);

class BehaviorTree;

struct BTContext {
    Blackboard& blackboard;
    f32 dt = 0.f;
    f64 time = 0.0;
    BehaviorTree& tree;
    void* user = nullptr; // owner (entity, controller) set via BehaviorTree::setUserData
};

class BTNode {
public:
    explicit BTNode(std::string name = {}) : m_name(std::move(name)) {}
    virtual ~BTNode() = default;
    BTNode(const BTNode&) = delete;
    BTNode& operator=(const BTNode&) = delete;

    BTStatus tick(BTContext& ctx);
    // Stops a running node (and its running descendants) without completing it.
    void abort(BTContext& ctx);
    void resetStatus();

    [[nodiscard]] virtual const char* typeName() const = 0;
    [[nodiscard]] const std::string& name() const { return m_name; }
    void setName(std::string n) { m_name = std::move(n); }
    [[nodiscard]] BTStatus status() const { return m_status; }
    [[nodiscard]] bool running() const { return m_status == BTStatus::Running; }
    [[nodiscard]] u32 id() const { return m_id; }
    [[nodiscard]] u64 lastTick() const { return m_lastTick; }
    [[nodiscard]] BTNode* parent() const { return m_parent; }
    [[nodiscard]] virtual std::span<const std::unique_ptr<BTNode>> children() const { return {}; }

    // JSON parameters (everything except type/name/children).
    virtual void saveParams(nlohmann::json& j) const {}

protected:
    virtual void onEnter(BTContext& ctx) {}
    virtual BTStatus onTick(BTContext& ctx) = 0;
    virtual void onExit(BTContext& ctx, BTStatus result) {}
    virtual void onAbort(BTContext& ctx);

private:
    friend class BehaviorTree;
    std::string m_name;
    BTStatus m_status = BTStatus::Idle;
    u32 m_id = 0;
    u64 m_lastTick = 0;
    BTNode* m_parent = nullptr;
};

// ---- composites -------------------------------------------------------------------------------------

class BTComposite : public BTNode {
public:
    using BTNode::BTNode;
    BTNode* addChild(std::unique_ptr<BTNode> child);
    [[nodiscard]] std::span<const std::unique_ptr<BTNode>> children() const override { return m_children; }

protected:
    std::vector<std::unique_ptr<BTNode>> m_children;
    usize m_index = 0;
};

// Runs children in order; fails on the first failure.
class BTSequence final : public BTComposite {
public:
    explicit BTSequence(std::string name = "Sequence") : BTComposite(std::move(name)) {}
    [[nodiscard]] const char* typeName() const override { return "Sequence"; }

protected:
    void onEnter(BTContext&) override { m_index = 0; }
    BTStatus onTick(BTContext& ctx) override;
};

// Tries children in priority order; succeeds on the first success. Honors lower-priority aborts.
class BTSelector final : public BTComposite {
public:
    explicit BTSelector(std::string name = "Selector") : BTComposite(std::move(name)) {}
    [[nodiscard]] const char* typeName() const override { return "Selector"; }

protected:
    void onEnter(BTContext&) override { m_index = 0; }
    BTStatus onTick(BTContext& ctx) override;
};

class BTParallel final : public BTComposite {
public:
    enum class Policy : u8 { RequireOne, RequireAll };
    BTParallel(Policy success = Policy::RequireAll, Policy failure = Policy::RequireOne, std::string name = "Parallel")
        : BTComposite(std::move(name)), m_success(success), m_failure(failure) {}
    [[nodiscard]] const char* typeName() const override { return "Parallel"; }
    void saveParams(nlohmann::json& j) const override;

protected:
    void onEnter(BTContext&) override;
    BTStatus onTick(BTContext& ctx) override;

private:
    Policy m_success, m_failure;
    std::vector<BTStatus> m_results;
};

// ---- decorators -------------------------------------------------------------------------------------

class BTDecorator : public BTNode {
public:
    using BTNode::BTNode;
    void setChild(std::unique_ptr<BTNode> child);
    [[nodiscard]] BTNode* child() const { return m_child.empty() ? nullptr : m_child.front().get(); }
    [[nodiscard]] std::span<const std::unique_ptr<BTNode>> children() const override { return m_child; }

protected:
    std::vector<std::unique_ptr<BTNode>> m_child; // 0 or 1 element (span-friendly)
};

class BTInverter final : public BTDecorator {
public:
    explicit BTInverter(std::string name = "Inverter") : BTDecorator(std::move(name)) {}
    [[nodiscard]] const char* typeName() const override { return "Inverter"; }

protected:
    BTStatus onTick(BTContext& ctx) override;
};

class BTSucceeder final : public BTDecorator {
public:
    explicit BTSucceeder(std::string name = "Succeeder") : BTDecorator(std::move(name)) {}
    [[nodiscard]] const char* typeName() const override { return "Succeeder"; }

protected:
    BTStatus onTick(BTContext& ctx) override;
};

// Repeats the child `count` times (-1 = forever). One child completion per tick.
class BTRepeater final : public BTDecorator {
public:
    explicit BTRepeater(i32 count = -1, bool ignoreFailure = false, std::string name = "Repeater")
        : BTDecorator(std::move(name)), m_count(count), m_ignoreFailure(ignoreFailure) {}
    [[nodiscard]] const char* typeName() const override { return "Repeater"; }
    void saveParams(nlohmann::json& j) const override;
    [[nodiscard]] i32 completedIterations() const { return m_done; }

protected:
    void onEnter(BTContext&) override { m_done = 0; }
    BTStatus onTick(BTContext& ctx) override;

private:
    i32 m_count;
    bool m_ignoreFailure;
    i32 m_done = 0;
};

// After the child finishes, fails immediately for `seconds`.
class BTCooldown final : public BTDecorator {
public:
    explicit BTCooldown(f32 seconds, std::string name = "Cooldown") : BTDecorator(std::move(name)), m_seconds(seconds) {}
    [[nodiscard]] const char* typeName() const override { return "Cooldown"; }
    void saveParams(nlohmann::json& j) const override;

protected:
    BTStatus onTick(BTContext& ctx) override;

private:
    f32 m_seconds;
    f64 m_readyAt = -1e300;
};

// Aborts the child and fails when it runs longer than `seconds`.
class BTTimeout final : public BTDecorator {
public:
    explicit BTTimeout(f32 seconds, std::string name = "Timeout") : BTDecorator(std::move(name)), m_seconds(seconds) {}
    [[nodiscard]] const char* typeName() const override { return "Timeout"; }
    void saveParams(nlohmann::json& j) const override;

protected:
    void onEnter(BTContext&) override { m_elapsed = 0.0; }
    BTStatus onTick(BTContext& ctx) override;

private:
    f32 m_seconds;
    f64 m_elapsed = 0.0;
};

enum class BBOp : u8 { IsSet, IsNotSet, Equals, NotEquals, Less, LessOrEqual, Greater, GreaterOrEqual };

// UE-style observer aborts.
//  Self          — while the guarded child runs, a blackboard change making the condition false aborts it.
//  LowerPriority — while a lower-priority sibling (later child of the parent Selector) runs, a change making
//                  the condition true aborts that sibling and re-enters this branch.
enum class BTAbortMode : u8 { None, Self, LowerPriority, Both };

// Blackboard condition. With a child it is a decorator; without one it acts as a condition leaf.
class BTBlackboardCondition final : public BTDecorator {
public:
    BTBlackboardCondition(std::string key, BBOp op, BlackboardValue value = {}, BTAbortMode abort = BTAbortMode::None,
                          std::string name = {});
    [[nodiscard]] const char* typeName() const override { return "BlackboardCondition"; }
    void saveParams(nlohmann::json& j) const override;

    [[nodiscard]] bool evaluate(const Blackboard& bb) const;
    [[nodiscard]] const std::string& key() const { return m_key; }
    [[nodiscard]] BTAbortMode abortMode() const { return m_abort; }
    [[nodiscard]] bool abortsSelf() const { return m_abort == BTAbortMode::Self || m_abort == BTAbortMode::Both; }
    [[nodiscard]] bool abortsLowerPriority() const {
        return m_abort == BTAbortMode::LowerPriority || m_abort == BTAbortMode::Both;
    }
    void markDirty() { m_dirty = true; }
    bool consumeDirty() { return std::exchange(m_dirty, false); }

protected:
    BTStatus onTick(BTContext& ctx) override;

private:
    std::string m_key;
    BBOp m_op;
    BlackboardValue m_value;
    BTAbortMode m_abort;
    bool m_dirty = false;
};

// ---- leaves -----------------------------------------------------------------------------------------

class BTCondition final : public BTNode {
public:
    using Fn = std::function<bool(BTContext&)>;
    BTCondition(std::string name, Fn fn) : BTNode(std::move(name)), m_fn(std::move(fn)) {}
    [[nodiscard]] const char* typeName() const override { return "Condition"; }
    void saveParams(nlohmann::json& j) const override { j["condition"] = name(); }

protected:
    BTStatus onTick(BTContext& ctx) override { return m_fn && m_fn(ctx) ? BTStatus::Success : BTStatus::Failure; }

private:
    Fn m_fn;
};

class BTAction final : public BTNode {
public:
    using Fn = std::function<BTStatus(BTContext&)>;
    using AbortFn = std::function<void(BTContext&)>;
    BTAction(std::string name, Fn fn, AbortFn onAbort = {})
        : BTNode(std::move(name)), m_fn(std::move(fn)), m_abortFn(std::move(onAbort)) {}
    [[nodiscard]] const char* typeName() const override { return "Action"; }
    void saveParams(nlohmann::json& j) const override { j["action"] = name(); }

protected:
    BTStatus onTick(BTContext& ctx) override { return m_fn ? m_fn(ctx) : BTStatus::Failure; }
    void onAbort(BTContext& ctx) override {
        if (m_abortFn) {
            m_abortFn(ctx);
        }
    }

private:
    Fn m_fn;
    AbortFn m_abortFn;
};

class BTWait final : public BTNode {
public:
    explicit BTWait(f32 seconds, std::string name = "Wait") : BTNode(std::move(name)), m_seconds(seconds) {}
    [[nodiscard]] const char* typeName() const override { return "Wait"; }
    void saveParams(nlohmann::json& j) const override { j["seconds"] = m_seconds; }

protected:
    // Elapsed time includes the entering tick: Wait(0.3) at 0.1 s ticks succeeds on the 3rd tick.
    void onEnter(BTContext&) override { m_elapsed = 0.0; }
    BTStatus onTick(BTContext& ctx) override {
        m_elapsed += static_cast<f64>(ctx.dt);
        return m_elapsed >= static_cast<f64>(m_seconds) - 1e-6 ? BTStatus::Success : BTStatus::Running;
    }

private:
    f32 m_seconds;
    f64 m_elapsed = 0.0;
};

class BTSetBlackboard final : public BTNode {
public:
    BTSetBlackboard(std::string key, BlackboardValue value, std::string name = "SetBlackboard")
        : BTNode(std::move(name)), m_key(std::move(key)), m_value(std::move(value)) {}
    [[nodiscard]] const char* typeName() const override { return "SetBlackboard"; }
    void saveParams(nlohmann::json& j) const override;

protected:
    BTStatus onTick(BTContext& ctx) override;

private:
    std::string m_key;
    BlackboardValue m_value;
};

// Embeds another tree (instanced copy of its root).
class BTSubTree final : public BTDecorator {
public:
    BTSubTree(std::string treeName, std::unique_ptr<BTNode> root);
    [[nodiscard]] const char* typeName() const override { return "SubTree"; }
    void saveParams(nlohmann::json& j) const override { j["tree"] = m_treeName; }
    [[nodiscard]] const std::string& treeName() const { return m_treeName; }

protected:
    BTStatus onTick(BTContext& ctx) override;

private:
    std::string m_treeName;
};

// ---- tree -------------------------------------------------------------------------------------------

class BehaviorTree {
public:
    explicit BehaviorTree(std::unique_ptr<BTNode> root, std::shared_ptr<Blackboard> blackboard = nullptr);
    ~BehaviorTree();
    BehaviorTree(const BehaviorTree&) = delete;
    BehaviorTree& operator=(const BehaviorTree&) = delete;

    BTStatus tick(f32 dt);
    void abort();
    void reset();

    [[nodiscard]] Blackboard& blackboard() { return *m_blackboard; }
    [[nodiscard]] const std::shared_ptr<Blackboard>& blackboardPtr() const { return m_blackboard; }
    [[nodiscard]] BTNode* root() const { return m_root.get(); }
    [[nodiscard]] BTStatus status() const { return m_root ? m_root->status() : BTStatus::Idle; }
    [[nodiscard]] u64 tickCount() const { return m_tick; }
    [[nodiscard]] f64 time() const { return m_time; }
    void setUserData(void* user) { m_user = user; }

    [[nodiscard]] BTNode* findNode(std::string_view name) const;
    void forEachNode(const std::function<void(BTNode&, u32 depth)>& fn) const;

    // Debug tracing for the editor visualiser: last status of every node (pre-order).
    struct TraceEntry {
        u32 id;
        u32 parentId; // == id for the root
        u32 depth;
        std::string name;
        std::string type;
        BTStatus status;
        u64 lastTick;
        bool tickedThisFrame;
    };
    [[nodiscard]] std::vector<TraceEntry> trace() const;
    // Called for every node tick (after it returned).
    void setTraceCallback(std::function<void(const BTNode&, BTStatus)> fn) { m_traceFn = std::move(fn); }

    [[nodiscard]] nlohmann::json toJson() const;

private:
    friend class BTNode;
    std::unique_ptr<BTNode> m_root;
    std::shared_ptr<Blackboard> m_blackboard;
    std::vector<Blackboard::ObserverId> m_observers;
    std::function<void(const BTNode&, BTStatus)> m_traceFn;
    u64 m_tick = 0;
    f64 m_time = 0.0;
    void* m_user = nullptr;
};

// ---- builder DSL -------------------------------------------------------------------------------------
//   auto tree = BTBuilder()
//       .selector()
//           .blackboardCondition("target", BBOp::IsSet, {}, BTAbortMode::Both)
//               .sequence("Attack").action("Aim", aim).wait(0.2f).action("Fire", fire).end()
//           .sequence("Patrol").action("NextWaypoint", next).wait(1.f).end()
//       .end()
//       .build();
// Decorators wrap exactly the next node; composites are closed with end().
class BTBuilder {
public:
    BTBuilder& sequence(std::string name = "Sequence");
    BTBuilder& selector(std::string name = "Selector");
    BTBuilder& parallel(BTParallel::Policy success = BTParallel::Policy::RequireAll,
                        BTParallel::Policy failure = BTParallel::Policy::RequireOne, std::string name = "Parallel");
    BTBuilder& inverter(std::string name = "Inverter");
    BTBuilder& succeeder(std::string name = "Succeeder");
    BTBuilder& repeater(i32 count = -1, bool ignoreFailure = false, std::string name = "Repeater");
    BTBuilder& cooldown(f32 seconds, std::string name = "Cooldown");
    BTBuilder& timeout(f32 seconds, std::string name = "Timeout");
    BTBuilder& blackboardCondition(std::string key, BBOp op, BlackboardValue value = {},
                                   BTAbortMode abort = BTAbortMode::None, std::string name = {});
    // Leaf form of a blackboard condition (no child, never aborts).
    BTBuilder& blackboardCheck(std::string key, BBOp op, BlackboardValue value = {}, std::string name = {});
    BTBuilder& condition(std::string name, BTCondition::Fn fn);
    BTBuilder& action(std::string name, BTAction::Fn fn, BTAction::AbortFn onAbort = {});
    BTBuilder& wait(f32 seconds, std::string name = "Wait");
    BTBuilder& setBlackboard(std::string key, BlackboardValue value);
    BTBuilder& subTree(std::string treeName, std::unique_ptr<BTNode> root);
    BTBuilder& node(std::unique_ptr<BTNode> node); // any custom node
    BTBuilder& end();

    std::unique_ptr<BTNode> buildRoot();
    std::unique_ptr<BehaviorTree> build(std::shared_ptr<Blackboard> blackboard = nullptr);

private:
    void push(std::unique_ptr<BTNode> node, bool leaf = false);
    void closeDecorators();
    std::unique_ptr<BTNode> m_root;
    std::vector<BTNode*> m_stack;
};

// ---- JSON factory ----------------------------------------------------------------------------------
// Node JSON: {"type": "Sequence", "name": "...", "children": [...]}, decorators use "child": {...},
// leaf parameters are flat keys ("seconds", "key", "op", "value", "abort", "action", "condition", "tree", ...).
class BTFactory {
public:
    using NodeCreator = std::function<std::unique_ptr<BTNode>(const nlohmann::json& j, const BTFactory& factory)>;

    BTFactory(); // registers all built-in node types
    void registerNode(const std::string& type, NodeCreator creator);
    void registerAction(const std::string& name, BTAction::Fn fn, BTAction::AbortFn onAbort = {});
    void registerCondition(const std::string& name, BTCondition::Fn fn);
    void registerTree(const std::string& name, nlohmann::json definition); // for "SubTree" references

    [[nodiscard]] std::unique_ptr<BTNode> createNode(const nlohmann::json& j) const;
    [[nodiscard]] std::unique_ptr<BehaviorTree> load(const nlohmann::json& j, std::shared_ptr<Blackboard> bb = nullptr) const;
    [[nodiscard]] std::unique_ptr<BehaviorTree> loadFile(const std::filesystem::path& path,
                                                         std::shared_ptr<Blackboard> bb = nullptr) const;
    [[nodiscard]] static nlohmann::json save(const BTNode& node);
    static bool saveFile(const BTNode& root, const std::filesystem::path& path);

    [[nodiscard]] const BTAction::Fn* findAction(const std::string& name) const;
    [[nodiscard]] const BTCondition::Fn* findCondition(const std::string& name) const;

private:
    std::unordered_map<std::string, NodeCreator> m_creators;
    std::unordered_map<std::string, std::pair<BTAction::Fn, BTAction::AbortFn>> m_actions;
    std::unordered_map<std::string, BTCondition::Fn> m_conditions;
    std::unordered_map<std::string, nlohmann::json> m_trees;
};

} // namespace ox::ai
