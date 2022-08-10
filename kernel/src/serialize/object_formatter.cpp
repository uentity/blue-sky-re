/// @author uentity
/// @date 01.07.2019
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include <bs/serialize/object_formatter.h>
#include <bs/objbase.h>
#include <bs/kernel/misc.h>
#include <bs/uuid.h>

#include <map>
#include <unordered_map>
#include <set>
#include <mutex>

NAMESPACE_BEGIN(blue_sky)
NAMESPACE_BEGIN()

/*-----------------------------------------------------------------------------
 *  Formatters manip impl
 *-----------------------------------------------------------------------------*/
struct fmaster {
	// obj_type_id -> set of unique object_formatter instances sorted by name
	// [NOTE] type_id is stored as string view!
	using fmt_storage_t = std::map< std::string_view, std::set<object_formatter, std::less<>>, std::less<> >;
	fmt_storage_t fmt_storage;

	using registry_t = std::unordered_map<const objbase*, const object_formatter*>;
	registry_t registry;

	using archive_registry_t = std::unordered_multimap<const object_formatter*, void*>;
	archive_registry_t archive_registry;

	// sync access to data above
	std::mutex fmt_guard, reg_guard, arch_reg_guard;

	fmaster() {}
	fmaster(const fmaster& rhs) : fmt_storage(rhs.fmt_storage) {}
	fmaster(fmaster&& rhs) : fmt_storage(std::move(rhs.fmt_storage)) {}

	static auto self() -> fmaster& {
		static fmaster& self = []() -> fmaster& {
			// generate random key
			auto& kstorage = kernel::idx_key_storage(to_string(gen_uuid()));
			auto r = kstorage.insert_element(0, fmaster());
			if(!r.first) throw error("Failed to make impl of object formatters in kernel storage!");
			return *r.first;
		}();

		return self;
	}

	auto install_formatter(const type_descriptor& obj_type, object_formatter&& of) -> bool {
		auto solo = std::lock_guard{ fmt_guard };
		return fmt_storage[obj_type.name].insert(std::move(of)).second;
	}

	auto uninstall_formatter(std::string_view obj_type_id, std::string fmt_name) -> bool {
		// deny removing fallback binary formatter
		if(fmt_name == detail::bin_fmt_name) return false;

		auto solo = std::lock_guard{ fmt_guard };
		if(auto fmts = fmt_storage.find(obj_type_id); fmts != fmt_storage.end()) {
			auto& fmt_set = fmts->second;
			if(auto pfmt = fmt_set.find(fmt_name); pfmt != fmt_set.end()) {
				// erase format
				fmt_set.erase(pfmt);
				return true;
			}
		}
		return false;
	}

	auto formatter_installed(std::string_view obj_type_id, std::string_view fmt_name) -> bool {
		auto solo = std::lock_guard{ fmt_guard };
		if(auto fmts = fmt_storage.find(obj_type_id); fmts != fmt_storage.end())
			return fmts->second.find(fmt_name) != fmts->second.end();
		return false;
	}

	auto list_installed_formatters(std::string_view obj_type_id) -> std::vector<std::string> {
		auto solo = std::lock_guard{ fmt_guard };
		auto res = std::vector<std::string>{};
		if(auto fmts = fmt_storage.find(obj_type_id); fmts != fmt_storage.end()) {
			res.reserve(fmts->second.size());
			for(const auto& f : fmts->second)
				res.emplace_back(f.name);
		}
		return res;
	}

	auto get_formatter(std::string_view obj_type_id, std::string_view fmt_name) -> object_formatter* {
		auto solo = std::lock_guard{ fmt_guard };
		if(auto fmts = fmt_storage.find(obj_type_id); fmts != fmt_storage.end()) {
			auto& fmt_set = fmts->second;
			if(auto pfmt = fmt_set.find(fmt_name); pfmt != fmt_set.end())
				return &const_cast<object_formatter&>(*pfmt);
		}
		return nullptr;
	}

	auto register_formatter(const objbase& obj, const object_formatter* obj_fmt) -> void {
		auto solo = std::lock_guard{ reg_guard };
		registry[&obj] = obj_fmt;
	}

	auto deregister_formatter(const objbase& obj) -> void {
		auto solo = std::lock_guard{ reg_guard };
		registry.erase(&obj);
	}

