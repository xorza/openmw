#pragma once

#include <cassert>
#include <concepts>
#include <optional>
#include <utility>
#include <variant>

namespace Rtx
{
    /// The failing half of a `Result`, named where it is made — Rust's `Err`. Named rather than
    /// converted, so a `Result` whose two types convert into each other is never ambiguous.
    template <class E>
    struct Err
    {
        E mError;
    };

    /// A value, or why there is none — Rust's `Result`. How every reader of content answers: what
    /// content supplies may be refused item by item and the game goes on, so a refusal is an
    /// answer and not an exception, and the code that decides what becomes of the content reports
    /// it (`Refusals`).
    template <class T, class E>
    class Result
    {
    public:
        template <class U>
        requires std::constructible_from<T, U&&> Result(U&& value)
            : mState(std::in_place_index<0>, std::forward<U>(value))
        {
        }

        template <class F>
        requires std::constructible_from<E, F&&> Result(Err<F> error)
            : mState(std::in_place_index<1>, std::move(error.mError))
        {
        }

        bool isOk() const { return mState.index() == 0; }

        const T& value() const
        {
            assert(isOk() && "the value of a result that failed");
            return std::get<0>(mState);
        }

        const E& error() const
        {
            assert(!isOk() && "the error of a result that holds a value");
            return std::get<1>(mState);
        }

    private:
        std::variant<T, E> mState;
    };

    /// A check's answer: nothing where it passed, and why where it did not — Rust's `Result<(), E>`.
    /// `return {};` is the pass.
    template <class E>
    class Result<void, E>
    {
    public:
        Result() = default;

        template <class F>
        requires std::constructible_from<E, F&&> Result(Err<F> error)
            : mError(std::move(error.mError))
        {
        }

        bool isOk() const { return !mError.has_value(); }

        const E& error() const
        {
            assert(!isOk() && "the error of a check that passed");
            return *mError;
        }

    private:
        std::optional<E> mError;
    };
}
