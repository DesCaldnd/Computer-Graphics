#pragma once

#include <oxwald/core/assert.hpp>

#include <format>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

// std::expected is C++23; the engine builds as C++20 so we ship a small equivalent.
namespace ox {

struct Error {
    std::string message;
};

template <class... Args>
[[nodiscard]] Error makeError(std::format_string<Args...> fmt, Args&&... args) {
    return Error{std::format(fmt, std::forward<Args>(args)...)};
}

template <class T>
class [[nodiscard]] Result {
public:
    using value_type = T;

    Result()
        requires std::is_default_constructible_v<T>
        : m_data(std::in_place_index<0>) {}
    Result(const T& value) : m_data(std::in_place_index<0>, value) {}
    Result(T&& value) : m_data(std::in_place_index<0>, std::move(value)) {}
    template <class U>
        requires(std::is_constructible_v<T, U &&> && !std::is_same_v<std::remove_cvref_t<U>, T> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Error> && !std::is_same_v<std::remove_cvref_t<U>, Result>)
    Result(U&& value) : m_data(std::in_place_index<0>, std::forward<U>(value)) {}
    Result(Error error) : m_data(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool hasValue() const { return m_data.index() == 0; }
    explicit operator bool() const { return hasValue(); }

    T& value() & {
        OX_ASSERT(hasValue(), "Result has no value: {}", std::get<1>(m_data).message);
        return std::get<0>(m_data);
    }
    const T& value() const& {
        OX_ASSERT(hasValue(), "Result has no value: {}", std::get<1>(m_data).message);
        return std::get<0>(m_data);
    }
    T&& value() && {
        OX_ASSERT(hasValue(), "Result has no value: {}", std::get<1>(m_data).message);
        return std::get<0>(std::move(m_data));
    }
    template <class U>
    T valueOr(U&& fallback) const& {
        return hasValue() ? std::get<0>(m_data) : static_cast<T>(std::forward<U>(fallback));
    }

    const Error& error() const {
        OX_ASSERT(!hasValue(), "Result holds a value");
        return std::get<1>(m_data);
    }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    std::variant<T, Error> m_data;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)), m_failed(true) {}

    [[nodiscard]] bool hasValue() const { return !m_failed; }
    explicit operator bool() const { return hasValue(); }
    void value() const { OX_ASSERT(hasValue(), "Result has no value: {}", m_error.message); }
    const Error& error() const {
        OX_ASSERT(m_failed, "Result holds a value");
        return m_error;
    }

private:
    Error m_error;
    bool m_failed = false;
};

using Status = Result<void>;

} // namespace ox
