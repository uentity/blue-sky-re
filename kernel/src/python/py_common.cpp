/// @file
/// @author uentity
/// @date 24.11.2016
/// @brief Python bindings for some common kernel API
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include <bs/python/common.h>
#include <bs/propdict.h>
#include <bs/python/enum.h>
#include <bs/python/property.h>
#include <bs/python/map.h>
#include "../kernel/python_subsyst_impl.h"

#include <ostream>
#include <iostream>
#include <sstream>

NAMESPACE_BEGIN(blue_sky)
using namespace std;

// def(str(self)) impl for plugin_descriptor
BS_HIDDEN_API ostream& operator<<(ostream& os, const plugin_descriptor& pd) {
	os << "{PLUGIN: " << pd.name << "; VERSIOM " << pd.version;
	os << "; INFO: " << pd.description;
	os << "; NAMESPACE: " << pd.py_namespace << '}';
	return os;
}

// def(str(self)) impl for type_descriptor
BS_HIDDEN_API ostream& operator<<(ostream& os, const type_descriptor& td) {
	if(td.is_nil())
		os << "BlueSky Nil type" << endl;
	else {
		os << "{TYPENAME: " << td.name;
		os << "; INFO: " << td.description << '}';
	}
	return os;
}

// def(str(self)) impl for type_info
BS_HIDDEN_API ostream& operator<<(ostream& os, const bs_type_info& ti) {
	os << "BlueSky C++ type_info: '" << ti.name() << "' at " << endl;
	return os;
}

NAMESPACE_BEGIN(python)

auto pyinfinite() -> py::object {
	static auto value = [] {
		// return `datetime.timedelta.max` value
		auto dtm = py::module::import("datetime");
		auto py_dt = dtm.attr("timedelta");
		return py_dt.attr("max");
	}();

	return value;
}

