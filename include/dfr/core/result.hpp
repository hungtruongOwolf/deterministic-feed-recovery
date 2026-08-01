// result<T>: a value paired with an error code.
//
// Modelled on simdjson's simdjson_result rather than on std::expected, for two
// reasons that matter here:
//
//   1. It stores the value *and* the error, so a decoder that has already
//      computed a partial result can return it alongside the error without
//      branching at construction. simdjson documents this constructor as "use
//      if you don't want to branch when creating the result".
//   2. The recommended access idiom is `T v; if (auto err = r.get(v)) {...}`,
//      which branches exactly once. std::expected's `value()` branches, then
//      the caller branches again.
//
// The monadic operations are named exactly as std::expected's, so that when
// C++23 is available this can become a thin adaptor rather than a rewrite.
//
// Storage: value and error side by side, which requires T to be default
// initializable. That is the same trade simdjson makes. It keeps the type
// trivially copyable when T is, needs no union or manual lifetime management,
// and costs sizeof(T) on the error path: acceptable because every T here is a
// small POD or a view.

#ifndef DFR_CORE_RESULT_HPP
#define DFR_CORE_RESULT_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>
#include <dfr/core/error.hpp>

#include <concepts>
#include <type_traits>
#include <utility>

// Exceptions are used in exactly one place: result<T>::value(), which exists
// for application and test code. Nothing in dfr::chaos or dfr::recovery calls
// it. Following simdjson, the whole facility disappears when the translation
// unit is compiled without exception support.
#if !defined(DFR_EXCEPTIONS)
#  if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#    define DFR_EXCEPTIONS 1
#  else
#    define DFR_EXCEPTIONS 0
#  endif
#endif

#if DFR_EXCEPTIONS
#  include <exception>
#endif

namespace dfr::inline v1 {

#if DFR_EXCEPTIONS
// Thrown only by result<T>::value(). Carries the code so a catch site can
// recover the same information the value-based API would have given it.
class bad_result_access : public std::exception {
 public:
  explicit bad_result_access(error err) noexcept : error_(err) {}

  [[nodiscard]] error code() const noexcept { return error_; }

  [[nodiscard]] const char* what() const noexcept override {
    // describe() returns a string_view over a string literal, so .data() is
    // NUL-terminated and outlives this object.
    return describe(error_).data();
  }

 private:
  error error_;
};
#endif

template <typename T>
class result;

namespace detail {

template <typename T>
struct is_result : std::false_type {};
template <typename T>
struct is_result<result<T>> : std::true_type {};

template <typename T>
concept a_result = is_result<std::remove_cvref_t<T>>::value;

}  // namespace detail

// ---------------------------------------------------------------------------
// result<T>
// ---------------------------------------------------------------------------

// [[nodiscard]] is on the class rather than on each function, which is
// tl::expected's approach: it cannot then be forgotten on a new member, and it
// applies to functions we do not own that happen to return this type.
template <typename T>
class [[nodiscard]] result {
  static_assert(std::default_initializable<T>,
                "result<T> stores the value and the error side by side, so T "
                "must be default initializable. Wrap a non-default-constructible "
                "type in std::optional, or use result<void> and an out-param.");

 public:
  using value_type = T;
  using error_type = error;

  // A default-constructed result is an error, not a success. The alternative:
  // defaulting to ok with a default-constructed value: would make a forgotten
  // assignment look like a successful decode.
  constexpr result() noexcept(std::is_nothrow_default_constructible_v<T>)
      : error_(error::invalid_argument) {}

  // Success.
  constexpr result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
      : value_(std::move(value)), error_(error::ok) {}

  // Failure, with a default-constructed value.
  constexpr result(error err) noexcept(
      std::is_nothrow_default_constructible_v<T>)
      : error_(err) {
    DFR_ASSERT(err != error::ok,
               "constructing a result from error::ok discards the value; use "
               "the value constructor instead");
  }

  // Both, for a decoder that produced something usable *and* wants to report a
  // condition: a header that parsed but whose sequence number revealed a gap,
  // for instance. This is the constructor that lets the hot path avoid a
  // branch at construction.
  constexpr result(T value, error err) noexcept(
      std::is_nothrow_move_constructible_v<T>)
      : value_(std::move(value)), error_(err), has_populated_value_(true) {}

