/// @author uentity
/// @date 19.02.2020
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include <bs/actor_common.h>
#include <bs/timetypes.h>
#include <bs/kernel/radio.h>
#include <bs/tree/errors.h>
#include <bs/tree/link.h>
#include <bs/detail/str_utils.h>
#include <bs/serialize/serialize_decl.h>

#include "../tree/link_impl.h"

#include <caf/typed_event_based_actor.hpp>
#include <caf/typed_response_promise.hpp>

#include <cereal/archives/json.hpp>

#include <filesystem>
#include <fstream>
#include <list>
#include <unordered_set>


NAMESPACE_BEGIN(blue_sky)

/// current version of TreeFS archive format
inline constexpr std::uint32_t tree_fs_version = 0;

/// extension of link files
inline constexpr auto link_file_ext = ".bsl";

/// objects/links directory name
inline constexpr auto objects_dirname = ".objects";
inline constexpr auto links_dirname = ".links";

/// name of file with empty payload objects IDs
inline constexpr auto empty_payload_fname = "empty_payload.bin";

NAMESPACE_END(blue_sky);


NAMESPACE_BEGIN(blue_sky::detail)
namespace fs = std::filesystem;

/*-----------------------------------------------------------------------------
 *  handle objects save/load jobs
 *-----------------------------------------------------------------------------*/
using objfrm_result_t = std::pair<std::vector<error::box>, std::vector<uuid>>;

using objfrm_manager_t = caf::typed_actor<
	// launch object formatting job
	caf::reacts_to<
		sp_obj /*what*/, std::string /*formatter name*/, std::string /*filename*/
	>,
	// end formatting session
	caf::reacts_to<a_bye>,
	// return collected job errors + objects with empty payload
	caf::replies_to< a_ack >::with< objfrm_result_t >
>;

// [NOTE] manager is valid only for one save/load session!
// on next session just spawn new manager
struct BS_HIDDEN_API objfrm_manager : objfrm_manager_t::base {
	using actor_type = objfrm_manager_t;

	objfrm_manager(caf::actor_config& cfg, bool is_saving);

	auto make_behavior() -> behavior_type override;

	// returns collected errors + objects with empty payload
	static auto wait_jobs_done(objfrm_manager_t self, timespan how_long)
	-> std::pair<std::vector<error>, std::vector<uuid>>;

private:
	// deliver session results back to requester
	auto session_ack() -> void;

	const bool is_saving_;
	bool session_finished_ = false;

	// errors collection
	std::vector<error::box> er_stack_;
	caf::typed_response_promise<objfrm_result_t> res_;
	// track running savers
	size_t nstarted_ = 0, nfinished_ = 0;
	// collect objects with empty payload
	std::vector<uuid> empty_payload_;
};

/*-----------------------------------------------------------------------------
 *  Manage link file streams and dirs during tree save/load
 *-----------------------------------------------------------------------------*/
template<bool Saving>
struct file_heads_manager {
	using Error = tree::Error;

	// setup traits depending on save/load mode
	template<bool Saving_, typename = void>
	struct trait {
		using neck_t = std::ofstream;
		using head_t = cereal::JSONOutputArchive;

		static constexpr auto neck_mode = std::ios::out | std::ios::trunc;
	};

	template<typename _>
	struct trait<false, _> {
		using neck_t = std::ifstream;
		using head_t = cereal::JSONInputArchive;

		static constexpr auto neck_mode = std::ios::in;
	};

	using trait_t = trait<Saving>;
	using neck_t = typename trait_t::neck_t;
	using head_t = typename trait_t::head_t;
	static constexpr auto neck_mode = trait_t::neck_mode;

	// ctor
	// [NOTE] assume that paths come in UTF-8
	file_heads_manager(TFSOpts opts, const std::string& root_fname) :
		opts_(opts), root_fname_(ustr2str(root_fname))
	{
		// [NOTE] `root_fname_`, `root_dname_` are converted to native encoding
#ifdef _WIN32
		// for Windows add '\\?\' prefix for long names support
		static constexpr auto magic_prefix = std::string_view{ "\\\\?\\" };
		// don't add prefix if path starts with slash
		if(!root_fname_.empty() && root_fname_[0] != '\\')
			root_fname_.insert(0, magic_prefix);
#endif
		// try convert root filename to absolute
		auto root_path = fs::path(root_fname_);
		auto abs_root = fs::path{};
		auto er = error::eval_safe([&]{ abs_root = fs::absolute(root_path); });
		if(!er) {
			// extract root dir from absolute filename
			root_dname_ = abs_root.parent_path().string();
			root_fname_ = abs_root.filename().string();
		}
		else if(root_path.has_parent_path()) {
			// could not make abs path
			root_dname_ = root_path.parent_path().string();
			root_fname_ = root_path.filename().string();
		}
		else {
			// best we can do
			// [TODO] throw error here
			error::eval_safe([&]{ root_dname_ = fs::current_path().string(); });
		}
	}

