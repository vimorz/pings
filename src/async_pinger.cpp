#include <fmt/core.h>
#include <fmt/color.h>
#include <fmt/chrono.h>

#include <pings/async_pinger.hpp>

#include <batch_state.hpp>
#include <icmp_codec.hpp>
#include <icmpv6_codec.hpp>
#include <ipv4_parser.hpp>
#include <network_bytes.hpp>
#include <ping_session.hpp>
#include <ping_state.hpp>
#include <repeat_state.hpp>
#include <repeat_statistics.hpp>
#include <reply_deadline.hpp>

#include <utils/debug_log.hpp>
#include <utils/encoding.hpp>

#ifdef _WIN32
#include <windows.h>
#define GET_PROCESS_ID() ::GetCurrentProcessId()
#elif defined(__linux__) || defined(__unix__)
#include <unistd.h>
#define GET_PROCESS_ID() ::getpid()
#elif defined(__APPLE__)
#include <unistd.h>
#define GET_PROCESS_ID() ::getpid()
#else
#error "Unsupported platform"
#endif

using namespace utils;

namespace
{
#ifdef DEBUG_ENABLED
    std::string packetToHexString(const std::vector<std::uint8_t> &packet, size_t maxBytes)
    {
        std::string result;
        size_t count = std::min<size_t>(maxBytes, packet.size());
        result += fmt::format("первые {} байт: ", maxBytes);
        for (size_t i = 0; i < count; ++i)
        {
            result += fmt::format("{:02x} ", packet[i]);
        }
        if (!result.empty())
            result.pop_back();
        return result;
    }
#endif
}

namespace pings
{
    AsyncPinger::AsyncPinger()
        : socket_(ioContext_),
          socketV6_(ioContext_),
          identifier_(
              static_cast<std::uint16_t>(
                  GET_PROCESS_ID() & 0xFFFF)),
          state_(detail::PingerState::Idle)
    {
    }

    AsyncPinger::~AsyncPinger()
    {
        DEBUG_WARN("AsyncPinger destructor");

        boost::system::error_code ec;
        socket_.close(ec);

        socketV6_.close(ec);
    }

    AsyncPinger::Ptr AsyncPinger::create()
    {
        auto pinger = std::shared_ptr<AsyncPinger>(new AsyncPinger());

        DEBUG_INFO("Pinger state: Idle");

        return pinger;
    }

    PingHandle AsyncPinger::pingAsync(
        const std::string &host,
        PingCallback callback)
    {
        return pingAsync(
            host,
            PingOptions{},
            std::move(callback));
    }

    PingHandle AsyncPinger::pingAsync(const std::string &host, const PingOptions &options, PingCallback callback)
    {
        DEBUG_INFO("pingAsync, Pinger state: {}", toString(state_));

        ensureAcceptingWork();

        if (!isValid(options))
        {
            postResult(host, PingStatus::InvalidOptions, std::move(callback));

            return PingHandle{};
        }

        bool registered = false;
        std::shared_ptr<detail::PingSession> session;

        try
        {
            session = std::make_shared<detail::PingSession>();
            session->host = host;
            session->options = options;
            session->sequenceNumber = allocateSequence();

            session->timeoutTimer = std::make_unique<boost::asio::steady_timer>(ioContext_);

            // Register the session before queuing work that may complete it.
            sessions_[session->sequenceNumber] = session;
            registered = true;

            session->callback = std::move(callback);

            // Defer initiation so callbacks cannot run inline from pingAsync().
            // Calls and run() must follow the single-thread usage contract.
            boost::asio::post(
                ioContext_,
                [self = shared_from_this(), session]()
                {
                    if (session->completed)
                    {
                        return;
                    }

                    try
                    {
                        self->startResolve(session);
                    }
                    catch ([[maybe_unused]] const std::exception &e)
                    {
                        DEBUG_ERROR("Exception while starting ping: {}", e.what());

                        self->completeSession(
                            session->sequenceNumber,
                            PingStatus::InternalError,
                            std::chrono::milliseconds{0});
                    }
                    catch (...)
                    {
                        DEBUG_ERROR("Unknown exception while starting ping");

                        self->completeSession(
                            session->sequenceNumber,
                            PingStatus::InternalError,
                            std::chrono::milliseconds{0});
                    }
                });

            // The handle identifies this session; network work is still queued.
            return PingHandle{
                weak_from_this(),
                session};
        }
        catch ([[maybe_unused]] const std::exception &e)
        {
            DEBUG_ERROR("Exception in pingAsync: {}", e.what());

            if (registered)
            {
                const std::uint16_t sequence = session->sequenceNumber;

                boost::asio::post(
                    ioContext_,
                    [self = shared_from_this(), sequence]()
                    {
                        self->completeSession(sequence, PingStatus::InternalError, std::chrono::milliseconds{0});
                    });
            }
            else
            {
                postResult(
                    host,
                    PingStatus::InternalError,
                    std::move(callback));
            }

            return PingHandle{};
        }
    }

