/// @file
/// @author uentity
/// @date 30.03.2020
/// @brief BS tree link events handling
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include "link_actor.h"
#include "ev_listener_actor.h"

#include <bs/kernel/radio.h>
#include <bs/log.h>
#include <bs/serialize/cafbind.h>
#include <bs/serialize/tree.h>

NAMESPACE_BEGIN(blue_sky::tree)
using event_handler = link::event_handler;

static auto make_listener(const link& self, event_handler f, Event listen_to) {
	using namespace kernel::radio;
	using namespace allow_enumops;
	using baby_t = ev_listener_actor<link>;

	static const auto handler_impl = [](
		baby_t* self, auto& weak_root, Event ev, prop::propdict params
	) {
		if(auto r = weak_root.lock())
			self->f(std::move(r), ev, std::move(params));
		else
			self->quit();
	};

	// produce event bhavior that calls passed callback with proper params
	auto make_ev_character = [weak_root = link::weak_ptr{self}, src_id = self.id(), listen_to](baby_t* self) {
		auto res = caf::message_handler{};
		if(enumval(listen_to & Event::LinkRenamed))
			res = res.or_else(
				[=](a_ack, a_lnk_rename, std::string new_name, std::string old_name) {
					handler_impl(self, weak_root, Event::LinkRenamed, {
						{"new_name", std::move(new_name)},
						{"prev_name", std::move(old_name)}
					});
				}
			);

		if(enumval(listen_to & Event::LinkStatusChanged))
			res = res.or_else(
				[=](a_ack, a_lnk_status, Req request, ReqStatus new_v, ReqStatus prev_v) {
					handler_impl(self, weak_root, Event::LinkStatusChanged, {
						{"request", new_v},
						{"new_status", new_v},
						{"prev_status", prev_v}
					});
				}
			);

		if(enumval(listen_to & Event::DataModified))
			res = res.or_else(
				[=](a_ack, a_data, tr_result::box tres_box) {
					auto params = prop::propdict{};
					if(auto tres = tr_result{std::move(tres_box)})
						params = extract_info(std::move(tres));
					else
						params["error"] = to_string(extract_err(std::move(tres)));
					handler_impl(self, weak_root, Event::DataModified, std::move(params));
				}
			);

		if(enumval(listen_to & Event::LinkDeleted))
			res = res.or_else(
				[=](a_bye) {
					self->quit();
					// distinguish link's bye signal from kernel kill all
					if(self->current_sender() == self->origin)
						// [NOTE] week_root can possibly be already expired but callback needs to be called
						self->f(weak_root.lock(), Event::LinkDeleted, {{"link_id", src_id}});
				}
			);

		return res;
	};

	// make baby event handler actor
	return system().spawn<baby_t, caf::lazy_init>(
		self.actor().address(), std::move(f), std::move(make_ev_character)
	);
}

auto link::subscribe(event_handler f, Event listen_to) const -> std::uint64_t {
	// ensure it has started & properly initialized
	// throw exception otherwise
	if(auto res = actorf<std::uint64_t>(
		pimpl()->actor(*this), infinite, a_subscribe(),
		make_listener(*this, std::move(f), listen_to)
	))
		return *res;
	else
		throw res.error();
}

auto link::subscribe(launch_async_t, event_handler f, Event listen_to) const -> std::uint64_t {
	auto baby = make_listener(*this, std::move(f), listen_to);
	auto baby_id = baby.id();
	caf::anon_send(pimpl()->actor(*this), a_subscribe{}, std::move(baby));
	return baby_id;
}

auto link::unsubscribe(deep_t) const -> void {
	if(is_nil()) return;
	unsubscribe();
	apply(launch_async, [](bare_link self) {
		if(auto N = self.data_node())
			N.unsubscribe(deep);
		return perfect;
	});
}

NAMESPACE_END(blue_sky::tree)
