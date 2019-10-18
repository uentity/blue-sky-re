/// @file
/// @author uentity
/// @date 22.07.2019
/// @brief Link implementation that contain link state
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include "link_impl.h"
#include "link_actor.h"
#include "actor_common.h"
#include <bs/atoms.h>

#include <boost/uuid/random_generator.hpp>

NAMESPACE_BEGIN(blue_sky::tree)

// global random UUID generator for BS links
static boost::uuids::random_generator idgen;

/*-----------------------------------------------------------------------------
 *  link_impl
 *-----------------------------------------------------------------------------*/
link_impl::link_impl(std::string name, Flags f)
	: timeout_(def_timeout()), id_(idgen()), name_(std::move(name)), flags_(f)
{}

link_impl::link_impl()
	: link_impl("", Flags::Plain)
{}

link_impl::~link_impl() = default;

auto link_impl::spawn_actor(std::shared_ptr<link_impl> limpl) const -> caf::actor {
	return spawn_lactor<link_actor>(std::move(limpl));
}

auto link_impl::reset_owner(const sp_node& new_owner) -> void {
	auto solo = std::unique_lock{guard_};
	owner_ = std::move(new_owner);
}

auto link_impl::req_status(Req request) const -> ReqStatus {
	if(const auto i = (unsigned)request; i < 2) {
		auto guard = detail::scope_atomic_flag{ status_[i].flag };
		return status_[i].value;
	}
	return ReqStatus::Void;
}

auto link_impl::rs_reset(
	Req request, ReqReset cond, ReqStatus new_rs, ReqStatus old_rs,
	on_rs_changed_fn on_rs_changed
) -> ReqStatus {
	const auto i = (unsigned)request;
	if(i >= 2) return ReqStatus::Error;

	// atomic set value
	auto& S = status_[i];
	auto guard = detail::scope_atomic_flag(S.flag);
	const auto self = S.value;
	if( cond == ReqReset::Always ||
		(cond == ReqReset::IfEq && self == old_rs) ||
		(cond == ReqReset::IfNeq && self != old_rs)
	) {
		S.value = new_rs;
		// Data = OK will always fire (work as 'data changed' signal)
		if(
			!bool(cond & ReqReset::Silent) &&
			(new_rs != self || (request == Req::Data && new_rs == ReqStatus::OK))
		)
			on_rs_changed(request, new_rs, self);
	}
	return self;
}

///////////////////////////////////////////////////////////////////////////////
//  inode
//
auto link_impl::get_inode() -> result_or_err<inodeptr> {
	// default implementation obtains inode from `data()->inode_`
	return data().and_then([](const sp_obj& obj) {
		return obj ?
			result_or_err<inodeptr>(obj->inode_.lock()) :
			tl::make_unexpected(error::quiet(Error::EmptyData));
	});
}

auto link_impl::make_inode(const sp_obj& obj, inodeptr new_i) -> inodeptr {
	auto obj_i = obj ? obj->inode_.lock() : nullptr;
	if(!obj_i)
		obj_i = new_i ? std::move(new_i) : std::make_shared<inode>();

	if(obj)
		obj->inode_ = obj_i;
	return obj_i;
}

/*-----------------------------------------------------------------------------
 *  ilink_impl
 *-----------------------------------------------------------------------------*/
ilink_impl::ilink_impl(std::string name, const sp_obj& data, Flags f)
	: super(std::move(name), f), inode_(make_inode(data))
{}

ilink_impl::ilink_impl()
	: super()
{}

auto ilink::pimpl() const -> ilink_impl* {
	return static_cast<ilink_impl*>(super::pimpl());
}

auto ilink_impl::get_inode() -> result_or_err<inodeptr> {
	return inode_;
};

NAMESPACE_END(blue_sky::tree)