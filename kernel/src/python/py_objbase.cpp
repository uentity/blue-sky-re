/// @file
/// @author uentity
/// @date 13.04.2017
/// @brief 
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include <bs/python/common.h>
#include <bs/python/expected.h>
#include <bs/python/tr_result.h>

#include <bs/objbase.h>
#include <bs/tree/inode.h>
#include <bs/serialize/object_formatter.h>

#include "kernel_queue.h"

#include <pybind11/functional.h>

NAMESPACE_BEGIN(blue_sky::python)

using py_obj_transaction = std::function< py::object(sp_obj) >;

void py_bind_objbase(py::module& m) {
	// objebase binding
	py::class_<objbase, sp_obj>(m, "objbase")
		BSPY_EXPORT_DEF(objbase)
		.def(py::init([](std::string custom_oid = "") -> sp_obj {
			return std::make_shared<objbase>(std::move(custom_oid));
		}), "custom_oid"_a = "")

		.def("bs_resolve_type", &objbase::bs_resolve_type, py::return_value_policy::reference)
		.def_property_readonly("type_id", &objbase::type_id)
		.def_property_readonly("id", &objbase::id)
		.def_property_readonly("data_node", &objbase::data_node)
		.def_property_readonly("info", &objbase::info)

		.def("apply",
			[](objbase& self, py_obj_transaction tr) {
				// capture Py transaction with sahred_ptr while GIL is held
				auto piped_tr = adapt_py_tr(std::move(tr), false);
				// release GIL & exec transaction in kernel's queue = in another thread
				const auto g = py::gil_scoped_release{};
				return self.apply(std::move(piped_tr));
			},
			"tr"_a, "Blocking execute `tr` in object's queue (sync)"
		)
		// callback into actor
		.def("apply",
			[](objbase& self, launch_async_t, py_obj_transaction tr) {
				self.apply(launch_async, adapt_py_tr(std::move(tr), true));
			},
			"launch_async"_a, "tr"_a, "Send transaction `tr` to object's queue, return immediately"
		)

		.def("apply",
			[](objbase& self, py_obj_transaction tr, objbase::process_tr_cb f) {
				self.apply(
					adapt_py_tr(std::move(tr), true), adapt_enqueue(std::move(f))
				);
			},
			"tr"_a, "f"_a,
			"Send transaction `tr` to object's queue and invoke `f` when tr finishes, return immediately"
		)

		.def("touch",
			&objbase::touch, "tres"_a = prop::propdict{},
			"Send empty transaction to trigger `data modified` signal"
		)

		// events subscrition
		.def("subscribe", [](objbase& obj, objbase::event_handler f, objbase::Event listen_to) {
			return obj.subscribe(adapt_enqueue(std::move(f)), listen_to);
		}, "event_cb"_a, "events"_a = objbase::Event::DataModified)

		.def("unsubscribe", py::overload_cast<>(&objbase::unsubscribe, py::const_))
		// static method
		.def("unsubscribe", [](const py::object&, std::uint64_t handler_id) {
			objbase::unsubscribe(handler_id);
		}, "handler_id"_a)
	;

	// objnode
	py::class_<objnode, objbase, sp_objnode>(m, "objnode")
		BSPY_EXPORT_DEF(objnode)
		.def(py::init<std::string>(), "custom_oid"_a = "")
		.def(py::init<tree::node, std::string>(), "N"_a, "custom_oid"_a = "")
	;

	// object formatters
	py::class_<object_formatter>(m, "object_formatter")
		.def(py::init([](
			std::string fmt_name, object_saver_fn saver, object_loader_fn loader, bool stores_node = false
		) {
			return object_formatter{ std::move(fmt_name), std::move(saver), std::move(loader), stores_node };
		}), "fmt_name"_a, "saver_fn"_a, "loader_fn"_a, "stores_node"_a = false)
		.def_readonly("name", &object_formatter::name,
			"Formatter name treated by default as file extension")
		.def_readonly("stores_node", &object_formatter::stores_node,
			"For node-derived objects: false (default) if object file doesn't include leafs, true if include")
		.def("save", &object_formatter::save, "obj"_a, "obj_fname"_a)
		.def("load", &object_formatter::load, "obj"_a, "obj_fname"_a)
	;

	m.def("install_formatter", &install_formatter, "obj_type"_a, "obj_formatter"_a);
	m.def("uninstall_formatter", &uninstall_formatter, "obj_type_id"_a, "fmt_name"_a);
	m.def("formatter_installed", &formatter_installed, "obj_type_id"_a, "fmt_name"_a);
	m.def("list_installed_formatters", &list_installed_formatters, "obj_type_id"_a);
	m.def("get_formatter", &get_formatter, "obj_type_id"_a, "fmt_name"_a,
		py::return_value_policy::reference);
}

NAMESPACE_END(blue_sky::python)
