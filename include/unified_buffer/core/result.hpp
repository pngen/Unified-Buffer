#pragma once
#include "unified_buffer/core/error.hpp"
#include <utility>
#include <variant>

namespace unified_buffer {

// Expected-style result type.  T must be movable.  Cannot hold references.
template <typename T>
class Result {
 public:
  Result() = default;
  Result(ErrorCode c, std::string msg = {}) : error_(c, std::move(msg)) {}
  Result(Error err) : error_(std::move(err)) {}
  Result(T value) : ok_(true), value_(std::move(value)) {}

  Result(const Result&) = default;
  Result& operator=(const Result&) = default;
  Result(Result&&) noexcept = default;
  Result& operator=(Result&&) noexcept = default;

  bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }

  ErrorCode error() const noexcept { return error_.code; }
  const Error& err() const noexcept { return error_; }
  const std::string& message() const noexcept { return error_.message; }

  T& value() & { return value_; }
  const T& value() const& { return value_; }
  T&& value() && { return std::move(value_); }

  T value_or(T fallback) const { return ok_ ? value_ : std::move(fallback); }

 private:
  bool ok_ = false;
  T value_{};
  Error error_;
};

template <typename T>
inline Result<T> ok(T value) { return Result<T>(std::move(value)); }

using Status = Result<std::monostate>;

inline Status ok_status() { return Status(std::monostate{}); }

template <>
class Result<void> {
 public:
  Result() = default;
  Result(ErrorCode c, std::string msg = {}) : ok_(false), error_(c, std::move(msg)) {}
  Result(Error err) : ok_(false), error_(std::move(err)) {}
  bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }
  ErrorCode error() const noexcept { return error_.code; }
  const Error& err() const noexcept { return error_; }
  const std::string& message() const noexcept { return error_.message; }
 private:
  bool ok_ = true;
  Error error_;
};

inline Result<void> ok_void() { return Result<void>(); }

} // namespace unified_buffer