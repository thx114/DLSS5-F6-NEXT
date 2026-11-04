#include "pch.h"
#include "Logger.h"
#include "StartupExceptionEvidence.h"
#include "CrashDumpEvidence.h"
#include "Config.h"
#include <iostream>
#include <filesystem>
#include <DbgHelp.h>

#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/callback_sink.h"
#include <include/spdlog_sink/debug_sink.h>

#include "Util.h"
#include "LogAsyncRule.h"
#include "LogAsyncEvidence.h"

// The logger in use and what its crash-path flush needs (LogAsyncRule.h). Left allocated at exit on purpose: destroying
// a thread pool at process exit joins a worker that is already gone. A logger that was replaced (the menu's file/console
// toggles) is parked in g_retiredSetups, never destroyed: see logasync::Retire.
static std::atomic<logasync::Setup*> g_logSetup { nullptr };
static std::vector<logasync::Setup*> g_retiredSetups;
static std::mutex g_retiredMutex;

static bool InitializeConsole()
{
    // Allocate a console for this app
    if (!AllocConsole())
        return false;

    FILE* pFile;

    // Redirect STDIN if the console has an input handle
    if (GetStdHandle(STD_INPUT_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&pFile, "CONIN$", "r", stdin) != 0)
            return false;
    }

    // Redirect STDOUT if the console has an output handle
    if (GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&pFile, "CONOUT$", "w", stdout) != 0)
            return false;
    }

    // Redirect STDERR if the console has an error handle
    if (GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE)
    {
        if (freopen_s(&pFile, "CONOUT$", "w", stderr) != 0)
            return false;
    }

    // Clear the error state for each of the C++ standard streams
    std::cin.clear();
    std::cout.clear();
    std::cerr.clear();
    std::wcin.clear();
    std::wcout.clear();
    std::wcerr.clear();

    // Make C++ standard streams point to console as well.
    std::ios::sync_with_stdio();

    WaitForEnter();

    return true;
}

void WaitForEnter()
{
    if (Config::Instance()->DebugWait.value_or_default())
    {
        std::cout << "Press ENTER to continue..." << std::endl;
        std::cin.get();
    }
}

