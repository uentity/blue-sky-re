/// @file
/// @author uentity
/// @date 14.07.2019
/// @brief Implementataion of node actor
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include "node_actor.h"
#include "node_extraidx_actor.h"
#include "link_impl.h"
#include "../serialize/tree_impl.h"

#include <bs/log.h>
#include <bs/tree/tree.h>
#include <bs/serialize/cafbind.h>
#include <bs/serialize/tree.h>

#include <caf/typed_event_based_actor.hpp>

#include <cereal/types/optional.hpp>
#include <cereal/types/vector.hpp>

#define DEBUG_ACTOR 0
#include "actor_debug.h"

NAMESPACE_BEGIN(blue_sky::tree)
using namespace allow_enumops;
using namespace std::chrono_literals;
using namespace std::string_literals;

[[maybe_unused]] auto adbg_impl(node_actor* A) -> caf::actor_ostream {
	auto res = caf::aout(A);
	res << "[N] ";
	if(auto pgrp = A->impl.home.get())
		res << "[" << pgrp->identifier() << "]";
	else
		res << "[homeless]";
	return res <<  ": ";
}

/*-----------------------------------------------------------------------------
 *  node_actor
 *-----------------------------------------------------------------------------*/
node_actor::node_actor(caf::actor_config& cfg, caf::group nhome, sp_nimpl Nimpl)
	: super(cfg), pimpl_(std::move(Nimpl)), impl([this]() -> node_impl& {
		if(!pimpl_) throw error{"node actor: bad (null) node impl passed"};
		return *pimpl_;
	}())
{
	// remember link's local group
	impl.home = std::move(nhome);
	if(impl.home)
		adbg(this) << "joined self group " << impl.home.get()->identifier() << std::endl;

	// exit after kernel
	KRADIO.register_citizen(this);

	// prevent termination in case some errors happens in group members
	// for ex. if they receive unexpected messages (translators normally do)
	set_error_handler([this](caf::error er) {
		switch(static_cast<caf::sec>(er.code())) {
		case caf::sec::unexpected_message :
		case caf::sec::request_timeout :
		case caf::sec::request_receiver_down :
			break;
		default:
			default_error_handler(this, er);
		}
	});

	set_default_handler([](auto*, auto&) -> caf::result<caf::message> {
		return caf::none;
	});
}

node_actor::~node_actor() = default;

auto node_actor::on_exit() -> void {
	adbg(this) << "dies" << std::endl;

	// be polite with everyone
	goodbye();
	// [IMPORTANT] manually reset pimpl, otherwise cycle won't break:
	// actor dtor never called until at least one strong ref to it still exists
	// (even though behavior is terminated by sending `exit` message)
	pimpl_.reset();

	KRADIO.release_citizen(this);
}

auto node_actor::goodbye() -> void {
	adbg(this) << "goodbye" << std::endl;
	if(auto& H = impl.home) {
		send(H, a_bye());
		leave(H);
	}
}

auto node_actor::name() const -> const char* {
	return "node_actor";
}

///////////////////////////////////////////////////////////////////////////////
//  leafs rename
//
auto node_actor::rename(std::vector<iterator<Key::Name>> namesakes, const std::string& new_name)
-> caf::result<std::size_t> {
	// renamer as recursive lambda
	auto res = make_response_promise<std::size_t>();
	auto do_rename = [=](auto&& self, auto work, size_t cur_res) mutable {
		if(work.empty()) {
			res.deliver(cur_res);
			return;
		}

		auto pos = work.back();
		work.pop_back();
		// [NOTE] use `await` to ensure node is not modified while link is renaming
		request(
			link_impl::actor(*pos), kernel::radio::timeout(), a_apply(),
			simple_transaction{[=]() -> error {
				adbg(this) << "-> a_lnk_rename [" << to_string(pos->id()) <<
					"][" << pos->name(unsafe) << "] -> [" << new_name << "]" << std::endl;

				impl.rename(pos, new_name);
				return perfect;
			}}
		).await(
			// on success rename next element
			[=, work = std::move(work)](error::box r) mutable {
				self(self, std::move(work), error(std::move(r)).ok() ? cur_res + 1 : cur_res);
			},
			// on error deliver number of already renamed leafs
			[=](const caf::error&) mutable { res.deliver(cur_res); }
		);
	};

	// launch rename work
	do_rename(do_rename, std::move(namesakes), 0);
	return res;
}