    // Propagate uncaught handler exceptions and reject new work until run() resumes.
    // Resuming processes remaining handlers; it does not retry the throwing handler.
    void AsyncPinger::run()
    {
        if (state_ == detail::PingerState::Stopped)
        {
            return;
        }

        if (state_ == detail::PingerState::Running || state_ == detail::PingerState::Stopping)
        {
            throw std::logic_error{"AsyncPinger::run() must not be called recursively"};
        }

        auto self = shared_from_this(); // Keep the pinger alive until run() returns or unwinds.

        if (ioContext_.stopped())
        {
            DEBUG_INFO("run, I/O context restart, state {}", toString(state_));
            ioContext_.restart();
        }

        state_ = state_ == detail::PingerState::Interrupted
                     ? interruptedState_
                     : detail::PingerState::Running;

        try
        {

            DEBUG_INFO("I/O context run");

            // Resume the interrupted phase, including any unfinished stop drain.
            if (state_ == detail::PingerState::Stopping)
            {
                drainStoppedOperations();
            }

            ioContext_.run();

            if (state_ == detail::PingerState::Stopping && hasPendingWork())
            {
                throw std::logic_error{
                    "Stop processing ended with pending operations"};
            }
        }
        catch (...)
        {
            interruptedState_ = state_;
            state_ = detail::PingerState::Interrupted;
            throw;
        }

        state_ = state_ == detail::PingerState::Stopping
                     ? detail::PingerState::Stopped
                     : detail::PingerState::Idle;
    }

    // Queue the stop request; the state changes when its handler executes.
    void AsyncPinger::stop()
    {
        boost::asio::post(
            ioContext_,
            [weakSelf = weak_from_this()]()
            {
                if (auto self = weakSelf.lock())
                {
                    self->stopInternal();
                }
            });
    }

    void AsyncPinger::stopInternal()
    {
        if (state_ == detail::PingerState::Stopped)
        {
            return;
        }

        DEBUG_INFO("stop, Pinger state: {} -> {}", toString(state_), toString(detail::PingerState::Stopping));
        state_ = detail::PingerState::Stopping;

        drainStoppedOperations();
    }

    void AsyncPinger::drainStoppedOperations()
    {
        // Finish series before sessions so late session results cannot advance them.
        // Re-read each container after completion, which may invoke user callbacks.
        while (!repeatStates_.empty())
        {
            const auto id = repeatStates_.begin()->first;

            finishRepeat(id, RepeatCompletionStatus::Stopped);
        }

        while (!sessions_.empty())
        {
            const auto sequence = sessions_.begin()->first;

            completeSession(sequence, PingStatus::Stopped, std::chrono::milliseconds{0});
        }

        shutdownIo();
    }

    // Accept new operations only while idle or running.
    void AsyncPinger::ensureAcceptingWork() const
    {
        if (state_ != detail::PingerState::Idle && state_ != detail::PingerState::Running)
        {
            throw std::logic_error{"AsyncPinger is not accepting new operations"};
        }
    }

    void AsyncPinger::requestCancel(std::weak_ptr<detail::PingSession> session)
    {
        boost::asio::post(
            ioContext_,
            [weakSelf = weak_from_this(), session]()
            {
                auto self = weakSelf.lock();
                auto current = session.lock();

                if (!self || !current || current->completed)
                {
                    return;
                }

                const auto it = self->sessions_.find(current->sequenceNumber);

                // Sequence numbers wrap: an old handle must not cancel a newer session.
                if (it == self->sessions_.end() || it->second != current)
                {
                    return;
                }

                self->completeSession(
                    current->sequenceNumber,
                    PingStatus::Cancelled,
                    std::chrono::milliseconds{0});
            });
    }

    BatchHandle AsyncPinger::pingBatch(
        const std::vector<PingTarget> &targets,
        BatchCompletionCallback callback)
    {
        return pingBatch(
            targets,
            {},
            std::move(callback));
    }

    BatchHandle AsyncPinger::pingBatch(
        const std::vector<PingTarget> &targets,
        BatchProgressCallback progressCallback,
        BatchCompletionCallback callback)
    {
        ensureAcceptingWork();

        auto handles = std::make_shared<std::vector<PingHandle>>();

        handles->reserve(targets.size());

        if (targets.empty())
        {
            if (callback)
            {
                boost::asio::post(
                    ioContext_,
                    [callback = std::move(callback)]() mutable
                    {
                        try
                        {
                            callback({});
                        }
                        catch (...)
                        {
                            // Isolate user callback exceptions from the event loop.
                        }
                    });
            }

            return BatchHandle{};
        }

        auto state = std::make_shared<detail::BatchState>(targets.size(), std::move(callback), std::move(progressCallback));

        for (std::size_t i = 0; i < targets.size(); ++i)
        {
            const auto &target = targets[i];

            auto handle =
                pingAsync(
                    target.host,
                    target.options,
                    // Store each result at its input index, regardless of completion order.
                    [state, i](const PingResult &result)
                    {
                        state->complete(i, result);
                    });

            handles->push_back(std::move(handle));
        }

        return BatchHandle{std::move(handles)};
    }

