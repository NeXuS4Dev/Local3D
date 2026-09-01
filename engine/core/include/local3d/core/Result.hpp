#pragma once
/// @file Result.hpp
/// @brief `Expected<T, E>` - the engine's explicit error handling primitive.
///
/// Local3D does not use exceptions for control flow (see
/// docs/architecture/error-handling.md).  Functions that can fail return
/// `Expected<T>`; the caller must inspect it before use, which the compiler
/// enforces through [[nodiscard]].

#include "local3d/core/Assert.hpp"
#include "local3d/core/Common.hpp"
#include "local3d/core/Status.hpp"

#include <type_traits>
#include <utility>
#include <variant>

namespace l3d {

/// Tag used to construct an Expected that holds an error.
template <typename E>
struct Unexpected {
    E value;
};

template <typename E>
Unexpected(E) -> Unexpected<E>;

/// A value-or-error result.  `E` defaults to l3d::Status.
///
/// Thread safety: none.  Like std::optional, an Expected is not synchronised;
/// share it via a mutex or make it const.
template <typename T, typename E = Status>
class [[nodiscard]] Expected {
    static_assert(!std::is_same_v<T, E>, "Expected<T, E> requires T != E");
    static_assert(!std::is_reference_v<T>, "Expected<T, E> cannot hold a reference");

public:
    using ValueType = T;
    using ErrorType = E;

    Expected() noexcept(std::is_nothrow_default_constructible_v<T>) : storage_(std::in_place_index<0>, T{}) {}

    // NOLINTNEXTLINE(google-explicit-constructor) - implicit success construction is the point.
    Expected(const T& value) : storage_(std::in_place_index<0>, value) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    Expected(T&& value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : storage_(std::in_place_index<0>, std::move(value)) {}

    // NOLINTNEXTLINE(google-explicit-constructor)
    Expected(Unexpected<E> error) : storage_(std::in_place_index<1>, std::move(error.value)) {}

    Expected(const Expected&) = default;
    Expected(Expected&&) = default;
    Expected& operator=(const Expected&) = default;
    Expected& operator=(Expected&&) = default;

    [[nodiscard]] bool HasValue() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] bool IsError() const noexcept { return storage_.index() == 1; }
    [[nodiscard]] explicit operator bool() const noexcept { return HasValue(); }

    /// Access the value.  Asserts when the Expected holds an error.
    [[nodiscard]] T& Value() & noexcept {
        L3D_ASSERT_MSG(HasValue(), "Expected::Value() called on an error result");
        return std::get<0>(storage_);
    }
    [[nodiscard]] const T& Value() const& noexcept {
        L3D_ASSERT_MSG(HasValue(), "Expected::Value() called on an error result");
        return std::get<0>(storage_);
    }
    [[nodiscard]] T&& Value() && noexcept {
        L3D_ASSERT_MSG(HasValue(), "Expected::Value() called on an error result");
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] T& operator*() & noexcept { return Value(); }
    [[nodiscard]] const T& operator*() const& noexcept { return Value(); }
    [[nodiscard]] T* operator->() noexcept { return &Value(); }
    [[nodiscard]] const T* operator->() const noexcept { return &Value(); }

    /// Access the error.  Asserts when the Expected holds a value.
    [[nodiscard]] const E& Error() const& noexcept {
        L3D_ASSERT_MSG(IsError(), "Expected::Error() called on a success result");
        return std::get<1>(storage_);
    }
    [[nodiscard]] E& Error() & noexcept {
        L3D_ASSERT_MSG(IsError(), "Expected::Error() called on a success result");
        return std::get<1>(storage_);
    }

    [[nodiscard]] T ValueOr(T fallback) const& noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return HasValue() ? Value() : std::move(fallback);
    }

    /// Monadic combinators (the C++23 std::expected API, spelled our way).
    template <typename F>
    [[nodiscard]] auto AndThen(F&& f) const& -> std::invoke_result_t<F, const T&> {
        using Result = std::invoke_result_t<F, const T&>;
        if (IsError()) {
            return Result{Unexpected<E>{Error()}};
        }
        return std::forward<F>(f)(Value());
    }

    template <typename F>
    [[nodiscard]] auto Map(F&& f) const& -> Expected<std::invoke_result_t<F, const T&>, E> {
        using U = std::invoke_result_t<F, const T&>;
        if (IsError()) {
            return Expected<U, E>{Unexpected<E>{Error()}};
        }
        return Expected<U, E>{std::forward<F>(f)(Value())};
    }

    template <typename F>
    [[nodiscard]] Expected<T, E> MapError(F&& f) const& {
        if (HasValue()) {
            return Expected<T, E>{Value()};
        }
        return Expected<T, E>{Unexpected<E>{std::forward<F>(f)(Error())}};
    }

private:
    std::variant<T, E> storage_;
};

/// Specialisation for operations that only report success/failure.
template <typename E>
class [[nodiscard]] Expected<void, E> {
public:
    using ValueType = void;
    using ErrorType = E;

    Expected() noexcept = default;
    // NOLINTNEXTLINE(google-explicit-constructor)
    Expected(Unexpected<E> error) : error_(std::move(error.value)), hasValue_(false) {}

    [[nodiscard]] bool HasValue() const noexcept { return hasValue_; }
    [[nodiscard]] bool IsError() const noexcept { return !hasValue_; }
    [[nodiscard]] explicit operator bool() const noexcept { return hasValue_; }

    [[nodiscard]] const E& Error() const& noexcept {
        L3D_ASSERT_MSG(IsError(), "Expected<void>::Error() called on a success result");
        return error_;
    }

    template <typename F>
    [[nodiscard]] auto AndThen(F&& f) const -> std::invoke_result_t<F> {
        using Result = std::invoke_result_t<F>;
        if (IsError()) {
            return Result{Unexpected<E>{Error()}};
        }
        return std::forward<F>(f)();
    }

private:
    E error_{};
    bool hasValue_ = true;
};

/// Convenience alias: the 99% case.
template <typename T>
using Result = Expected<T, Status>;
using OperationResult = Expected<void, Status>;

} // namespace l3d

/// Unwrap an Expected into a named variable, early-returning the error.
///
///   auto result = LoadAsset(id);
///   L3D_TRY(asset, LoadAsset(id));
///   Use(asset);
#define L3D_TRY(name, expr)                                                                        \
    auto&& L3D_CONCAT(name, _expected_) = (expr);                                                  \
    if (!L3D_CONCAT(name, _expected_).HasValue()) {                                                \
        return ::l3d::Unexpected(L3D_CONCAT(name, _expected_).Error());                            \
    }                                                                                              \
    auto&& name = L3D_CONCAT(name, _expected_).Value()

/// Propagate a Status-returning call.  Binds the result to a name for the
/// lifetime of the block so the error can be forwarded unchanged.
#define L3D_RETURN_IF_ERROR(expr)                                                                  \
    do {                                                                                           \
        const auto& L3D_CONCAT(l3d_result_, __LINE__) = (expr);                                    \
        if (L3D_CONCAT(l3d_result_, __LINE__).IsError()) {                                         \
            return ::l3d::Unexpected(L3D_CONCAT(l3d_result_, __LINE__).Error());                   \
        }                                                                                          \
    } while (false)
