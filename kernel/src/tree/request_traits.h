/// @author Alexander Gagarin (@uentity)
/// @date 21.01.2022
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include <bs/actor_common.h>

#include <variant>

NAMESPACE_BEGIN(blue_sky::tree::detail)

template<typename R, typename F>
struct request_traits {
	// helper that passes pointer to bg actor making a request if functor needs it
	static constexpr auto invoke_f_request(F& f, caf::event_based_actor* origin) {
		if constexpr(std::is_invocable_v<F, caf::event_based_actor*>)
			return f(origin);
		else
			return f();
	};

	using f_ret_t = decltype( invoke_f_request(std::declval<F&>(), std::declval<caf::event_based_actor*>()) );
	static constexpr bool can_invoke_inplace = tl::detail::is_expected<f_ret_t>::value;

	// deduce request result type (must be result_or_errbox<R>)
	using R_deduced = std::conditional_t<std::is_same_v<R, void>, f_ret_t, result_or_errbox<R>>;
	static_assert(tl::detail::is_expected<R_deduced>::value);
	// assume request result is `result_or_errbox<R>`
	using res_t = result_or_errbox<typename R_deduced::value_type>;
	// value type that worker actor returns (always transfers error in a box)
	using worker_ret_t = std::conditional_t<can_invoke_inplace, res_t, f_ret_t>;
	// type of extra result processing functor
	// can also accept bool flag indicating whether result obtained after performed request (true)
	// or after wait (false)
	using custom_rp_f = std::variant<std::function<void(res_t)>, std::function<void(res_t, bool)>>;
	// worker actor iface
	using actor_type = caf::typed_actor<
		typename caf::replies_to<a_ack>::template with<res_t>,
		typename caf::replies_to<a_ack, custom_rp_f>::template with<res_t>
	>;

	static auto invoke_rp(const custom_rp_f& rp, res_t res, bool after_request) {
		std::visit(meta::overloaded{
			[&](const std::function<void(res_t)>& f) {
				error::eval_safe([&] { f(std::move(res)); });
			},
			[&](const std::function<void(res_t, bool)>& f) {
				error::eval_safe([&] { f(std::move(res), after_request); });
			}
		}, rp);
	}

	template<typename U, typename V>
	static auto chain_rp(U base_rp, V extra_rp) {
		return [base_rp = std::move(base_rp), extra_rp = std::move(extra_rp)]
		(res_t obj, bool after_request) mutable {
			invoke_rp(custom_rp_f{base_rp}, obj, after_request);
			if constexpr(!std::is_same_v<V, std::nullopt_t>)
				invoke_rp(custom_rp_f{extra_rp}, std::move(obj), after_request);
		};
	};
};

NAMESPACE_END(blue_sky::tree::detail)

NAMESPACE_BEGIN(caf)

// mark all custom result processors as non-serializable
template<typename T>
struct allowed_unsafe_message_type<
	std::variant<std::function<void(T)>, std::function<void(T, bool)>>
> : std::true_type {};

NAMESPACE_END(caf)
