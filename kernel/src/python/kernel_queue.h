/// @author Alexander Gagarin (@uentity)
/// @date 03.02.2021
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include <bs/actor_common.h>
#include <bs/python/common.h>
#include <bs/python/result_converter.h>
#include <bs/detail/function_view.h>

#include "../kernel/radio_subsyst.h"

#include <optional>
#include <tuple>

NAMESPACE_BEGIN(blue_sky::python)

template<typename F, typename R, typename... Args, typename... EnqArgs>
auto pipe_queue_impl(F&& f, const identity<R (Args...)> _, EnqArgs... enq_args) {
	return [f = std::make_shared<meta::remove_cvref_t<F>>(std::forward<F>(f))](Args... args) {
		KRADIO.enqueue(
			EnqArgs{}...,
			[f, argtup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
				std::apply(*f, std::move(argtup));
				return perfect;
			}
		);
	};
};

// run any given callable through kernel's queue
template<typename F, typename... EnqArgs>
auto pipe_through_queue(F&& f, EnqArgs... enq_args) {
	return pipe_queue_impl(std::forward<F>(f), identity< deduce_callable_t<F> >{}, enq_args...);
};

// run Python transaction (applied to link/object) through queue
template<typename... Ts>
auto pytr_through_queue(std::function< py::object(Ts...) > tr, bool launch_async) {
	// If we know that calling side is going to wait for `tr` result (launch_async == false)
	// then force running `tr` in anon queue if we're currently inside anoter transaction
	return [
		tr = make_result_converter<tr_result>(std::move(tr), perfect),
		force_anon = launch_async ? false : KRADIO.is_queue_thread()
	](caf::event_based_actor* papa, Ts... args) mutable -> caf::result<tr_result::box> {
		return KRADIO.enqueue(
			papa,
			// wrap tr in `optional` to implement early release
			[tr = std::optional{std::move(tr)}, args = std::make_tuple(std::forward<Ts>(args)...)]() mutable {
				auto r = std::apply(std::move(*tr), std::move(args));
				tr.reset();
				return r;
			},
			force_anon
		);
	};
}

NAMESPACE_END(blue_sky::python)
