// Pairs an object's invariant check on entry and exit of a scope.
//
// Its own file because it is a distinct idea from the assertion macros: those
// check a condition at a point, this checks a type's invariant across a
// mutation. It is also how TIGER_STYLE's "assert every property on two
// different code paths" gets applied without writing the check twice.

#ifndef DFR_CORE_INVARIANT_GUARD_HPP
#define DFR_CORE_INVARIANT_GUARD_HPP

#include <dfr/core/assert.hpp>
#include <dfr/core/attributes.hpp>

namespace dfr::inline v1 {

// Requires `obj.check_invariants()` to be callable and side-effect free.
template <typename T>
class invariant_guard {
 public:
  explicit invariant_guard(const T& obj DFR_LIFETIME_BOUND) : obj_(&obj) {
    if constexpr (kParanoidAssertionsEnabled) {
      obj_->check_invariants();
    }
  }

  ~invariant_guard() {
    if constexpr (kParanoidAssertionsEnabled) {
      obj_->check_invariants();
    }
  }

  invariant_guard(const invariant_guard&) = delete;
  invariant_guard& operator=(const invariant_guard&) = delete;
  invariant_guard(invariant_guard&&) = delete;
  invariant_guard& operator=(invariant_guard&&) = delete;

 private:
  const T* obj_;
};

}  // namespace dfr::inline v1

// Two-level indirection: ## suppresses expansion of its operands, so a single
// level would name every guard dfr_invariant_guard___LINE__ and two guards in
// one scope would collide.
#define DFR_DETAIL_CONCAT_(a, b) a##b
#define DFR_DETAIL_CONCAT(a, b) DFR_DETAIL_CONCAT_(a, b)

// Usage: DFR_INVARIANT_GUARD(*this); as the first statement of a mutator.
#define DFR_INVARIANT_GUARD(obj)                                               \
  const ::dfr::invariant_guard DFR_DETAIL_CONCAT(dfr_invariant_guard_,         \
                                                 __LINE__) { (obj) }

#endif  // DFR_CORE_INVARIANT_GUARD_HPP