    RepeatHandle AsyncPinger::pingRepeated(const std::string &host, const RepeatOptions &options, RepeatResultCallback resultCallback, RepeatCompletionCallback completionCallback)
    {
        ensureAcceptingWork();

        if (!isValid(options))
        {
            if (completionCallback)
            {
                boost::asio::post(
                    ioContext_,
                    [callback = std::move(completionCallback)]()
                    {
                        RepeatSummary summary;
                        summary.status =
                            RepeatCompletionStatus::InvalidOptions;

                        try
                        {
                            callback(summary);
                        }
                        catch ([[maybe_unused]] const std::exception &e)
                        {
                            DEBUG_ERROR(
                                "Exception in repeat completion callback: {}",
                                e.what());
                        }
                        catch (...)
                        {
                            DEBUG_ERROR(
                                "Unknown exception in repeat completion callback");
                        }
                    });
            }

            return RepeatHandle{};
        }

        const std::uint64_t id = nextRepeatId_++;

        auto state = std::make_shared<detail::RepeatState>();

        state->host = host;
        state->options = options;
        state->callback = std::move(resultCallback);
        state->completionCallback = std::move(completionCallback);

        state->intervalTimer = std::make_unique<boost::asio::steady_timer>(ioContext_);

        repeatStates_[id] = state;

        startRepeatPing(id);

        return RepeatHandle(weak_from_this(), id);
    }

    void AsyncPinger::startRepeatPing(std::uint64_t id)
    {
        auto it = repeatStates_.find(id);

        if (it == repeatStates_.end())
        {
            return;
        }

        auto state = it->second;

        if (state->cancelled || state->finished)
        {
            return;
        }

        state->currentPing = pingAsync(
            state->host,
            state->options.ping,
            [weakSelf = weak_from_this(), id](const PingResult &result)
            {
                if (auto self = weakSelf.lock())
                {
                    self->handleRepeatResult(id, result);
                }
            });
    }

    void AsyncPinger::scheduleNextRepeat(
        std::uint64_t id)
    {
        auto it = repeatStates_.find(id);

        if (it == repeatStates_.end())
        {
            return;
        }

        auto state = it->second;

        // The interval starts after the previous result has been processed.
        state->intervalTimer->expires_after(state->options.interval);

        state->intervalTimer->async_wait(
            [weakSelf = weak_from_this(), id](const boost::system::error_code &ec)
            {
                if (ec)
                {
                    return;
                }

                if (auto self = weakSelf.lock())
                {
                    self->startRepeatPing(id);
                }
            });
    }

    void AsyncPinger::requestCancelRepeat(std::uint64_t id)
    {
        boost::asio::post(
            ioContext_,
            [weakSelf = weak_from_this(), id]()
            {
                if (auto self = weakSelf.lock())
                {
                    self->cancelRepeat(id);
                }
            });
    }

    void AsyncPinger::cancelRepeat(std::uint64_t id)
    {
        auto it = repeatStates_.find(id);

        if (it == repeatStates_.end())
        {
            return;
        }

        auto state = it->second;

        state->cancelled = true;

        if (state->intervalTimer)
        {
            boost::system::error_code ec;
            state->intervalTimer->cancel(ec);
        }

        state->currentPing.cancel();

        // Complete the series now; its unfinished attempt is excluded from the summary.
        finishRepeat(id, RepeatCompletionStatus::Cancelled);

        if (!hasPendingWork())
        {
            shutdownIo();
        }
    }

    void AsyncPinger::finishRepeat(std::uint64_t id, RepeatCompletionStatus status)
    {
        auto it = repeatStates_.find(id);

        if (it == repeatStates_.end())
        {
            return;
        }

        auto state = it->second;

        state->finished = true;

        if (state->intervalTimer)
        {
            boost::system::error_code ec;
            state->intervalTimer->cancel(ec);
        }

        RepeatSummary summary;

        summary.completed = state->completed;
        summary.statistics = state->statistics;
        summary.status = status;

        auto callback = std::move(state->completionCallback);

        // Remove the series before notifying user code; late results are ignored.
        repeatStates_.erase(it);

        if (callback)
        {
            try
            {
                callback(summary);
            }
            catch ([[maybe_unused]] const std::exception &e)
            {
                DEBUG_ERROR(
                    "Exception in repeat completion callback: {}",
                    e.what());
            }
            catch (...)
            {
                DEBUG_ERROR(
                    "Unknown exception in repeat completion callback");
            }
        }
    }

