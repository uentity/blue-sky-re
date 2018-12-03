/// @file
/// @author uentity
/// @date 19.11.2018
/// @brief BS kernel config subsystem API
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include <bs/common.h>
#include <caf/actor_system_config.hpp>

#include <deque>
#include <set>
#include <map>

NAMESPACE_BEGIN(blue_sky) NAMESPACE_BEGIN(detail)

struct BS_HIDDEN_API kernel_config_subsyst {
	using string_list = std::vector<std::string>;

	kernel_config_subsyst();

	/// impl is taken from CAF sources
	/// parse config options from arguments and ini file content
	/// if `force` is true, force config files reparse
	void configure(string_list args = {}, std::string ini_fname = "", bool force = false);

	void clear_config();

	// kernel's actor system & config
	caf::actor_system_config actor_cfg_;
	// config values storage: map from string key -> any parsed value
	caf::config_value_map confdata_;

private:
	// paths of possible config file location
	string_list conf_path_;
	// predefined config options that can be parsed from CLI or config file
	caf::config_option_set confopt_;
	// flag indicating if kernel was configured at least once
	bool kernel_configured = false;
	bool cli_helptext_printed = false;
};

NAMESPACE_END(detail) NAMESPACE_END(blue_sky)
