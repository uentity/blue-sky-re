/// @file
/// @author uentity
/// @date 16.12.2016
/// @brief Python-transparent array
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include "common.h"
#include "../compat/arrbase_shared.h"
#include "../compat/array.h"

#include <pybind11/numpy.h>

/*-----------------------------------------------------------------------------
 *  numpy traits for bs_array
 *-----------------------------------------------------------------------------*/
NAMESPACE_BEGIN(blue_sky)

template< typename T >
class bs_nparray_traits
	: public bs_arrbase< T >, public pybind11::array_t< T, pybind11::array::forcecast > {
public:
    // traits for bs_array
	using container = bs_nparray_traits< T >;
	using arrbase = bs_arrbase< T >;
	using bs_array_base = bs_nparray_traits;
	using typename arrbase::sp_arrbase;

	// inherited from bs_arrbase class
	using typename arrbase::value_type;
	using typename arrbase::key_type;
	using typename arrbase::size_type;

	using typename arrbase::pointer;
	using typename arrbase::reference;
	using typename arrbase::const_pointer;
	using typename arrbase::const_reference;
	using typename arrbase::iterator;
	using typename arrbase::const_iterator;
	using typename arrbase::reverse_iterator;
	using typename arrbase::const_reverse_iterator;

	using arrbase::begin;
	using arrbase::end;

	using pyarray_t = pybind11::array_t< T, pybind11::array::forcecast >;

	// import from base pybind11 array_t
	using pyarray_t::pyarray_t;
	using pyarray_t::m_ptr;
	using pyarray_t::resize;
	using pyarray_t::data;

	// ctors required by bs_array
	bs_nparray_traits() : pyarray_t(0) {}

	bs_nparray_traits(size_type sz, const T& init_value) : pyarray_t(sz) {
		std::fill(data(), data() + this->size(), init_value);
	}

	bs_nparray_traits(const T* from, const T* to) : pyarray_t(to > from ? to - from : 0) {
		if(to > from)
			std::copy(from, to, data());
	}

	// specific implementation of resize
	// disables refcheck - use with care
	void resize(size_type new_size) {
		pyarray_t::resize({new_size}, false);
	}

	void resize(size_type new_size, value_type init_value) {
		auto old_size = size();

		// resize can fail to actually resize array
		resize(new_size);
		auto new_data = data();
		new_size = size();
		std::fill(new_data + std::min(old_size, new_size), new_data + new_size, init_value);
	}

	void swap(bs_nparray_traits& lhs) {
		bs_nparray_traits t(*this);
		*this = lhs;
		lhs = t;
	}

	pointer data() {
		return pyarray_t::mutable_data();
	}
	const_pointer data() const {
		return pyarray_t::data();
	}

	reference operator[](const key_type& k) {
		return data()[k];
	}
	const_reference operator[](const key_type& k) const {
		return data()[k];
	}

	size_type size() const {
		return static_cast< size_type >(pyarray_t::size());
	}

	sp_arrbase clone() const {
		return std::make_shared< bs_nparray_traits >(begin(), end());
	}
};

// alias
template< class T > using bs_numpy_array = bs_array< T, bs_nparray_traits >;

NAMESPACE_END(blue_sky)

/*-----------------------------------------------------------------------------
 *  Python bindings of bs_array
 *-----------------------------------------------------------------------------*/
NAMESPACE_BEGIN(pybind11)
NAMESPACE_BEGIN(detail)

// Casts bs_array type to numpy array. If given a base, the numpy array references the src data,
// otherwise it'll make a copy.  writeable lets you turn off the writeable flag for the array.
template< typename Array >
handle bs_array_cast(Array const& src, handle base = handle(), bool writeable = true) {
	array a(
		src.dtype(), array::ShapeContainer(src.shape(), src.shape() + src.ndim()),
		array::StridesContainer(src.strides(), src.strides() + src.ndim()), src.data(), base
	);
	if (!writeable)
		array_proxy(a.ptr())->flags &= ~detail::npy_api::NPY_ARRAY_WRITEABLE_;

	return a.release();
}

// Takes an lvalue ref to some bs_array type and a (python) base object, creating a numpy array that
// reference the bs_array object's data with `base` as the python-registered base class (if omitted,
// the base will be set to None, and lifetime management is up to the caller).  The numpy array is
// non-writeable if the given type is const.
template< typename ArrayPtr >
handle bs_ref_array(ArrayPtr &src, handle parent = none()) {
	// none here is to get past array's should-we-copy detection, which currently always
	// copies when there is no base.  Setting the base to None should be harmless.
	return bs_array_cast(
		*src, parent, !std::is_const< typename ArrayPtr::element_type >::value
	);
}

