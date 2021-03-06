#include "Orchestrator.h"
#include "ExitTrap.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <iterator>

using namespace std::literals;

namespace Chili {

void Orchestrator::Task::MarkHandlingInProcess(bool b) {
    _inProcess = b;
}

bool Orchestrator::Task::IsHandlingInProcess() const {
    return _inProcess;
}

void Orchestrator::Task::Activate() {
    _orchestrator->RecordChannelEvent<ChannelActivating>(*_channel);

    auto onExit = CreateExitTrap([&] {
        _orchestrator->RecordChannelEvent<ChannelActivated>(*_channel);
    });

    if (ReachedInactivityTimeout()) {
        Log::Info("Channel {} reached inactivity timeout", _channel->GetId());

        // If it happened while it was in the poller, then remove
        // it from there as well. Otherwise, this call should be
        // okay with us trying to remove a non-existent channel.
        _orchestrator->_poller.Remove(_channel->GetStream());

        _channel->Close();
        _inProcess = false;
        _orchestrator->WakeUp();
        return;
    }

    // Money line
    _channel->Advance();

    _lastActive.Set(Clock::GetCurrentTime());

    // In case we're sending it off to the poller,
    // we don't need to notify our main thread,
    // because the task won't be ready until it
    // comes back from the poller with an event.
    bool notify = false;

    switch (_channel->GetDefiniteStage()) {
        case Channel::Stage::WaitReadable: {
            _orchestrator->_poller.Poll(_channel->GetStream(),
                                        Poller::Events::Completion | Poller::Events::Readable);
        } break;

        case Channel::Stage::WaitWritable: {
            _orchestrator->_poller.Poll(_channel->GetStream(),
                                        Poller::Events::Completion | Poller::Events::Writable);
        } break;

        default:
            // Ok, it's not going to the poller.
            // It's ready for its next stage already.
            // Therefore, wake up our main thread
            // so that it could schedule its next
            // stage whenever it sees fit.
            //
            // If we got a throttling timeout, then
            // we will still wake up the main thread
            // in order to recalculate the new timeout.
            notify = true;
            break;
    }

    // Now it makes sense to be rescheduled again,
    // either immediately, or when we come back
    // from the poller with an event.
    _inProcess = false;

    if (notify) {
        // Wake up our main thread so that our
        // next stage can be scheduled.
        _orchestrator->WakeUp();
    }
}

bool Orchestrator::Task::ReachedInactivityTimeout() const {
    if (!_channel->IsWaitingForClient()) {
        // can't blame the client, we just
        // haven't got to handling it yet
        return false;
    }

    auto diff = Clock::GetCurrentTime() - _lastActive.GetCopy();
    return diff >= _orchestrator->_inactivityTimeout.load();
}

Channel& Orchestrator::Task::GetChannel() {
    return *_channel;
}

std::mutex& Orchestrator::Task::GetMutex() {
    return _mutex;
}

std::shared_ptr<Orchestrator> Orchestrator::Create(std::shared_ptr<ChannelFactory> f, int threads) {
    return std::shared_ptr<Orchestrator>(new Orchestrator(std::move(f), threads));
}

Orchestrator::Orchestrator(std::shared_ptr<ChannelFactory> channelFactory, int threads) :
    _channelFactory(std::move(channelFactory)),
    _poller(8),
    _activationThreadPool(threads),
    _masterReadThrottler(std::make_shared<Throttler>()),
    _masterWriteThrottler(std::make_shared<Throttler>()) {
    _poller.OnStop += [this] {
        _stop = true;
        WakeUp();
    };
}

Orchestrator::~Orchestrator() {
    Stop();
}

std::future<void> Orchestrator::Start() {
    _stop = false;
    _threadPromise = std::promise<void>();

    _thread = std::thread([this] {
        try {
            while (!_stop)
                IterateOnce();

            InternalStop();
        } catch (...) {
            InternalForceStopOnError();
        }
    });

    _pollerTask = _poller.Start([this](std::shared_ptr<FileStream> fs, int events) {
        return OnEvent(std::move(fs), events);
    });

    return _threadPromise.get_future();
}

void Orchestrator::InternalStop() {
    try {
        _poller.Stop();
        _activationThreadPool.Stop();
        OnStop();
        _pollerTask.get();
        _threadPromise.set_value();
    } catch (...) {
        _threadPromise.set_exception(std::current_exception());
    }
}

void Orchestrator::InternalForceStopOnError() {
    Log::Error("Orchestrator stopped due to error!");

    try {
        _stop = true;
        _poller.Stop();
        _activationThreadPool.Stop();
        OnStop();
        _pollerTask.get();
        _threadPromise.set_exception(std::current_exception());
    } catch (...) {
        _threadPromise.set_exception(std::current_exception());
    }
}

void Orchestrator::Stop() {
    std::unique_lock<std::mutex> lock(_mutex);
    _stop = true;
    lock.unlock();
    WakeUp();

    if (_thread.joinable())
        _thread.join();
}

void Orchestrator::Add(std::shared_ptr<FileStream> stream) {
    auto task = std::make_shared<Task>();

    task->_orchestrator = this;
    task->_channel = _channelFactory->CreateChannel(std::move(stream));
    task->_channel->Initialize(shared_from_this());
    task->_channel->_throttlers.Read.Master = _masterReadThrottler;
    task->_channel->_throttlers.Write.Master = _masterWriteThrottler;
    task->_lastActive.Set(Clock::GetCurrentTime());

    {
        std::lock_guard lock(_mutex);
        _tasks.push_back(task);
        _taskFastLookup[task->GetChannel().GetStream().get()] = task;
    }

    _poller.Poll(task->GetChannel().GetStream(), Poller::Events::Completion | Poller::Events::Readable);
}

void Orchestrator::ThrottleRead(Throttler t) {
    *_masterReadThrottler = std::move(t);
}

void Orchestrator::ThrottleWrite(Throttler t) {
    *_masterWriteThrottler = std::move(t);
}

void Orchestrator::SetInactivityTimeout(std::chrono::milliseconds ms) {
    _inactivityTimeout = ms;
}

void Orchestrator::WakeUp() {
    _newEvent.Signal();
    Profiler::Record<OrchestratorSignalled>();
}

void Orchestrator::OnEvent(std::shared_ptr<FileStream> fs, int events) {
    // Lock the shared task list state, because we're
    // going to try to find the relevant task for the
    // triggered file stream in it.
    std::unique_lock<std::mutex> lock(_mutex);

    auto it = _taskFastLookup.find(fs.get());

    if (it == end(_taskFastLookup))
        return;

    auto task = it->second.lock();

    // Got our task. We can release the lock.
    lock.unlock();

    auto& channel = task->GetChannel();

    if (events & Poller::Events::Completion) {
        // No use talking to a wall. Even if we had other events,
        // no one's going to be listening to our replies.
        RecordChannelEvent<ChannelCompleted>(channel);
        Log::Verbose("Channel {} received completion event", channel.GetId());
        channel.Close();
    } else {
        std::lock_guard taskLock(task->GetMutex());
        HandleChannelEvent(channel, events);
    }

    // Either way, we need to react to what just happened,
    // either by garbage-collection or by advancing the
    // relevant task's state-machine. So we need to wake
    // our main thread up to do the work.
    WakeUp();
}

void Orchestrator::HandleChannelEvent(Channel& channel, int events) {
    switch (channel.GetDefiniteStage()) {
        case Channel::Stage::WaitReadable: {
            if (events & Poller::Events::Readable) {
                RecordChannelEvent<ChannelReadable>(channel);
                Log::Verbose("Channel {} became readable", channel.GetId());
                channel.SetStage(Channel::Stage::Read);
            } else {
                Log::Error("Channel {} was waiting for readbility but got different "
                                      "event. Check poll logic!", channel.GetId());
            }
        } break;

        case Channel::Stage::WaitWritable: {
            if (events & Poller::Events::Writable) {
                RecordChannelEvent<ChannelWritable>(channel);
                Log::Verbose("Channel {} became writable", channel.GetId());
                channel.SetStage(Channel::Stage::Write);
            } else {
                Log::Error("Channel {} was waiting for writability but got different "
                                      "event. Check poll logic!", channel.GetId());
            }
        } break;

        case Channel::Stage::Closed: {
            // One reason this can happen is if the channel has reached an
            // inactivity timeout after the event was dispatched but
            // before it was processed.
            Log::Verbose("Ignoring event on already closed channel {}", channel.GetId());
            return;
         }

        default: {
            // The client is not supposed to be in the poller
            // if it wasn't waiting for anything... This must
            // be caused by a programming error.
            Log::Error("Channel {} was not in a waiting stage but received an event. "
                                  "Check poll logic!", channel.GetId());
            channel.Close();
            return;
         }
    }
}

void Orchestrator::IterateOnce() {
    for (auto& task : CaptureTasks()) {
        // Exit ASAP if server needs to stop,
        // don't wait for the next call.
        if (_stop)
            break;

        // Mark it ias being handled right here so that we
        // don't need to wait for the thread to get to it.
        // This way, the next call of CaptureTasks()
        // will filter this task out for us.
        task->MarkHandlingInProcess(true);

        _activationThreadPool.Post([=] {
            std::lock_guard lock(task->GetMutex());
            task->Activate();
        });
    }
}

std::vector<std::shared_ptr<Orchestrator::Task>> Orchestrator::CaptureTasks() {
    // We capture ready tasks into a new vector so that
    // the original vector would be released, in terms
    // of locks and mutexes, and new tasks could be
    // added to it even while we're processing tasks
    // that are already ready to be processed.
    // This way, we don't take the lock for too long.
    std::unique_lock<std::mutex> lock(_mutex);

    Clock::TimePoint timeout;

    Profiler::Record<OrchestratorCapturingTasks>();

    while ((timeout = GetLatestAllowedWakeup()) > Clock::GetCurrentTime()) {
        lock.unlock();
        Profiler::Record<OrchestratorWaiting>();
        _newEvent.WaitUntilAndReset(timeout);
        lock.lock();
        Profiler::Record<OrchestratorWokeUp>();

        // Should the server stop?
        if (_stop || AtLeastOneTaskIsReady())
            break;
    }

    CollectGarbage();

    return FilterReadyTasks();
}

std::vector<std::shared_ptr<Orchestrator::Task>> Orchestrator::FilterReadyTasks() {
    auto snapshot = std::vector<std::shared_ptr<Orchestrator::Task>>();
    snapshot.reserve(_tasks.size());

    std::copy_if(begin(_tasks), end(_tasks), back_inserter(snapshot), [this](auto& t) {
        return this->IsTaskReady(*t);
    });

    return snapshot;
}

bool Orchestrator::AtLeastOneTaskIsReady() {
    return end(_tasks) != std::find_if(begin(_tasks), end(_tasks), [this](auto& t) {
        return this->IsTaskReady(*t);
    });
}

bool Orchestrator::IsTaskReady(Task& t) {
    // Although the task is in our list,
    // it is in fact currently being
    // processed by some thread. So
    // we don't need to do anything
    // extra about it for now.
    if (t.IsHandlingInProcess())
        return false;

    // If the task has reached its inactivity
    // timeout, it has to close itself, by itself.
    if (t.ReachedInactivityTimeout())
        return true;

    // Finally, is it ready for some
    // actual happy-path processing?
    return t.GetChannel().IsReady();
}

Clock::TimePoint Orchestrator::GetLatestAllowedWakeup() {
    // Our latest possible timeout (i.e. default)
    // if nothing else is requested, is in fact
    // our inactivity timeout, when we check
    // if any channels have remained inactive
    // for too long, in which case we close them.
    auto now = Clock::GetCurrentTime();
    auto timeout = now + _inactivityTimeout.load();

    for (auto& t : _tasks) {
        auto& channel = t->GetChannel();
        auto&& requestedTimeout = channel.GetRequestedTimeout();

        if ((requestedTimeout >= now) && (requestedTimeout < timeout)) {
            // This client has requested an earlier timeout than
            // the one we were going to use. In order that we can
            // respond to its event as quickly as possible, we'll
            // take its request.
            timeout = requestedTimeout;
        }
    }

    return timeout;
}

void Orchestrator::CollectGarbage() {
    auto garbageIterator = std::stable_partition(begin(_tasks), end(_tasks), [](auto& t) {
        return t->GetChannel().GetTentativeStage() != Channel::Stage::Closed;
    });

    for (auto i = garbageIterator, e = end(_tasks); i != e; ++i)
        _taskFastLookup.erase((*i)->GetChannel().GetStream().get());

    _tasks.erase(garbageIterator, end(_tasks));
}

std::string OrchestratorEvent::GetSource() const {
    return "Orchestrator";
}

std::string OrchestratorEvent::GetSummary() const {
    return "Event on Orchestrator";
}

void OrchestratorEvent::Accept(ProfileEventReader& reader) const {
    reader.Read(*this);
}

void OrchestratorWokeUp::Accept(ProfileEventReader& reader) const {
    reader.Read(*this);
}

void OrchestratorWaiting::Accept(ProfileEventReader& reader) const {
    reader.Read(*this);
}

void OrchestratorSignalled::Accept(ProfileEventReader& reader) const {
    reader.Read(*this);
}

void OrchestratorCapturingTasks::Accept(ProfileEventReader& reader) const {
    reader.Read(*this);
}

} // namespace Chili

