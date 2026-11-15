#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <thread>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <pings/async_pinger.hpp>
#include <pings/ping_target.hpp>

#include "pings_ui.hpp"
#include "tui/app_model.hpp"
#include "tui/demo_source.hpp"

namespace ui
{
    namespace
    {
        std::string formatLatency(const std::optional<int> &latency)
        {
            if (!latency)
            {
                return "—";
            }

            return std::to_string(*latency) + " ms";
        }

        std::string statusText(SampleStatus status)
        {
            switch (status)
            {
            case SampleStatus::Reply:
                return "Reply";
            case SampleStatus::Timeout:
                return "Timeout";
            case SampleStatus::Error:
                return "Error";
            }

            return "Unknown";
        }

        ftxui::Color statusColor(SampleStatus status)
        {
            using ftxui::Color;

            switch (status)
            {
            case SampleStatus::Reply:
                return Color::Green;
            case SampleStatus::Timeout:
                return Color::Red;
            case SampleStatus::Error:
                return Color::Yellow;
            }

            return Color::Palette16::GrayDark;
        }

        ftxui::Element renderHistory(
            const HostRow &host,
            std::size_t visibleCount)
        {
            using namespace ftxui;

            visibleCount = std::min(visibleCount, historyCapacity);

            const auto sampleCount = std::min(visibleCount, host.history.size());

            const auto emptyCount = visibleCount - sampleCount;
            const auto firstSample = host.history.size() - sampleCount;

            Elements cells;

            for (std::size_t i = 0; i < emptyCount; ++i)
            {
                cells.push_back(text(" ") | bgcolor(Color::GrayDark));
            }

            for (std::size_t i = firstSample; i < host.history.size(); ++i)
            {
                cells.push_back(
                    text(" ") | bgcolor(statusColor(host.history[i].status)));
            }
            return hbox(std::move(cells));
        }

        struct TableLayout
        {
            bool compact = false;

            int markerWidth = 2;
            int nameWidth = 15;
            int addressWidth = 20;
            int latencyWidth = 10;
            int statusWidth = 12;
            int bracketWidth = 3;

            std::size_t visibleCount = 0;

            int fixedWidth() const
            {
                return markerWidth + nameWidth + addressWidth + latencyWidth + statusWidth + bracketWidth;
            }
        };

        std::optional<TableLayout> makeTableLayout(int terminalWidth)
        {
            constexpr int frameWidth = 3;
            constexpr int preferredHistoryWidth = 15;
            constexpr int minimumHistoryWidth = 5;

            TableLayout layout;

            // If the full table leaves too little room for the history,
            // hide the host name and status text.
            if (terminalWidth - frameWidth - layout.fixedWidth() < preferredHistoryWidth)
            {
                layout.compact = true;
                layout.nameWidth = 0;
                layout.statusWidth = 0;
            }

            const int availableWidth = terminalWidth - frameWidth - layout.fixedWidth();

            if (availableWidth < minimumHistoryWidth)
            {
                return std::nullopt;
            }

            layout.visibleCount = static_cast<std::size_t>(std::min(availableWidth, static_cast<int>(historyCapacity)));

            return layout;
        }

        ftxui::Element renderHost(const HostRow &host, bool selected, const TableLayout &layout)
        {
            using namespace ftxui;

            std::string status = host.enabled ? "No data" : "Disabled";
            std::string latency = "—";
            ftxui::Color currentColor = Color::GrayDark;

            if (!host.history.empty())
            {
                const auto &latest = host.history.back();
                status = statusText(latest.status);
                latency = formatLatency(latest.latencyMs);
                currentColor = statusColor(latest.status);
            }

            Elements columns;

            columns.push_back(text(selected ? "> " : "  ") | size(WIDTH, EQUAL, layout.markerWidth));

            if (!layout.compact)
            {
                columns.push_back(
                    text(host.name) | size(WIDTH, EQUAL, layout.nameWidth));
            }

            columns.push_back(text(host.address) | size(WIDTH, EQUAL, layout.addressWidth));

            columns.push_back(
                text(latency) | color(currentColor) | size(WIDTH, EQUAL, layout.latencyWidth));

            if (!layout.compact)
            {
                columns.push_back(
                    text(status) | color(currentColor) | size(WIDTH, EQUAL, layout.statusWidth));
            }

            columns.push_back(text(" ["));
            columns.push_back(renderHistory(host, layout.visibleCount));
            columns.push_back(text("]"));

            auto row = hbox(std::move(columns));

            if (selected)
            {
                row = row | bold | focus;
            }

            return row;
        }

