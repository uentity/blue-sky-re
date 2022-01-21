/// @author Alexander Gagarin (@uentity)
/// @date 21.01.2022
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#define BOOST_TEST_DYN_LINK

#include <bs/objbase.h>
#include <bs/tree/map_link.h>
#include <bs/tree/tree.h>
#include <bs/log.h>

#include <boost/test/unit_test.hpp>

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>

using namespace blue_sky;
using namespace blue_sky::log;
using namespace blue_sky::tree;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_CASE(test_map_link) {
	std::cout << "\n\n*** testing map_link..." << std::endl;
	std::cout << "*********************************************************************" << std::endl;

	// counters
	using counters_t = std::unordered_map<Event, std::atomic<int>>;
	auto counters = std::make_shared<counters_t>();
	(*counters)[Event::LinkInserted] = 0;
	(*counters)[Event::LinkErased] = 0;
	(*counters)[Event::LinkDeleted] = 0;
	(*counters)[Event::LinkRenamed] = 0;
	(*counters)[Event::LinkStatusChanged] = 0;

	// mapper
	auto dir_mapper = [=](node src, node dst, event ev) {
		const auto ev_to_string = [&]() -> std::string_view {
			switch(ev.code) {
			case Event::Nil: return "Nil";
			case Event::LinkInserted: return "LinkInserted";
			case Event::LinkErased: return "LinkErased";
			case Event::LinkDeleted: return "LinkDeleted";
			case Event::LinkRenamed: return "LinkRenamed";
			case Event::LinkStatusChanged: return "LinkStatusChanged";
			default: return "<unknown event>";
			};
		};

		bsout() << "=> dir_mapper, ev {}" << ev_to_string() << bs_end;
		++(*counters)[ev.code];
		std::this_thread::sleep_for(100ms);
		dst.insert("t", std::make_shared<objbase>());
	};

	auto pt_node = node();

	auto ml = map_link(dir_mapper, "mtest", pt_node, {}, Event::All);
	// obtain data node from map link to start initial refresh
	ml.data_node(noop);
	// then touch data in src node multiple times
	constexpr auto reps = 10;
	for(auto i = 0; i < reps; ++i)
		pt_node.insert("N", node());

	std::this_thread::sleep_for(2s);
	BOOST_TEST( ml.data_node().size() == reps + 1 );
	BOOST_TEST( (*counters)[Event::Nil] == 1 );
	BOOST_TEST( (*counters)[Event::LinkInserted] == reps );
}

