#include "async_test_utils.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace ox;

namespace {

Task<int> value(int v) { co_return v; }
Task<int> add(int a, int b) { co_return co_await value(a) + co_await value(b); }
Task<void> thrower() {
    co_await value(1);
    throw std::runtime_error("boom");
}
Task<int> catcher() {
    try {
        co_await thrower();
    } catch (const std::runtime_error& e) {
        co_return std::string(e.what()) == "boom" ? 7 : -1;
    }
    co_return 0;
}

// Drives a Task to completion without a scheduler (only valid for tasks that never suspend for real).
template <class T>
T runSync(Task<T> t) {
    auto h = t.handle();
    h.resume();
    EXPECT_TRUE(h.done());
    return h.promise().result();
}

Generator<int> range(int n) {
    for (int i = 0; i < n; ++i) {
        co_yield i;
    }
}
Generator<std::string> words() {
    co_yield "a";
    co_yield std::string("b");
}

} // namespace

TEST(AsyncTask, LazyAndSymmetricTransfer) {
    bool ran = false;
    auto body = [&]() -> Task<int> { // named: the closure must outlive the coroutine
        ran = true;
        co_return 3;
    };
    auto t = body();
    EXPECT_FALSE(ran);
    EXPECT_EQ(runSync(std::move(t)), 3);
    EXPECT_TRUE(ran);
    EXPECT_EQ(runSync(add(2, 5)), 7);
}

TEST(AsyncTask, DeepRecursionDoesNotOverflow) {
    // Symmetric transfer: 100k nested awaits must not grow the stack.
    std::function<Task<int>(int)> depth = [&](int n) -> Task<int> {
        if (n == 0) {
            co_return 0;
        }
        co_return 1 + co_await depth(n - 1);
    };
    EXPECT_EQ(runSync(depth(100000)), 100000);
}

TEST(AsyncTask, ExceptionsPropagateToAwaiter) {
    EXPECT_EQ(runSync(catcher()), 7);
    auto t = thrower();
    t.handle().resume();
    EXPECT_THROW(t.handle().promise().result(), std::runtime_error);
}

TEST(AsyncTask, DestroyingSuspendedTaskRunsDestructors) {
    oxtest::LiveCounter::reset();
    Promise<int> never;
    {
        auto t = [](Future<int> f) -> Task<int> {
            oxtest::LiveCounter c;
            co_return co_await f;
        }(never.future());
        t.handle().resume(); // suspends on the future (inline continuation, no scheduler)
        EXPECT_EQ(oxtest::LiveCounter::alive, 1);
    }
    EXPECT_EQ(oxtest::LiveCounter::alive, 0);
    // The continuation was detached: completing the promise later must not resume a dead frame.
    never.setValue(1);
}

TEST(AsyncGenerator, Iterates) {
    std::vector<int> v;
    for (int i : range(5)) {
        v.push_back(i);
    }
    EXPECT_EQ(v, (std::vector<int>{0, 1, 2, 3, 4}));
    std::string s;
    for (const std::string& w : words()) {
        s += w;
    }
    EXPECT_EQ(s, "ab");
}

TEST(AsyncFuture, PromiseBasics) {
    Promise<int> p;
    Future<int> f = p.future();
    EXPECT_FALSE(f.isReady());
    int seen = 0;
    f.onReady([&] { seen = f.get(); });
    EXPECT_TRUE(p.setValue(5));
    EXPECT_FALSE(p.setValue(6));
    EXPECT_TRUE(f.isReady());
    EXPECT_EQ(f.get(), 5);
    EXPECT_EQ(seen, 5);
}

TEST(AsyncFuture, BrokenPromiseAndErrors) {
    Future<int> f;
    {
        Promise<int> p;
        f = p.future();
    }
    ASSERT_TRUE(f.isReady());
    EXPECT_TRUE(f.hasError());
    EXPECT_THROW(f.get(), BrokenPromise);

    auto e = makeErrorFuture<std::string>("nope");
    EXPECT_EQ(e.errorMessage(), "nope");
}

TEST(AsyncFuture, AwaitCallbackAndRunAsync) {
    std::function<void(int)> pending;
    Future<int> f = awaitCallback<int>([&](auto resume) { pending = resume; });
    EXPECT_FALSE(f.isReady());
    pending(41);
    EXPECT_EQ(f.get(), 41);

    Future<void> dropped = awaitCallback([](auto) { /* never calls resume */ });
    EXPECT_TRUE(dropped.hasError());

    ThreadPoolExecutor pool(2);
    Future<int> r = runAsync(pool, [] { return 6 * 7; });
    r.wait();
    EXPECT_EQ(r.get(), 42);
    Future<int> bad = runAsync(pool, []() -> int { throw std::logic_error("x"); });
    bad.wait();
    EXPECT_THROW(bad.get(), std::logic_error);
}

TEST(AsyncCancellation, TokenCallbacksAndLinking) {
    CancellationSource parent;
    CancellationSource child(parent.token());
    int calls = 0;
    auto reg = child.token().onCancel([&] { ++calls; });
    {
        auto dropped = child.token().onCancel([&] { calls += 100; });
    }
    EXPECT_FALSE(child.isCancelled());
    parent.cancel();
    EXPECT_TRUE(child.isCancelled());
    EXPECT_EQ(calls, 1);
    auto late = child.token().onCancel([&] { calls += 10; }); // already cancelled: runs inline
    EXPECT_EQ(calls, 11);
    EXPECT_FALSE(CancellationToken{}.canBeCancelled());
}