        ftxui::Element renderTable(
            const AppModel &model,
            int terminalWidth)
        {
            using namespace ftxui;

            if (model.hosts.empty())
            {
                return text("No hosts configured");
            }

            const auto layout = makeTableLayout(terminalWidth);

            if (!layout)
            {
                return paragraph(
                    "Not enough room for the table. Widen the terminal.");
            }

            Elements rows;

            for (std::size_t i = 0; i < model.hosts.size(); ++i)
            {
                rows.push_back(renderHost(
                    model.hosts[i],
                    static_cast<int>(i) == model.selected,
                    *layout));
            }

            return vbox(std::move(rows));
        }

        ftxui::Element renderDetails(const AppModel &model)
        {
            using namespace ftxui;

            const auto *host = selectedHost(model);

            if (!host)
            {
                return text("No host selected");
            }

            std::string status = host->enabled ? "No data" : "Disabled";

            if (!host->history.empty())
            {
                const auto &latest = host->history.back();
                status = statusText(latest.status);

                if (!latest.detail.empty())
                {
                    status += " — ";
                    status += latest.detail;
                }
            }

            const auto summary = summarizeHistory(*host);

            std::string statusLine = "Status: " + status;

            if (host->replyTimeout)
            {
                statusLine = "Reply timeout: " +
                             std::to_string(host->replyTimeout->count()) +
                             " ms | " + statusLine;
            }

            return vbox({
                paragraph("Selected: " + host->name + " | " + host->address),
                paragraph(statusLine),
                paragraph(
                    "History: " + std::to_string(host->history.size()) +
                    " | Replies: " + std::to_string(summary.replies) +
                    " | Timeouts: " + std::to_string(summary.timeouts) +
                    " | Errors: " + std::to_string(summary.errors)),
            });
        }

        using BatchResults = std::vector<pings::PingResult>;

        BatchResults runBatch(const std::vector<pings::PingTarget> &targets, std::atomic<std::size_t> &completedCount)
        {
            std::optional<BatchResults> completed;
            auto pinger = pings::AsyncPinger::create();

            [[maybe_unused]] auto handle = pinger->pingBatch(
                targets,
                [&](std::size_t completedSoFar, std::size_t /*total*/)
                {
                    completedCount.store(completedSoFar);
                },
                [&](const BatchResults &results)
                {
                    completed = results;
                });

            pinger->run();

            if (!completed)
            {
                throw std::runtime_error(
                    "Batch finished without delivering results");
            }

            return std::move(*completed);
        }

        PingSample toSample(const pings::PingResult &result)
        {
            if (result.status == pings::PingStatus::Success)
            {
                std::optional<int> latencyMs;
                if (result.latency)
                {
                    latencyMs = static_cast<int>(result.latency->count());
                }
                return {SampleStatus::Reply, latencyMs};
            }

            const auto status = result.status == pings::PingStatus::Timeout
                                    ? SampleStatus::Timeout
                                    : SampleStatus::Error;

            std::string detail{pings::toString(result.status)};

            if (result.error)
            {
                detail += " [";
                detail += result.error.category().name();
                detail += ':';
                detail += std::to_string(result.error.value());
                detail += ']';
            }

            return {status, std::nullopt, std::move(detail)};
        }
    } // namespace

