/// @author uentity
/// @date 23.05.2019
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include "common.h"

NAMESPACE_BEGIN(blue_sky::python)
NAMESPACE_BEGIN(detail)
using namespace py::detail;
///////////////////////////////////////////////////////////////////////////////
//  traits
//
// trait to detect whether map has `erase()` method or not
template<typename Map, typename = void> struct has_erase : std::false_type {};

template<typename Map>
struct has_erase<Map, std::void_t<decltype(std::declval<Map>().erase(typename Map::iterator{}))>> :
	std::true_type {};

template<typename Map>
inline constexpr auto has_erase_v = has_erase<Map>::value;

// same for `try_emplace()`
template<typename Map, typename = void> struct has_try_emplace : std::false_type {};

template<typename Map>
struct has_try_emplace<Map, std::void_t<decltype(
	std::declval<Map>().try_emplace(typename Map::key_type{}, typename Map::mapped_type{})
)>> : std::true_type {};

template<typename Map>
inline constexpr auto has_try_emplace_v = has_try_emplace<Map>::value;

///////////////////////////////////////////////////////////////////////////////
// map assignment impl
//
template <typename, typename, typename... Args> void map_setitem(Args&&...) {}

// Map assignment when copy-assignable: just copy the value
template <typename Map, typename Class_>
void map_setitem(Class_& cl) {
	using KeyType = typename Map::key_type;
	using MappedType = typename Map::mapped_type;

	cl.def("__setitem__", [](Map &m, py::object k, py::object v) {
		if constexpr(has_try_emplace_v<Map>) {
			m.try_emplace(
				py::cast<KeyType>(std::move(k)), py::cast<MappedType>(std::move(v))
			);
		}
		else {
			auto ck = py::cast<KeyType>(std::move(k));
			if(auto it = m.find(ck); it != m.end())
				it->second = py::cast<MappedType>(std::move(v));
			else
				m.emplace(std::move(ck), py::cast<MappedType>(std::move(v)));
		}
	});
}

// this is copy-paste from pybind11 map binding except that `__del__` is only added if `erase()` detected
template <typename Map, typename holder_type = std::unique_ptr<Map>, typename... Args>
auto bind_base_map(py::handle scope, const std::string &name, Args&&... args) {
	using KeyType = typename Map::key_type;
	using MappedType = typename Map::mapped_type;
	using Class_ = py::class_<Map, holder_type>;
	using namespace py;

	// If either type is a non-module-local bound type then make the map binding non-local as well;
	// otherwise (e.g. both types are either module-local or converting) the map will be
	// module-local.
	auto tinfo = py::detail::get_type_info(typeid(MappedType));
	bool local = !tinfo || tinfo->module_local;
	if (local) {
		tinfo = py::detail::get_type_info(typeid(KeyType));
		local = !tinfo || tinfo->module_local;
	}

	Class_ cl(scope, name.c_str(), py::module_local(local), std::forward<Args>(args)...);

	cl.def(py::init<>());

	// Register stream insertion operator (if possible)
	py::detail::map_if_insertion_operator<Map, Class_>(cl, name);
	// Elements sssignment
	map_setitem<Map, Class_>(cl);

	cl.def("__bool__",
		[](const Map &m) -> bool { return !m.empty(); },
		"Check whether the dict is nonempty"
	);

	cl.def("__iter__",
		[](Map &m) { return py::make_key_iterator(m.begin(), m.end()); },
		keep_alive<0, 1>() /* Essential: keep list alive while iterator exists */
	);

	cl.def("items",
		[](Map &m) { return py::make_iterator(m.begin(), m.end()); },
		keep_alive<0, 1>() /* Essential: keep list alive while iterator exists */
	);

	cl.def("__getitem__",
		[](Map &m, const KeyType &k) -> MappedType & {
			if(auto it = m.find(k); it != m.end())
				return it->second;
			throw key_error();
		},
		return_value_policy::reference_internal // ref + keepalive
	);

	cl.def("__contains__",
		[](Map &m, const KeyType &k) -> bool {
			return m.find(k) != m.end();
		}
	);

	cl.def("__len__", &Map::size);

	if constexpr(has_erase_v<Map>) {
		cl.def("__delitem__",
			[](Map &m, const KeyType &k) {
				if(auto it = m.find(k); it != m.end())
					m.erase(it);
				throw key_error();
			}
		);
	}

	return cl;
}

