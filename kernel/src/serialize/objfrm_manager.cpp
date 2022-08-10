/// @author uentity
/// @date 08.04.2020
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include "tree_fs_impl.h"

#include <bs/actor_common.h>
#include <bs/objbase.h>
#include <bs/tree/errors.h>

#include "../objbase_actor.h"

#include <fmt/format.h>

#include <algorithm>

NAMESPACE_BEGIN(blue_sky::detail)
using errb_vector = std::vector<error::box>;
using blue_sky::tree::Error;

objfrm_manager::objfrm_manager(caf::actor_config& cfg, bool is_saving) :
	objfrm_manager_t::base(cfg), is_saving_(is_saving)
{}

auto objfrm_manager::session_ack() -> void {
	if(session_finished_ && (nstarted_ == nfinished_) && res_.pending()) {
		res_.deliver(objfrm_result_t{std::move(er_stack_), std::move(empty_payload_)});
		nstarted_ = nfinished_ = 0;
		session_finished_ = false;
	}
}

auto objfrm_manager::make_behavior() -> behavior_type {
return {
	// stop session
	[=](a_bye) {
		if(!session_finished_) {
			session_finished_ = true;
			session_ack();
		}
	},

	// process given object
	[=](const sp_obj& obj, std::string fmt_name, std::string fname) {
		// sanity
		if(!obj) er_stack_.emplace_back(error{Error::EmptyData});
		// run job in object's queue
		++nstarted_;
		auto objA = objbase_actor::actor(*obj);
		auto frm_job = is_saving_ ?
			request(objA, caf::infinite, a_save(), std::move(fmt_name), std::move(fname)) :
			request(objA, caf::infinite, a_load(), std::move(fmt_name), std::move(fname))
		;
		// process result
		frm_job.then([=, obj_hid = to_uuid(obj->home_id())](error::box er) {
			// save result of finished job & inc finished counter
			++nfinished_;
			// check if obj had empty payload
			static const auto empty_data_ec = make_error_code(Error::EmptyData);
			if(er.ec == empty_data_ec.value() && er.domain == empty_data_ec.category().name())
				obj_hid.map([&](auto& obj_uid) { empty_payload_.emplace_back(obj_uid); });
			// otherwise collect store error if eny
			else if(er.ec)
				er_stack_.push_back(std::move(er));
			session_ack();
		}, [=](const caf::error& er) {
			// in case smth went wrong with job posting
			--nstarted_;
			er_stack_.emplace_back(forward_caf_error(er, fmt::format(
				"failed to enqueue {} job: object[{}, {}] <-> {}",
				(is_saving_ ? "save" : "load"), obj->type_id(), obj->id(), fname
			)));
		});
	},

	[=](a_ack) -> caf::result<objfrm_result_t> {
		res_ = make_response_promise<objfrm_result_t>();
		session_ack();
		return res_;
	}
}; }

auto objfrm_manager::wait_jobs_done(objfrm_manager_t self, timespan how_long)
-> std::pair<std::vector<error>, std::vector<uuid>> {
	auto fmanager = caf::make_function_view(
		self, how_long == infinite ? caf::infinite : how_long
	);

	auto res_errors = std::vector<error>{};
	auto res = actorf<objfrm_result_t>(fmanager, a_ack());
	if(res) {
		auto& [boxed_errs, empty_payload] = *res;
		res_errors.reserve(boxed_errs.size());
		for(auto& er_box : boxed_errs)
			res_errors.push_back( error::unpack(std::move(er_box)) );
		return { std::move(res_errors), std::move(empty_payload) };
	}
	else {
		res_errors.push_back(std::move(res.error()));
		return { std::move(res_errors), {} };
	}
}

NAMESPACE_END(blue_sky::detail)
