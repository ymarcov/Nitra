#pragma once

#include "Channel.h"
#include "ChannelFactory.h"
#include "Clock.h"
#include "FileStream.h"
#include "Poller.h"
#include "Profiler.h"
#include "Signal.h"
#include "Synchronized.h"
#include "ThreadPool.h"
#include "Throttler.h"
#include "WaitEvent.h"

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace Chili {

class Orchestrator : public std::enable_shared_from_this<Orchestrator> {
public:
    static std::shared_ptr<Orchestrator> Create(std::shared_ptr<ChannelFactory>, int threads);

    ~Orchestrator();

    std::future<void> Start();
    void Stop();

    void Add(std::shared_ptr<FileStream>);
    void ThrottleRead(Throttler);
    void ThrottleWrite(Throttler);
    void SetInactivityTimeout(std::chrono::milliseconds);

    Signal<> OnStop;

private:
    class Task {
    public:
        void MarkHandlingInProcess(bool);
        bool IsHandlingInProcess() const;
        void Activate();
        bool ReachedInactivityTimeout() const;
        Channel& GetChannel();
        std::mutex& GetMutex();

    private:
        Orchestrator* _orchestrator;
        std::shared_ptr<Channel> _channel;
        Synchronized<Clock::TimePoint> _lastActive;
        std::mutex _mutex;
        std::atomic_bool _inProcess{false};

        friend void Orchestrator::Add(std::shared_ptr<FileStream>);
    };

    Orchestrator(std::shared_ptr<ChannelFactory>, int threads);

    template <class T>
    void RecordChannelEvent(const Channel&) const;

    void WakeUp();
    void OnEvent(std::shared_ptr<FileStream>, int events);
    void HandleChannelEvent(Channel&, int events);
    void IterateOnce();
    std::vector<std::shared_ptr<Task>> CaptureTasks();
    std::vector<std::shared_ptr<Task>> FilterReadyTasks();
    void InternalStop();
    void InternalForceStopOnError();
    bool AtLeastOneTaskIsReady();
    bool IsTaskReady(Task&);
    Clock::TimePoint GetLatestAllowedWakeup();
    void CollectGarbage();

    std::shared_ptr<ChannelFactory> _channelFactory;
    std::promise<void> _threadPromise;
    Poller _poller;
    ThreadPool _activationThreadPool;
    std::future<void> _pollerTask;
    std::shared_ptr<Throttler> _masterReadThrottler;
    std::shared_ptr<Throttler> _masterWriteThrottler;
    std::thread _thread;
    WaitEvent _newEvent;
    std::atomic_bool _stop{true};
    std::mutex _mutex;
    std::map<void*, std::weak_ptr<Task>> _taskFastLookup;
    std::vector<std::shared_ptr<Task>> _tasks;
    std::atomic<std::chrono::milliseconds> _inactivityTimeout{std::chrono::milliseconds(10000)};

    friend class Channel;
};

template <class T>
void Orchestrator::RecordChannelEvent(const Channel& c) const {
    Profiler::Record<T>("Orchestrator", c.GetId());
}

/**
 * Profiling
 */

class OrchestratorEvent : public ProfileEvent {
public:
    std::string GetSource() const override;
    std::string GetSummary() const override;
    void Accept(ProfileEventReader&) const override;
};

class OrchestratorWokeUp : public OrchestratorEvent {
public:
    using OrchestratorEvent::OrchestratorEvent;
    void Accept(ProfileEventReader&) const override;
};

class OrchestratorWaiting : public OrchestratorEvent {
public:
    using OrchestratorEvent::OrchestratorEvent;
    void Accept(ProfileEventReader&) const override;
};

class OrchestratorSignalled : public OrchestratorEvent {
public:
    using OrchestratorEvent::OrchestratorEvent;
    void Accept(ProfileEventReader&) const override;
};

class OrchestratorCapturingTasks : public OrchestratorEvent {
public:
    using OrchestratorEvent::OrchestratorEvent;
    void Accept(ProfileEventReader&) const override;
};

} // namespace Chili

