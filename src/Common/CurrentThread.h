#pragma once

#include <Interpreters/Context_fwd.h>
#include <Common/ThreadStatus.h>
#include <Common/Scheduler/ResourceLink.h>

#include <memory>
#include <string_view>


namespace ProfileEvents
{
class Counters;
}

class MemoryTracker;


namespace DB
{

class QueryStatus;
struct Progress;
class InternalTextLogsQueue;

/** Collection of static methods to work with thread-local objects.
  * Allows to attach and detach query/process (thread group) to a thread
  * (to calculate query-related metrics and to allow to obtain query-related data from a thread).
  * Thread will propagate it's metrics to attached query.
  */
class CurrentThread
{
public:
    /// Return true in case of successful initialization
    static bool isInitialized();

    /// Handler to current thread
    static ThreadStatus & get();

    /// Group to which belongs current thread
    static ThreadGroupPtr getGroup();

    /// MemoryTracker for user that owns current thread if any
    static MemoryTracker * getUserMemoryTracker();

    /// Adjust counters in MemoryTracker hierarchy if untracked_memory is not 0.
    static void flushUntrackedMemory();

    /// A logs queue used by TCPHandler to pass logs to a client
    static void attachInternalTextLogsQueue(const std::shared_ptr<InternalTextLogsQueue> & logs_queue,
                                            LogsLevel client_logs_level);
    static std::shared_ptr<InternalTextLogsQueue> getInternalTextLogsQueue();

    static void attachInternalProfileEventsQueue(const InternalProfileEventsQueuePtr & queue);
    static InternalProfileEventsQueuePtr getInternalProfileEventsQueue();

    static void attachQueryForLog(const String & query_);

    /// Makes system calls to update ProfileEvents that contain info from rusage and taskstats
    static void updatePerformanceCounters();
    static void updatePerformanceCountersIfNeeded();

    static ProfileEvents::Counters & getProfileEvents();
    inline ALWAYS_INLINE static MemoryTracker * getMemoryTracker()
    {
        if (!current_thread) [[unlikely]]
            return nullptr;
        return &current_thread->memory_tracker;
    }

    /// Update read and write rows (bytes) statistics (used in system.query_thread_log)
    static void updateProgressIn(const Progress & value);
    static void updateProgressOut(const Progress & value);

    /// You must call one of these methods when create a query child thread:
    /// Add current thread to a group associated with the thread group
    static void attachToGroup(const ThreadGroupPtr & thread_group);
    /// Is useful for a ThreadPool tasks
    static void attachToGroupIfDetached(const ThreadGroupPtr & thread_group);

    /// Non-master threads call this method in destructor automatically
    static void detachFromGroupIfNotDetached();

    /// Update ProfileEvents and dumps info to system.query_thread_log
    static void finalizePerformanceCounters();

    /// Returns a non-empty string if the thread is attached to a query

    /// Returns attached query context
    static ContextPtr getQueryContext();

    static std::string_view getQueryId();

    // For IO Scheduling
    static void attachReadResource(ResourceLink link);
    static void detachReadResource();
    static ResourceLink getReadResourceLink();
    static void attachWriteResource(ResourceLink link);
    static void detachWriteResource();
    static ResourceLink getWriteResourceLink();

    // For IO Throttling
    static void attachReadThrottler(const ThrottlerPtr & throttler);
    static void detachReadThrottler();
    static ThrottlerPtr getReadThrottler();
    static void attachWriteThrottler(const ThrottlerPtr & throttler);
    static void detachWriteThrottler();
    static ThrottlerPtr getWriteThrottler();

    /// Initializes query with current thread as master thread in constructor, and detaches it in destructor
    struct QueryScope : private boost::noncopyable
    {
        explicit QueryScope(ContextMutablePtr query_context, std::function<void()> fatal_error_callback = {});
        explicit QueryScope(ContextPtr query_context, std::function<void()> fatal_error_callback = {});
        ~QueryScope();

        void logPeakMemoryUsage();
        bool log_peak_memory_usage_in_destructor = true;
    };

    /// Scoped attach/detach of IO resource links
    struct IOSchedulingScope : private boost::noncopyable
    {
        IOSchedulingScope(ResourceLink read_resource_link, ResourceLink write_resource_link)
        {
            readResource(read_resource_link);
            writeResource(write_resource_link);
        }

        explicit IOSchedulingScope(const IOSchedulingSettings & settings)
            : IOSchedulingScope(settings.read_resource_link, settings.write_resource_link)
        {}

        ~IOSchedulingScope()
        {
            if (read_resource_attached)
                detachReadResource();
            if (write_resource_attached)
                detachWriteResource();
        }

    private:
        void readResource(ResourceLink link)
        {
            if (link)
            {
                attachReadResource(link);
                read_resource_attached = true;
            }
        }

        void writeResource(ResourceLink link)
        {
            if (link)
            {
                attachWriteResource(link);
                write_resource_attached = true;
            }
        }

        bool read_resource_attached = false;
        bool write_resource_attached = false;
    };

    /// Scoped attach/detach of read throttler
    struct ReadThrottlingScope : private boost::noncopyable
    {
        explicit ReadThrottlingScope(const ThrottlerPtr & read_throttler_)
        {
            if (read_throttler_)
            {
                attachReadThrottler(read_throttler_);
                read_throttler_attached = true;
            }
        }

        ~ReadThrottlingScope()
        {
            if (read_throttler_attached)
                detachReadThrottler();
        }

    private:
        bool read_throttler_attached = false;
    };

    /// Scoped attach/detach of write throttler
    struct WriteThrottlingScope : private boost::noncopyable
    {
        explicit WriteThrottlingScope(const ThrottlerPtr & write_throttler_)
        {
            if (write_throttler_)
            {
                attachWriteThrottler(write_throttler_);
                write_throttler_attached = true;
            }
        }

        ~WriteThrottlingScope()
        {
            if (write_throttler_attached)
                detachWriteThrottler();
        }

    private:
        bool write_throttler_attached = false;
    };

};

}