    // DNS and timer handlers retain the pinger, session and resolver until completion.
    void AsyncPinger::startResolve(std::shared_ptr<detail::PingSession> session)
    {
        boost::system::error_code addressEc;

        const auto address = boost::asio::ip::make_address(session->host, addressEc);

        DEBUG_INFO(
            "make_address('{}'): ec={}, v4={}, v6={}",
            session->host,
            addressEc.value(),
            !addressEc && address.is_v4(),
            !addressEc && address.is_v6());

        // Numeric addresses bypass DNS but must satisfy the requested family.
        if (!addressEc)
        {
            const auto family = session->options.addressFamily;

            const bool familyMismatch =
                (family == AddressFamily::IPv4 && !address.is_v4()) ||
                (family == AddressFamily::IPv6 && !address.is_v6());

            if (familyMismatch)
            {
                completeSession(
                    session->sequenceNumber,
                    PingStatus::InvalidOptions,
                    std::chrono::milliseconds{0},
                    boost::system::errc::make_error_code(
                        boost::system::errc::invalid_argument));

                return;
            }

            if (address.is_v4())
            {
                session->ipFamily = detail::IpFamily::IPv4;

                session->destination = boost::asio::ip::icmp::endpoint(address, 0);

                DEBUG_INFO("Host {} is already an IPv4 address", session->host);

                sendRequest(session);
                return;
            }

            if (address.is_v6())
            {
                session->ipFamily = detail::IpFamily::IPv6;

                session->destination = boost::asio::ip::icmp::endpoint(address, 0);

                DEBUG_INFO("Host {} is already an IPv6 address", session->host);

                sendRequest(session);
                return;
            }
        }

        auto resolver = std::make_shared<boost::asio::ip::icmp::resolver>(ioContext_);

        // DNS resolution has a separate deadline from waiting for an Echo Reply.
        session->resolveTimer =
            std::make_unique<boost::asio::steady_timer>(
                ioContext_);

        session->resolveTimer->expires_after(
            session->options.resolveTimeout);

        session->resolveTimer->async_wait(
            [self = shared_from_this(),
             session,
             resolver](
                const boost::system::error_code &ec)
            {
                // A cancelled timer still invokes its handler; do not treat it as a timeout.
                if (ec)
                {
                    return;
                }

                if (session->completed)
                {
                    return;
                }

                DEBUG_WARN("DNS resolve timeout for {}", session->host);

                // Request resolver cancellation, then complete the session.
                // The resolver handler may still run later.
                resolver->cancel();

                self->completeSession(
                    session->sequenceNumber,
                    PingStatus::ResolveTimeout,
                    std::chrono::milliseconds{0},
                    ec);
            });

        auto resolveHandler =
            [self = shared_from_this(), session, resolver](const boost::system::error_code &ec,
                                                           boost::asio::ip::icmp::resolver::results_type results)
        {
            DEBUG_INFO(
                "Resolve handler returned for {}, ec={} ({})",
                session->host,
                ec.value(),
                utils::error_to_utf8(ec));

            if (session->resolveTimer)
            {
                boost::system::error_code timerEc;
                session->resolveTimer->cancel(timerEc);
            }

            // Timeout, cancellation or stop may have completed the session before DNS returned.
            if (session->completed)
            {
                DEBUG_INFO("Session {} completed, from DNS resolver", session->host);
                return;
            }

            if (ec)
            {
                DEBUG_ERROR("DNS resolve failed for {} >", session->host);
                DEBUG_ERROR("{}: {}", ec.value(), utils::error_to_utf8(ec));
                self->completeSession(session->sequenceNumber, PingStatus::ResolveError, std::chrono::milliseconds(0), ec);
                return;
            }

            if (results.begin() == results.end())
            {
                self->completeSession(session->sequenceNumber, PingStatus::ResolveError, std::chrono::milliseconds{0});
                return;
            }

            // Use the first resolver result. No address or family fallback is attempted.
            session->destination = results.begin()->endpoint();

            const auto resolvedAddress = session->destination.address();

            session->ipFamily = resolvedAddress.is_v4()
                                    ? detail::IpFamily::IPv4
                                    : detail::IpFamily::IPv6;

            DEBUG_INFO(
                "Resolved {} -> {} ({})",
                session->host,
                resolvedAddress.to_string(),
                resolvedAddress.is_v4() ? "IPv4" : "IPv6");

            self->sendRequest(session);
        };

        using Icmp = boost::asio::ip::icmp;
        using Resolver = Icmp::resolver;

        // Restrict DNS by policy; Any preserves the system resolver ordering.
        switch (session->options.addressFamily)
        {
        case AddressFamily::Any:
            resolver->async_resolve(
                session->host,
                "",
                Resolver::flags{},
                std::move(resolveHandler));
            break;

        case AddressFamily::IPv4:
            resolver->async_resolve(
                Icmp::v4(),
                session->host,
                "",
                Resolver::flags{},
                std::move(resolveHandler));
            break;

        case AddressFamily::IPv6:
            resolver->async_resolve(
                Icmp::v6(),
                session->host,
                "",
                Resolver::flags{},
                std::move(resolveHandler));
            break;

        default:
            completeSession(
                session->sequenceNumber,
                PingStatus::InvalidOptions,
                std::chrono::milliseconds{0},
                boost::system::errc::make_error_code(
                    boost::system::errc::invalid_argument));
            return;
        }
    }

    void AsyncPinger::sendRequest(std::shared_ptr<detail::PingSession> session)
    {
        switch (session->ipFamily)
        {
        case detail::IpFamily::IPv4:
            sendRequestV4(session);
            return;

        case detail::IpFamily::IPv6:
            sendRequestV6(session);
            return;

        case detail::IpFamily::Unknown:
            completeSession(
                session->sequenceNumber,
                PingStatus::InternalError,
                std::chrono::milliseconds{0});
            return;
        }
    }