// Takes a pointer to some bs_array type, builds a capsule around it, then returns a numpy
// array that references the encapsulated data with a python-side reference to the capsule to tie
// its destruction to that of any dependent python objects.  Const-ness is determined by whether or
// not the Type of the pointer given is const.
template< typename Array >
handle bs_array_encapsulate(const std::shared_ptr< Array > &src) {
	auto h = new std::shared_ptr< Array >(src);
	capsule base(h, [](void *o) {
		delete static_cast< std::shared_ptr< Array>* >(o);
	});
	return bs_ref_array(src, base);
}

// Specialization for bs_nparray_traits - we don't need capsule and can pass source array directly
// as base
template< typename T >
handle bs_array_encapsulate(
	const std::shared_ptr< blue_sky::bs_array< T, blue_sky::bs_nparray_traits > >& src
) {
	return bs_ref_array(src, *src);
}

// Base class for casting bs_array objects back to python.
template <typename Array> struct bs_array_caster {
private:
	using sp_array = std::shared_ptr< Array >;

public:

	// Directly referencing a ref/map's data is a bit dangerous (whatever the map/ref points to hashas
	// to stay around), but we'll allow it under the assumption that you know what you're doing (and
	// have an appropriate keep_alive in place).  We return a numpy array pointing directly at the
	// ref's data (The numpy array ends up read-only if the ref was to a const matrix type.) Note
	// that this means you need to ensure you don't destroy the object in some other way (e.g. with
	// an appropriate keep_alive, or with a reference to a statically allocated matrix).
	static handle cast(const sp_array& src, return_value_policy policy, handle parent) {
		switch (policy) {
			case return_value_policy::copy:
				return bs_ref_array(src);
			case return_value_policy::reference_internal:
				return bs_ref_array(src, parent);
			case return_value_policy::reference:
			case return_value_policy::automatic:
			case return_value_policy::automatic_reference:
			default:
				return bs_array_encapsulate(src);
			//default:
			//	// move, take_ownership don't make any sense for a ref/map:
			//	pybind11_fail("Invalid return_value_policy for bs_array type");
		}
	}

	static constexpr auto name = _("numpy.ndarray[") +
		npy_format_descriptor< typename Array::arrbase::value_type >::name +
		_("]");

	// Explicitly delete these: support python -> C++ conversion on these (i.e. these can be return
	// types but not bound arguments).  We still provide them (with an explicitly delete) so that
	// you end up here if you try anyway.
	bool load(handle, bool) = delete;
	operator Array() = delete;
	operator sp_array() = delete;
	template <typename> using cast_op_type = sp_array;
};

// Cast generic bs_array to Python. Special case of bs_nparray_traits is specialized next
template< class T, template< class > class cont_traits >
struct type_caster< std::shared_ptr< blue_sky::bs_array< T, cont_traits > > >
	: public bs_array_caster< blue_sky::bs_array< T, cont_traits > > {};

// Special case of bs_nparray_traits
template< class T >
struct type_caster< std::shared_ptr< blue_sky::bs_array< T, blue_sky::bs_nparray_traits > > >
	: public bs_array_caster< blue_sky::bs_array< T, blue_sky::bs_nparray_traits > >
{
	using Array = blue_sky::bs_array< T, blue_sky::bs_nparray_traits >;
	using sp_array = std::shared_ptr< Array >;
	using Scalar = typename Array::arrbase::value_type;
	using pyarray_t = typename Array::pyarray_t;

	// Our array.  When possible, this is just a numpy array pointing to the source data, but
	// sometimes we can't avoid copying (e.g. input is not a numpy array at all, has an incompatible
	// layout, or is an array of a type that needs to be converted).  Using a numpy temporary
	// (rather than an Eigen temporary) saves an extra copy when we need both type conversion and
	// storage order conversion.  (Note that we refuse to use this temporary copy when loading an
	// argument for a Ref<M> with M non-const, i.e. a read-write reference).
	sp_array value;

public:

	// DEBUG
	//~type_caster() {
	//	std::cout << "Caster died, value refs = " << value.use_count() << std::endl;
	//}

	bool load(handle src, bool convert) {
		// First check whether what we have is already an array of the right type.  If not, we can't
		// avoid a copy (because the copy is also going to do type conversion).
		bool need_copy = !isinstance< pyarray_t >(src);

		if(!need_copy) {
			pyarray_t aref = reinterpret_borrow< Array >(src);
			if(aref && aref.writeable()) {
				value = std::make_shared< Array >(std::move(aref));
				return true;
			}
		}

		// check if src is None, then assign nullptr to value and we're done
		if(src.is(none())) {
			value = nullptr;
			return true;
		}

		// We need to copy: If we need a mutable reference, or we're not supposed to convert
		// (either because we're in the no-convert overload pass, or because we're explicitly
		// instructed not to copy (via `py::arg().noconvert()`) we have to fail loading.
		if (!convert) return false;

		pyarray_t aref = Array::ensure(src);
		if(!aref) return false;
		value = std::make_shared< Array >(std::move(aref));
		return true;
	}

	operator sp_array() { return value; }
	template <typename> using cast_op_type = sp_array;
};

NAMESPACE_END(detail)
NAMESPACE_END(pybind11)
