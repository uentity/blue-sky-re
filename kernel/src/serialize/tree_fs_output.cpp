/// @file
/// @author uentity
/// @date 29.05.2019
/// @brief Tree filesystem archive implementation
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include "tree_fs_impl.h"

#include <bs/actor_common.h>
#include <bs/log.h>
#include <bs/tree/errors.h>
#include <bs/tree/node.h>
#include <bs/kernel/radio.h>

#include <bs/serialize/tree_fs_output.h>
#include <bs/serialize/object_formatter.h>
#include <bs/serialize/base_types.h>
#include <bs/serialize/tree.h>
#include <bs/serialize/cafbind.h>

#include <cereal/types/vector.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include <caf/typed_event_based_actor.hpp>

NAMESPACE_BEGIN(blue_sky)
namespace fs = std::filesystem;

///////////////////////////////////////////////////////////////////////////////
//  tree_fs_output::impl
//
struct tree_fs_output::impl : detail::file_heads_manager<true> {
	using heads_mgr_t = detail::file_heads_manager<true>;
	using fmanager_t = detail::objfrm_manager;
	using Error = tree::Error;

	impl(std::string root_fname, std::string objects_dirname) :
		heads_mgr_t{ std::move(root_fname), std::move(objects_dirname) },
		manager_(kernel::radio::system().spawn<fmanager_t>(true))
	{}

	auto begin_link(const tree::link& L) -> error {
		if(auto er = enter_root()) return er;
		if(cur_path_ == root_path_) return perfect;

		return add_head(cur_path_ / to_string(L.id()));
	}

	auto end_link() -> error {
		pop_head();
		return perfect;
	}

	auto begin_node(const tree::node& N) -> error {
		return error::eval_safe(
			[&]{ return head().map( [&](auto* ar) { prologue(*ar, N); }); },
			// skip empty nodes
			[&]{ return enter_root(); },
			// [NOTE] delay enter node's dir until first head is added
			// this prevents creation of empty dirs (for nodes without leafs)
			[&]{ if(N) cur_path_ /= N.home_id(); }
		);
	}

	auto end_node(const tree::node& N) -> error {
		if(cur_path_.empty() || cur_path_ == root_path_) return Error::NodeWasntStarted;
		return error::eval_safe(
			// write down node's metadata nessessary to load it later
			[&]{ return head().map( [&](auto* ar) {
				if(N) {
					// node directory
					(*ar)(cereal::make_nvp("node_dir", N.home_id()));

					// custom leafs order
					std::vector<std::string> leafs_order = N.skeys(tree::Key::ID, tree::Key::AnyOrder);
					(*ar)(cereal::make_nvp("leafs_order", leafs_order));
				}
				// and finish
				epilogue(*ar, N);
			}); },
			// enter parent dir
			[&] { return N ? enter_dir(cur_path_.parent_path(), cur_path_) : perfect; }
		);
	}

	auto save_object(tree_fs_output& ar, const objbase& obj) -> error {
	return error::eval_safe([&]() -> error {
		// open node
		auto cur_head = head();
		if(!cur_head) return cur_head.error();
		prologue(**cur_head, obj);

		std::string obj_fmt;
		bool fmt_ok = false;

		auto finally = scope_guard{ [&]{
			// if error happened we still need to write values
			if(!fmt_ok) ar(cereal::make_nvp("fmt", "<error>"));
			// ... and close node
			epilogue(**cur_head, obj);
		} };

		// 1. obtain & write down formatter
		auto F = get_active_formatter(obj.type_id());
		if(!F) return { obj.type_id(), Error::MissingFormatter };
		obj_fmt = F->name;
		// write down object formatter name
		ar(cereal::make_nvp("fmt", obj_fmt));
		fmt_ok = true;

		// 2. store object's metadata (objbase)
		(**cur_head)(cereal::make_nvp( "objbase", obj ));
		// if object is node and formatter don't store leafs, then save 'em explicitly
		if(!F->stores_node)
			ar(cereal::make_nvp( "node", obj.data_node() ));

		// 3. if object is pure node - we're done and can skip data processing
		if(obj.bs_resolve_type() == objnode::bs_type())
			return perfect;

		// enter objects directory
		EVAL
			[&]{ return enter_root(); },
			[&]{ return objects_path_.empty() ?
				enter_dir(root_path_ / objects_dname_, objects_path_) : perfect;
			}
		RETURN_EVAL_ERR

		// 4. and actually save object data to file
		auto obj_path = objects_path_ / (obj.home_id() + '.' + obj_fmt);
		auto abs_obj_path = fs::path{};
		SCOPE_EVAL_SAFE
			abs_obj_path = fs::absolute(obj_path);
		RETURN_SCOPE_ERR
		caf::anon_send(
			manager_, caf::actor_cast<caf::actor>(manager_),
			obj.shared_from_this(), obj_fmt, abs_obj_path.u8string()
		);
		// defer wait until save completes
		if(!has_wait_deferred_) {
			ar(cereal::defer(cereal::Functor{ [](auto& ar){ ar.wait_objects_saved(); } }));
			has_wait_deferred_ = true;
		}
		return perfect;
	}); }