  // ---- inspection -------------------------------------------------------
  //
  // None of these assert, because asking whether a result succeeded is always
  // legitimate.

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr error error_code() const noexcept {
    return error_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool has_value() const noexcept {
    return error_ == error::ok;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr explicit operator bool()
      const noexcept {
    return has_value();
  }
  // True when a value is present *and* a condition was reported. A caller that
  // treats `if (!r)` as "nothing to do" would silently drop these.
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool has_value_and_error()
      const noexcept {
    return error_ != error::ok && has_populated_value_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool is_fatal() const noexcept {
    return ::dfr::is_fatal(error_);
  }

  // ---- the recommended accessor -----------------------------------------

  // Moves the value out and returns the error. Branches once at the call site:
  //
  //   header h;
  //   if (const auto err = decode(bytes).get(h); err != error::ok) { ... }
  //
  // Returns the error even when a value was also produced, so a caller cannot
  // ignore a reported condition by accident.
  DFR_FLATTEN_INLINE constexpr error get(T& out) && noexcept(
      std::is_nothrow_move_assignable_v<T>) {
    out = std::move(value_);
    return error_;
  }

  DFR_FLATTEN_INLINE constexpr error get(T& out) const& noexcept(
      std::is_nothrow_copy_assignable_v<T>) {
    out = value_;
    return error_;
  }

  // ---- unchecked access -------------------------------------------------
  //
  // Asserts rather than throws. Reading the value of a failed result is a
  // programmer error, and docs/DESIGN.md section 2 places programmer errors in
  // the assertion tier.

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr T& operator*() & noexcept {
    DFR_ASSERT(has_value(), "dereferenced a failed result");
    return value_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr const T& operator*()
      const& noexcept {
    DFR_ASSERT(has_value(), "dereferenced a failed result");
    return value_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr T&& operator*() && noexcept {
    DFR_ASSERT(has_value(), "dereferenced a failed result");
    return std::move(value_);
  }

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr T* operator->() noexcept {
    DFR_ASSERT(has_value(), "dereferenced a failed result");
    return &value_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr const T* operator->()
      const noexcept {
    DFR_ASSERT(has_value(), "dereferenced a failed result");
    return &value_;
  }

  // Named to be conspicuous at the call site. simdjson's guidance on its
  // equivalent is "we discourage the use of value_unsafe()", and the same
  // applies: reach for get() instead.
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr T&& value_unsafe() && noexcept {
    return std::move(value_);
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr const T& value_unsafe()
      const& noexcept {
    return value_;
  }

  // ---- checked access, for application and test code --------------------

#if DFR_EXCEPTIONS
  [[nodiscard]] constexpr T&& value() && {
    if (!has_value()) {
      throw bad_result_access{error_};
    }
    return std::move(value_);
  }
  [[nodiscard]] constexpr const T& value() const& {
    if (!has_value()) {
      throw bad_result_access{error_};
    }
    return value_;
  }
#endif

  template <typename U>
  [[nodiscard]] constexpr T value_or(U&& fallback) const& {
    return has_value() ? value_ : static_cast<T>(std::forward<U>(fallback));
  }
  template <typename U>
  [[nodiscard]] constexpr T value_or(U&& fallback) && {
    return has_value() ? std::move(value_)
                       : static_cast<T>(std::forward<U>(fallback));
  }

  // std::expected's spelling: the error if there is one, otherwise `fallback`.
  [[nodiscard]] constexpr error error_or(error fallback) const noexcept {
    return has_value() ? fallback : error_;
  }

  // ---- monadic operations ------------------------------------------------
  //
  // Names and semantics match std::expected so that migrating in C++23 is a
  // typedef rather than a rewrite.

  // F: T -> result<U>
  template <typename F>
    requires detail::a_result<std::invoke_result_t<F, T&&>>
  [[nodiscard]] constexpr auto and_then(F&& f) && {
    using out = std::remove_cvref_t<std::invoke_result_t<F, T&&>>;
    if (!has_value()) {
      return out{error_};
    }
    return std::forward<F>(f)(std::move(value_));
  }

  // F: T -> U
  template <typename F>
  [[nodiscard]] constexpr auto transform(F&& f) && {
    using out = result<std::remove_cvref_t<std::invoke_result_t<F, T&&>>>;
    if (!has_value()) {
      return out{error_};
    }
    return out{std::forward<F>(f)(std::move(value_))};
  }

  // F: error -> result<T>. Recovery hooks live here.
  template <typename F>
    requires detail::a_result<std::invoke_result_t<F, error>>
  [[nodiscard]] constexpr result or_else(F&& f) && {
    if (has_value()) {
      return std::move(*this);
    }
    return std::forward<F>(f)(error_);
  }

  // F: error -> error. For translating a lower layer's code into this layer's.
  template <typename F>
  [[nodiscard]] constexpr result transform_error(F&& f) && {
    if (has_value()) {
      return std::move(*this);
    }
    return result{std::move(value_), std::forward<F>(f)(error_)};
  }

 private:
  T value_{};
  error error_;
  // Distinguishes "error, and the value is meaningless" from "error, and the
  // value is a usable partial result". Set only by the two-argument
  // constructor, so the common paths pay nothing to maintain it beyond a byte
  // that packs into the padding after `error_`.
  bool has_populated_value_{false};
};

// Reads better than the two-argument constructor at a return statement, and
// makes the intent(a usable value plus a reported condition) explicit.
template <typename T>
[[nodiscard]] constexpr result<T> partial(T value, error err) {
  DFR_ASSERT(err != error::ok,
             "partial() reports a condition alongside a value; use the value "
             "constructor for success");
  return result<T>{std::move(value), err};
}

// ---------------------------------------------------------------------------
// result<void>: for operations that report only an error.
//
// A distinct specialisation rather than result<std::monostate>, so that
// `if (auto r = step(); !r)` reads the same for both and no caller has to name
// a placeholder type.
// ---------------------------------------------------------------------------

template <>
class [[nodiscard]] result<void> {
 public:
  using value_type = void;
  using error_type = error;

  // Unlike result<T>, the default is success: there is no value that could
  // have been forgotten, and `result<void> r;` reading as "nothing went wrong"
  // is what every call site means.
  constexpr result() noexcept = default;
  constexpr result(error err) noexcept : error_(err) {}

  [[nodiscard]] DFR_FLATTEN_INLINE constexpr error error_code() const noexcept {
    return error_;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool has_value() const noexcept {
    return error_ == error::ok;
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr explicit operator bool()
      const noexcept {
    return has_value();
  }
  [[nodiscard]] DFR_FLATTEN_INLINE constexpr bool is_fatal() const noexcept {
    return ::dfr::is_fatal(error_);
  }
  [[nodiscard]] constexpr error error_or(error fallback) const noexcept {
    return has_value() ? fallback : error_;
  }

#if DFR_EXCEPTIONS
  constexpr void value() const {
    if (!has_value()) {
      throw bad_result_access{error_};
    }
  }
#endif

  template <typename F>
    requires detail::a_result<std::invoke_result_t<F>>
  [[nodiscard]] constexpr auto and_then(F&& f) const {
    using out = std::remove_cvref_t<std::invoke_result_t<F>>;
    if (!has_value()) {
      return out{error_};
    }
    return std::forward<F>(f)();
  }

  template <typename F>
    requires detail::a_result<std::invoke_result_t<F, error>>
  [[nodiscard]] constexpr result or_else(F&& f) const {
    if (has_value()) {
      return *this;
    }
    return std::forward<F>(f)(error_);
  }

 private:
  error error_{error::ok};
};

// Spelled out so that `return dfr::ok();` reads better than `return {};` at the
// end of a long function.
[[nodiscard]] DFR_FLATTEN_INLINE constexpr result<void> ok() noexcept {
  return result<void>{};
}

}  // namespace dfr::inline v1

#endif  // DFR_CORE_RESULT_HPP
