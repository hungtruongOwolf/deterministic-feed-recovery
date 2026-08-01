// A conversion that is a cast only when the types differ.
//
// `static_cast<std::size_t>` on a `std::uint64_t` is a no-op on LP64, where the two are the same type, and
// necessary on a 32-bit target where they are not. GCC's `-Wuseless-cast`, which this project enables on
// purpose: is right on the first platform and wrong on the second.
//
// So the choice is made at compile time rather than argued about: the cast happens exactly when it is a
// conversion. Two files needed it before it got its own header, which is the point at which a helper living
// inside `rng.hpp` stopped being a detail of the generator.
//
// Deliberately not a general `narrow_cast` with a range check. Every use here converts a value the caller has
// already bounded(a sequence delta inside a buffer, an index inside a count) and a runtime check would be
// paid on a hot path to test something an assertion above it already established. Where a value is *not*
// already bounded, the right tool is `DFR_ASSERT` at the point the bound is known, not a silent clamp here.

#ifndef DFR_CORE_NARROW_HPP
#define DFR_CORE_NARROW_HPP

#include <type_traits>

namespace dfr::inline v1 {

template <typename To, typename From>
[[nodiscard]] constexpr To narrowed(From value) noexcept {
  if constexpr (std::is_same_v<To, From>) {
    return value;
  } else {
    return static_cast<To>(value);
  }
}

}  // namespace dfr::inline v1

#endif  // DFR_CORE_NARROW_HPP