// additional methods for map-like containers
template<typename Map, typename PyMap>
auto make_rich_map(PyMap& cl) -> PyMap& {
	using KeyType = typename Map::key_type;
	using MappedType = typename Map::mapped_type;

	// allow constructing from Python dict
	const auto from_pydict = [](Map& tgt, py::dict src) {
		auto key_caster = py::detail::make_caster<KeyType>();
		auto value_caster = py::detail::make_caster<MappedType>();
		for(const auto& [sk, sv] : src) {
			if(!key_caster.load(sk, true) || !value_caster.load(sv, true)) continue;
			tgt[py::detail::cast_op<KeyType>(key_caster)] = py::detail::cast_op<MappedType>(value_caster);
		}
	};

	cl.def(py::init([&from_pydict](py::dict D) {
		Map M;
		from_pydict(M, std::move(D));
		return M;
	}));
	// and explicit conversion from Py dict
	cl.def("from_dict", from_pydict, "src"_a);

	// copy content to Python dict
	cl.def("to_dict", [](const Map& m) {
		py::dict res;
		for(const auto& [k, v] : m)
			res[py::cast(k)] = py::cast(v);
		return res;
	});

	cl.def("keys", [](const Map& m) {
		py::list res;
		for(const auto& [k, v] : m)
			res.append(k);
		return res;
	});

	cl.def("values", [](const Map& m) {
		py::list res;
		for(const auto& [k, v] : m)
			res.append(v);
		return res;
	});

	const auto contains = [](const Map& m, py::object key) {
		auto key_caster = py::detail::make_caster<KeyType>();
		if(key_caster.load(key, true))
			return m.find(py::detail::cast_op<KeyType>(key_caster)) != m.end();
		return false;
	};
	cl.def("__contains__", contains);
	cl.def("has_key", contains, "key"_a);

	cl.def("clear", [](Map& m) { m.clear(); });

	if constexpr(has_erase_v<Map>)
		cl.def("pop", [](Map& m, py::object key, py::object def_value) {
			auto key_caster = py::detail::make_caster<KeyType>();
			if(!key_caster.load(key, true)) return def_value;

			auto pval = m.find(py::detail::cast_op<KeyType>(key_caster));
			if(pval != m.end()) {
				auto res = py::cast(pval->second);
				m.erase(pval);
				return res;
			}
			return def_value;
		}, "key"_a, "default"_a = py::none());

	cl.def("get", [](Map& m, py::object key, py::object def_value) {
		auto key_caster = py::detail::make_caster<KeyType>();
		if(!key_caster.load(key, true)) return def_value;

		auto pval = m.find(py::detail::cast_op<KeyType>(key_caster));
		return pval != m.end() ? py::cast(pval->second) : std::move(def_value);
	}, "key"_a, "default"_a = py::none());

	cl.def(py::self == py::self);
	cl.def(py::self != py::self);
	cl.def(py::self < py::self);
	cl.def(py::self <= py::self);
	cl.def(py::self > py::self);
	cl.def(py::self >= py::self);

	// make Python dictionary implicitly convertible to Map
	py::implicitly_convertible<py::dict, Map>();
	return cl;
}

NAMESPACE_END(detail)

/// rich binder for map-like container (autodetects `erase()` presence)
template <typename Map, typename holder_type = std::unique_ptr<Map>, typename... Args>
auto bind_rich_map(py::handle scope, std::string const &name, Args&&... args) {
	// gen base bindings
	auto cl = detail::bind_base_map<Map>(scope, name, std::forward<Args>(args)...);
	// add rich methods
	detail::make_rich_map<Map>(cl);
	return cl;
}

NAMESPACE_END(blue_sky::python)
