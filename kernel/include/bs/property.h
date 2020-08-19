/// @file
/// @author uentity
/// @date 10.03.2019
/// @brief Define BS object 'property'
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include "timetypes.h"
#include "meta.h"

#include <caf/detail/type_list.hpp>

#include <cstdint>
#include <iosfwd>
#include <variant>
#include <type_traits>

NAMESPACE_BEGIN(blue_sky::prop)
// aliases for scalar types that property can handle
using integer   = std::int64_t;
using boolean   = bool;
using real      = double;
using timespan  = blue_sky::timespan;
using timestamp = blue_sky::timestamp;
using string    = std::string;
using object    = sp_obj;

class property;

NAMESPACE_BEGIN(detail)
/// enumerate scalars carried by BS property
using scalar_ts = caf::detail::type_list<
	integer, boolean, real, timespan, timestamp, string, object
>;
inline constexpr auto scalar_ts_num = caf::detail::tl_size<scalar_ts>::value;

/// underlying type for list of scalars
template<typename T> struct scalar_list { using type = std::vector<T>; };
template<typename T> using scalar_list_t = typename scalar_list<T>::type;
using list_ts = caf::detail::tl_map_t<scalar_ts, scalar_list>;

/// underlying `std::variant` type of BS property
using carry_ts = caf::detail::tl_concat<scalar_ts, list_ts>::type;
using variant_prop_t = caf::detail::tl_apply_t<carry_ts, std::variant>;

/// helper compile-time checkers
template<typename U>
constexpr auto is_prop() {
	constexpr auto is_property = std::is_same_v<meta::remove_cvref_t<U>, property>;
	static_assert(is_property, "Argument isn't a blue-sky property instance");
	return is_property;
}
template<typename U> inline constexpr auto is_prop_v = is_prop<U>();

template<typename T>
constexpr auto can_carry_scalar() {
	constexpr auto res = caf::detail::tl_contains<detail::scalar_ts, T>::value;
	static_assert(res, "Property can't carry requested scalar type");
	return res;
}
template<typename U> inline constexpr auto can_carry_scalar_v = can_carry_scalar<U>();

template<typename T>
constexpr auto can_carry_list() {
	constexpr auto res = caf::detail::tl_contains<detail::list_ts, T>::value;
	static_assert(res, "Property can't carry requested list type");
	return res;
}
template<typename U> inline constexpr auto can_carry_list_v = can_carry_list<U>();

template<typename T>
constexpr auto can_carry_type() {
	constexpr auto res = caf::detail::tl_contains<detail::carry_ts, T>::value;
	static_assert(res, "Property can't carry requested type");
	return res;
}
template<typename U> inline constexpr auto can_carry_type_v = can_carry_type<U>();

NAMESPACE_END(detail)

// tag for easier lists manipulation
template<typename T> using list_of = detail::scalar_list_t<T>;
template<typename T> using in_place_list_t = std::in_place_type_t<list_of<T>>;
template<typename T> inline constexpr in_place_list_t<T> in_place_list{};

/*-----------------------------------------------------------------------------
 *  BS property definition
 *-----------------------------------------------------------------------------*/
class property : public detail::variant_prop_t {
public:
	using underlying_type = detail::variant_prop_t;

	using underlying_type::underlying_type;
	using underlying_type::operator=;

	// support for class enums
	template<typename E, typename = std::enable_if_t<meta::is_enum_class_v<E>>>
	constexpr property(E value) noexcept : underlying_type{static_cast<integer>(value)} {}

	template<typename E, typename = std::enable_if_t<meta::is_enum_class_v<E>>>
	constexpr auto operator=(E value) noexcept -> property& {
		*this = static_cast<integer>(value);
		return *this;
	}
};

///////////////////////////////////////////////////////////////////////////////
//  helpers for manipulating property value
//
template<typename T, typename U>
constexpr decltype(auto) get(U&& p) {
	if constexpr(detail::is_prop_v<U>) {
		if constexpr(std::is_enum_v<T>)
			return static_cast<T>(std::get<integer>(std::forward<U>(p)));
		else if constexpr(detail::can_carry_type_v<T>)
			return std::get<T>(std::forward<U>(p));
	}
}

template<std::size_t I, typename U>
constexpr decltype(auto) get(U&& p) {
	if constexpr(detail::is_prop_v<U>)
		return std::get<I>(std::forward<U>(p));
}

template<typename T, typename U>
constexpr auto get_if(U* p) noexcept {
	if constexpr(detail::is_prop_v<U> && detail::can_carry_type_v<T>)
		return std::get_if<T>(p);
}

/// Intended to appear on right side, that's why const refs
template<typename T>
constexpr decltype(auto) get_or(const property* p, const T& def_value) noexcept {
	if constexpr(std::is_enum_v<T>) {
		auto pv = std::get_if<integer>(p);
		return pv ? static_cast<T>(*pv) : def_value;
	}
	else if constexpr(detail::can_carry_type_v<T>) {
		auto pv = std::get_if<T>(p);
		return pv ? *pv : def_value;
	}
	else
		return def_value;
}

/// Tries to extract value of type `From`
/// If property doesn't hold such alternative,
/// return if value was found and updated
template<typename From = void, typename To = void>
constexpr bool extract(const property& source, To& target) noexcept {
	using carryT = std::conditional_t< std::is_same_v<From, void>, To, From >;
	if constexpr(detail::can_carry_type_v<carryT>) {
		if(std::holds_alternative<carryT>(source)) {
			target = std::get<carryT>(source);
			return true;
		}
		return false;
	}
}

/// check if property holds 'None' value (null `sp_obj`)
constexpr bool is_none(const property& P) noexcept {
	return std::holds_alternative<object>(P) && !get<object>(P);
}

inline auto none() noexcept { return property{ sp_obj() }; }

/// custom analog of `std::visit` to fix compile issues with VS
template<
	typename F, typename P,
	typename = std::enable_if_t<std::is_same_v<meta::remove_cvref_t<P>, property>>
>
constexpr decltype(auto) visit(F&& f, P&& p) {
	// forward underlying type to std::visit
	using UT = typename std::remove_reference_t<P>::underlying_type;
	return std::visit(std::forward<F>(f), meta::forward_as<P, UT>(p));
}

///  formatting support
BS_API std::string to_string(const property& p);
BS_API std::ostream& operator <<(std::ostream& os, const property& x);

NAMESPACE_END(blue_sky::prop)

/// ease writing visitors
template<class... Ts> struct overload : Ts... { using Ts::operator()...; };
template<class... Ts> overload(Ts...) -> overload<Ts...>;

NAMESPACE_BEGIN(std)

template<> struct variant_size<blue_sky::prop::property> :
	variant_size<typename blue_sky::prop::property::underlying_type> {};

template<size_t Np>
struct variant_alternative<Np, blue_sky::prop::property> :
	variant_alternative<Np, typename blue_sky::prop::property::underlying_type> {};

template<> struct hash<blue_sky::prop::property> :
	hash<typename blue_sky::prop::property::underlying_type> {};

NAMESPACE_END(std)