void PrepareLogger()
{
    try
    {
        if (spdlog::default_logger() != nullptr)
            spdlog::default_logger().reset();

        if (Config::Instance()->LogToConsole.value_or_default() || Config::Instance()->LogToFile.value_or_default() ||
            Config::Instance()->LogToNGX.value_or_default() || Config::Instance()->LogToDebug.value_or_default())
        {
            if (Config::Instance()->OpenConsole.value_or_default())
                InitializeConsole();

            std::shared_ptr<spdlog::logger> shared_logger = nullptr;

            // Auto: a worker writes the file, so no game thread waits for a disk (a USB drive stalled presents for
            // hundreds of ms). An explicit LogAsync=false writes every line on the calling thread.
            const auto logAsync = Config::Instance()->LogAsync.has_value()
                                      ? std::optional<bool>(Config::Instance()->LogAsync.value())
                                      : std::nullopt;
            const auto logMode = logasync::ChooseMode(logAsync, Config::Instance()->LogToFile.value_or_default());

            if (logMode == logasync::Mode::AsyncBlock)
            {
                // Set the queue size for asynchronous logging
                spdlog::init_thread_pool(logasync::kBlockQueueSize,
                                         Config::Instance()->LogAsyncThreads.value_or_default());
            }

            std::vector<spdlog::sink_ptr> sinks;

            if (Config::Instance()->LogToDebug.value_or_default())
            {
                auto debug_sink = std::make_shared<spdlog::sinks::debug_sink_mt>();
                debug_sink->set_level(spdlog::level::level_enum::trace);

#ifdef LOG_ASYNC
                debug_sink->set_pattern("%H:%M:%S.%f\t%L\t%v");
#else
                debug_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");
                // file_sink->set_pattern("[%H:%M:%S.%f] [thread %t] [%L] %v");
#endif // LOG_ASYNC

                sinks.push_back(debug_sink);
            }

            if (Config::Instance()->LogToConsole.value_or_default())
            {
                auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
                console_sink->set_level(spdlog::level::level_enum::info);
                console_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");

                sinks.push_back(console_sink);
            }

            if (Config::Instance()->LogToFile.value_or_default())
            {
                auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    Config::Instance()->LogFileName.value_or_default(), true);
                file_sink->set_level(spdlog::level::level_enum::trace);
#ifdef LOG_ASYNC
                file_sink->set_pattern("%H:%M:%S.%f\t%L\t%v");
#else
                file_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");
                // file_sink->set_pattern("[%H:%M:%S.%f] [thread %t] [%L] %v");
#endif // LOG_ASYNC

                sinks.push_back(file_sink);
            }

            auto callback_sink = std::make_shared<spdlog::sinks::callback_sink_mt>(
                [](const spdlog::details::log_msg& msg)
                {
                    if (Config::Instance()->LogToNGX.value_or_default() &&
                        State::Instance().NVNGX_Logger.LoggingCallback != nullptr &&
                        State::Instance().NVNGX_Logger.MinimumLoggingLevel != NVSDK_NGX_LOGGING_LEVEL_OFF &&
                        (State::Instance().NVNGX_Logger.MinimumLoggingLevel == NVSDK_NGX_LOGGING_LEVEL_VERBOSE ||
                         msg.level >= spdlog::level::info))
                    {
                        auto message = (char*) msg.payload.data();
                        State::Instance().NVNGX_Logger.LoggingCallback(message, NVSDK_NGX_LOGGING_LEVEL_ON,
                                                                       NVSDK_NGX_Feature_SuperSampling);
                    }
                });

            callback_sink->set_level(spdlog::level::level_enum::trace);
            callback_sink->set_pattern("[%H:%M:%S.%f] [%L] %v");

            sinks.push_back(callback_sink);

            auto* setup = new logasync::Setup(
                logasync::MakeLogger(logMode == logasync::Mode::Sync ? "multi_sink" : "multi_sink_logger", sinks,
                                     logMode, logMode == logasync::Mode::AsyncBlock ? spdlog::thread_pool() : nullptr));
            shared_logger = setup->logger;

            shared_logger->set_level((spdlog::level::level_enum) Config::Instance()->LogLevel.value_or_default());
            // Do not turn every per-frame debug line into a separate disk flush.
            // Warnings/errors still flush immediately; exit/crash paths explicitly drain.
            shared_logger->flush_on(spdlog::level::warn);

            // New lines go to the new logger first, then the old setup is parked: its worker writes its own backlog and
            // nothing here waits for it (a join on this thread was seconds on a stalled disk)
            spdlog::set_default_logger(shared_logger);

            auto* previous = g_logSetup.exchange(setup);

            std::lock_guard lock(g_retiredMutex);
            logasync::Retire(g_retiredSetups, previous);
        }
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cerr << ex.what() << std::endl;

        auto logger = spdlog::stdout_color_mt("xess");
        logger->set_pattern("[%H:%M:%S.%f] [%L] %v");
        logger->set_level((spdlog::level::level_enum) 2);
        spdlog::set_default_logger(logger);
    }
}

// Marker-wait budget only; does not bound the existing synchronous/block-policy paths
static constexpr auto kFlushWait = std::chrono::milliseconds(500);

// Set once an exit drain was attempted (not necessarily completed); ExitProcess may be called more than once
static std::atomic<bool> g_drainedForExit { false };

void CloseLogger()
{
    NoteDroppedLogLines(); // a game that never reaches a periodic report still hears of it once

    auto* setup = g_logSetup.load();

    if (setup != nullptr)
        logasync::evidence::FlushAndObserve(*setup, kFlushWait, logasync::evidence::Context::Close);
    else if (spdlog::default_logger() != nullptr)
        spdlog::default_logger()->flush();

    spdlog::shutdown();

    // The setup is left allocated on purpose, drained or not: destroying it joins its worker, and this runs under the
    // loader lock (a worker blocked on a disk would hang the unload) or at process exit (the worker is already gone).
}

