/// @file
/// @author uentity
/// @date 26.10.2016
/// @brief Useful tools to discover BlueSky kernel
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include "../common.h"
#include "../tree/link.h"

NAMESPACE_BEGIN(blue_sky::kernel::tools)

BS_API std::string print_loaded_types();

BS_API std::string get_backtrace(int backtrace_depth = 16, int skip = 2);

BS_API void print_link(
	tree::link l, bool follow_symlinks = false, int level = 0
);

NAMESPACE_END(blue_sky::kernel::tools)
