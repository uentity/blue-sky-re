/// @file
/// @author uentity
/// @date 30.08.2016
/// @brief Kernel logging subsystem impl
/// @copyright
/// This Source Code Form is subject to the terms of the Mozilla Public License,
/// v. 2.0. If a copy of the MPL was not distributed with this file,
/// You can obtain one at https://mozilla.org/MPL/2.0/

#include "logging_subsyst.h"

#include <bs/error.h>
#include <bs/log.h>
#include <bs/kernel/errors.h>
#include <bs/kernel/config.h>

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/pattern_formatter.h>

#include <iostream>
#include <filesystem>
#include <unordered_map>
#include <atomic>
#include <mutex>

#define BSCONFIG ::blue_sky::kernel::config::config()
constexpr char FILE_LOG_PATTERN[] = "[%Y-%m-%d %T.%e] [%L] [%*] %v";
constexpr char CONSOLE_LOG_PATTERN[] = "[%L] [%*] %v";
constexpr char LOG_FNAME_PREFIX[] = "bs_";
constexpr auto CUSTOM_TAG_FIELD = std::string_view{ "[%*]" };
constexpr auto ROTATING_FSIZE_DEFAULT = 1024*1024*5;
constexpr auto DEF_FLUSH_INTERVAL = 5;
constexpr auto DEF_FLUSH_LEVEL = spdlog::level::err;

using namespace blue_sky::detail;
using namespace blue_sky;
namespace fs = std::filesystem;

NAMESPACE_BEGIN()
///////////////////////////////////////////////////////////////////////////////
//  log-related globals
//
// fallback null sink
const auto& null_sink() {
	static const auto nowhere = std::make_shared<spdlog::sinks::null_sink_mt>();
	return nowhere;
}

// flag that indicates whether we have switched to mt logs
auto& are_logs_mt() {
	static std::atomic<bool> mt_state{false};
	return mt_state;
}

///////////////////////////////////////////////////////////////////////////////
//  custom patter flag that adds predefined tag to every record
//
struct custom_tag_flag : public spdlog::custom_flag_formatter {
	static auto update_tag(std::string t) {
		// tag value storage
		static auto tagv_ = std::string{};
		static auto mut = std::mutex{};
		std::lock_guard _{ mut };
		tagv_ = std::move(t);
		tag() = tagv_;
	}

	auto format(const spdlog::details::log_msg&, const std::tm&, spdlog::memory_buf_t& dest)
	-> void override {
		dest.append(tag().begin(), tag().end());
	}

	auto clone() const -> std::unique_ptr<custom_flag_formatter> override {
		return std::make_unique<custom_tag_flag>();
	}

private:
	inline static auto tag() -> std::string_view& {
		static auto tag_ = std::string_view{};
		return tag_;
	}
};

auto make_formatter(std::string pat_format) {
	// append custom tag field if it is missing
	if(pat_format.find(CUSTOM_TAG_FIELD) == std::string::npos)
		pat_format.insert(0, CUSTOM_TAG_FIELD);
	// make formatter with custom flag
	auto res = std::make_unique<spdlog::pattern_formatter>();
	res->add_flag<custom_tag_flag>('*').set_pattern(std::move(pat_format));
	return res;
}

///////////////////////////////////////////////////////////////////////////////
//  create sinks
//
// purpose of this function is to find log filename that is not locked by another process
// if `logger_name` is set -- tune sink with configured values
spdlog::sink_ptr create_file_sink(const std::string& desired_fname, const std::string& logger_name = "") {
	static std::unordered_map<std::string, spdlog::sink_ptr> sinks_;
	static std::mutex solo_;

	// protect sinks_ map from mt-access
	std::lock_guard<std::mutex> play_solo(solo_);

	spdlog::sink_ptr res;
	// check if sink with desired fname is already created
	auto S = sinks_.find(desired_fname);
	if(S != sinks_.end()) {
		res = S->second;
	}
	else {
		// if not -- create it
		const auto logf = fs::path(desired_fname);
		const auto logf_ext = logf.extension();
		const auto logf_parent = logf.parent_path();
		const auto logf_body = logf_parent / logf.stem();
		// create parent dir
		if(!logf_parent.empty()) {
			std::error_code er;
			if(!fs::exists(logf_parent, er) && !er)
				fs::create_directories(logf_parent, er);
			if(er) {
				std::cerr << "[E] Failed to create/access dirs for log file " << desired_fname
					<< ": " << er.message() << std::endl;
				return null_sink();
			}
		}

		const auto fsize = logger_name.empty() ? ROTATING_FSIZE_DEFAULT :
			caf::get_or(BSCONFIG, std::string("logger.") + logger_name + "-file-size", ROTATING_FSIZE_DEFAULT);
		for(int i = 0; i < 100; i++) {
			auto cur_logf = logf_body;
			if(i)
				cur_logf += std::string("_") + std::to_string(i);
			cur_logf += logf_ext;

			try {
				res = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
					cur_logf.string(), fsize, 1
				);
				if(res) {
					res->set_formatter(make_formatter(FILE_LOG_PATTERN));
					sinks_.insert( {desired_fname, res} );
					std::cout << "[I] Using log file " << cur_logf.string() << std::endl;
					break;
				}
			}
			catch(...) {}
		}
	}
	// print err if couldn't create log file
	if(!res) std::cerr << "[E] Failed to create log file " << desired_fname << std::endl;

	// configure sink
	if(res && !logger_name.empty()) {
		res->set_formatter(make_formatter(caf::get_or(
			BSCONFIG, std::string("logger.") + logger_name + "-file-format",
			FILE_LOG_PATTERN
		)));
	}

	return res ? res : null_sink();
}