    int ui(AppModel model, UiMode mode, std::vector<pings::PingTarget> targets)
    {
        using namespace ftxui;

        const bool demoMode = mode == UiMode::Demo;

        std::vector<std::size_t> targetRows;

        for (std::size_t i = 0; i < model.hosts.size(); ++i)
        {
            if (model.hosts[i].enabled)
            {
                targetRows.push_back(i);
            }
        }

        if (!demoMode && targets.size() != targetRows.size())
        {
            throw std::logic_error("Batch targets do not match TUI rows");
        }

        if (!demoMode)
        {
            for (std::size_t i = 0; i < targets.size(); ++i)
            {
                model.hosts[targetRows[i]].replyTimeout =
                    targets[i].options.replyTimeout;
            }
        }

        std::atomic<std::size_t> completedCount{0};
        std::future<BatchResults> pendingBatch;
        bool quitAfterBatch = false;

        using Clock = std::chrono::steady_clock;
        constexpr auto batchInterval = std::chrono::milliseconds{500};

        bool continuousMode = false;
        bool monitoring = false;
        Clock::time_point nextBatchAt{};

        std::string batchStatus = targets.empty()
                                      ? "No enabled hosts"
                                      : "Ready to check";

        auto startBatch = [&]() -> bool
        {
            if (pendingBatch.valid() || targets.empty() || quitAfterBatch)
            {
                return false;
            }

            try
            {
                completedCount.store(0);

                pendingBatch = std::async(
                    std::launch::async,
                    [job = targets, &completedCount]
                    {
                        return runBatch(job, completedCount);
                    });

                return true;
            }
            catch (const std::exception &error)
            {
                batchStatus = std::string{"Could not start: "} + error.what();
                return false;
            }
        };

        auto screen = ScreenInteractive::Fullscreen();

        auto renderer = Renderer([&]() -> Element
                                 {
            const auto terminalSize = Terminal::Size();

            constexpr int minimumWidth = 43;
            constexpr int minimumHeight = 20;

            if (terminalSize.dimx < minimumWidth ||
                terminalSize.dimy < minimumHeight) {
                return vbox({
                    paragraph("Resize the terminal to at least 43 × 20."),
                    paragraph(
                        "Current size: " +
                        std::to_string(terminalSize.dimx) + " × " +
                        std::to_string(terminalSize.dimy)),
                    text("q: exit"),
                }) | border;
            }

            Elements sections;

            sections.push_back(
                text(demoMode
                    ? "Pings — Offline demo"
                    : "Pings — Host monitor")
                | bold);

            // In a narrow terminal, leave more room for the table.
            // The selected host's status text is still visible in the details panel.
            if (terminalSize.dimx >= 100) {
                sections.push_back(
                    paragraph(
                        "Green: reply | Red: timeout | Yellow: error | Gray: no data"));
            }

            std::string visibleBatchStatus = batchStatus;

            if (quitAfterBatch) {
                visibleBatchStatus = "Exiting after this pass";
            } else if (pendingBatch.valid()) {
                if (continuousMode && !monitoring) {
                    visibleBatchStatus = "Stopping after this pass";
                } else {
                    visibleBatchStatus =
                        "Checked: " + std::to_string(completedCount.load()) +
                        " of " + std::to_string(targets.size());
                }
            } else if (monitoring) {
                visibleBatchStatus = "Waiting for the next pass";
            }

            if (!demoMode) {
                sections.push_back(text(continuousMode
                    ? "Mode: Continuous"
                    : "Mode: Single check"));
            }

            sections.push_back(text(
                !demoMode ? visibleBatchStatus
                        : model.paused ? "Status: Paused" : "Status: Demo running"));

            sections.push_back(separator());

            sections.push_back(
                renderTable(model, terminalSize.dimx)
                | vscroll_indicator
                | frame
                | flex);

            sections.push_back(separator());

            // Keep long host names and addresses from crowding out the table.
            sections.push_back(
                renderDetails(model)
                | frame
                | size(HEIGHT, LESS_THAN, 4));

            sections.push_back(separator());

            sections.push_back(paragraph(
                demoMode
                    ? "↑/↓: select | n: step | p: pause | c: clear | q: quit"
                    : "↑/↓: select | m: mode | s: start | c: stop | q: quit"));

            return vbox(std::move(sections)) | border; });

        auto root = CatchEvent(renderer, [&](Event event)
                               {
                                if (event == Event::Character('q'))
                                {
                                    monitoring = false;

                                    if (pendingBatch.valid()) {
                                        quitAfterBatch = true;
                                        batchStatus = "Exiting after this pass";
                                        return true;
                                    }

                                    screen.ExitLoopClosure()();
                                    return true;
                                }
                                if (event == Event::ArrowUp)
                                {
                                    moveSelection(model, -1);
                                    return true;
                                }
                                if (event == Event::ArrowDown)
                                {
                                    moveSelection(model, 1);
                                    return true;
                                }
                                if (!demoMode) {
                                    if (event == Event::Character('m')) {
                                        if (!pendingBatch.valid() && !monitoring && !quitAfterBatch) {
                                            continuousMode = !continuousMode;
                                        }
                                        return true;
                                    }

                                    if (event == Event::Character('s')) {
                                        if (!monitoring && startBatch()) {
                                            monitoring = continuousMode;
                                        }
                                        return true;
                                    }

                                    if (event == Event::Character('c')) {
                                        if (monitoring) {
                                            monitoring = false;
                                            batchStatus = pendingBatch.valid()
                                                ? "Stopping after this pass"
                                                : "Monitoring stopped";
                                        }
                                        return true;
                                    }

                                    if (event != Event::Custom) {
                                        return false;
                                    }

                                    if (!pendingBatch.valid()) {
                                        if (monitoring && Clock::now() >= nextBatchAt) {
                                            if (!startBatch()) {
                                                monitoring = false;
                                            }
                                        }
                                        return true;
                                    }

                                    if (pendingBatch.wait_for(std::chrono::seconds{0}) !=
                                        std::future_status::ready) {
                                        return true;
                                    }

                                    try {
                                        const auto results = pendingBatch.get();

                                        if (results.size() != targetRows.size()) {
                                            throw std::runtime_error(
                                                "Unexpected number of batch results");
                                        }

                                        for (std::size_t i = 0; i < results.size(); ++i) {
                                            appendSample(model.hosts[targetRows[i]], toSample(results[i]));
                                        }

                                        batchStatus = "Check complete: " +
                                            std::to_string(results.size()) + " of " +
                                            std::to_string(targets.size());

                                        if (monitoring) {
                                            nextBatchAt = Clock::now() + batchInterval;
                                        }
                                    } catch (const std::exception& error) {
                                        monitoring = false;
                                        batchStatus = std::string{"Check failed: "} + error.what();
                                    }

                                    if (quitAfterBatch) {
                                        screen.ExitLoopClosure()();
                                    }

                                    return true;
                                }
                                if (event == Event::Character('p')) {
                                    model.paused = !model.paused;
                                    return true;
                                }
                                if (event == Event::Character('n')) {
                                    advanceDemo(model);
                                    return true;
                                }
                                if (event == Event::Character('c')) {
                                    clearHistory(model);
                                    return true;
                                }
                                if (event == Event::Custom) {
                                    if (!model.paused) {
                                        advanceDemo(model);
                                    }
                                    return true;
                                }
                                return false; });

        std::jthread ticker([&screen, demoMode](std::stop_token stop)
                            {
                using namespace std::chrono_literals;

                while (!stop.stop_requested()) {
                    std::this_thread::sleep_for(demoMode ? 1000ms : 100ms);
                    if (stop.stop_requested()) {
                        break;
                    }

                    screen.PostEvent(Event::Custom);
                } });

        screen.Loop(root);
        ticker.request_stop();

        return 0;
    }
} // namespace ui
