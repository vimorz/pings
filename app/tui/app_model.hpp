#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace ui
{

    inline constexpr std::size_t historyCapacity = 60;

    enum class SampleStatus
    {
        Reply,
        Timeout,
        Error,
    };

    struct PingSample
    {
        SampleStatus status;
        std::optional<int> latencyMs;
        std::string detail{};
    };

    struct HostRow
    {
        std::string name;
        std::string address;
        std::deque<PingSample> history;
        bool enabled = true;
        std::optional<std::chrono::milliseconds> replyTimeout{};
    };

    struct AppModel
    {
        std::vector<HostRow> hosts;
        int selected = 0;

        bool paused = false;
        unsigned int demoTick = 0;
    };

    struct HistorySummary
    {
        std::size_t replies = 0;
        std::size_t timeouts = 0;
        std::size_t errors = 0;
    };

    HistorySummary summarizeHistory(const HostRow &host);

    void appendSample(HostRow &host, PingSample sample);
    void clearHistory(AppModel &model);

    const HostRow *selectedHost(const AppModel &model);
    void moveSelection(AppModel &model, int offset);

} // namespace ui