// if `logger_name` is set -- tune sink with configured values
template<typename Sink>
auto create_console_sink(const std::string& logger_name = "") -> spdlog::sink_ptr {
	static const auto sink_ = []() -> spdlog::sink_ptr {
		spdlog::sink_ptr S = null_sink();
		try {
			if((S = std::make_shared<Sink>()))
				S->set_formatter(make_formatter(CONSOLE_LOG_PATTERN));
		}
		catch(...) {}
		return S;
	}();

	if(!logger_name.empty()) {
		sink_->set_formatter(make_formatter(caf::get_or(
			BSCONFIG, std::string("logger.") + logger_name + "-console-format",
			CONSOLE_LOG_PATTERN
		)));
	}

	return sink_;
}

///////////////////////////////////////////////////////////////////////////////
//  create loggers
//
template< typename... Sinks >
auto create_logger(const char* log_name, Sinks... sinks) -> std::shared_ptr<spdlog::logger> {
	static const auto null_st_logger = spdlog::create<spdlog::sinks::null_sink_mt>("null");

	spdlog::sink_ptr S[] = { std::move(sinks)... };
	try {
		auto L = std::make_shared<spdlog::logger>( log_name, std::begin(S), std::end(S) );
		spdlog::register_logger(L);
		return L;
	}
	//catch(const spdlog::spdlog_ex&) {}
	catch(...) {}
	return null_st_logger;
}

template< typename... Sinks >
auto create_async_logger(const char* log_name, Sinks... sinks) -> std::shared_ptr<spdlog::logger> {
	// create null logger and ensure thread pool is initialized
	static const auto null_mt_logger = spdlog::create_async_nb<spdlog::sinks::null_sink_mt>("null");

	spdlog::sink_ptr S[] = { std::move(sinks)... };
	try {
		auto L = std::make_shared<spdlog::async_logger>(
			log_name, std::begin(S), std::end(S), spdlog::thread_pool()
		);
		L->flush_on(static_cast<spdlog::level::level_enum>(caf::get_or(
			BSCONFIG, std::string("logger.") + log_name + "-flush-level",
			std::uint8_t(DEF_FLUSH_LEVEL)
		)));
		spdlog::register_logger(L);
		return L;
	}
	catch(...) {}
	return null_mt_logger;
}

// global bs_log loggers for "out" and "err"
auto& predefined_logs() {
	static auto loggers = []{
		// do some default initialization
		// set global minimum flush level
		spdlog::flush_on(DEF_FLUSH_LEVEL);
		// ... and create `bs_log` instances
		return std::tuple{
			std::make_unique<log::bs_log>("out"),
			std::make_unique<log::bs_log>("err")
		};
	}();
	return loggers;
}

NAMESPACE_END() // hidden namespace

NAMESPACE_BEGIN(blue_sky)
NAMESPACE_BEGIN(log)
/*-----------------------------------------------------------------------------
 *  get/create spdlog::logger backend for bs_log
 *-----------------------------------------------------------------------------*/
auto get_logger(const char* log_name) -> spdlog::logger& {
	// switch create on async/not async
	auto create = [log_name](auto&&... sinks) {
		return are_logs_mt() ?
			create_async_logger(log_name, std::forward<decltype(sinks)>(sinks)...) :
			create_logger(log_name, std::forward<decltype(sinks)>(sinks)...);
	};

	// if log already registered -- return it
	if(auto L = spdlog::get(log_name))
		return *L;
	// otherwise create it
	const bool is_error = std::string_view(log_name) == "err";
	return *create(
		is_error ?
			create_console_sink<spdlog::sinks::stderr_sink_mt>() :
			create_console_sink<spdlog::sinks::stdout_sink_mt>(),
		kernel::config::is_configured() ?
			create_file_sink(caf::get_or(BSCONFIG,
				std::string("logger.") + log_name + "-file-name",
				std::string(LOG_FNAME_PREFIX) + log_name + ".log"), log_name
			) :
			null_sink()
	);
}

auto set_custom_tag(std::string tag) -> void {
	custom_tag_flag::update_tag(std::move(tag));
}

NAMESPACE_END(log)

/*-----------------------------------------------------------------------------
 *  kernel logging subsyst impl
 *-----------------------------------------------------------------------------*/
NAMESPACE_BEGIN(kernel::detail)

logging_subsyst::logging_subsyst() {
	// ensure that log globals are created before kernel
	// that means log will be alive as long as kernel alive
	const spdlog::logger* const init_logs[] = {
		&log::get_logger("out"), &log::get_logger("err")
	};
	(void)init_logs;
}

// switch between mt- and st- logs
auto logging_subsyst::toggle_mt_logs(bool turn_on) -> void {
	if(are_logs_mt().exchange(turn_on) != turn_on) {
		// drop all previousely created logs
		spdlog::drop_all();
		// and create new ones
		predefined_logs() = {
			std::make_unique<log::bs_log>("out"),
			std::make_unique<log::bs_log>("err")
		};
		// setup periodic flush
		if(turn_on)
			spdlog::flush_every(std::chrono::seconds(caf::get_or(
				BSCONFIG, "logger.flush-interval", DEF_FLUSH_INTERVAL
			)));
		else
			// disable periodic flush for non-mt loggers
			spdlog::flush_every(std::chrono::seconds::zero());
	}
}

NAMESPACE_END(kernel::detail)

/*-----------------------------------------------------------------
 * access to main log channels
 *----------------------------------------------------------------*/
log::bs_log& bsout() {
	return *std::get<0>(predefined_logs());
}

log::bs_log& bserr() {
	return *std::get<1>(predefined_logs());
}

NAMESPACE_END(blue_sky)

