/// @file
/// @author uentity
/// @date 02.07.2019
/// @brief Tools to help producing object formatters
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/
#pragma once

#include "object_formatter.h"
#include "make_base_class.h"
#include "../objbase.h"

#include <cereal/cereal.hpp>
#include <cereal/archives/portable_binary.hpp>

#include <functional>
#include <fstream>

NAMESPACE_BEGIN(blue_sky)

template<typename T>
inline auto install_bin_formatter(bool store_node = false, bool force = false) {
	static_assert(std::is_base_of_v<objbase, T>, "Formatters applicable only to BS objects");
	const auto& td = T::bs_type();
	if(!force && formatter_installed(td.name, detail::bin_fmt_name)) return false;

	auto bin_saver = [](const objbase& obj, std::ofstream& obj_file, std::string_view) -> error {
		cereal::PortableBinaryOutputArchive binar(obj_file);
		binar(static_cast< std::add_lvalue_reference_t<const T> >(obj));
		return perfect;
	};

	auto bin_loader = [](objbase& obj, std::ifstream& obj_file, std::string_view) -> error {
		cereal::PortableBinaryInputArchive binar(obj_file);
		binar(static_cast< std::add_lvalue_reference_t<T> >(obj));
		return perfect;
	};

	return install_formatter(
		td, { detail::bin_fmt_name, std::move(bin_saver), std::move(bin_loader), store_node }
	);
}

template<typename T, typename Archive>
struct formatter_tools {
private:
	static constexpr auto sibling_archive() {
		if constexpr(Archive::is_saving::value)
			return (typename cereal::traits::detail::get_input_from_output<Archive>::type*)nullptr;
		else
			return (typename cereal::traits::detail::get_output_from_input<Archive>::type*)nullptr;
	}
	using siblig_archive_t = std::remove_pointer_t<decltype(sibling_archive())>;

public:
	using type = std::decay_t<T>;
	using archive_t = Archive;
	/// make sure that `custom_node_serialization` is the same in both input and output archives
	static_assert(
		detail::custom_node_serialization_v<Archive> == detail::custom_node_serialization_v<siblig_archive_t>,
		"Custom node serialization switch must be the same for both Input and Output archives"
	);

	template<typename Saver, typename Loader>
	static auto install_formatter(
		std::string fmt_name, Saver&& saver, Loader&& loader, bool make_active = true
	) {
		// scan saver & loader signature for compatibility
		static_assert(
			std::is_invocable_r_v<Saver, error, const type&, std::ofstream&, std::string_view>,
			"Saver signature must be compatible with `(const T& obj, std::ofstream& obj_file, std::string_view fmt_name) -> error`"
		);
		static_assert(
			std::is_invocable_r_v<Loader, error, type&, std::ifstream&, std::string_view>,
			"Loader signature must be compatible with `(T& obj, std::ifstream& obj_file, std::string_view fmt_name) -> error`"
		);

		auto saver_adapter = [f = std::forward<Saver>(saver)] (
			const objbase& obj, std::ofstream& obj_file, std::string_view fmt_name
		) -> error {
			return std::invoke(f, static_cast<const type&>(obj), obj_file, fmt_name);
		};

		auto loader_adapter = [f = std::forward<Loader>(loader)] (
			objbase& obj, std::ifstream& obj_file, std::string_view fmt_name
		) -> error {
			return std::invoke(f, static_cast<type&>(obj), obj_file, fmt_name);
		};

		return install_formatter(
			type::bs_type(), fmt_name, { std::move(saver_adapter), std::move(loader_adapter) }
		);
	}

	static auto get_active_formatter() -> object_formatter& {
		if(auto pfmt = get_active_formatter(type::bs_type().name); pfmt)
			return pfmt->first;
		else {
			install_bin_formatter<type>();
			return get_active_formatter();
		}
	}

	static auto get_active_saver() -> object_saver_fn& {
		return get_active_formatter().first;
	}

	static auto get_active_loader() -> object_loader_fn& {
		return get_active_formatter().second;
	}
};

NAMESPACE_END(blue_sky)
