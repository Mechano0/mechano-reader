#pragma once
// Result<T>: a value OR an ErrorInfo. Used instead of exceptions across module
// boundaries so a corrupted archive can never crash the app with an uncaught throw.
#include <utility>
#include <variant>
#include "utils/ErrorInfo.h"

namespace mzr {

template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}              // NOLINT: implicit on purpose
    Result(ErrorInfo error) : data_(std::move(error)) {}      // NOLINT

    bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return ok(); }

    T& value() { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }
    const ErrorInfo& error() const { return std::get<ErrorInfo>(data_); }

private:
    std::variant<T, ErrorInfo> data_;
};

} // namespace mzr