void DrainLogForExit()
{
    auto* setup = g_logSetup.load();

    if (setup == nullptr || !logasync::ShouldDrainAtExit(setup->mode, g_drainedForExit.exchange(true)))
        return;

    logasync::evidence::FlushAndObserve(*setup, kFlushWait, logasync::evidence::Context::Exit);
}

void NoteDroppedLogLines()
{
    static std::mutex reportMutex;
    static logasync::DropReporter reporter;
    static logasync::Setup* reportedSetup = nullptr;

    auto* setup = g_logSetup.load();

    if (setup == nullptr || setup->mode != logasync::Mode::AsyncDropOldest || setup->pool == nullptr)
        return;

    std::unique_lock lock(reportMutex, std::try_to_lock);

    if (!lock.owns_lock())
        return;

    // Periodic normal-path samples only: spdlog accessors take its queue mutex.
    // Failure evidence never calls them, and reports sample age/UNKNOWN explicitly.
    const auto overruns = setup->pool->overrun_counter();
    if (setup->marker) setup->marker->SampleQueue(setup->pool->queue_size(), overruns);

    // A rebuilt logger counts its overruns from 0 again
    if (setup != reportedSetup)
    {
        reporter.Reset();
        reportedSetup = setup;
    }

    if (const auto dropped = reporter.Poll(overruns, Util::MillisecondsNow()); dropped > 0)
        LOG_WARN("{} log lines dropped (the log could not keep up)", dropped);
}

void NoteDroppedLogLinesOnPresent()
{
    static std::atomic<uint32_t> presents { 0 };

    if ((presents.fetch_add(1, std::memory_order_relaxed) & 255) == 255)
        NoteDroppedLogLines();
}

// Windows calls only one unhandled-exception filter, the last one set. Ours stays that one (the hook on
// SetUnhandledExceptionFilter keeps a game's later filter in g_filterChain instead of letting it replace ours), drains the
// log, writes the dump, then calls the game's filter and returns its answer.
static LONG WINAPI CrashDumpHandler(EXCEPTION_POINTERS* exceptionInfo);

using FilterFn = LPTOP_LEVEL_EXCEPTION_FILTER;
using SetFilterFn = FilterFn(WINAPI*)(FilterFn);

static logasync::FilterChain<FilterFn> g_filterChain(CrashDumpHandler);
static std::atomic<bool> g_crashHandlerInstalled { false };
// The real SetUnhandledExceptionFilter (the detour's trampoline once hooked), so our own calls are not taken for a game's
static std::atomic<SetFilterFn> g_realSetFilter { nullptr };

static FilterFn SetFilterReal(FilterFn filter)
{
    const SetFilterFn real = g_realSetFilter.load();
    return real != nullptr ? real(filter) : SetUnhandledExceptionFilter(filter);
}

