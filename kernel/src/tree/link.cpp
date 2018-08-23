/// @file
/// @author uentity
/// @date 14.09.2016
/// @brief Implementation os link
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include <bs/tree/link.h>
#include <bs/tree/node.h>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

NAMESPACE_BEGIN(blue_sky)
NAMESPACE_BEGIN(tree)

namespace {

// global random UUID generator for BS links
static boost::uuids::random_generator gen;

} // eof hidden namespace

/*-----------------------------------------------------------------------------
 *  inode impl
 *-----------------------------------------------------------------------------*/
inode::inode()
	: suid(false), sgid(false), sticky(false),
	u(7), g(5), o(5),
	mod_time(std::chrono::system_clock::now())
{}

/*-----------------------------------------------------------------------------
 *  link impl
 *-----------------------------------------------------------------------------*/
link::link(std::string name, Flags f)
	: name_(std::move(name)),
	id_(gen()), flags_(f)
{}

link::~link() {}

// get link's object ID
std::string link::oid() const {
	if(auto obj = data()) return obj->id();
	return boost::uuids::to_string(boost::uuids::nil_uuid());
}

std::string link::obj_type_id() const {
	if(auto obj = data()) return obj->type_id();
	return type_descriptor::nil().name;
}

void link::reset_owner(const sp_node& new_owner) {
	owner_ = new_owner;
}

link::Flags link::flags() const {
	return flags_;
}

void link::set_flags(Flags new_flags) {
	flags_ = new_flags;
}

bool link::rename(std::string new_name) {
	if(auto O = owner()) {
		return O->rename(O->find(id()), std::move(new_name));
	}
	name_ = std::move(new_name);
	return true;
}

const inode& link::info() const {
	return inode_;
}
inode& link::info() {
	return inode_;
}

NAMESPACE_END(tree)
NAMESPACE_END(blue_sky)

