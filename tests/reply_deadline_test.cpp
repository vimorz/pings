#include <chrono>

#include <reply_deadline.hpp>

int main()
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;

    constexpr auto sentAt = Clock::time_point{} + 1s;
    constexpr auto limit = 60ms;
    constexpr auto tick = Clock::duration{1};
    constexpr auto deadline = sentAt + limit;
    constexpr auto sendCompletedAt = sentAt + 8ms;

    using pings::detail::replyDeadline;
    using pings::detail::replyLatency;

    static_assert(replyDeadline(sentAt, limit) == deadline);
    static_assert(replyDeadline(sentAt, limit) - sendCompletedAt == 52ms);

    static_assert(replyLatency(sentAt, limit, sentAt) == 0ms);
    static_assert(replyLatency(sentAt, limit, deadline - tick) == 59ms);
    static_assert(!replyLatency(sentAt, limit, deadline));
    static_assert(!replyLatency(sentAt, limit, deadline + tick));
    static_assert(!replyLatency(sentAt, limit, sentAt + 68ms));
}