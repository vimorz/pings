#pragma once

#include <chrono>
#include <optional>

namespace pings::detail
{
    inline constexpr auto replyDeadline(
        std::chrono::steady_clock::time_point sendTime,
        std::chrono::milliseconds timeout) noexcept
    {
        return sendTime + timeout;
    }

    inline constexpr std::optional<std::chrono::milliseconds> replyLatency(
        std::chrono::steady_clock::time_point sendTime,
        std::chrono::milliseconds timeout,
        std::chrono::steady_clock::time_point processedAt) noexcept
    {
        if (processedAt >= replyDeadline(sendTime, timeout))
        {
            return std::nullopt;
        }

        return std::chrono::duration_cast<std::chrono::milliseconds>(
            processedAt - sendTime);
    }
}