#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/icmp.hpp>
#include <boost/system/error_code.hpp>

#include <pings/batch_handle.hpp>
#include <pings/batch_types.hpp>
#include <pings/ping_handle.hpp>
#include <pings/ping_options.hpp>
#include <pings/ping_result.hpp>
#include <pings/ping_target.hpp>
#include <pings/ping_types.hpp>
#include <pings/repeat_handle.hpp>
#include <pings/repeat_options.hpp>
#include <pings/repeat_summary.hpp>
#include <pings/repeat_types.hpp>

namespace pings
{
    namespace detail
    {
        struct PingSession;
        struct RepeatState;
        enum class PingerState;
    }

    // Асинхронный пингер с одним io_context.
    // Unless otherwise stated, AsyncPinger operations
    // must be initiated from the same thread.
    class AsyncPinger : public std::enable_shared_from_this<AsyncPinger>
    {
        friend class PingHandle;
        friend class RepeatHandle;

    public:
        using Ptr = std::shared_ptr<AsyncPinger>;

        AsyncPinger(const AsyncPinger &) = delete;
        AsyncPinger &operator=(const AsyncPinger &) = delete;

        AsyncPinger(AsyncPinger &&) = delete;
        AsyncPinger &operator=(AsyncPinger &&) = delete;

        ~AsyncPinger();

        // Factory
        [[nodiscard]]
        static Ptr create();

        // pingAsync(), pingBatch() и pingRepeated():
        // в состояниях Stopping/Stopped бросают std::logic_error.
        // При таком отказе операция не создаётся, callbacks не вызываются.
        // Запуск асинхронного пинга для одного хоста
        [[nodiscard]]
        PingHandle pingAsync(const std::string &host, PingCallback callback);
        [[nodiscard]]
        PingHandle pingAsync(const std::string &host, const PingOptions &options, PingCallback callback);

        [[nodiscard]]
        BatchHandle pingBatch(const std::vector<PingTarget> &targets, BatchCompletionCallback callback);
        [[nodiscard]]
        BatchHandle pingBatch(const std::vector<PingTarget> &targets, BatchProgressCallback progressCallback, BatchCompletionCallback callback);

        // Repeated ping operations
        [[nodiscard]]
        RepeatHandle pingRepeated(const std::string &host, const RepeatOptions &options, RepeatResultCallback callback, RepeatCompletionCallback completionCallback);

        // Запуск обработки событий (блокирует текущий поток)
        void run();
        // Запрашивает остановку AsyncPinger
        // Активные операции завершаются асинхронно со статусом Stopped
        void stop();

    private:
        AsyncPinger();
        void finishRepeat(std::uint64_t id, RepeatCompletionStatus status);

        void startRepeatPing(std::uint64_t id);
        void scheduleNextRepeat(std::uint64_t id);

        void requestCancelRepeat(std::uint64_t id);
        void cancelRepeat(std::uint64_t id);

        // Resolve the target address, or use a numeric address directly.
        void startResolve(std::shared_ptr<detail::PingSession> session);
        void sendRequest(std::shared_ptr<detail::PingSession> session);
        void sendRequestV4(std::shared_ptr<detail::PingSession> session);
        void sendRequestV6(std::shared_ptr<detail::PingSession> session);
        boost::system::error_code ensureSocketV4();
        boost::system::error_code ensureSocketV6();
        void startWaitForReply(std::shared_ptr<detail::PingSession> session);
        // Start an IPv4 receive unless one is already pending.
        void startReceive();
        void startReceiveV6();

        // Обрабатывает полученный ICMP пакет и сопоставляет его
        // с активной ping-сессией.
        void handleReceive(
            const boost::system::error_code &ec,
            std::size_t bytesReceived);
        void handleReceiveV6(
            const boost::system::error_code &ec,
            std::size_t bytesReceived);

        void completeReply(detail::PingSession &session);

        void handleRepeatResult(
            std::uint64_t id,
            const PingResult &result);

        void postResult(
            std::string host,
            PingStatus status,
            PingCallback callback,
            boost::system::error_code error = {});

        void ensureAcceptingWork() const;

        void requestCancel(std::weak_ptr<detail::PingSession> session);

        void stopInternal();
        void drainStoppedOperations();
        void completeSession(std::uint16_t sequence, PingStatus status, std::chrono::milliseconds latency, boost::system::error_code error = {});
        void shutdownIo();

        std::uint16_t allocateSequence();

        [[nodiscard]]
        bool hasPendingWork() const noexcept;

        boost::asio::io_context ioContext_;

        boost::asio::ip::icmp::socket socket_;
        boost::asio::ip::icmp::socket socketV6_;
        std::uint16_t nextSequence_{1};

        // Буфер используется единственным общим receive loop.
        // Одновременно на socket выполняется только одна операция чтения.
        std::array<std::uint8_t, 2048> receiveBuffer_{};
        bool receivePending_{false};
        boost::asio::ip::icmp::endpoint senderEndpoint_;

        std::array<std::uint8_t, 2048> receiveBufferV6_{};
        boost::asio::ip::icmp::endpoint senderEndpointV6_;
        bool receivePendingV6_{false};

        // Храним сессии для отображения sequence -> host
        std::map<std::uint16_t, std::shared_ptr<detail::PingSession>> sessions_;
        std::uint16_t identifier_{};

        // Состояние
        detail::PingerState state_;
        detail::PingerState interruptedState_{};

        std::unordered_map<std::uint64_t, std::shared_ptr<detail::RepeatState>> repeatStates_;
        std::uint64_t nextRepeatId_{};
    };

} // namespace pings
