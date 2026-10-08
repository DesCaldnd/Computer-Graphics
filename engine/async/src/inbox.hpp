#pragma once

#include <oxwald/async/detail/context.hpp>

#include <coroutine>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace ox::detail {

// Thread-safe queue of things the scheduler thread must process during its next tick.
struct InboxItem {
    enum class Kind : u8 {
        Resume,     // an off-thread chain hopped back (mainThread()) or a cancelled chain is parked
        DriverDone, // a whenAll/whenAny branch finished off-thread
        RootDone,   // a root task finished off-thread
        Signal,     // a remote wait (future/event) completed
        Call,       // IExecutor::post
    };
    Kind kind = Kind::Call;
    AsyncContext* ctx = nullptr;
    std::coroutine_handle<> handle;
    u64 id = 0;
    std::function<void()> fn;
};

struct Inbox {
    std::mutex mutex;
    std::vector<InboxItem> items;

    void push(InboxItem item) {
        std::lock_guard lock(mutex);
        items.push_back(std::move(item));
    }
    std::vector<InboxItem> take() {
        std::vector<InboxItem> out;
        std::lock_guard lock(mutex);
        out.swap(items);
        return out;
    }
    bool empty() {
        std::lock_guard lock(mutex);
        return items.empty();
    }
};

} // namespace ox::detail
