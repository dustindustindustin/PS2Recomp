#ifndef PS2_LOG_H
#define PS2_LOG_H

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef PS2_RUNTIME_LOGS
#define PS2_RUNTIME_LOGS 0
#endif

#ifndef AGRESSIVE_LOGS
#define AGRESSIVE_LOGS 0
#endif

#define RUNTIME_ERROR(x)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        std::ostringstream _ps2_runtime_error_stream;                                                                  \
        _ps2_runtime_error_stream << x;                                                                                \
        const std::string _ps2_runtime_error_text =                                                                    \
            _ps2_runtime_error_stream.str();                                                                           \
                                                                                                                       \
        std::cerr << _ps2_runtime_error_text;                                                                           \
        ps2_log::append_runtime_log_text(_ps2_runtime_error_text);                                                      \
        ps2_log::record_diagnostic_event("error", _ps2_runtime_error_text);                                            \
        ps2_log::dump_diagnostic_events();                                                                              \
    } while (0)

namespace ps2_log
{
struct DiagnosticEvent
{
    uint64_t sequence = 0;
    uint64_t elapsedMicros = 0;
    char category[24]{};
    char text[232]{};
};

inline constexpr size_t kDiagnosticEventCapacity = 2048;

inline std::mutex &diagnostic_event_mutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::array<DiagnosticEvent, kDiagnosticEventCapacity> &diagnostic_events()
{
    static std::array<DiagnosticEvent, kDiagnosticEventCapacity> events{};
    return events;
}

inline uint64_t &diagnostic_event_count()
{
    static uint64_t count = 0;
    return count;
}

inline const std::chrono::steady_clock::time_point &diagnostic_start_time()
{
    static const auto start = std::chrono::steady_clock::now();
    return start;
}

inline std::string diagnostic_dump_path()
{
    if (const char *configured = std::getenv("PS2X_DIAGNOSTIC_DUMP");
        configured != nullptr && configured[0] != '\0')
    {
        return configured;
    }
    return (std::filesystem::current_path() / "ps2_diagnostic_events.log").string();
}

inline void record_diagnostic_event(const char *category, const std::string &text)
{
    std::lock_guard<std::mutex> lock(diagnostic_event_mutex());
    const uint64_t sequence = diagnostic_event_count()++;
    DiagnosticEvent &event = diagnostic_events()[sequence % kDiagnosticEventCapacity];
    event = {};
    event.sequence = sequence;
    event.elapsedMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - diagnostic_start_time()).count());
    std::strncpy(event.category, category != nullptr ? category : "event",
                 sizeof(event.category) - 1);
    std::strncpy(event.text, text.c_str(), sizeof(event.text) - 1);
}

inline bool dump_diagnostic_events(const std::string &path = {})
{
    std::lock_guard<std::mutex> lock(diagnostic_event_mutex());
    std::ofstream output(path.empty() ? diagnostic_dump_path() : path,
                         std::ios::out | std::ios::trunc);
    if (!output.is_open())
    {
        return false;
    }

    const uint64_t count = diagnostic_event_count();
    const uint64_t first = count > kDiagnosticEventCapacity
        ? count - kDiagnosticEventCapacity : 0;
    for (uint64_t sequence = first; sequence < count; ++sequence)
    {
        const DiagnosticEvent &event =
            diagnostic_events()[sequence % kDiagnosticEventCapacity];
        if (event.sequence != sequence)
        {
            continue;
        }
        output << event.sequence << ' ' << event.elapsedMicros << "us ["
               << event.category << "] " << event.text;
        if (event.text[0] != '\0' && event.text[std::strlen(event.text) - 1] != '\n')
        {
            output << '\n';
        }
    }
    return output.good();
}

struct RuntimeLogEntry
{
    uint64_t seq = 0;
    std::string text;
};

inline constexpr size_t kMaxRuntimeLogEntries = 4096;

inline std::mutex &runtime_log_mutex()
{
    static std::mutex m;
    return m;
}

inline std::deque<RuntimeLogEntry> &runtime_log_entries()
{
    static std::deque<RuntimeLogEntry> entries;
    return entries;
}

inline uint64_t &runtime_log_next_seq()
{
    static uint64_t seq = 1;
    return seq;
}

inline bool &runtime_log_paused()
{
    static bool paused = false;
    return paused;
}

inline void set_runtime_log_paused(bool paused)
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    runtime_log_paused() = paused;
}