    void AsyncPinger::sendRequestV4(std::shared_ptr<detail::PingSession> session)
    {
        const auto socketError = ensureSocketV4();

        if (socketError)
        {
            completeSession(
                session->sequenceNumber,
                PingStatus::SendError,
                std::chrono::milliseconds{0},
                socketError);
            return;
        }

        const std::size_t payloadSize = session->options.payloadSize;

        const std::size_t packetSize = icmp::EchoHeaderSize + payloadSize;

        auto packet = std::make_shared<std::vector<std::uint8_t>>(packetSize);

        icmp::writeEchoRequestHeader(packet->data(), identifier_, session->sequenceNumber);

        std::memset(packet->data() + icmp::EchoHeaderSize, 'B', payloadSize);

        const std::uint16_t checksum = icmp::calculateChecksum(packet->data(), packet->size());
        netbytes::writeUint16BigEndian(packet->data() + icmp::ChecksumOffset, checksum);

        DEBUG_INFO("Socket is_open: {}", socket_.is_open());
        DEBUG_INFO("Destination: {}", session->destination.address().to_string());
        DEBUG_INFO("Packet size: {}", packet->size());
        DEBUG_INFO("ICMP: type={}, code={}, id={}, seq={}, checksum={:04x}",
                   (*packet)[0], (*packet)[1], identifier_, session->sequenceNumber, checksum);

        if (!socket_.is_open())
        {
            DEBUG_ERROR("Socket is not open for {}", session->host);
            completeSession(session->sequenceNumber, PingStatus::SendError, std::chrono::milliseconds(0));
            return;
        }

        if (session->destination.address().is_unspecified())
        {
            DEBUG_ERROR("Invalid destination address for {}", session->host);
            completeSession(session->sequenceNumber, PingStatus::SendError, std::chrono::milliseconds(0));
            return;
        }

        DEBUG_INFO("Packet hex, {}", packetToHexString(*packet, 16));

        try
        {


            // Arm receive first: a loopback reply may arrive before the send handler.
            startReceive();

            session->sendTime = std::chrono::steady_clock::now();

            socket_.async_send_to(
                boost::asio::buffer(*packet),
                session->destination,
                // Keep the send buffer alive until the asynchronous operation completes.
                [self = shared_from_this(), session, packet](const boost::system::error_code &ec, std::size_t bytesSent)
                {
                    // A reply, cancellation or stop may win the race with this handler.
                    if (session->completed)
                    {
                        DEBUG_INFO("Session {} completed, from async_send_to", session->host);
                        return;
                    }

                    if (!session)
                    {
                        DEBUG_ERROR("Session is null in send callback");
                        return;
                    }

                    if (ec)
                    {
                        const PingStatus status =
                            ec == boost::asio::error::operation_aborted
                                ? PingStatus::Cancelled
                                : PingStatus::SendError;

                        if (status == PingStatus::Cancelled)
                        {
                            DEBUG_WARN(
                                "Send canceled for {}",
                                session->host);
                        }
                        else
                        {
                            DEBUG_ERROR(
                                "Send failed for {}: {} ({})",
                                session->host,
                                ec.value(),
                                utils::error_to_utf8(ec));
                        }

                        self->completeSession(
                            session->sequenceNumber,
                            status,
                            std::chrono::milliseconds{0},
                            ec);

                        return;
                    }

                    session->requestSent = bytesSent == packet->size();

                    DEBUG_INFO("Sent ICMP request to {} ({} of {} bytes)", session->host, bytesSent, packet->size());

                    self->startWaitForReply(session);
                });
        }
        catch ([[maybe_unused]] const boost::system::system_error &e)
        {
            DEBUG_ERROR("System error in async_send_to: {} ({})", e.code().value(), e.what());
            completeSession(session->sequenceNumber, PingStatus::SendError, std::chrono::milliseconds(0));
        }
        catch ([[maybe_unused]] const std::bad_alloc &e)
        {
            DEBUG_ERROR("Memory allocation failed: {}", e.what());
            completeSession(session->sequenceNumber, PingStatus::SendError, std::chrono::milliseconds(0));
        }
        catch ([[maybe_unused]] const std::exception &e)
        {
            DEBUG_ERROR("Exception in async_send_to: {}", e.what());
            completeSession(session->sequenceNumber, PingStatus::SendError, std::chrono::milliseconds(0));
        }
        catch (...)
        {
            DEBUG_ERROR("Unknown exception in async_send_to");
            completeSession(session->sequenceNumber, PingStatus::SendError, std::chrono::milliseconds(0));
        }
    }