///////////////////////////////////////////////////////////////////////////////
//  leafs insert & erase
//
NAMESPACE_BEGIN()

const auto noop_await_insert = [](const error::box&) {};

template<typename Postproc, typename OnAwait = decltype(noop_await_insert)>
auto do_insert(
	node_actor* self, link L, InsertPolicy pol, Postproc pp, OnAwait aw = noop_await_insert
) -> caf::response_promise {
	// insert atomically for both node & link
	auto res = self->make_response_promise();
	self->request(
		link_impl::actor(L), kernel::radio::timeout(), a_apply(),
		simple_transaction([=, pp = std::move(pp)]() mutable -> error {
			adbg(self) << "-> a_node_insert [L][" << to_string(L.id()) <<
				"][" << L.name(unsafe) << "]" << std::endl;
			// make insertion
			auto ir = self->impl.insert(L, pol);
			// invoke postprocessing & deliver result
			res.deliver(pp(ir));
			// report success if insertion happened
			return ir.second ? perfect : quiet_fail;
		})
	).await(
		aw,
		// on error forward caf error to await handler
		[=, Lid = L.id()](const caf::error& er) mutable {
			aw(forward_caf_error(
				er, "in node["s + std::string(self->impl.home_id()) + "] insert link[" + to_string(Lid) + ']'
			));
		}
	);
	return res;
}

auto notify_after_insert(node_actor* self) {
	return [=](insert_status<Key::ID> res) -> node::insert_status {
		auto& [pchild, is_inserted] = res;
		if(is_inserted) {
			self->impl.send_home<high_prio>(
				self, a_ack(), self, a_node_insert(),
				pchild->id(), *self->impl.index<Key::ID>(pchild)
			);
		}
		return { self->impl.index<Key::ID>(std::move(res.first)), res.second };
	};
}

auto on_erase(const link& L, node_actor& self) {
	// collect link IDs of all deleted subtree elements
	// first elem is erased link itself
	lids_v lids{ L.id() };
	walk(L, [&lids](const link&, std::list<link> Ns, std::vector<link> Os) {
		const auto dump_erased = [&](const link& erl) {
			lids.push_back(erl.id());
		};
		std::for_each(Ns.cbegin(), Ns.cend(), dump_erased);
	});

	// send message that link erased
	self.impl.send_home<high_prio>(
		&self, a_ack(), &self, a_node_erase(), std::move(lids)
	);
}

NAMESPACE_END()

auto node_actor::insert(link L, InsertPolicy pol) -> caf::response_promise {
	return do_insert(this, L, pol, notify_after_insert(this));
}

auto node_actor::insert(link L, std::size_t to_idx, InsertPolicy pol) -> caf::response_promise {
	return do_insert(this, L, pol, [=](insert_status<Key::ID> res) mutable -> node::insert_status {
		// 1. insert an element using ID index
		auto [pchild, is_inserted] = res;
		auto res_idx = impl.index<Key::ID>(pchild);
		if(!res_idx) return { res_idx, is_inserted };

		// 2. reposition an element in AnyOrder index
		to_idx = std::min(to_idx, impl.size());
		auto from = impl.project<Key::ID>(pchild);
		auto to = std::next(impl.begin(), to_idx);
		// noop if to == from
		impl.links_.get<Key_tag<Key::AnyOrder>>().relocate(to, from);

		// detect move and send proper message
		if(is_inserted) // normal insert
			impl.send_home<high_prio>(
				this, a_ack(), this, a_node_insert(), pchild->id(), to_idx
			);
		else if(to != from) // move
			impl.send_home<high_prio>(
				this, a_ack(), this, a_node_insert(), pchild->id(), to_idx, *res_idx
			);
		return { to_idx, is_inserted };
	});
}

auto node_actor::insert(links_v Ls, InsertPolicy pol) -> caf::result<std::size_t> {
	auto res = make_response_promise<std::size_t>();
	auto insert_many = [=](auto&& self, auto work, size_t cur_res) mutable {
		if(work.empty()) {
			res.deliver(cur_res);
			return;
		}

		auto L = work.back();
		work.pop_back();
		do_insert(
			this, L, pol, notify_after_insert(this),
			[=, work = std::move(work)](error::box erb) mutable {
				if(auto er = error(std::move(erb))) {
					// if CAF error happens, stop insertion & deliver result
					// otherwise just not increment inserted leafs counter
					if(er.code.category().name() == "CAF"s) {
						res.deliver(cur_res);
						return;
					}
				}
				else
					cur_res += 1;
				// recurse to next element
				self(self, std::move(work), cur_res);
			}
		);
	};

	insert_many(insert_many, std::move(Ls), 0);
	return res;
}