	auto get_obj_formatter(const objbase* obj) -> const object_formatter* {
		auto solo = std::lock_guard{ reg_guard };
		if(auto r = registry.find(obj); r != registry.end())
			return r->second;
		return nullptr;
	}

	auto register_archive(const object_formatter* frm, void* archive) -> void {
		auto solo = std::lock_guard{ arch_reg_guard };
		archive_registry.emplace(frm, archive);
	}

	auto deregister_archive(const object_formatter* frm) -> void {
		auto solo = std::lock_guard{ arch_reg_guard };
		archive_registry.erase(frm);
	}

	auto deregister_archive(const object_formatter* frm, void* archive) -> void {
		auto solo = std::lock_guard{ arch_reg_guard };
		for(auto [a, b] = archive_registry.equal_range(frm); a != b; ++a) {
			if(a->second == archive) {
				archive_registry.erase(a);
				break;
			}
		}
	}

	auto contains_archive(const object_formatter* frm, void* archive) -> bool {
		auto solo = std::lock_guard{ arch_reg_guard };
		for(auto [a, b] = archive_registry.equal_range(frm); a != b; ++a) {
			if(a->second == archive)
				return true;
		}
		return false;
	}

};

NAMESPACE_END()

/*-----------------------------------------------------------------------------
 *  Formatters manip public API
 *-----------------------------------------------------------------------------*/
#define FM fmaster::self()

auto install_formatter(const type_descriptor& obj_type, object_formatter of) -> bool {
	return FM.install_formatter(obj_type, std::move(of));
}

auto uninstall_formatter(std::string_view obj_type_id, std::string fmt_name) -> bool {
	return FM.uninstall_formatter(obj_type_id, std::move(fmt_name));
}

auto formatter_installed(std::string_view obj_type_id, std::string_view fmt_name) -> bool {
	return FM.formatter_installed(obj_type_id, fmt_name);
}

auto list_installed_formatters(std::string_view obj_type_id) -> std::vector<std::string> {
	return FM.list_installed_formatters(obj_type_id);
}

auto get_formatter(std::string_view obj_type_id, std::string_view fmt_name) -> object_formatter* {
	return FM.get_formatter(obj_type_id, fmt_name);
}

auto get_obj_formatter(const objbase* obj) -> const object_formatter* {
	return FM.get_obj_formatter(obj);
}

/*-----------------------------------------------------------------------------
 *  object_formatter
 *-----------------------------------------------------------------------------*/
object_formatter::object_formatter(
	std::string fmt_name, object_saver_fn saver, object_loader_fn loader, bool stores_node_
) : base_t{std::move(saver), std::move(loader)}, name(std::move(fmt_name)), stores_node(stores_node_)
{}

// compare formatters by name
auto operator<(const object_formatter& lhs, const object_formatter& rhs) {
	return lhs.name < rhs.name;
}
// ... and with arbitrary string key
auto operator<(const object_formatter& lhs, std::string_view rhs) {
	return lhs.name < rhs;
}
auto operator<(std::string_view lhs, const object_formatter& rhs) {
	return lhs < rhs.name;
}

auto object_formatter::save(const objbase& obj, std::string obj_fname) noexcept -> error {
	// if obj has empty payload, skip save
	if(obj.empty_payload()) return tree::Error::EmptyData;

	const auto finally = scope_guard{[&] { error::eval_safe([&] { FM.deregister_formatter(obj); }); }};
	return error::eval_safe([&] {
		FM.register_formatter(obj, this);
		first(*this, obj, std::move(obj_fname), name);
	});
}

auto object_formatter::load(objbase& obj, std::string obj_fname) noexcept -> error {
	const auto finally = scope_guard{[&] { error::eval_safe([&] { FM.deregister_formatter(obj); }); }};
	return error::eval_safe([&] {
		FM.register_formatter(obj, this);
		second(*this, obj, std::move(obj_fname), name);
	});
}

auto object_formatter::bind_archive(void* archive) const -> void {
	FM.register_archive(this, archive);
}

auto object_formatter::unbind_archive(void* archive) const -> void {
	FM.deregister_archive(this, archive);
}

auto object_formatter::unbind_archive() const -> void {
	FM.deregister_archive(this);
}

auto object_formatter::is_archive_binded(void* archive) const -> bool {
	return FM.contains_archive(this, archive);
}

NAMESPACE_END(blue_sky)