    void AsyncPinger::sendRequestV6(
        std::shared_ptr<detail::PingSession> session)
    {
        const auto socketError = ensureSocketV6();

        if (socketError)
        {
            completeSession(
                session->sequenceNumber,
                PingStatus::InternalError,
                std::chrono::milliseconds{0},
                socketError);

            return;
        }

        const auto address = session->destination.address();

        if (!address.is_v6() || address.is_unspecified())
        {
            completeSession(
                session->sequenceNumber,
                PingStatus::SendError,
                std::chrono::milliseconds{0},
                boost::system::errc::make_error_code(
                    boost::system::errc::invalid_argument));

            return;
        }

        try
        {
            const auto payloadSize = session->options.payloadSize;
            const auto packetSize =
                icmpv6::EchoHeaderSize + payloadSize;

            auto packet =
                std::make_shared<std::vector<std::uint8_t>>(
                    packetSize);

            icmpv6::writeEchoRequestHeader(
                packet->data(),
                identifier_,
                session->sequenceNumber);

            std::memset(
                packet->data() + icmpv6::EchoHeaderSize,
                'B',
                payloadSize);

            // Receive may complete before send; both paths share the session completion guard.
            startReceiveV6();

            // The ICMPv6 checksum field is left zero for the raw socket transport to fill.
            session->sendTime = std::chrono::steady_clock::now();

            socketV6_.async_send_to(
                boost::asio::buffer(*packet),
                session->destination,
                // asio::buffer does not own memory; retain the packet through send completion.
                [self = shared_from_this(), session, packet](
                    const boost::system::error_code &ec,
                    std::size_t bytesSent)
                {
                    if (session->completed)
                    {
                        return;
                    }

                    if (ec)
                    {
                        DEBUG_ERROR(
                            "IPv6 send failed for {}: {} ({}, category={})",
                            session->host,
                            utils::error_to_utf8(ec),
                            ec.value(),
                            ec.category().name());

                        const auto status =
                            ec == boost::asio::error::operation_aborted
                                ? PingStatus::Cancelled
                                : PingStatus::SendError;

                        self->completeSession(
                            session->sequenceNumber,
                            status,
                            std::chrono::milliseconds{0},
                            ec);

                        return;
                    }

                    if (bytesSent != packet->size())
                    {
                        self->completeSession(
                            session->sequenceNumber,
                            PingStatus::SendError,
                            std::chrono::milliseconds{0},
                            boost::asio::error::message_size);

                        return;
                    }

                    session->requestSent = true;

                    DEBUG_INFO(
                        "Sent ICMPv6 request to {} ({} bytes)",
                        session->host,
                        bytesSent);

                    self->startWaitForReply(session);
                });
        }
        catch ([[maybe_unused]] const boost::system::system_error &e)
        {
            DEBUG_ERROR("IPv6 send initiation failed: {}", e.what());

            completeSession(
                session->sequenceNumber,
                PingStatus::SendError,
                std::chrono::milliseconds{0},
                e.code());
        }
        catch ([[maybe_unused]] const std::exception &e)
        {
            DEBUG_ERROR("IPv6 send initiation failed: {}", e.what());

            completeSession(
                session->sequenceNumber,
                PingStatus::InternalError,
                std::chrono::milliseconds{0});
        }
    }

    // Open IPv4 only when an operation needs to send a request.
    boost::system::error_code AsyncPinger::ensureSocketV4()
    {
        if (socket_.is_open())
        {
            return {};
        }

        boost::system::error_code ec;
        socket_.open(boost::asio::ip::icmp::v4(), ec);

        if (ec)
        {
            return ec;
        }

        const boost::asio::ip::icmp::endpoint localEndpoint(
            boost::asio::ip::address_v4::any(), 0);

        socket_.bind(localEndpoint, ec);

        if (ec)
        {
            boost::system::error_code closeError;
            socket_.close(closeError);
            return ec;
        }

        return {};
    }

    // Open IPv6 on demand so IPv4-only use does not require IPv6 socket setup.
    boost::system::error_code AsyncPinger::ensureSocketV6()
    {
        if (socketV6_.is_open())
        {
            return {};
        }

        boost::system::error_code ec;
        socketV6_.open(boost::asio::ip::icmp::v6(), ec);

        if (ec)
        {
            return ec;
        }

        const boost::asio::ip::icmp::endpoint localEndpoint(
            boost::asio::ip::address_v6::any(), 0);

        socketV6_.bind(localEndpoint, ec);

        if (ec)
        {
            // Roll back a failed bind so a later call can retry initialization.
            boost::system::error_code closeError;
            socketV6_.close(closeError);
            return ec;
        }

        return {};
    }

    void AsyncPinger::startWaitForReply(std::shared_ptr<detail::PingSession> session)
    {
        // Start the per-session reply deadline after send completion is reported.
        session->timeoutTimer->expires_at(detail::replyDeadline(session->sendTime, session->options.replyTimeout));

        session->timeoutTimer->async_wait(
            [self = shared_from_this(), session](const boost::system::error_code &ec)
            {
                if (session->completed)
                    return;

                // Ignore cancellation or other timer errors; only normal expiry is a timeout.
                if (ec)
                {
                    return;
                }

                DEBUG_ERROR("Timeout for {}", session->host);

                self->completeSession(session->sequenceNumber, PingStatus::Timeout, std::chrono::milliseconds(0));
            });
    }

