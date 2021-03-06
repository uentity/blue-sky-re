/// @file
/// @author uentity
/// @date 28.10.2016
/// @brief 
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#pragma once

#include "../../setup_plugin_api.h"
#include <type_traits>

namespace blue_sky {

class BS_API_PLUGIN bs_serialize {
public:
	template< class Archive, class T >
	struct save {
		static void go(
			Archive&,
			const T&,
			const unsigned int
		)
		{}
	};

	template< class Archive, class T >
	struct load {
		static void go (
			Archive&,
			T&,
			const unsigned int
		)
		{}
	};

	template< class Archive, class T >
	struct serialize {
		static void go(
			Archive&,
			T&,
			const unsigned int
		)
		{}
	};

	template< class Archive, class T >
	struct save_construct_data {
		static void go (
			Archive&,
			const T*,
			const unsigned int
		)
		{}
	};

	template< class Archive, class T >
	struct load_construct_data {
		static void go (
			Archive&,
			T*,
			const unsigned int
		)
		{}
	};
};

namespace detail {

template< class T >
struct bs_init_eti;

}

/// @brief Force boost::serialization::extended_type_info creation (and registering)
///
/// @tparam T
template< class T >
void serialize_register_eti();

/*-----------------------------------------------------------------
 * check whether given serialization data fix is applicable to given type
 *----------------------------------------------------------------*/
template< class T, class fixer >
struct serialize_fix_applicable {
	// does given fixer applicable to given type T during saving?
	typedef std::false_type on_save;
	// and during loading
	typedef std::false_type on_load;
	// type returned by fixer save
	typedef T save_ret_t;
};

}  // eof blue_sky

