#pragma once

#include "Poller.h"
#include "Request.h"
#include "Response.h"
#include "Signal.h"
#include "Throttler.h"

#include <mutex>

namespace Nitra {

class AbstractChannel {
public:
    enum class Stage {
        WaitReadable,
        ReadTimeout,
        Read,
        Process,
        WaitWritable,
        WriteTimeout,
        Write,
        Closed
    };

    struct Throttlers {
        struct Group {
            Throttler Dedicated;
            std::shared_ptr<Throttler> Master;
        } Read, Write;
    };

    enum class Control {
        FetchContent,
        RejectContent,
        SendResponse
    };

    AbstractChannel(std::shared_ptr<FileStream>);
    virtual ~AbstractChannel();

protected:
    virtual Control Process() = 0;

private:
    struct ThrottlingInfo {
        std::size_t currentQuota;
        std::size_t capacity;
        bool full;
        std::chrono::time_point<Throttler::Clock> fillTime;
    };

    ThrottlingInfo GetThrottlingInfo(const Throttlers::Group&) const;

    /**
     * Take the next step in the state machine.
     */
    void Advance();

    /**
     * Gets and sets the stage the channel is currently in.
     */
    Stage GetStage() const;
    void SetStage(Stage);

    /**
     * If the channel is in a waiting stage, gets the timeout
     * to wait for before performing another stage, even
     * if data is already available.
     */
    std::chrono::time_point<std::chrono::steady_clock> GetRequestedTimeout() const;

    /**
     * Gets whether the channel is ready to perform its stage.
     */
    bool IsReady() const;

    /**
     * Gets whether the channel cannot make progress
     * until the client does something.
     */
    bool IsWaitingForClient() const;

    void OnRead();
    void OnProcess();
    void OnWrite();
    void Close();

    const std::shared_ptr<FileStream>& GetStream() const;
    int GetId() const;

    bool FetchData(bool(Request::*)(std::size_t, std::size_t&), std::size_t maxRead);
    void LogNewRequest();
    void SendInternalError();
    void HandleControlDirective(Control);
    bool FlushData(std::size_t maxWrite);

    std::uint64_t _id;
    std::shared_ptr<FileStream> _stream;
    Throttlers _throttlers;
    Request _request;
    Response _response;
    std::atomic<std::chrono::time_point<std::chrono::steady_clock>> _timeout;
    std::atomic<Stage> _stage;
    bool _forceClose = false;
    bool _fetchingContent = false;
    bool _autoFetchContent = true;

    friend class Channel;
    friend class Orchestrator;
};

} // namespace Nitra