    void AsyncPinger::completeSession(std::uint16_t sequence, PingStatus status, std::chrono::milliseconds latency, boost::system::error_code error)
    {
        // A missing active entry makes this completion request a no-op.
        auto sessionIt = sessions_.find(sequence);

        if (sessionIt == sessions_.end())
        {
            return;
        }

        // Retain the session while preparing the result and removing its active entry.
        auto session = sessionIt->second;

        // Prepare the result before committing completion: copying the host may throw.
        PingResult result;

        result.host = session->host;

        if (!session->destination.address().is_unspecified())
        {
            result.address = session->destination.address();
        }

        result.status = status;
        result.error = error;

        // A matching reply proves the request was sent, even if its send handler is pending.
        result.requestSent =
            session->requestSent || status == PingStatus::Success;

        if (status == PingStatus::Success)
        {
            result.latency = latency;
        }

        // Commit completion before invoking user code or processing late handlers.
        session->requestSent = result.requestSent;
        session->completed = true;

        sessions_.erase(sessionIt);

        auto callback = std::move(session->callback);

        // Cancel obsolete timers; their queued handlers may still run.
        if (session->resolveTimer)
        {
            boost::system::error_code timerEc;
            session->resolveTimer->cancel(timerEc);
        }

        if (session->timeoutTimer)
        {
            boost::system::error_code timerEc;
            session->timeoutTimer->cancel(timerEc);
        }

        // Notify user code only after the session is removed and marked complete.
        // Reentrant initiation remains subject to the lifecycle state checks.
        if (callback)
        {
            try
            {
                callback(result);
            }
            catch ([[maybe_unused]] const std::exception &e)
            {
                DEBUG_ERROR("Exception in ping callback: {}", e.what());
            }
            catch (...)
            {
                DEBUG_ERROR("Unknown exception in ping callback");
            }
        }

        // Re-check after the callback, which may have queued new work.
        if (!hasPendingWork())
        {
            shutdownIo();
        }
    }

    // Cancel pending socket operations without closing sockets or changing lifecycle state.
    void AsyncPinger::shutdownIo()
    {
        DEBUG_INFO("shutdownIo requested, current state={}", toString(state_));

        if (state_ == detail::PingerState::Stopped)
        {
            return;
        }

        boost::system::error_code ec;
        if (socket_.is_open())
        {
            socket_.cancel(ec);
        }

        if (socketV6_.is_open())
        {
            boost::system::error_code cancelError;
            socketV6_.cancel(cancelError);

            if (cancelError)
            {
                DEBUG_WARN(
                    "IPv6 socket cancel failed: {}",
                    cancelError.message());
            }
        }

        if (ec)
        {
            DEBUG_WARN(
                "Socket cancel failed: {}",
                ec.message());
        }
    }

    void AsyncPinger::startReceive()
    {
        if (!socket_.is_open())
        {
            return;
        }

        if (receivePending_)
        {
            return;
        }

        receivePending_ = true;

        // Observe through weak_ptr: the receive handler must not keep the pinger alive by itself.
        socket_.async_receive_from(
            boost::asio::buffer(receiveBuffer_),
            senderEndpoint_,
            [weakSelf = weak_from_this()](
                const boost::system::error_code &ec,
                std::size_t bytesReceived)
            {
                if (auto self = weakSelf.lock())
                {
                    // This receive completed; another operation may now use the shared buffer.
                    self->receivePending_ = false;

                    self->handleReceive(ec, bytesReceived);
                }
            });
    }

    void AsyncPinger::handleReceive(
        const boost::system::error_code &ec,
        std::size_t bytesReceived)
    {
        if (ec)
        {
            // Do not rearm a receive cancelled during shutdown or idle cleanup.
            if (ec == boost::asio::error::operation_aborted)
            {
                DEBUG_INFO("Global receive operation canceled");
                return;
            }

            DEBUG_ERROR(
                "Global receive failed: {} ({}, category={})",
                utils::error_to_utf8(ec), ec.value(), ec.category().name());

            // Keep receiving after a non-cancellation error while the socket remains open.
            if (socket_.is_open())
            {
                startReceive();
            }

            return;
        }

        DEBUG_INFO(
            "Global receive: {} bytes from {}",
            bytesReceived,
            senderEndpoint_.address().to_string());

        // IPv4 raw receive includes the IP header; parse it before the ICMP message.
        const auto ipv4Packet = ipv4::parse(receiveBuffer_.data(), bytesReceived);

        if (ipv4Packet)
        {
            constexpr std::uint8_t IcmpProtocolNumber = 1;

            if (ipv4Packet->protocol == IcmpProtocolNumber)
            {
                const auto reply =
                    icmp::parseEchoReply(
                        ipv4Packet->payload,
                        ipv4Packet->payloadSize);
                if (reply)
                {
                    if (reply->identifier == identifier_)
                    {
                        auto sessionIt =
                            sessions_.find(reply->sequence);

                        if (sessionIt != sessions_.end())
                        {
                            const auto &session =
                                sessionIt->second;
                            DEBUG_INFO(
                                "ICMP reply: identifier={}, sequence={}",
                                reply->identifier,
                                reply->sequence);
                            if (ipv4Packet->sourceAddress == session->destination.address() &&
                                senderEndpoint_.address() == session->destination.address())
                            {
                                completeReply(*session);
                            }
                        }
                    }
                }
            }
        }

        // Ignore unrelated packets and continue while sessions remain active.
        if (socket_.is_open() && !sessions_.empty())
        {
            startReceive();
        }
    }

