#include <algorithm>
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
                return "Ответ";
            case SampleStatus::Timeout:
                return "Тайм-аут";
            case SampleStatus::Error:
                return "Ошибка";
            }

            return "Неизвестно";
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

            // Если полная таблица оставляет слишком мало места
            // для истории, скрываем имя и текстовый статус.
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

            std::string status = host.enabled ? "Нет данных" : "Отключён";
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
                return text("Хосты не добавлены");
            }

            const auto layout = makeTableLayout(terminalWidth);

            if (!layout)
            {
                return paragraph(
                    "Недостаточно места для таблицы. Расширьте терминал.");
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
                return text("Нет выбранного хоста");
            }

            std::string status = host->enabled ? "Нет данных" : "Отключён";

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

            std::string statusLine = "Статус: " + status;

            if (host->replyTimeout)
            {
                statusLine = "Лимит ответа: " +
                             std::to_string(host->replyTimeout->count()) +
                             " мс | " + statusLine;
            }

            return vbox({
                paragraph("Выбран: " + host->name + " | " + host->address),
                paragraph(statusLine),
                paragraph(
                    "В истории: " + std::to_string(host->history.size()) +
                    " | Ответов: " + std::to_string(summary.replies) +
                    " | Тайм-аутов: " + std::to_string(summary.timeouts) +
                    " | Ошибок: " + std::to_string(summary.errors)),
            });
        }

        using BatchResults = std::vector<pings::PingResult>;

        BatchResults runBatch(const std::vector<pings::PingTarget> &targets)
        {
            std::optional<BatchResults> completed;
            auto pinger = pings::AsyncPinger::create();

            [[maybe_unused]] auto handle = pinger->pingBatch(
                targets,
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

        std::future<BatchResults> pendingBatch;
        bool quitAfterBatch = false;

        std::string batchStatus = targets.empty()
                                      ? "Нет включённых хостов"
                                      : "Готово к проверке";

        auto screen = ScreenInteractive::Fullscreen();

        auto renderer = Renderer([&]() -> Element
                                 {
            const auto terminalSize = Terminal::Size();

            constexpr int minimumWidth = 43;
            constexpr int minimumHeight = 20;

            if (terminalSize.dimx < minimumWidth ||
                terminalSize.dimy < minimumHeight) {
                return vbox({
                    paragraph("Увеличьте окно терминала до 43 × 20."),
                    paragraph(
                        "Сейчас: " +
                        std::to_string(terminalSize.dimx) + " × " +
                        std::to_string(terminalSize.dimy)),
                    text("q: выход"),
                }) | border;
            }

            Elements sections;

            sections.push_back(
                text(demoMode
                    ? "Pings — демонстрационный монитор"
                    : "Pings — хосты из конфигурации")
                | bold);

            // В узком окне освобождаем место для таблицы.
            // Текстовый статус выбранного хоста есть в подробностях.
            if (terminalSize.dimx >= 100) {
                sections.push_back(
                    paragraph(
                        "Зелёный: ответ | Красный: тайм-аут | "
                        "Жёлтый: ошибка | Серый: нет измерения"));
            }

            sections.push_back(text(
                !demoMode ? batchStatus
                        : model.paused ? "Состояние: пауза"
                                        : "Состояние: демонстрация"));

            sections.push_back(separator());

            sections.push_back(
                renderTable(model, terminalSize.dimx)
                | vscroll_indicator
                | frame
                | flex);

            sections.push_back(separator());

            // Длинное имя или адрес не должны вытеснить таблицу.
            sections.push_back(
                renderDetails(model)
                | frame
                | size(HEIGHT, LESS_THAN, 4));

            sections.push_back(separator());

            sections.push_back(paragraph(
                demoMode
                    ? "↑/↓: выбор | n: шаг | p: пауза | c: очистить | q: выход"
                    : "↑/↓: выбор | s: проверка | q: выход"));

            return vbox(std::move(sections)) | border; });

        auto root = CatchEvent(renderer, [&](Event event)
                               {
                                if (event == Event::Character('q'))
                                {
                                    if (pendingBatch.valid()) {
                                        quitAfterBatch = true;
                                        batchStatus = "Выход после завершения проверки";
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
                                    if (event == Event::Character('s')) {
                                        if (!pendingBatch.valid() && !targets.empty()) {
                                            try {
                                                pendingBatch = std::async(
                                                    std::launch::async,
                                                    [job = targets] {
                                                        return runBatch(job);
                                                    });
                                                batchStatus = "Проверка выполняется";
                                            } catch (const std::exception& error) {
                                                batchStatus = std::string{"Не удалось начать: "} +
                                                            error.what();
                                            }
                                        }
                                        return true;
                                    }

                                    if (event == Event::Custom &&
                                        pendingBatch.valid() &&
                                        pendingBatch.wait_for(std::chrono::seconds{0}) ==
                                            std::future_status::ready) {
                                        try {
                                            const auto results = pendingBatch.get();

                                            if (results.size() != targetRows.size()) {
                                                throw std::runtime_error(
                                                    "Unexpected number of batch results");
                                            }

                                            for (std::size_t i = 0; i < results.size(); ++i) {
                                                appendSample(
                                                    model.hosts[targetRows[i]],
                                                    toSample(results[i]));
                                            }

                                            batchStatus = "Проверка завершена";
                                        } catch (const std::exception& error) {
                                            batchStatus = std::string{"Ошибка проверки: "} + error.what();
                                        }

                                        if (quitAfterBatch) {
                                            screen.ExitLoopClosure()();
                                        }

                                        return true;
                                    }

                                    return false;
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

        // std::jthread ticker([&screen](std::stop_token stop)
        //                     {
        //     using namespace std::chrono_literals;

        //     while (!stop.stop_requested()) {
        //         std::this_thread::sleep_for(1s);

        //         if (stop.stop_requested()){
        //             break;
        //         }

        //         screen.PostEvent(Event::Custom);
        //     } });

        // screen.Loop(root);
        // ticker.request_stop();

        // std::jthread ticker;

        // if (demoMode)
        // {
        std::jthread ticker([&screen](std::stop_token stop)
                            {
                using namespace std::chrono_literals;

                while (!stop.stop_requested()) {
                    std::this_thread::sleep_for(1s);

                    if (stop.stop_requested()) {
                        break;
                    }

                    screen.PostEvent(Event::Custom);
                } });
        // }

        screen.Loop(root);
        ticker.request_stop();

        return 0;
    }
} // namespace ui
