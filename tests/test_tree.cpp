/// @file
/// @author uentity
/// @date 29.06.2018
/// @brief Test cases for BS tree
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#define BOOST_TEST_DYN_LINK

#include "test_objects.h"
#include "test_serialization.h"

#include <bs/log.h>
#include <bs/propdict.h>
#include <bs/kernel/kernel.h>
#include <bs/kernel/tools.h>
#include <bs/kernel/types_factory.h>
#include <bs/tree/tree.h>

#include <bs/serialize/base_types.h>
#include <bs/serialize/array.h>
#include <bs/serialize/tree.h>

#include <boost/uuid/uuid_io.hpp>
#include <boost/test/unit_test.hpp>
#include <caf/scoped_actor.hpp>

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

using namespace blue_sky;
using namespace blue_sky::log;
using namespace blue_sky::tree;
using namespace std::chrono_literals;

NAMESPACE_BEGIN()

class fusion_client : public fusion_iface {
	auto populate(sp_obj root, const std::string& child_type_id = "") -> error override {
		bsout() << "fusion_client::populate() called" << end;
		return error::quiet();
	}

	auto pull_data(sp_obj root) -> error override {
		bsout() << "fusion_client::pull_data() called" << end;
		return error::quiet();
	}
};

NAMESPACE_END()

NAMESPACE_BEGIN(blue_sky)

auto make_persons_tree() -> tree::link {
	// create root link and node
	node N = node();
	// create several persons and insert 'em into node
	for(int i = 0; i < 10; ++i) {
		std::string p_name = "Citizen_" + std::to_string(i);
		N.insert(hard_link{
			p_name, kernel::tfactory::create_object("bs_person", p_name, double(i + 20))
		});
	}
	// create hard link referencing first object
	N.insert(hard_link{
		"hard_Citizen_0", N.find(0).data()
	});
	// create weak link referencing 2nd object
	N.insert(weak_link{
		"weak_Citizen_1", N.find(1).data()
	});
	// create sym link referencing 3rd object
	N.insert(sym_link{
		"sym_Citizen_2", abspath(N.find(2))
	});
	N.insert(sym_link{
		"sym_Citizen_3", abspath( deref_path(abspath(N.find(3), Key::Name), N, Key::Name) )
	});
	N.insert(sym_link{
		"sym_dot", "."
	});

	return link::make_root<hard_link>("r", std::move(N));
}

NAMESPACE_END(blue_sky)

BOOST_AUTO_TEST_CASE(test_tree) {
	std::cout << "\n\n*** testing tree..." << std::endl;
	std::cout << "*********************************************************************" << std::endl;

	// person
	sp_obj P = kernel::tfactory::create_object(bs_person::bs_type(), std::string("Tyler"), double(33));
	// person link
	auto L = hard_link("person link", std::move(P));
	BOOST_TEST(L);
	auto L1 = tree::link{};
	test_json(L, L1, true);
	BOOST_TEST(L.name() == L1.name());

	auto hN = make_persons_tree();
	auto N = hN.data_node();
	bsout() << "root node abspath: {}" << abspath(hN) << bs_end;
	bsout() << "root node abspath: {}" << convert_path(abspath(hN), hN, Key::ID, Key::Name) << bs_end;
	bsout() << "sym_Citizen_2 abspath: {}" << convert_path(
		abspath(N.find("sym_Citizen_2", Key::Name)), hN, Key::ID, Key::Name
	) << bs_end;
	kernel::tools::print_link(hN, false);

	// serializze node
	auto N1 = node();
	test_json(N, N1);

	// print loaded tree content
	//auto RL = hard_link("r", N1);
	auto RL = link::make_root<hard_link>("r", N1);
	kernel::tools::print_link(RL, false);
	BOOST_TEST(N1);
	BOOST_TEST(N1.size() == N.size());

	// serialize to FS
	bsout() << "\n===========================\n" << bs_end;
	save_tree(hN, "tree_fs/.data", TreeArchive::FS);
	load_tree("tree_fs/.data", TreeArchive::FS).map([](const tree::link& hN1) {
		kernel::tools::print_link(hN1, false);
	});

	// test async dereference
	deref_path([](const tree::link& lnk) {
		std::cout << "*** Async deref callback: link : " <<
		(lnk ? abspath(lnk, Key::Name) : "None") << ' ' <<
		lnk.obj_type_id() << ' ' << (void*)lnk.data().get() << std::endl;
	}, "hard_Citizen_0", hN, Key::Name);
}

