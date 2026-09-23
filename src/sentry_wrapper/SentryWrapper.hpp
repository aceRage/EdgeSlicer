#ifndef slic3r_SentryWrapper_hpp_
#define slic3r_SentryWrapper_hpp_

#include <string>
#include "bury_cfg/bury_point.hpp"

namespace Slic3r {

	// Starts the crash handler (crashpad). Always keeps local minidumps; uploads crash reports
	// only when this build has a destination (a DSN compiled in from CI, or a tester's
	// "ultra_sentry_dsn" in the app config) AND the user switched on Preferences > General >
	// "Send crash reports" (send_crash_reports, default off). Call
	// common::set_datadir_from_command_line() first so it reads the right EdgeSlicer.conf.
	void initSentry();

	void exitSentry();

	// Follows the "Send crash reports" preference at runtime, through Sentry's own consent
	// switch, so it covers crashpad's minidump uploads too. No effect without a destination.
	void setSentryUserConsent(bool agreed);

	// True when this build has somewhere to send crash reports (see initSentry()).
	bool sentryCrashReportsAvailable();

	// True when the previous run of the app ended in a crash that the handler caught.
	// With clear = true the marker is removed, so the next start says false again.
	bool sentryCrashedLastRun(bool clear);

	// Adds one app log line as a breadcrumb of the next crash report: only while crash reports
	// are switched on, only for warnings and worse, rate-limited, cut to 512 bytes and passed
	// through scrub_for_crash_report() first. `boost_severity` is boost::log::trivial's level
	// (0 trace .. 5 fatal).
	void sentryAddLogBreadcrumb(int boost_severity, const std::string& message);

    typedef enum SENTRY_LOG_LEVEL {
        SENTRY_LOG_TRACE   = -2,
        SENTRY_LOG_DEBUG   = -1,
        SENTRY_LOG_INFO    = 0,
        SENTRY_LOG_WARNING = 1,
        SENTRY_LOG_ERROR   = 2,
        SENTRY_LOG_FATAL   = 3,
    };

	// EdgeSlicer sends crash reports only. These usage / "bury point" events and structured
	// logs are never sent anywhere: the call sites are kept, the function does nothing.
	void sentryReportLog(SENTRY_LOG_LEVEL   logLevel,
                         const std::string& logContent,
                         const std::string& funcModule  = "",
                         const std::string& logTagKey   = "",
                         const std::string& logTagValue = "",
                         const std::string& logTraceId = "");

    void set_sentry_tags(const std::string& tag_key,const std::string& tag_value);
    } // namespace Slic3r

#endif // slic3r_SentryWrapper_hpp_
