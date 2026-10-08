#pragma once

// ox::Generator<T> — synchronous, lazy generator usable in range-for.
//
//   ox::Generator<int> range(int n) { for (int i = 0; i < n; ++i) co_yield i; }
//   for (int i : range(10)) { ... }
//
// Exceptions thrown by the body are rethrown from begin()/operator++.

#include <oxwald/async/detail/frame_pool.hpp>

#include <coroutine>
#include <exception>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

namespace ox {

template <class T>
class [[nodiscard]] Generator {
public:
    using value_type = std::remove_cvref_t<T>;
    using reference = std::conditional_t<std::is_reference_v<T>, T, const value_type&>;

    struct promise_type {
        const value_type* current = nullptr;
        std::exception_ptr exception;

        Generator get_return_object() noexcept { return Generator{Handle::from_promise(*this)}; }
        std::suspend_always initial_suspend() const noexcept { return {}; }
        std::suspend_always final_suspend() const noexcept { return {}; }
        std::suspend_always yield_value(const value_type& v) noexcept {
            current = std::addressof(v);
            return {};
        }
        // Yielding a temporary: it lives in the frame until the next resumption.
        std::suspend_always yield_value(value_type&& v) noexcept {
            current = std::addressof(v);
            return {};
        }
        void return_void() const noexcept {}
        void unhandled_exception() noexcept { exception = std::current_exception(); }
        template <class U>
        std::suspend_never await_transform(U&&) = delete; // no co_await inside a synchronous generator

        static void* operator new(std::size_t size) { return detail::framePoolAllocate(size); }
        static void operator delete(void* p, std::size_t size) noexcept { detail::framePoolFree(p, size); }
    };
    using Handle = std::coroutine_handle<promise_type>;

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = Generator::value_type;

        iterator() = default;
        explicit iterator(Handle h) : m_h(h) {}
        reference operator*() const { return static_cast<reference>(*m_h.promise().current); }
        const value_type* operator->() const { return m_h.promise().current; }
        iterator& operator++() {
            advance(m_h);
            return *this;
        }
        void operator++(int) { ++*this; }
        bool operator==(std::default_sentinel_t) const { return !m_h || m_h.done(); }

    private:
        Handle m_h;
    };

    Generator() = default;
    explicit Generator(Handle h) : m_h(h) {}
    Generator(Generator&& o) noexcept : m_h(std::exchange(o.m_h, {})) {}
    Generator& operator=(Generator&& o) noexcept {
        if (this != &o) {
            if (m_h) {
                m_h.destroy();
            }
            m_h = std::exchange(o.m_h, {});
        }
        return *this;
    }
    ~Generator() {
        if (m_h) {
            m_h.destroy();
        }
    }

    iterator begin() {
        advance(m_h);
        return iterator{m_h};
    }
    std::default_sentinel_t end() const noexcept { return {}; }

private:
    static void advance(Handle h) {
        if (!h || h.done()) {
            return;
        }
        h.resume();
        if (h.promise().exception) {
            std::rethrow_exception(std::exchange(h.promise().exception, {}));
        }
    }
    Handle m_h;
};

} // namespace ox