    void AsyncPinger::startReceiveV6()
    {
        if (!socketV6_.is_open() || receivePendingV6_)
        {
            return;
        }

        receivePendingV6_ = true;

        try
        {
            // Use separate IPv6 storage; IPv4 and IPv6 receives may be pending together.
            socketV6_.async_receive_from(
                boost::asio::buffer(receiveBufferV6_),
                senderEndpointV6_,
                [weakSelf = weak_from_this()](
                    const boost::system::error_code &ec,
                    std::size_t bytesReceived)
                {
                    if (auto self = weakSelf.lock())
                    {
                        self->receivePendingV6_ = false;
                        self->handleReceiveV6(ec, bytesReceived);
                    }
                });
        }
        catch (...)
        {
            // Initiation failed, so no completion handler will clear the pending flag.
            receivePendingV6_ = false;
            throw;
        }
    }

    void AsyncPinger::handleReceiveV6(
        const boost::system::error_code &ec,
        std::size_t bytesReceived)
    {
        if (ec == boost::asio::error::operation_aborted)
        {
            return;
        }

        if (ec)
        {
            DEBUG_ERROR(
                "IPv6 receive failed: {} ({})",
                ec.message(),
                ec.value());
        }
        else
        {
            DEBUG_INFO(
                "IPv6 receive: {} bytes from {}",
                bytesReceived,
                senderEndpointV6_.address().to_string());

            // Raw IPv6 receive supplies ICMPv6 without the IPv6 headers.
            // Field parsing relies on the transport for checksum validation.
            const auto reply = icmpv6::parseEchoReply(
                receiveBufferV6_.data(),
                bytesReceived);

            if (reply && reply->identifier == identifier_)
            {
                const auto it = sessions_.find(reply->sequence);

                if (it != sessions_.end())
                {
                    const auto session = it->second;

                    if (!session->completed &&
                        session->ipFamily == detail::IpFamily::IPv6 &&
                        senderEndpointV6_.address() ==
                            session->destination.address())
                    {
                        completeReply(*session);
                    }
                }
            }
        }

        // Rearm only while an active IPv6 session still needs replies.
        for (const auto &[sequence, session] : sessions_)
        {
            if (!session->completed &&
                session->ipFamily == detail::IpFamily::IPv6)
            {
                startReceiveV6();
                break;
            }
        }
    }

    void AsyncPinger::completeReply(detail::PingSession &session)
    {
        const auto latency = detail::replyLatency(
            session.sendTime,
            session.options.replyTimeout,
            std::chrono::steady_clock::now());

        if (latency)
        {
            DEBUG_INFO("Latency: {}ms", latency->count());
        }

        // A matching reply confirms the request was sent, even if it is late.
        session.requestSent = true;

        completeSession(
            session.sequenceNumber,
            latency ? PingStatus::Success : PingStatus::Timeout,
            latency.value_or(std::chrono::milliseconds{0}));
    }

    void AsyncPinger::handleRepeatResult(std::uint64_t id, const PingResult &result)
    {
        auto it = repeatStates_.find(id);

        if (it == repeatStates_.end())
        {
            return;
        }

        auto state = it->second;

        // Results arriving after series cancellation/completion do not affect the summary.
        if (state->cancelled || state->finished)
        {
            return;
        }

        ++state->completed;

        // Statistics cover recorded results, not every packet over the series lifetime.
        detail::recordRepeatResult(
            state->statistics,
            state->totalLatency,
            result);

        if (state->callback)
        {
            try
            {
                state->callback(result, state->completed);
            }
            catch ([[maybe_unused]] const std::exception &e)
            {
                DEBUG_ERROR("Exception in repeat result callback: {}", e.what());
            }
            catch (...)
            {
                DEBUG_ERROR("Unknown exception in repeat result callback");
            }
        }

        const bool finite = state->options.count != 0;

        if (finite && state->completed >= state->options.count)
        {
            finishRepeat(id, RepeatCompletionStatus::Completed);

            return;
        }

        scheduleNextRepeat(id);
    }

    // Deliver results without a registered session through the same asynchronous callback model.
    void AsyncPinger::postResult(
        std::string host,
        PingStatus status,
        PingCallback callback,
        boost::system::error_code error)
    {
        if (!callback)
        {
            return;
        }

        boost::asio::post(
            ioContext_,
            [host = std::move(host),
             status,
             callback = std::move(callback),
             error]() mutable
            {
                PingResult result;

                result.host = std::move(host);
                result.status = status;
                result.error = error;

                try
                {
                    callback(result);
                }
                catch ([[maybe_unused]] const std::exception &e)
                {
                    DEBUG_ERROR("Exception in ping callback:{}", e.what());
                }
                catch (...)
                {
                    DEBUG_ERROR("Unknown exception in ping callback");
                }
            });
    }

    std::uint16_t AsyncPinger::allocateSequence()
    {
        constexpr std::size_t MaxSequences =
            static_cast<std::size_t>(std::numeric_limits<std::uint16_t>::max()) + 1;

        // Wraparound is allowed, but a sequence must not collide with an active session.
        for (std::size_t attempt = 0;
             attempt < MaxSequences;
             ++attempt)
        {
            const std::uint16_t sequence = nextSequence_++;

            if (!sessions_.contains(sequence))
            {
                return sequence;
            }
        }

        throw std::runtime_error(
            "No free ICMP sequence numbers");
    }

    bool AsyncPinger::hasPendingWork() const noexcept
    {
        return !sessions_.empty() || !repeatStates_.empty();
    }
}
