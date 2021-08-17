/// @date 05.10.2020
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include "map_engine.h"
#include "node_impl.h"
#include "request_impl.h"

#include <bs/tree/tree.h>
#include <bs/detail/enumops.h>

#include <caf/stateful_actor.hpp>

#include <algorithm>

#define DEBUG_ACTOR 0
#include "actor_debug.h"

NAMESPACE_BEGIN(blue_sky::tree)
NAMESPACE_BEGIN()
///////////////////////////////////////////////////////////////////////////////
//  helper retranslator that forwards acks from input node to parent `map_link`
//
struct iar_state {
	map_impl_base::map_actor_type papa;
	node::actor_type input, output;
};

auto input_ack_retranslator(
	caf::stateful_actor<iar_state>* self, map_impl_base::map_actor_type papa,
	node::actor_type input, node::actor_type output,
	Event update_on, TreeOpts opts
) {
	using namespace allow_enumops;

	self->state = { std::move(papa), std::move(input), std::move(output) };

	// ignore unexpected messages
	self->set_default_handler(noop_r<caf::message>());

	const auto send_parent = [self, update_on, opts]
	(Event src_ev, const lid_type& src_id, caf::actor origin = {}) {
		// skip unsibscribed events
		if(!enumval(update_on & src_ev)) return;

		const auto& [papa, input, output] = self->state;
		if(!origin) origin = caf::actor_cast<caf::actor>(input);

		// find source link (depending on deep flag)
		auto notify_parent = [=, ev = event{ std::move(origin), {{"link_id", src_id}}, src_ev }]
		() mutable {
			self->send(self->state.papa, a_ack(), a_apply(), src_id, std::move(ev));
		};

		// check if event comes from output node and must be muted
		if(enumval(opts & TreeOpts::MuteOutputNode)) {
			// send notification only if `src_id` wasn't found in output node
			if(enumval(opts & TreeOpts::Deep))
				self->request(output, caf::infinite, a_node_deep_search(), src_id)
				.then([notify_parent = std::move(notify_parent)](const links_v& ls) mutable {
					if(ls.empty()) notify_parent();
				});
			else
				self->request(output, caf::infinite, a_node_find(), src_id)
				.then([notify_parent = std::move(notify_parent)](const link& lnk) mutable {
					if(!lnk) notify_parent();
				});
		}
		else
			notify_parent();
	};

	// build behavior
	// 1. Base behavior
	auto res = first_then_second( node_impl::leafs_ack_actor_type::behavior_type{

		// Input node direct leafs acks are always retranslated
		// leaf rename
		[=](a_ack, const lid_type& lid, a_lnk_rename, const std::string&, const std::string&)
		{ send_parent(Event::LinkRenamed, lid); },
		// leaf status
		[=](a_ack, const lid_type& lid, a_lnk_status, Req, ReqStatus, ReqStatus)
		{ send_parent(Event::LinkStatusChanged, lid); },
		// leaf data altered by transaction
		[=](a_ack, const lid_type& lid, a_data, const tr_result::box&)
		{ send_parent(Event::DataModified, lid); }

	}, node_impl::self_ack_actor_type::behavior_type{

		// ack on insert
		[=](a_ack, caf::actor N, a_node_insert, const lid_type& lid, size_t)
		{ send_parent(Event::LinkInserted, lid, std::move(N)); },
		// ignore moves within same node
		[=](a_ack, const caf::actor& N, a_node_insert, const lid_type&, size_t, size_t) {},
		// ack on erase
		[=](a_ack, const caf::actor& N, a_node_erase, const lids_v& erased_leafs) {
			if(erased_leafs.empty()) return;
			send_parent(Event::LinkErased, erased_leafs.front(), N);
			if(enumval(opts & TreeOpts::Deep)) {
				std::for_each(erased_leafs.begin()++, erased_leafs.end(), [&](auto& cut_leaf) {
					send_parent(Event::LinkErased, cut_leaf, N);
				});
			}
		}

	}).unbox();

	// 2. Deeper layers - retranslate only if deep processign enabled
	if(enumval(opts & TreeOpts::Deep))
		return first_then_second(std::move(res), link_impl::deep_ack_actor_type::behavior_type{
			// leaf rename
			[=](
				a_ack, caf::actor N, const lid_type& lid,
				a_lnk_rename, const std::string&, const std::string&
			) { send_parent(Event::LinkRenamed, lid, std::move(N)); },
			// leaf status
			[=](a_ack, caf::actor N, const lid_type& lid, a_lnk_status, Req, ReqStatus, ReqStatus)
			{ send_parent(Event::LinkStatusChanged, lid, std::move(N)); },
			// leaf data altered by transaction
			[=](a_ack, caf::actor N, const lid_type& lid, a_data, const tr_result::box&)
			{ send_parent(Event::DataModified, lid, std::move(N)); }
		});
	else
		return res;
}

NAMESPACE_END()

/*-----------------------------------------------------------------------------
 *  map_link_actor
 *-----------------------------------------------------------------------------*/
