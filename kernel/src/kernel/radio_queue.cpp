/// @author Alexander Gagarin (@uentity)
/// @date 19.02.2021
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include "radio_subsyst.h"

#include <bs/actor_common.h>
#include <bs/kernel/radio.h>

#include <caf/typed_event_based_actor.hpp>

NAMESPACE_BEGIN(blue_sky::kernel::detail)
NAMESPACE_BEGIN()

// actor that implements kernel's queue
auto kqueue_processor(kqueue_actor_type::pointer self) -> kqueue_actor_type::behavior_type {
	// never die on error
	self->set_error_handler(noop);
	// completely ignore unexpected messages without error backpropagation
	self->set_default_handler(noop_r<caf::message>());

	return {
		[](const transaction& tr) -> tr_result::box {
			return pack(tr_eval(tr));
		},

		[](a_ack) { return std::this_thread::get_id(); }
	};
}

NAMESPACE_END()

///////////////////////////////////////////////////////////////////////////////
//  queue management
//
auto radio_subsyst::spawn_queue() -> void {
	// we can save thread ID, because detached actors run in their own thread
	queue_ = actor_sys_->spawn<caf::detached>(kqueue_processor);
	// [NOTE] need to explicitly supply scoped_actor because radio init is not yet finished
	auto r = caf::scoped_actor{*actor_sys_};
	actorf<std::thread::id>(r, queue_, kernel::radio::timeout(), a_ack_v)
	.map([this](std::thread::id tid) { queue_tid_ = std::move(tid); })
	.map_error([](const error& er) { throw er; });
}

auto radio_subsyst::queue_thread_id() const -> std::thread::id {
	return queue_tid_;
}

auto radio_subsyst::stop_queue(bool wait_exit) -> void {
	if(!queue_) return;
	auto self = caf::scoped_actor{system(), false};
	self->send_exit(queue_, caf::exit_reason::user_shutdown);
	if(wait_exit)
		self->wait_for(queue_);
	// [NOTE} need to explicitly reset queue handle, otherwise exit hangs
	// [TODO] check if this bug present in CAF 0.18
	queue_ = nullptr;
}

auto radio_subsyst::queue_actor() -> kqueue_actor_type& { return queue_; }

auto radio_subsyst::enqueue(transaction tr) -> tr_result {
	return actorf<tr_result>(queue_actor(), kernel::radio::timeout(true), std::move(tr));
}

auto radio_subsyst::enqueue(launch_async_t, transaction tr) -> void {
	caf::anon_send(queue_actor(), std::move(tr));
}

NAMESPACE_END(blue_sky::kernel::detail)