	auto get_active_formatter(std::string_view obj_type_id) -> object_formatter* {
		if(auto paf = active_fmt_.find(obj_type_id); paf != active_fmt_.end())
			return get_formatter(obj_type_id, paf->second);
		else {
			// if bin format installed - select it
			if(select_active_formatter(obj_type_id, detail::bin_fmt_name))
				return get_active_formatter(obj_type_id);
			else if(
				auto frms = list_installed_formatters(obj_type_id);
				!frms.empty() && select_active_formatter(obj_type_id, *frms.begin())
			)
				return get_active_formatter(obj_type_id);

		}
		return nullptr;
	}

	auto select_active_formatter(std::string_view obj_type_id, std::string_view fmt_name) -> bool {
		if(auto pfmt = get_formatter(obj_type_id, fmt_name); pfmt) {
			active_fmt_[obj_type_id] = fmt_name;
			return true;
		}
		return false;
	}

	auto wait_objects_saved(timespan how_long) -> std::vector<error> {
		auto res = fmanager_t::wait_jobs_done(manager_, how_long);
		has_wait_deferred_ = false;
		return res;
	}

	///////////////////////////////////////////////////////////////////////////////
	//  data
	//
	// obj_type_id -> formatter name
	using active_fmt_t = std::map<std::string_view, std::string, std::less<>>;
	active_fmt_t active_fmt_;

	// async savers manager
	fmanager_t::actor_type manager_;
	bool has_wait_deferred_ = false;
};

///////////////////////////////////////////////////////////////////////////////
//  output archive
//
tree_fs_output::tree_fs_output(
	std::string root_fname, std::string objects_dir
)
	: Base(this), pimpl_{ std::make_unique<impl>(std::move(root_fname), std::move(objects_dir)) }
{}

tree_fs_output::~tree_fs_output() = default;

auto tree_fs_output::head() -> result_or_err<cereal::JSONOutputArchive*> {
	return pimpl_->head();
}

auto tree_fs_output::begin_link(const tree::link& L) -> error {
	return pimpl_->begin_link(L);
}

auto tree_fs_output::end_link() -> error {
	return pimpl_->end_link();
}

auto tree_fs_output::begin_node(const tree::node& N) -> error {
	return pimpl_->begin_node(N);
}

auto tree_fs_output::end_node(const tree::node& N) -> error {
	return pimpl_->end_node(N);
}

auto tree_fs_output::save_object(const objbase& obj) -> error {
	return pimpl_->save_object(*this, obj);
}

auto tree_fs_output::wait_objects_saved(timespan how_long) const -> std::vector<error> {
	return pimpl_->wait_objects_saved(how_long);
}

auto tree_fs_output::saveBinaryValue(const void* data, size_t size, const char* name) -> void {
	head().map([=](cereal::JSONOutputArchive* jar) {
		jar->saveBinaryValue(data, size, name);
	});
}

auto tree_fs_output::get_active_formatter(std::string_view obj_type_id) -> object_formatter* {
	return pimpl_->get_active_formatter(obj_type_id);
}

auto tree_fs_output::select_active_formatter(std::string_view obj_type_id, std::string_view fmt_name) -> bool {
	return pimpl_->select_active_formatter(obj_type_id, fmt_name);
}

///////////////////////////////////////////////////////////////////////////////
//  prologue, epilogue
//

auto prologue(tree_fs_output& ar, tree::link const& L) -> void {
	if(auto er = ar.begin_link(L)) throw er;
}

auto epilogue(tree_fs_output& ar, tree::link const&) -> void {
	if(auto er = ar.end_link()) throw er;
}

auto prologue(tree_fs_output& ar, tree::node const& N) -> void {
	if(auto er = ar.begin_node(N)) throw er;
}

auto epilogue(tree_fs_output& ar, tree::node const& N) -> void {
	if(auto er = ar.end_node(N)) throw er;
}

NAMESPACE_END(blue_sky)