map_link_actor::map_link_actor(caf::actor_config& cfg, caf::group self_grp, sp_limpl Limpl) :
	super(cfg, std::move(self_grp), std::move(Limpl))
{
	// auto-respawn listener when it exits
	set_down_handler([=](const caf::down_msg&) {
		const auto& simpl = mimpl();
		adbg(this) << "starting listener on input node, is_nil = " << simpl.in_.is_nil()
			<< ", update_on = " << enumval(simpl.update_on_) << std::endl;

		if(simpl.in_ && enumval(simpl.update_on_))
			// [NOTE] spawn monitored actor <=> calling monitor(...) afterwards
			inp_listener_ = this->spawn_in_group<caf::monitored>(
				simpl.in_.home(), input_ack_retranslator,
				caf::actor_cast<map_impl_base::map_actor_type>(this), simpl.in_.actor(), simpl.out_.actor(),
				simpl.update_on_, simpl.opts_
			);
		else {
			inp_listener_ = nullptr;
			adbg(this) << "~~~ Event listener not started!" << std::endl;
		}
	});

	// start input node events tracker
	reset_input_listener();
}

auto map_link_actor::reset_input_listener() -> void {
	// copy detached flag to ReqOpts
	const auto& simpl = mimpl();
	if(enumval(simpl.opts_ & TreeOpts::DetachedWorkers))
		ropts_.data_node |= ReqOpts::Detached;
	else
		ropts_.data_node &= ~ReqOpts::Detached;

	// if listener wasn't started fake it's death to trigger respawn
	// otherwise kill & restart running one
	if(!inp_listener_)
		send<caf::message_priority::high>(this, caf::down_msg{});
	else
		send_exit(inp_listener_, caf::exit_reason::user_shutdown);
}

auto map_link_actor::on_exit() -> void {
	// stop respawning input listener
	demonitor(inp_listener_);
	// early destroy mapper
	if(mimpl().is_link_mapper)
		static_cast<map_link_impl&>(mimpl()).mf_ = nullptr;
	else
		static_cast<map_node_impl&>(mimpl()).mf_ = nullptr;
	super::on_exit();
	// explicitly terminate listener
	send_exit(inp_listener_, caf::exit_reason::user_shutdown);
}

auto map_link_actor::make_casual_behavior() -> typed_behavior {
	return first_then_second(typed_behavior_overload{

		[=](a_data, bool) -> obj_or_errbox {
			adbg(this) << "<- a_data" << std::endl;
			return unexpected_err_quiet(Error::EmptyData);
		},

		[=](a_data_node, bool wait_if_busy) -> caf::result<node_or_errbox> {
			adbg(this) << "<- a_data_node (casual)" << std::endl;
			return request_data_node(
				unsafe, *this,
				ReqOpts::HasDataCache | (wait_if_busy ? ReqOpts::WaitIfBusy : ReqOpts::ErrorIfBusy)
			);
		},

		[=](a_lazy, a_node_clear) {
			adbg(this) << "<- lazy clear" << std::endl;
			// [NOTE] installs refresh behavior (default) that will force clear + remap
			become(make_behavior().unbox());
		},

		[=](a_node_clear) -> caf::result<node_or_errbox> {
			adbg(this) << "<- immediate clear" << std::endl;
			become(make_behavior().unbox());
			// imediately invoke DataNode request on refresh behavior
			return delegate(caf::actor_cast<actor_type>(this), a_data_node(), true);
		},

		[=](a_ack, a_apply, const lid_type& src_id, event ev) {
			adbg(this) << "<- update (casual)" << std::endl;
			if(mimpl().is_link_mapper) {
				const auto src_node = caf::actor_cast<node::actor_type>(ev.origin);
				request(src_node, caf::infinite, a_node_find(), src_id)
				.then([=, ev = std::move(ev)](const link& inp_link) mutable {
					mimpl().update(this, inp_link, std::move(ev));
				});
			}
			else
				// node mapper doesn't care about particular source link
				mimpl().update(this, link{}, std::move(ev));
		},

		[=](a_ack, a_node_erase, const lid_type& src_id, event ev) {
			adbg(this) << "<- erase (casual)" << std::endl;
			mimpl().erase(this, src_id, std::move(ev));
		},

		[](a_mlnk_fresh) { return true; },

		[=](a_apply, const Event update_on, const TreeOpts opts) {
			auto& simpl = mimpl();
			simpl.update_on_ = update_on;
			simpl.opts_ = opts;
			reset_input_listener();
		}

	}, super::make_typed_behavior());
}

auto map_link_actor::make_refresh_behavior() -> refresh_behavior_overload {
	auto refresh_once = [this, casual_bhv = make_casual_behavior().unbox()](event ev) {
		adbg(this) << "<- a_data_node (refresh)" << std::endl;
		// install casual behavior
		become(casual_bhv);
		// invoke refresh
		return mimpl().refresh(this, std::move(ev));
	};

	// invoke refresh once on DataNode request
	return refresh_behavior_overload{
		// if output node is filled after deserialization, just switch to casual bhv
		[=, casual_bhv = make_casual_behavior().unbox()](a_mlnk_fresh) {
			become(casual_bhv);
			return true;
		},

		[=](a_data_node, bool) -> caf::result<node_or_errbox> {
			return refresh_once({this, {}, Event::Nil});
		},

		[=](a_ack, a_apply, const lid_type&, event ev) {
			adbg(this) << "<- update (refresh)" << std::endl;
			refresh_once(std::move(ev));
		},

		[=](a_ack, a_node_erase, const lid_type&, event ev) {
			adbg(this) << "<- erase (refresh)" << std::endl;
			refresh_once(std::move(ev));
		}

	};
}

auto map_link_actor::make_behavior() -> behavior_type {
	return first_then_second(make_refresh_behavior(), make_casual_behavior()).unbox();
}

NAMESPACE_END(blue_sky::tree)
