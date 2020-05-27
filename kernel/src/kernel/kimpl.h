/// @file
/// @author uentity
/// @date 24.08.2016
/// @brief kernel signleton declaration
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include <bs/any_array.h>
#include "logging_subsyst.h"
#include "plugins_subsyst.h"
#include "instance_subsyst.h"
#include "config_subsyst.h"
#include "python_subsyst.h"

#include <caf/fwd.hpp>

#include <mutex>

NAMESPACE_BEGIN(blue_sky::kernel)
/*-----------------------------------------------------------------------------
 *  kernel impl
 *-----------------------------------------------------------------------------*/
class BS_HIDDEN_API kimpl :
	public detail::logging_subsyst,
	public detail::config_subsyst,
	public detail::plugins_subsyst,
	public detail::instance_subsyst
{
public:
	// kernel generic data storage
	using str_any_map_t = std::map< std::string, str_any_array, std::less<> >;
	str_any_map_t str_key_storage_;

	using idx_any_map_t = std::map< std::string, idx_any_array, std::less<> >;
	idx_any_map_t idx_key_storage_;

	std::mutex sync_storage_;

	// kernel's actor system
	// delayed actor system initialization
	std::unique_ptr<caf::actor_system> actor_sys_;

	// indicator of kernel initialization state
	enum class InitState { NonInitialized, Initialized, Down };
	std::atomic<InitState> init_state_;

	// Python support depends on compile flags and can be 'dumb' or 'real'
	std::unique_ptr<detail::python_subsyst> pysupport_;

	kimpl();
	~kimpl();

	using type_tuple = tfactory::type_tuple;
	auto find_type(const std::string& key) const -> type_tuple;

	auto str_key_storage(const std::string& key) -> str_any_array&;

	auto idx_key_storage(const std::string& key) -> idx_any_array&;

	auto actor_system() -> caf::actor_system&;
};

/// Kernel internal singleton
using give_kimpl = singleton<kimpl>;
#define KIMPL ::blue_sky::kernel::give_kimpl::Instance()

NAMESPACE_END(blue_sky::kernel)
