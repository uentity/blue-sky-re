/// @file
/// @author uentity
/// @date 29.06.2018
/// @brief BS tree node implementation part of PIMPL
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include <bs/actor_common.h>
#include <bs/log.h>
#include <bs/tree/node.h>

#include "node_leafs_storage.h"
#include "link_impl.h"

#include <cereal/types/vector.hpp>

NAMESPACE_BEGIN(blue_sky::tree)
using existing_index = typename node::existing_index;

/*-----------------------------------------------------------------------------
 *  node_impl
 *-----------------------------------------------------------------------------*/
class BS_HIDDEN_API node_impl :
	public engine::impl, public engine::impl::access<node>,
	public bs_detail::sharded_mutex<engine_impl_mutex>
{
public:
	friend node;

	using sp_nimpl = std::shared_ptr<node_impl>;
	using sp_scoped_actor = engine::impl::sp_scoped_actor;

	///////////////////////////////////////////////////////////////////////////////
	//  private node messaging interface
	//
	// public node iface extended with some private messages
	using primary_actor_type = node::actor_type
	::extend<
		// join self group
		caf::reacts_to<a_hi>,
		// erase link by ID with specified options
		caf::replies_to<a_node_erase, lid_type, EraseOpts>::with<std::size_t>,
		// deep search by ID with active symlinks
		caf::replies_to<a_node_deep_search, lid_type /* key */, lids_v /* active_symlinks */>::with<links_v>,
		// deep search by key with active symlinks
		caf::replies_to<a_node_deep_search, std::string, Key, bool /* return_first */, lids_v>::with<links_v>
	>
	::extend_with<engine_actor_type<node>>
	::extend_with<engine_home_actor_type>;

	// ack signals that this node send to home group
	using self_ack_actor_type = caf::typed_actor<
		// ack on insert - reflect insert from sibling node actor
		caf::reacts_to<a_ack, caf::actor, a_node_insert, lid_type, size_t>,
		// ack on link move
		caf::reacts_to<a_ack, caf::actor, a_node_insert, lid_type, size_t, size_t>,
		// ack on link erase from sibling node
		caf::reacts_to<a_ack, caf::actor, a_node_erase, lids_v>
	>;
	// acks that this node receives from it's leafs
	using leafs_ack_actor_type = caf::typed_actor<
		// track leaf rename
		caf::reacts_to<a_ack, lid_type, a_lnk_rename, std::string, std::string>,
		// track leaf status
		caf::reacts_to<a_ack, lid_type, a_lnk_status, Req, ReqStatus, ReqStatus>,
		// data altered ack
		caf::reacts_to<a_ack, lid_type, a_data, tr_result::box>
	>;

	// all acks processed by node: self acks + self leafs acks + subtree acks
	// [NOTE] `subtree_ack_actor_type` includes `self_ack_actor_type`
	using ack_actor_type = link_impl::subtree_ack_actor_type::extend_with<leafs_ack_actor_type>;

	// all acks are pumped via node's home group
	using home_actor_type = ack_actor_type::extend_with<engine_home_actor_type>;

	// append private behavior to public iface
	using actor_type = primary_actor_type::extend_with<ack_actor_type>;

	///////////////////////////////////////////////////////////////////////////////
	//  member variables
	//
	// leafs
	links_container links_;

	///////////////////////////////////////////////////////////////////////////////
	//  API
	//
	// manipulate with node's handle (protected by mutex)
	auto handle() const -> link;
	auto set_handle(const link& handle) -> void;

	// setup super (weak engine ptr) + correct leafs owner
	// node is REQUIRED to call this after engine is started
	auto propagate_owner(const node& super, bool deep) -> void;

	/// clone this impl
	auto clone(node_actor* papa, bool deep = false) const -> caf::result<sp_nimpl>;

	static auto spawn_actor(sp_nimpl nimpl) -> caf::actor;

	ENGINE_TYPE_DECL

	///////////////////////////////////////////////////////////////////////////////
	//  iterate
	//
	template<Key K = Key::AnyOrder>
	auto begin() const {
		return links_.get<Key_tag<K>>().begin();
	}
	template<Key K = Key::AnyOrder>
	auto end() const {
		return links_.get<Key_tag<K>>().end();
	}

	template<Key K, Key R = Key::AnyOrder>
	auto project(iterator<K> pos) const {
		if constexpr(K == R)
			return pos;
		else
			return links_.project<Key_tag<R>>(std::move(pos));
	}

	///////////////////////////////////////////////////////////////////////////////
	//  search
	//
	template<Key K, Key R = Key::AnyOrder>
	auto find(const Key_type<K>& key) const {
		if constexpr(K == Key::AnyOrder)
			// prevent indexing past array size
			return project<K, R>(std::next( begin<K>(), std::min(key, links_.size()) ));
		else
			return project<K, R>(links_.get<Key_tag<K>>().find(key));
	}

	// same as find, but returns link (null if not found)
	template<Key K>
	auto search(const Key_type<K>& key) const -> link {
		if(auto p = find<K, K>(key); p != end<K>())
			return *p;
		return {};
	}

	auto search(const std::string& key, Key key_meaning) const -> link;

	// equal key
	template<Key K = Key::ID>
	auto equal_range(const Key_type<K>& key) const -> range<K> {
		if constexpr(K != Key::AnyOrder) {
			return links_.get<Key_tag<K>>().equal_range(key);
		}
		else {
			auto pos = find<K>(key);
			return {pos, std::next(pos)};
		}
	}

	auto equal_range(const std::string& key, Key key_meaning) const -> links_v;

	///////////////////////////////////////////////////////////////////////////////
	//  index of element in AnyOrder
	//
	// convert iterator to offset from beginning of AnyOrder index
	template<Key K = Key::AnyOrder>
	auto index(iterator<K> pos) const -> existing_index {
		auto& I = links_.get<Key_tag<Key::AnyOrder>>();
		if(auto ipos = project<K>(std::move(pos)); ipos != I.end())
			return static_cast<std::size_t>(std::distance(I.begin(), ipos));
		return {};
	}

	// index of link with given key in AnyOrder index
	template<Key K>
	auto index(const Key_type<K>& key) const -> existing_index {
		return index(find<K>(key));
	}

	auto index(const std::string& key, Key key_meaning) const -> existing_index;

	///////////////////////////////////////////////////////////////////////////////
	//  keys & values
	//
	template<Key K, typename Iterator>
	static auto keys(Iterator start, Iterator finish) {
		return range_t<Iterator>{ std::move(start), std::move(finish) }
		.template extract_keys<K>();
	}

	// AnyOrder keys are special
	template<typename Iterator>
	auto ikeys(Iterator start, Iterator finish) const {
		return range_t<Iterator>{ std::move(start), std::move(finish) }
		.template extract_it<std::size_t>([&](auto it) {
			// builtin iterators are always projected to valid index
			if constexpr(std::is_same_v<Iterator, iterator<Key::ID>>)
				return *index<Key::ID>(std::move(it));
			else if constexpr(std::is_same_v<Iterator, iterator<Key::Name>>)
				return *index<Key::Name>(std::move(it));
			else if constexpr(std::is_same_v<Iterator, iterator<Key::AnyOrder>>)
				return *index<Key::AnyOrder>(std::move(it));
			else {
				// non-builtin iterator must point to link, find index via link's ID
				// not found elems will have index > number of links (-1)
				auto idx = index<Key::ID>(it->id());
				return idx ? *idx : static_cast<std::size_t>(-1);
			}
		});
	}

	template<Key K, typename Container>
	static auto keys(const Container& links) {
		return keys<K>(links.begin(), links.end());
	}

	template<Key K, Key Order = K>
	auto keys() const {
		static_assert(has_builtin_index_v<Order>);
		if constexpr(K == Key::AnyOrder)
			return ikeys(begin<Order>(), end<Order>());
		else
			return keys<K>(begin<Order>(), end<Order>());
	}

	auto keys(Key order) const -> lids_v;
	auto ikeys(Key order) const -> std::vector<std::size_t>;

	template<Key K>
	auto values() const -> links_v {
		auto res = links_v(links_.size());
		std::copy(begin<K>(), end<K>(), res.begin());
		return res;
	}

	auto leafs(Key order) const -> links_v;

	auto size() const -> std::size_t;

	///////////////////////////////////////////////////////////////////////////////
	//  insert & erase
	//
	using leaf_postproc_fn = function_view< void(const link&) >;

	// insert to back of AnyOrder
	auto insert(link L, InsertPolicy pol = InsertPolicy::AllowDupNames)
	-> insert_status<Key::ID>;

	template<Key K = Key::ID>
	auto erase(const Key_type<K>& key, leaf_postproc_fn ppf = noop) -> size_t {
		if constexpr(K == Key::ID || K == Key::AnyOrder) {
			if(auto victim = find<K, Key::ID>(key); victim != end<Key::ID>())
				return erase_impl(std::move(victim), std::move(ppf));
			return 0;
		}
		else
			return erase<K>(equal_range<K>(key), std::move(ppf));
	}

	auto erase(const std::string& key, Key key_meaning, leaf_postproc_fn ppf = noop) -> size_t;

	auto erase(const lids_v& r, leaf_postproc_fn ppf = noop) -> std::size_t;

	///////////////////////////////////////////////////////////////////////////////
	//  rename
	//
	// [NOTE] don't check iterator validity, uses unsafe link API
	auto rename(iterator<Key::Name> pos, std::string new_name) -> void;

	///////////////////////////////////////////////////////////////////////////////
	//  rearrange
	//
	template<Key K>
	auto rearrange(const std::vector<Key_type<K>>& new_order) -> error {
		if(new_order.size() != size()) return Error::WrongOrderSize;
		// convert vector of keys into vector of iterators
		std::vector<std::reference_wrapper<const link>> i_order;
		i_order.reserve(size());
		const auto p_end = end<Key::AnyOrder>();
		for(const auto& k : new_order) {
			if(auto p_elem = find<K>(k); p_elem != p_end)
				i_order.push_back(std::ref(*p_elem));
			else
				return Error::KeyMismatch;
		}
		// apply order
		links_.get<Key_tag<Key::AnyOrder>>().rearrange(i_order.begin());
		return perfect;
	}

private:
	// weak ref to parent link
	link::weak_ptr handle_;

	// returns index of removed element
	// [NOTE] don't do range checking
	auto erase_impl(iterator<Key::ID> key, leaf_postproc_fn ppf = noop) -> std::size_t;

	// erase multiple elements given in valid (!) range
	template<Key K = Key::ID>
	auto erase(const range<K>& r, leaf_postproc_fn ppf = noop) -> size_t {
		size_t res = 0;
		for(auto x = r.begin(); x != r.end();)
			res += erase_impl(project<K, Key::ID>(x++), ppf);
		return res;
	}
};
using sp_nimpl = node_impl::sp_nimpl;

NAMESPACE_END(blue_sky::tree)
