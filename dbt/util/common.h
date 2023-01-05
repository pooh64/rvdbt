#pragma once

#include <bit>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "dbt/config.h"

typedef uintptr_t uptr;
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef intptr_t iptr;
typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;
typedef int8_t i8;

#define EMPTY_MACRO(...)

#define POISON_PTR ((void *)0xb00bab00deaddead)
#define POISON_GUEST ((u32)0xdedb00ba)

#define likely(v) __builtin_expect(!!(v), 1)
#define unlikely(v) __builtin_expect(!!(v), 0)

#define ALWAYS_INLINE inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))

#define unreachable(str)                                                                                     \
	do {                                                                                                 \
		assert((str) && 0);                                                                          \
		__builtin_unreachable();                                                                     \
	} while (0)

#define NO_COPY(name)                                                                                        \
	name(const name &) = delete;                                                                         \
	name &operator=(const name &) = delete;

#define NO_MOVE(name)                                                                                        \
	name(name &&) = delete;                                                                              \
	name &operator=(name &&) = delete;

#define DEFAULT_COPY(name)                                                                                   \
	name(const name &) = default;                                                                        \
	name &operator=(const name &) = default;

#define DEFAULT_MOVE(name)                                                                                   \
	name(name &&) = default;                                                                             \
	name &operator=(name &&) = default;

template <typename T, typename A>
inline T roundup(T x, A y)
{
	return ((x + (y - 1)) / y) * y;
}

template <typename T, typename A>
inline T rounddown(T x, A y)
{
	return x - x % y;
}

template <typename T>
T unaligned_load(T const *ptr)
{
	struct uT {
		T x;
	} __attribute__((packed));
	return reinterpret_cast<uT const *>(ptr)->x;
}

template <typename T>
void unaligned_store(T *ptr, T val)
{
	struct uT {
		T x;
	} __attribute__((packed));
	reinterpret_cast<uT *>(ptr)->x = val;
}

template <typename T, typename P>
typename std::enable_if_t<!std::is_same_v<T, P>, T> unaligned_load(P const *ptr)
{
	return unaligned_load<T>(reinterpret_cast<T const *>(ptr));
}

template <typename T, typename P, typename V>
typename std::enable_if_t<!std::is_same_v<T, P>> unaligned_store(P *ptr, V val)
{
	unaligned_store<T>(reinterpret_cast<T *>(ptr), static_cast<T>(val));
}

// c++23
template <class Enum>
constexpr std::underlying_type_t<Enum> to_underlying(Enum e)
{
	return static_cast<std::underlying_type_t<Enum>>(e);
}

template <class Enum>
constexpr size_t enum_bits(Enum e)
{
	return std::bit_width(to_underlying(e) - 1u);
}

template <typename T>
static constexpr size_t bit_size = sizeof(T) * CHAR_BIT;

namespace dbt
{
void __attribute__((noreturn)) Panic(char const *msg = "");
} // namespace dbt
