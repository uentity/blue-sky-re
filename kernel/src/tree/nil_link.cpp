/// @file
/// @author uentity
/// @date 06.02.2020
/// @brief Nil (invalid) link impl
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include "nil_engine.h"
#include "nil_engine_impl.h"
#include "link_impl.h"

#include <bs/defaults.h>

NAMESPACE_BEGIN(blue_sky::tree)
///////////////////////////////////////////////////////////////////////////////
//  base nil actor
//
nil_engine_actor::nil_engine_actor(caf::actor_config& cfg) :
	super(cfg)
{
	// never die on error or exit msg
	set_error_handler(noop);
	set_exit_handler(noop);
	// completely ignore unexpected messages without error backpropagation
	set_default_handler(noop_r<caf::message>());
}

///////////////////////////////////////////////////////////////////////////////
//  nil link actor
//
struct nil_link::self_actor : nil_engine_actor {
	using super = nil_engine_actor;
	using super::super;

	auto name() const -> const char* override { return "nil_link actor"; }

	auto make_behavior() -> behavior_type override { return link::actor_type::behavior_type{
	
		[](a_home) -> caf::group { return {}; },
		[](a_home_id) -> std::string { return nil_oid; },
		[](a_lnk_id) -> lid_type { return nil_uid; },
		[](a_lnk_oid) -> std::string { return nil_oid; },
		[](a_lnk_otid) -> std::string { return nil_otid; },
		[](a_clone, bool) { return link{}; },

		// deny rename
		[](a_lnk_name) -> std::string { return defaults::tree::nil_link_name; },
		[](a_lnk_rename, const std::string&) -> std::size_t { return 0; },

		// status alwaye Void
		[](a_lnk_status, Req) -> ReqStatus { return ReqStatus::Void; },
		[](a_lnk_status, Req, ReqReset, ReqStatus, ReqStatus) -> ReqStatus {
			return ReqStatus::Void;
		},

		[](a_lnk_flags) { return Flags::Nil; },
		[](a_lnk_flags, Flags) {},

		// all data is null
		[](a_lnk_inode) -> result_or_errbox<inodeptr> {
			return unexpected_err(Error::EmptyInode);
		},
		[](a_data, bool) -> obj_or_errbox {
			return unexpected_err(Error::EmptyData);
		},
		[](a_data_node, bool) -> node_or_errbox {
			return unexpected_err(Error::EmptyData);
		},

		[](a_apply, const link_transaction&) -> tr_result::box {
			return error{blue_sky::Error::TrEmptyTarget};
		},
		[](a_apply, a_data, const obj_transaction&) -> tr_result::box {
			return pack(error{blue_sky::Error::TrEmptyTarget});
		}

	}.unbox(); }

	auto on_exit() -> void override {
		nil_link::reset();
	}
};

///////////////////////////////////////////////////////////////////////////////
//  nil link impl
//
struct nil_link::self_impl : nil_engine_impl<nil_link, link_impl> {
	using super = nil_engine_impl<nil_link, link_impl>;
	using super::super;

	// always return same actor from internals
	auto spawn_actor(sp_limpl) const -> caf::actor override {
		return internals().raw_actor();
	}

	auto clone(link_actor*, bool deep) const -> caf::result<sp_limpl> override {
		return nullptr;
	}

	auto data() -> obj_or_err override {
		return tl::make_unexpected(Error::EmptyData);
	}

	auto data(unsafe_t) const -> sp_obj override { return nullptr; }

	self_impl()
		: super(defaults::tree::nil_link_name, Flags::Nil)
	{
		id_ = nil_uid;
	}

	ENGINE_TYPE_DECL
};

ENGINE_TYPE_DEF(nil_link::self_impl, "__nil_link__")

///////////////////////////////////////////////////////////////////////////////
//  nil link
//
auto nil_link::nil_engine() -> const engine& {
	return nil_link::self_impl::internals();
}

auto nil_link::pimpl() -> const engine::sp_engine_impl& {
	return nil_link::self_impl::internals().pimpl_;
}

auto nil_link::actor() -> const engine::sp_ahandle& {
	return nil_link::self_impl::internals().actor_;
}

auto nil_link::reset() -> void {
	nil_link::self_impl::internals().reset();
}

auto nil_link::stop(bool wait_exit) -> void {
	nil_link::self_impl::internals().stop(wait_exit);
}

NAMESPACE_END(blue_sky::tree)
