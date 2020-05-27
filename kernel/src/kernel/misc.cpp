/// @file
/// @author uentity
/// @date 14.01.2019
/// @brief Kernel misc API impl
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include <bs/error.h>
#include <bs/kernel/misc.h>
#include <bs/misc.h>
#include "kimpl.h"

#include <spdlog/spdlog.h>

NAMESPACE_BEGIN(blue_sky::kernel)

auto init() -> void {
	using InitState = kimpl::InitState;

	// do initialization only once from non-initialized state
	auto expected_state = InitState::NonInitialized;
	if(KIMPL.init_state_.compare_exchange_strong(expected_state, InitState::Initialized)) {
		// configure kernel
		KIMPL.configure();
		// switch to mt logs
		KIMPL.toggle_async(true);
		// init actor system
		auto& actor_sys = KIMPL.actor_sys_;
		if(!actor_sys) {
			actor_sys = std::make_unique<caf::actor_system>(KIMPL.actor_cfg_);
			if(!actor_sys)
				throw error("Can't create CAF actor_system!");
		}
	}
}

auto shutdown() -> void {
	using InitState = kimpl::InitState;

	// shut down if not already Down
	if(KIMPL.init_state_.exchange(InitState::Down) != InitState::Down) {
		// destroy actor system
		if(KIMPL.actor_sys_) {
			KIMPL.actor_sys_.release();
		}
		// shutdown mt logs
		detail::logging_subsyst::shutdown();
	}
}

auto unify_serialization() -> void {
	KIMPL.unify_serialization();
}

auto k_descriptor() -> const plugin_descriptor& {
	return KIMPL.kernel_pd();
}

auto k_pymod() -> void* {
	return KIMPL.pysupport_->py_kmod();
}

auto last_error() -> std::string {
	return last_system_message();
}

auto str_key_storage(const std::string& key) -> str_any_array& {
	auto& kimpl = KIMPL;
	auto solo = std::lock_guard{ kimpl.sync_storage_ };
	return kimpl.str_key_storage(key);
}

auto idx_key_storage(const std::string& key) -> idx_any_array& {
	auto& kimpl = KIMPL;
	auto solo = std::lock_guard{ kimpl.sync_storage_ };
	return kimpl.idx_key_storage(key);
}

NAMESPACE_END(blue_sky::kernel)
