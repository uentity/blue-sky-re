/// @date 01.09.2020
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include "common.h"
#include "error.h"
#include "propdict.h"
#include "meta.h"
#include "meta/variant.h"

#include <caf/allowed_unsafe_message_type.hpp>
#include <cereal/cereal.hpp>

#include <optional>
#include <variant>

NAMESPACE_BEGIN(blue_sky)

/*-----------------------------------------------------------------------------
 *  define transaction result as variant of error & propdict with some useful methods
 *-----------------------------------------------------------------------------*/
// Why not just result_or_err<propdict>?
//
// Because error is 'normal' transaction result (it may not return additional info)
// Adding such errors using `tl::make_unexpected()` or via `unexpected_error()`
// is misleading.
// Constructor of `error` natively accepts `tr_result` and turns into succcess value if 'props' slot
// of result is filled.

struct tr_result : std::variant<prop::propdict, error> {
	using underlying_type = variant;
	using underlying_type::underlying_type;

	// construct from serializable `tr_result_box`
	using box = std::variant<prop::propdict, error::box>;

	tr_result(box tbox) {
		visit(meta::overloaded{
			[&](prop::propdict&& p) { emplace<0>(std::move(p)); },
			[&](error::box&& i) { emplace<1>(std::move(i)); }
		}, std::move(tbox));
	}

	// pack `tr_result` into `tr_result::box` suitable for serialization
	template<typename T>
	friend auto pack(T&& tres)
	-> std::enable_if_t<std::is_same_v<meta::remove_cvref_t<T>, tr_result>, box> {
		box res;
		visit(meta::overloaded{[&](auto&& v) {
			using V = decltype(v);
			using V_ = meta::remove_cvref_t<V>;
			if constexpr(std::is_same_v<V_, prop::propdict>)
				res.emplace<0>(std::forward<V>(v));
			else
				res.emplace<1>(std::forward<V>(v));
		}}, meta::forward_as<T, underlying_type>(tres));
		return res;
	}

	// check if return value carry props
	inline auto has_info() const -> bool { return index() == 1; }

	// extract props unchecked
	decltype(auto) info() const { return std::get<0>(*this); }
	decltype(auto) info() { return std::get<0>(*this); }
	// extract error unchecked
	decltype(auto) err() const { return std::get<1>(*this); }
	decltype(auto) err() { return std::get<1>(*this); }

	// check if transaction was successfull, i.e. error is OK or props passed
	operator bool() const { return has_info() ? true : err().ok(); }

	// checked info extractor: if rvalue passed in. then move error from it, otherwise copy
	template<typename T>
	friend auto extract_info(T&& tres)
	-> std::enable_if_t<std::is_same_v<meta::remove_cvref_t<T>, tr_result>, prop::propdict> {
		if(!tres.has_info()) return {};
		if constexpr(std::is_lvalue_reference_v<T>)
			return tres.info();
		else
			return std::move(tres.info());
	}

	// checked error extractor: if rvalue passed in. then move error from it, otherwise copy
	template<typename T>
	friend auto extract_err(T&& tres)
	-> std::enable_if_t<std::is_same_v<meta::remove_cvref_t<T>, tr_result>, error> {
		if(tres.has_info()) return perfect;
		if constexpr(std::is_lvalue_reference_v<T>)
			return tres.err();
		else
			return std::move(tres.err());
	}

	// implement map (like in `expected`)
	template<typename F>
	decltype(auto) map(F&& f) const {
		if(auto p = std::get_if<0>(this))
			return tr_result(f(*p));
		return *this;
	}

	template<typename F>
	decltype(auto) map(F&& f) {
		if(auto p = std::get_if<0>(this))
			return tr_result(f(*p));
		return *this;
	}

	template<typename F>
	decltype(auto) map_error(F&& f) const {
		if(auto e = std::get_if<1>(this))
			return tr_result(f(*e));
		return *this;
	}

	template<typename F>
	decltype(auto) map_error(F&& f) {
		if(auto e = std::get_if<1>(this))
			return tr_result(f(*e));
		return *this;
	}
};

/// transaction is a function that is executed atomically in actor handler of corresponding object
template<typename R, typename... Ts> using transaction_t = std::function< R(Ts...) >;
using transaction = transaction_t<tr_result>;
using obj_transaction =  transaction_t<tr_result, sp_obj>;

/// simple transaction returns error instead of extended transaction result
using simple_transaction = transaction_t<error>;
using link_transaction = transaction_t<error, tree::link>;
using node_transaction = transaction_t<error, tree::node>;

/// check if type is transaction
template<typename T>
struct is_transaction : public std::false_type {};

template<typename R, typename... Ts>
struct is_transaction<transaction_t<R, Ts...>> : public std::true_type {};

template<typename T> using is_transaction_t = is_transaction<meta::remove_cvref_t<T>>;
template<typename T> inline constexpr auto is_transaction_v = is_transaction_t<T>::value;

/// run transaction & capture all exceptions into error slot of `tr_result`
template<typename R, typename... Ts, typename... Targs>
auto tr_eval(const transaction_t<R, Ts...>& tr, Targs&&... targs) {
	auto tres = std::optional<R>{};
	if(auto er = error::eval_safe([&] { tres.emplace( tr(std::forward<Targs>(targs)...) ); }))
		tres.emplace(std::move(er));
	return std::move(*tres);
}

NAMESPACE_END(blue_sky)

NAMESPACE_BEGIN(caf)

// mark all transaction types as non-serializable
template<typename R, typename... Ts>
struct allowed_unsafe_message_type<blue_sky::transaction_t<R, Ts...>> : std::true_type {};

NAMESPACE_END(caf)

BS_ALLOW_VISIT(blue_sky::tr_result)