auto node_actor::erase(const lid_type& victim, EraseOpts opts) -> size_t {
	const auto ppf = [=](const link& L) { on_erase(L, *this); };
	std::size_t res = 0;
	error::eval_safe([&] { res = impl.erase<Key::ID>(
		victim,
		enumval(opts & EraseOpts::Silent) ? noop : function_view{ ppf }
	); });
	return res;
}

///////////////////////////////////////////////////////////////////////////////
//  primary behavior
//
auto node_actor::make_primary_behavior() -> primary_actor_type::behavior_type {
return {
	[=](a_impl) -> sp_nimpl { return pimpl_; },

	[=](a_apply, simple_transaction tr) -> error::box {
		return tr_eval(std::move(tr));
	},

	[=](a_apply, node_transaction tr) -> error::box {
		return tr_eval(std::move(tr), bare_node(pimpl_));
	},

	// unconditionally join home group - used after deserialization
	[=](a_hi) { join(impl.home); },

	[=](a_bye) { if(current_sender() != this) quit(); },

	[=](a_home) { return impl.home; },

	[=](a_home_id) { return std::string(impl.home_id()); },

	[=](a_node_handle) { return impl.handle(); },

	[=](a_node_size) { return impl.size(); },

	[=](a_node_leafs, Key order) -> caf::result<links_v> {
		adbg(this) << "{a_node_leafs} " << static_cast<int>(order) << std::endl;
		if(has_builtin_index(order))
			return impl.leafs(order);
		return delegate(
			system().spawn(extraidx_search_actor),
			a_node_leafs(), order, impl.leafs(Key::AnyOrder)
		);
	},

	[=](a_node_keys, Key order) -> caf::result<lids_v> {
		// builtin indexes can be processed directly
		if(has_builtin_index(order))
			return impl.keys(order);
		// others via extra index actor
		return delegate(
			system().spawn(extraidx_search_actor),
			a_node_keys(), order, impl.leafs(Key::AnyOrder)
		);
	},

	[=](a_node_ikeys, Key order) -> caf::result<std::vector<std::size_t>> {
		// builtin indexes can be processed directly
		if(has_builtin_index(order))
			return impl.ikeys(order);

		// others via extra sorted leafs
		auto rp = make_response_promise();
		request(
			system().spawn(extraidx_search_actor), kernel::radio::timeout(true),
			a_node_leafs(), order, impl.leafs(Key::AnyOrder)
		).then(
			[=](const links_v& leafs) mutable {
				rp.deliver( impl.ikeys(leafs.begin(), leafs.end()) );
			}
		);
		return rp;
	},

	[=](a_node_keys, Key meaning, Key order) -> caf::result<std::vector<std::string>> {
		return delegate(
			system().spawn(extraidx_search_actor),
			a_node_keys(), meaning, order, impl.leafs(Key::AnyOrder)
		);
	},

	// find
	[=](a_node_find, const lid_type& lid) -> link {
		adbg(this) << "-> a_node_find lid " << to_string(lid) << std::endl;
		auto res = impl.search<Key::ID>(lid);
		return res;
	},

	[=](a_node_find, std::size_t idx) -> link {
		adbg(this) << "-> a_node_find idx " << idx << std::endl;
		return impl.search<Key::AnyOrder>(idx);
	},

	[=](a_node_find, std::string key, Key key_meaning) -> caf::result<link> {
		adbg(this) << "-> a_node_find key " << key << std::endl;
		if(has_builtin_index(key_meaning))
			return impl.search(key, key_meaning);
		else
			return delegate(
				system().spawn(extraidx_search_actor),
				a_node_find(), std::move(key), key_meaning, impl.values<Key::AnyOrder>()
			);
	},

	// deep search
	[=](a_node_deep_search, lid_type lid) -> caf::result<link> {
		adbg(this) << "-> a_node_deep_search lid " << to_string(lid) << std::endl;
		return delegate(
			system().spawn(extraidx_deep_search_actor, actor()),
			a_node_deep_search(), std::move(lid)
		);
	},

	[=](a_node_deep_search, std::string key, Key key_meaning, bool search_all)
	-> caf::result<links_v> {
		adbg(this) << "-> a_node_deep_search key " << key << std::endl;
		return delegate(
			system().spawn(extraidx_deep_search_actor, actor()),
			a_node_deep_search(), std::move(key), key_meaning, search_all
		);
	},

	// index
	[=](a_node_index, const lid_type& lid) -> existing_index {
		return impl.index<Key::ID>(lid);
	},

	[=](a_node_index, std::string key, Key key_meaning) -> caf::result<existing_index> {
		if(has_builtin_index(key_meaning))
			return impl.index(key, key_meaning);
		else
			return delegate(
				system().spawn(extraidx_search_actor),
				a_node_index(), std::move(key), key_meaning, impl.values<Key::AnyOrder>()
			);
	},

	// equal_range
	[=](a_node_equal_range, std::string key, Key key_meaning) -> caf::result<links_v> {
		if(has_builtin_index(key_meaning))
			return impl.equal_range(key, key_meaning);
		else
			return delegate(
				system().spawn(extraidx_search_actor),
				a_node_equal_range(), std::move(key), key_meaning, impl.values<Key::AnyOrder>()
			);
	},

	// insert new link
	[=](a_node_insert, link L, InsertPolicy pol) -> caf::result<node::insert_status> {
		return insert(std::move(L), pol);
	},

	// insert into specified position
	[=](a_node_insert, link L, std::size_t idx, InsertPolicy pol) -> caf::result<node::insert_status> {
		return insert(std::move(L), idx, pol);
	},

	// insert bunch of links
	[=](a_node_insert, links_v Ls, InsertPolicy pol) {
		return insert(std::move(Ls), pol);
	},

	// normal link erase
	[=](a_node_erase, const lid_type& lid) -> std::size_t {
		return erase(lid);
	},

	// all other erase overloads do normal erase
	[=](a_node_erase, std::size_t idx) {
		return impl.erase<Key::AnyOrder>(
			idx, [=](const link& L) { on_erase(L, *this); }
		);
	},

	[=](a_node_erase, std::string key, Key key_meaning) -> caf::result<std::size_t>{
		if(has_builtin_index(key_meaning)) {
			auto res = std::size_t{};
			error::eval_safe([&] {
				res = impl.erase(
					key, key_meaning, [=](const link& L) { on_erase(L, *this); }
				);
			});
			return res;
		}
		else
			return delegate(
				system().spawn(extraidx_erase_actor, actor()),
				a_node_erase(), std::move(key), key_meaning, impl.values<Key::AnyOrder>()
			);
	},

	// erase bunch of links
	[=](a_node_erase, const lids_v& lids) {
		return impl.erase(
			lids, [=](const link& L) { on_erase(L, *this); }
		);
	},

	[=](a_node_clear) -> std::size_t {
		auto res = impl.links_.size();
		impl.links_.clear();
		return res;
	},

	// rename
	[=](a_lnk_rename, const lid_type& lid, const std::string& new_name) -> caf::result<std::size_t> {
		return rename<Key::ID>(lid, new_name);
	},

	[=](a_lnk_rename, std::size_t idx, const std::string& new_name) -> caf::result<std::size_t> {
		return rename<Key::AnyOrder>(idx, new_name);
	},

	[=](a_lnk_rename, const std::string& old_name, const std::string& new_name) -> caf::result<std::size_t> {
		return rename<Key::Name>(old_name, new_name);
	},

	// apply custom order
	[=](a_node_rearrange, const std::vector<std::size_t>& new_order) -> error::box {
		return error::eval_safe([&] { impl.rearrange<Key::AnyOrder>(new_order); });
	},

	[=](a_node_rearrange, const lids_v& new_order) -> error::box {
		return error::eval_safe([&] { impl.rearrange<Key::ID>(new_order); });
	},

	/// Non-public extensions
	// erase link by ID with specified options
	[=](a_node_erase, const lid_type& lid, EraseOpts opts) -> std::size_t {
		return erase(lid, opts);
	},
}; }

auto node_actor::make_behavior() -> behavior_type {
	return actor_type::behavior_type{first_then_second(
		make_ack_behavior(), make_primary_behavior()
	)}.unbox();
}

NAMESPACE_END(blue_sky::tree)