	// if entering `src_path` is successfull, set `tar_path` to src_path
	template<typename Path>
	auto enter_dir(Path src_path, fs::path& tar_path, TFSOpts opts = TFSOpts::None) -> error {
		auto path = fs::path(std::move(src_path));
		if(path.empty()) return { path.u8string(), Error::EmptyPath };

		EVAL_SAFE
			// do something when path doesn't exist
			[&] {
				if(!fs::exists(path)) {
					if constexpr(Saving) {
						// clear directory when saving if TFSOpts::SaveClearDirs is set
						if(enumval(opts & TFSOpts::ClearDirs))
							fs::remove_all(path);
						fs::create_directories(path);
					}
					else
						return error{ path.u8string(), Error::PathNotExists };
				}
				return success();
			},
			// check that path is a directory
			[&] {
				return fs::is_directory(path) ?
					success() : error{ path.u8string(), Error::PathNotDirectory };
			}
		RETURN_EVAL_ERR

		tar_path = std::move(path);
		return perfect;
	}

	template<typename Path>
	auto enter_dir(Path src_path, TFSOpts opts) {
		auto unused = fs::path{};
		return enter_dir(std::move(src_path), unused, opts);
	}

	template<typename Path>
	auto enter_dir(Path src_path) {
		return enter_dir(std::move(src_path), TFSOpts::None);
	}

	auto enter_root() -> error {
		if(root_path_.empty()) {
			if(auto er = enter_dir(root_dname_, root_path_)) return er;
		}
		if(cur_path_.empty()) cur_path_ = root_path_;
		return perfect;
	}

	// `fname` must not contain any dirs!
	static auto prehash_stem(const fs::path& fname) {
		if(auto stem = fname.stem().string(); !stem.empty())
			return stem.substr(0, 1) / fname;
		return fname;
	}

	auto add_head(fs::path head_path) -> error {
	return error::eval_safe(
		// enter parent dir
		// [NOTE] explicit capture `head_path` because VS doesn't capture it with simple '&'
		// because of constexpr if?
		[this, &head_path] {
			// [NOTE] don't enter parent dir (and reset `cur_path_`) when loading because:
			// 1. we already antered parent dir in all usage conditions
			// 2. resetting `cur_path_` is an error, because there's an optimization for not creating
			// empty dirs for empty nodes
			if constexpr(Saving) {
				return enter_dir(head_path.parent_path());
			}
		},

		[&] {
			if(auto neck = neck_t(head_path, neck_mode)) {
				necks_.push_back(std::move(neck));
				heads_.emplace_back(necks_.back());
				return success();
			}
			return error{ head_path.u8string(), Saving ? Error::CantWriteFile : Error::CantReadFile };
		}
	); }

	auto pop_head() -> void {
		if(!heads_.empty()) {
			heads_.pop_back();
			necks_.pop_back();
		}
	}

	auto head() -> result_or_err<head_t*> {
		using namespace cereal;

		if(heads_.empty()) {
			if(auto er = error::eval_safe(
				[&] { return enter_root(); },
				[&] { return add_head(root_path_ / root_fname_); },
				[&] {
					// read/write format version
					auto& rhead = heads_.back();
					rhead(make_nvp("format_version", version_));
					// read links/objects directory path
					if constexpr(!Saving) {
						static constexpr auto gf = fs::path::format::generic_format;
						// can skip UTF-8 to native conversion as dirnames are guaranteed ASCII
						auto buf = std::string{};
						rhead( make_nvp("links_dir", buf) );
						links_path_ = root_path_ / fs::path(buf, gf).make_preferred();
						rhead( make_nvp("objects_dir", buf) );
						objects_path_ = root_path_ / fs::path(buf, gf).make_preferred();
					}
				}
			)) return tl::make_unexpected(std::move(er));
			// start new formatters manager
			manager_ = kernel::radio::system().spawn<objfrm_manager>(Saving);
		}
		return &heads_.back();
	}

	auto end_link(const tree::link& L) -> error {
		pop_head();
		// tell manager that session finished when very first head (root_fname_) is popped
		if(heads_.empty())
			caf::anon_send(manager_, a_bye());
		// setup link to trigger dealyed object load
		if constexpr(!Saving) {
			if(L) {
				if(auto r = tree::link_impl::actorf<bool>(L, a_lazy(), a_load()); !r)
					return r.error();
			}
		}
		return perfect;
	}

	TFSOpts opts_;

	std::string root_fname_, root_dname_;
	fs::path root_path_, cur_path_, links_path_, objects_path_;

	objfrm_manager_t manager_;

	std::list<neck_t> necks_;
	std::list<head_t> heads_;

	std::uint32_t version_ = tree_fs_version;
};

NAMESPACE_END(blue_sky::detail)