static LONG WINAPI CrashDumpHandler(EXCEPTION_POINTERS* exceptionInfo)
{
    // Best effort early evidence before filesystem/spdlog/dump work. Entry does not prove fatality.
    startupav::Observe(exceptionInfo, startupav::Kind::UnhandledFilterEntry);
    logasync::FilterChain<FilterFn>::Entry entry;

    // A crash inside this handler: no second dump, the game's filter is next
    if (entry.First())
    {
        try
        {
            wchar_t dumpName[160]{};
            if (!crashdump::Name(dumpName))
                throw std::runtime_error("cannot obtain crash dump process identity");
            auto dumpPath = std::filesystem::path(Config::Instance()->LogFileName.value_or_default()).parent_path() / dumpName;

            // Best-effort logging must not prevent the actual dump attempt.
            try
            {
                spdlog::critical("Unhandled exception {:X} at {:X}, writing {}",
                                 (unsigned long) exceptionInfo->ExceptionRecord->ExceptionCode,
                                 (size_t) exceptionInfo->ExceptionRecord->ExceptionAddress, dumpPath.string());

                // Best-effort marker wait before the dump. Failure is recorded outside spdlog;
                // this does not make the existing synchronous/block-policy flush paths bounded.
                if (auto* setup = g_logSetup.load(); setup != nullptr)
                    logasync::evidence::FlushAndObserve(*setup, kFlushWait, logasync::evidence::Context::CrashBeforeDump);
                else
                    spdlog::default_logger()->flush();
            }
            catch (...) {}

            const auto written=crashdump::Write(dumpPath.c_str(), exceptionInfo);
            spdlog::critical("Crash dump UEF reached: pid={} tid={} written={} error={} bytes={} size_error={} path={} exception_parameters={} parameter0={} coverage=UEF-only",
                GetCurrentProcessId(), GetCurrentThreadId(), written.written, written.error, written.bytes,
                written.sizeError, dumpPath.string(), exceptionInfo->ExceptionRecord->NumberParameters,
                exceptionInfo->ExceptionRecord->NumberParameters?exceptionInfo->ExceptionRecord->ExceptionInformation[0]:0);
            if (auto* setup = g_logSetup.load(); setup != nullptr)
                logasync::evidence::FlushAndObserve(*setup, kFlushWait, logasync::evidence::Context::CrashAfterDump);
            else
                spdlog::default_logger()->flush();
        }
        catch (...)
        {
            // Diagnostic aid only: never let this handler itself change how the crash is reported.
        }
    }

    // The game's own filter (its dump, its message box) still runs, and its answer decides
    if (const auto next = g_filterChain.Next(); next != nullptr)
        return next(exceptionInfo);

    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashHandler()
{
    // Whatever filter was there first stays in the chain
    g_filterChain.Adopt(SetFilterReal(CrashDumpHandler));
    g_crashHandlerInstalled = true;
}

LPTOP_LEVEL_EXCEPTION_FILTER CrashFilterFromGame(LPTOP_LEVEL_EXCEPTION_FILTER filter, SetFilterFn original)
{
    // The detour is live but its original is not stored yet (a call between attach and the assignment): calling the API
    // here would come straight back
    if (original == nullptr)
        return nullptr;

    g_realSetFilter = original;

    // Ours is not in yet: InstallCrashHandler will pick this one up as the filter that was already there
    if (!g_crashHandlerInstalled)
        return SetFilterReal(filter);

    return g_filterChain.Replace(filter);
}

void CrashFilterApiHooked(LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI* original)(LPTOP_LEVEL_EXCEPTION_FILTER))
{
    g_realSetFilter = original;

    // A game may have set its filter after ours and before this hook went in: put ours back on top and keep its
    if (g_crashHandlerInstalled)
        g_filterChain.Adopt(SetFilterReal(CrashDumpHandler));
}

// Existing worker calls this outside DllMain. Failure is diagnostic-only and leaves the
// pre-existing crash filter and all input/render/provider paths unchanged.
void startupav::Prepare(HMODULE optModule) noexcept
{
    // Independent of AV evidence initialization. All file/mapping work is done here,
    // on the existing post-DllMain worker; crash/exit paths only write mapped records.
    try
    {
        const auto name = L"OptiScaler_log_drain_" + std::to_wstring(GetCurrentProcessId()) + L".bin";
        const auto path = std::filesystem::absolute(
            std::filesystem::path(Config::Instance()->LogFileName.value_or_default()).parent_path() / name);
        const DWORD error = logasync::evidence::Initialize(path.c_str());
        LOG_INFO("Log drain failure evidence init error={} path={} (out-of-band, cached queue samples)",
                 error, path.string());
    }
    catch (...) {} // diagnostic setup must not affect startup or exception handling

    try
    {
        const auto name = L"OptiScaler_startup_av_" + std::to_wstring(GetCurrentProcessId()) + L".bin";
        const auto path = std::filesystem::absolute(
            std::filesystem::path(Config::Instance()->LogFileName.value_or_default()).parent_path() / name);
        const DWORD error = Initialize(path.c_str(), optModule);
        LOG_INFO("Startup AV scoped evidence init error={} path={} (search filter only; no stack)",
                 error, path.string());
    }
    catch (...)
    {
        // Never let an optional diagnostic setup failure change ordinary initialization.
    }
}