// exporting function
void py_bind_common(py::module& m) {
	m.def("nil_type_info", &nil_type_info);
	m.def("is_nil", &is_nil);

	// bs_type_info binding
	// empty constructor creates nil type
	py::class_< bs_type_info >(m, "type_info")
		.def(py::init( []() { return std::make_unique<bs_type_info>(nil_type_info()); } ))
		.def_property_readonly("name", &bs_type_info::name)
		.def_property_readonly_static(
			"nil", [](py::object){ return nil_type_info(); }
		)
		.def(py::self == py::self)
		.def(py::self != py::self)
		.def(py::self < py::self)
		.def(py::self > py::self)
		.def(py::self <= py::self)
		.def(py::self >= py::self)
		.def("__repr__", &bs_type_info::name)
	;

	// plugin_descriptor binding
	py::class_< plugin_descriptor >(m, "plugin_descriptor")
		.def(py::init< const char* >(), "plugin_name"_a)
		.def(py::init< const std::string& >(), "plugin_name"_a)
		.def(py::init([](
			const bs_type_info& tag, const char* name, const char* version,
			const char* description, const char* py_namespace
		){
			return std::make_unique<plugin_descriptor>(tag, name, version, description, py_namespace);
		}), "tag_type_info"_a, "plug_name"_a, "version"_a, "description"_a = "", "py_namespace"_a = "")
		.def_readonly("name", &plugin_descriptor::name)
		.def_readonly("version", &plugin_descriptor::version)
		.def_readonly("description", &plugin_descriptor::description)
		.def_readonly("py_namespace", &plugin_descriptor::py_namespace)
		.def_property_readonly("is_nil", &plugin_descriptor::is_nil)
		.def_property_readonly_static(
			"nil", [](py::object){ return plugin_descriptor::nil(); }, py::return_value_policy::reference
		)
		.def(py::self < py::self)
		.def(py::self == py::self)
		.def(py::self != py::self)
		.def("__repr__", [](const plugin_descriptor& pd) { return pd.name; })
	;
	// enable implicit conversion from string -> plugin_descriptor
	py::implicitly_convertible<std::string, plugin_descriptor>();

	// [NOTE] important to bind *before* type_descriptor
	// propdict binding
	bind_rich_map<prop::propdict>(m, "propdict", py::module_local(false));
	// opaque bindings of propbooks
	bind_rich_map<prop::propbook_s>(m, "propbook_s", py::module_local(false));
	bind_rich_map<prop::propbook_i>(m, "propbook_i", py::module_local(false));

	// type_desccriptor bind
	py::class_< type_descriptor >(m, "type_descriptor")
		.def(py::init<std::string_view>(), "type_name"_a)
		.def_property_readonly_static(
			"nil", [](py::object){ return type_descriptor::nil(); }, py::return_value_policy::reference
		)
		.def_readonly("name", &type_descriptor::name)
		.def_readonly("description", &type_descriptor::description)
		.def_property_readonly("is_nil", &type_descriptor::is_nil)
		.def_property_readonly("is_copyable", &type_descriptor::is_copyable)
	
		.def("parent_td", &type_descriptor::parent_td, py::return_value_policy::reference)
		.def("add_copy_constructor", (void (type_descriptor::*)(BS_TYPE_COPY_FUN) const)
			&type_descriptor::add_copy_constructor, "copy_fun"_a
		)

		.def("construct", [](const type_descriptor& self){
			return (std::shared_ptr< objbase >)self.construct();
		}, "Default construct type")

		.def("clone", [](const type_descriptor& td, bs_type_copy_param src){
			return (std::shared_ptr< objbase >)td.clone(src);
		})

		.def("assign", &type_descriptor::assign,
			"target"_a, "source"_a, "params"_a = prop::propdict{}, "Assign content from source to target")

		.def(py::self == py::self)
		.def(py::self != py::self)
		.def(py::self < py::self)
		.def(py::self < std::string())
		.def(py::self == std::string())
		.def(py::self != std::string())
		.def("__repr__", [](const type_descriptor& td) -> std::string {
			return "[" + td.name + "] [" + td.description + ']';
		})
	;
	// enable implicit conversion from string -> type_descriptor
	py::implicitly_convertible<std::string, type_descriptor>();

	m.def("isinstance", py::overload_cast<const sp_cobj&, const type_descriptor&>(&isinstance),
		"obj"_a, "td"_a, "Check if obj type match given type_descriptor");
	m.def("isinstance", py::overload_cast<const sp_cobj&, std::string_view>(&isinstance),
		"obj"_a, "obj_type_id"_a, "Check if obj is of given type name");

	// add marker for infinite timespan
	m.attr("infinite") = pyinfinite();

	// async tag
	py::class_<launch_async_t>(m, "launch_async_t");
	m.attr("launch_async") = launch_async;

	// unsafe tag
	py::class_<unsafe_t>(m, "unsafe_t");
	m.attr("unsafe") = unsafe;

	// deep tag
	py::class_<deep_t>(m, "deep_t");
	m.attr("deep") = deep;

	using Event = tree::Event;
	bind_enum_with_ops<Event>(m, "Event")
		.value("Nil", Event::Nil)
		.value("LinkRenamed", Event::LinkRenamed)
		.value("LinkStatusChanged", Event::LinkStatusChanged)
		.value("LinkInserted", Event::LinkInserted)
		.value("LinkErased", Event::LinkErased)
		.value("LinkDeleted", Event::LinkDeleted)
		.value("DataModified", Event::DataModified)
		.value("DataNodeModified", Event::DataNodeModified)
		.value("All", Event::All)
	;

	// event
	using event = tree::event;
	py::class_<event>(m, "event")
		.def_readonly("params", &event::params)
		.def_readonly("code", &event::code)
		.def("origin_link", &event::origin_link, "If event source is link, return it")
		.def("origin_node", &event::origin_node, "If event source is node, return it")
		.def("origin_object", &event::origin_object, "If event source is object, return it")
	;
}

NAMESPACE_END(python)
NAMESPACE_END(blue_sky)