inline bool is_runtime_log_paused()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    return runtime_log_paused();
}

inline void append_runtime_log_text(const std::string &text)
{
    if (text.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    if (runtime_log_paused())
    {
        return;
    }

    auto &entries = runtime_log_entries();
    RuntimeLogEntry entry{};
    entry.seq = runtime_log_next_seq()++;
    entry.text = text;
    entries.push_back(std::move(entry));

    while (entries.size() > kMaxRuntimeLogEntries)
    {
        entries.pop_front();
    }
}

inline std::vector<RuntimeLogEntry> snapshot_runtime_log_entries()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    const auto &entries = runtime_log_entries();
    return std::vector<RuntimeLogEntry>(entries.begin(), entries.end());
}

inline void clear_runtime_log_entries()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    runtime_log_entries().clear();
}
}

#if PS2_RUNTIME_LOGS || AGRESSIVE_LOGS
#define RUNTIME_LOG(x)                                                                                                  \
    do                                                                                                                  \
    {                                                                                                                   \
        std::ostringstream _ps2_runtime_log_stream;                                                                     \
        _ps2_runtime_log_stream << x;                                                                                   \
        const std::string _ps2_runtime_log_text = _ps2_runtime_log_stream.str();                                        \
        if (_ps2_runtime_log_text.empty())                                                                            \
        {                                                                                                               \
            std::cout.flush();                                                                                          \
        }                                                                                                               \
        else                                                                                                            \
        {                                                                                                               \
            std::cout << _ps2_runtime_log_text;                                                                         \
            ps2_log::append_runtime_log_text(_ps2_runtime_log_text);                                                    \
        }                                                                                                               \
    } while (0)
#else
#define RUNTIME_LOG(x) do {} while(0)
#endif

#if AGRESSIVE_LOGS

namespace ps2_log
{
inline std::string log_path()
{
    static const std::string path =
        (std::filesystem::current_path() / "ps2_log.txt").string();
    return path;
}
inline std::ostream &log_stream()
{
    static std::ofstream f(log_path(), std::ios::out);
    return f.is_open() ? f : std::cerr;
}
inline int &depth()
{
    static thread_local int d = 0;
    return d;
}
inline std::mutex &log_mutex()
{
    static std::mutex mutex;
    return mutex;
}
inline bool log_entry(const char *name)
{
    static const uint64_t limit = [] {
        const char *value = std::getenv("PS2X_FUNCTION_TRACE_LIMIT");
        if (value == nullptr || value[0] == '\0')
            return uint64_t{100000};
        return static_cast<uint64_t>(std::strtoull(value, nullptr, 10));
    }();
    static const std::string filter = [] {
        const char *value = std::getenv("PS2X_FUNCTION_TRACE_FILTER");
        return value != nullptr ? std::string(value) : std::string{};
    }();
    static std::atomic<uint64_t> emitted{0};

    if ((!filter.empty() && std::strstr(name, filter.c_str()) == nullptr) ||
        emitted.fetch_add(2, std::memory_order_relaxed) >= limit)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(log_mutex());
    for (int i = 0; i < depth(); ++i)
        log_stream() << '\t';
    log_stream() << ">> " << name << " enter\n";
    depth()++;
    return true;
}
inline void log_exit(const char *name)
{
    std::lock_guard<std::mutex> lock(log_mutex());
    depth()--;
    for (int i = 0; i < depth(); ++i)
        log_stream() << '\t';
    log_stream() << "<< " << name << " exit\n";
}
inline void print_saved_location()
{
    std::cout << "[PS2 LOG] Logs saved at " << log_path() << std::endl;
}
}

#define PS_LOG_ENTRY(name) \
    struct _ps2_log_guard_ { const char *_n; bool _active; \
        _ps2_log_guard_(const char *n) : _n(n), _active(ps2_log::log_entry(n)) {} \
        ~_ps2_log_guard_() { if (_active) ps2_log::log_exit(_n); } } _ps2_log_guard_(name)
#define PS2_IF_AGRESSIVE_LOGS(code) \
    do                              \
    {                               \
        code;                       \
    } while (0)

#else

namespace ps2_log
{
inline std::string log_path()
{
    return (std::filesystem::current_path() / "ps2_log.txt").string();
}
inline void print_saved_location() {}
}
#define PS_LOG_ENTRY(name) ((void)0)
#define PS2_IF_AGRESSIVE_LOGS(code) ((void)0)

#endif

#endif
