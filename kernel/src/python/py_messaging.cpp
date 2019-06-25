/// @file
/// @author uentity
/// @date 12.01.2016
/// @brief 
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include <bs/python/common.h>
#include <bs/compat/messaging.h>

BSPY_ANY_CAST_EXTRA(long double)
#include <bs/python/any.h>

NAMESPACE_BEGIN(blue_sky::python)

void py_bind_signal(py::module&);

NAMESPACE_BEGIN()
using namespace std;

// bs_slot wrapper
class py_bs_slot : public bs_slot {
public:
	using bs_slot::bs_slot;

	void execute(std::any sender, int signal_code, std::any param) const override {
		// Use modified version of PYBIND11_OVERLOAD_PURE macro code
		// original implementation would fail with runtime error that pure virtual method is called
		// if no overload was found. But slot should be safe in sutuation when Python object is
		// already destroyed. In such a case just DO NOTHING and return.
		pybind11::gil_scoped_acquire gil;
		pybind11::function overload = pybind11::get_overload(static_cast<const bs_slot *>(this), "execute");
		if (overload) {
			auto o = overload(std::move(sender), signal_code, std::move(param));
			if (pybind11::detail::cast_is_temporary_value_reference<void>::value) {
				static pybind11::detail::overload_caster_t<void> caster;
				return pybind11::detail::cast_ref<void>(std::move(o), caster);
			}
			else return pybind11::detail::cast_safe<void>(std::move(o));
		}
		//PYBIND11_OVERLOAD_PURE(
		//	void,
		//	bs_slot,
		//	execute,
		//	std::move(sender), signal_code, std::move(param)
		//);
	}
};

// bs_imessaging wrapper
// template used to flatten trampoline hierarchy -- they don't support multiple inheritance
template<typename Next = bs_imessaging>
class py_bs_imessaging : public Next {
public:
	using Next::Next;

	bool subscribe(int signal_code, sp_slot slot) const override {
		PYBIND11_OVERLOAD_PURE(
			bool,
			Next,
			subscribe,
			signal_code, std::move(slot)
		);
	}

	bool unsubscribe(int signal_code, sp_slot slot) const override {
		PYBIND11_OVERLOAD_PURE(
			bool,
			Next,
			unsubscribe,
			signal_code, std::move(slot)
		);
	}

	ulong num_slots(int signal_code) const override {
		PYBIND11_OVERLOAD_PURE(
			ulong,
			Next,
			num_slots,
			signal_code
		);
	}

	bool fire_signal(int signal_code, std::any param, std::any sender) const override {
		PYBIND11_OVERLOAD_PURE(
			bool,
			Next,
			fire_signal,
			signal_code, std::move(param), std::move(sender)
		);
	}

	std::vector< int > get_signal_list() const override {
		PYBIND11_OVERLOAD_PURE(
			std::vector< int >,
			Next,
			get_signal_list
		);
	}
};

// resulting trampoline for bs_mesagins
class py_bs_messaging : public py_bs_imessaging< py_object<bs_messaging> > {
public:
	using py_bs_imessaging<py_object<bs_messaging>>::py_bs_imessaging;

	bool subscribe(int signal_code, sp_slot slot) const override {
		PYBIND11_OVERLOAD(
			bool,
			bs_messaging,
			subscribe,
			signal_code, std::move(slot)
		);
	}

	bool unsubscribe(int signal_code, sp_slot slot) const override {
		PYBIND11_OVERLOAD(
			bool,
			bs_messaging,
			unsubscribe,
			signal_code, std::move(slot)
		);
	}

	ulong num_slots(int signal_code) const override {
		PYBIND11_OVERLOAD(
			ulong,
			bs_messaging,
			num_slots,
			signal_code
		);
	}

	bool fire_signal(int signal_code, std::any param, std::any sender) const override {
		PYBIND11_OVERLOAD(
			bool,
			bs_messaging,
			fire_signal,
			signal_code, std::move(param), std::move(sender)
		);
	}

	std::vector< int > get_signal_list() const override {
		PYBIND11_OVERLOAD(
			std::vector< int >,
			bs_messaging,
			get_signal_list
		);
	}

	bool add_signal(int signal_code) override {
		PYBIND11_OVERLOAD(
			bool,
			bs_messaging,
			add_signal,
			signal_code
		);
	}

	bool remove_signal(int signal_code) override {
		PYBIND11_OVERLOAD(
			bool,
			bs_messaging,
			remove_signal,
			signal_code
		);
	}
};

void slot_tester(int c, const sp_slot& slot, std::any param) {
	slot->execute(nullptr, c, std::move(param));
}

NAMESPACE_END()

// exporting function
void py_bind_messaging(py::module& m) {
	// slot
	py::class_<
		bs_slot, py_bs_slot, std::shared_ptr<bs_slot>
	>(m, "slot")
		.def(py::init<>())
		.def("execute", &bs_slot::execute)
	;

	// signal
	py::class_<
		bs_signal,
		std::shared_ptr< bs_signal >
	>(m, "signal")
		.def(py::init<int>())
		.def("init", &bs_signal::init)
		.def_property_readonly("get_code", &bs_signal::get_code)
		.def("connect", &bs_signal::connect, "slot"_a, "sender"_a = nullptr)
		.def("disconnect", &bs_signal::disconnect)
		.def_property_readonly("num_slots", &bs_signal::num_slots)
		.def("fire", &bs_signal::fire, "sender"_a = nullptr, "param"_a = std::any{})
	;

	// DEBUG
	m.def("slot_tester", &slot_tester);

	// bs_imessaging abstract class
	py::class_<
		bs_imessaging, py_bs_imessaging<>, std::shared_ptr<bs_imessaging>
	>(m, "imessaging")
		.def("subscribe"       , &bs_imessaging::subscribe)
		.def("unsubscribe"     , &bs_imessaging::unsubscribe)
		.def("num_slots"       , &bs_imessaging::num_slots)
		.def("fire_signal"     , &bs_imessaging::fire_signal,
			"signal_code"_a, "param"_a = nullptr, "sender"_a = nullptr
		)
		.def("get_signal_list" , &bs_imessaging::get_signal_list)
	;

	// bs_messaging
	bool (bs_messaging::*add_signal_ptr)(int) = &bs_messaging::add_signal;

	py::class_<
		bs_messaging, bs_imessaging, objbase, py_bs_messaging,
		std::shared_ptr< bs_messaging >
	>(m, "messaging", py::multiple_inheritance())
		BSPY_EXPORT_DEF(bs_messaging)

		.def(py::init_alias<>())
		.def(py::init_alias< bs_messaging::sig_range_t >())
		.def("subscribe"       , &bs_messaging::subscribe)
		.def("unsubscribe"     , &bs_messaging::unsubscribe)
		.def("num_slots"       , &bs_messaging::num_slots)
		.def("fire_signal"     , &bs_messaging::fire_signal,
			"signal_code"_a, "param"_a = nullptr, "sender"_a = nullptr
		)
		.def("add_signal"      , add_signal_ptr)
		.def("remove_signal"   , &bs_messaging::remove_signal)
		.def("get_signal_list" , &bs_messaging::get_signal_list)
		.def("clear"           , &bs_messaging::clear)
		.def("test_slot1", [](const bs_messaging& src, const sp_slot& slot, const sp_obj param = nullptr) {
			slot->execute(src.shared_from_this(), 42, param);
		})
	;
}

NAMESPACE_END(blue_sky::python)
