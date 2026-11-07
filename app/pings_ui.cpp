#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <thread>
#include <string>
#include <vector>

#include "pings_ui.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace ui
{
    namespace
    {
        constexpr std::size_t historyCapacity = 60;

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
        };

        struct HostRow
        {
            std::string name;
            std::string address;
            std::deque<PingSample> history;
        };

        struct AppModel
        {
            std::vector<HostRow> hosts;
            int selected = 0;

            bool paused = false;
            unsigned int demoTick = 0;
        };

        void appendSample(HostRow &host, PingSample sample)
        {
            host.history.push_back(sample);

            if (host.history.size() > historyCapacity)
            {
                host.history.pop_front();
            }
        }

        std::string formatLatency(const std::optional<int> &latency)
        {
            if (!latency)
            {
                return "-";
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

        AppModel makeDemoModel()
        {
            AppModel model{
                {
                    {"Google DNS", "8.8.8.8", {}},
                    {"Cloudflare", "1.1.1.1", {}},
                    {"Local IPv6", "::1", {}},
                    {"Example", "192.0.2.1", {}},
                    {"Google DNS", "8.8.8.8", {}},
                    {"Cloudflare", "1.1.1.1", {}},
                    {"Local IPv6", "::1", {}},
                    {"Example", "192.0.2.1", {}},
                    {"Google DNS", "8.8.8.8", {}},
                    {"Cloudflare", "1.1.1.1", {}},
                    {"Local IPv6", "::1", {}},
                    {"Example", "192.0.2.1", {}},
                },
                0,
            };

            for (std::size_t row = 0; row < model.hosts.size(); ++row)
            {
                for (int tick = 0; tick < 45; ++tick)
                {
                    const bool timeout =
                        (tick + static_cast<int>(row) * 3) % 13 == 0;

                    appendSample(
                        model.hosts[row],
                        {
                            timeout ? SampleStatus::Timeout
                                    : SampleStatus::Reply,
                            timeout ? std::optional<int>{}
                                    : std::optional<int>{10 + tick % 17},
                        });
                }
            }

            return model;
        }

        void advanceDemo(AppModel &model)
        {
            ++model.demoTick;

            for (std::size_t i = 0; i < model.hosts.size(); ++i)
            {
                const auto value =
                    model.demoTick + static_cast<unsigned int>(i) * 7u;

                PingSample sample{
                    SampleStatus::Reply,
                    static_cast<int>(5u + value % 40u),
                };

                if (value % 19u == 0u)
                {
                    sample = {SampleStatus::Error, std::nullopt};
                }
                else if (value % 7u == 0u)
                {
                    sample = {SampleStatus::Timeout, std::nullopt};
                }

                appendSample(model.hosts[i], sample);
            }
        }

        void clearHistory(AppModel &model)
        {
            for (auto &host : model.hosts)
            {
                host.history.clear();
            }
        }

        const HostRow *selectedHost(const AppModel &model)
        {
            if (model.selected < 0 ||
                static_cast<std::size_t>(model.selected) >= model.hosts.size())
            {
                return nullptr;
            }

            return &model.hosts[static_cast<std::size_t>(model.selected)];
        }

        void moveSelection(AppModel &model, int offset)
        {
            if (model.hosts.empty())
            {
                model.selected = 0;
                return;
            }

            const auto last = static_cast<int>(model.hosts.size()) - 1;
            model.selected = std::clamp(model.selected + offset, 0, last);
        }

        ftxui::Element renderHistory(const HostRow &host)
        {
            using namespace ftxui;

            Elements cells;

            // Неизмеренная часть находится слева:
            // новые результаты появляются справа.
            const auto emptyCount = historyCapacity - host.history.size();

            for (std::size_t i = 0; i < emptyCount; ++i)
            {
                cells.push_back(text(" ") | bgcolor(Color::GrayDark));
            }

            for (const auto &sample : host.history)
            {
                cells.push_back(text(" ") | bgcolor(statusColor(sample.status)));
            }

            return hbox(std::move(cells));
        }

        ftxui::Element renderHost(const HostRow &host, bool selected)
        {
            using namespace ftxui;

            std::string status = "Нет данных";
            std::string latency = "—";
            ftxui::Color currentColor = Color::Red;

            if (!host.history.empty())
            {
                const auto &latest = host.history.back();
                status = statusText(latest.status);
                latency = formatLatency(latest.latencyMs);
                currentColor = statusColor(latest.status);
            }

            auto row = hbox({
                text(selected ? "> " : "  "),
                text(host.name) | size(WIDTH, EQUAL, 15),
                text(host.address) | size(WIDTH, EQUAL, 20),
                text(latency) | size(WIDTH, EQUAL, 10),
                text(status) | color(currentColor) | size(WIDTH, EQUAL, 12),
                text(" ["),
                renderHistory(host),
                text("]"),
            });

            if (selected)
            {
                // focus сообщает frame, какую строку держать видимой.
                row = row | bold | focus;
            }

            return row;
        }

        ftxui::Element renderTable(const AppModel &model)
        {
            using namespace ftxui;

            if (model.hosts.empty())
            {
                return text("Хосты не добавлены");
            }

            Elements rows;

            for (std::size_t i = 0; i < model.hosts.size(); ++i)
            {
                rows.push_back(renderHost(
                    model.hosts[i],
                    static_cast<int>(i) == model.selected));
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

            return vbox({
                paragraph("Выбран: " + host->name + " | " + host->address),
                text("Результатов в истории: " +
                     std::to_string(host->history.size())),
            });
        }
    } // namespace

    int ui()
    {
        using namespace ftxui;

        auto model = makeDemoModel();
        // AppModel model;
        auto screen = ScreenInteractive::Fullscreen();

        auto renderer = Renderer(
            [&]
            {
                return vbox({
                           text("Pings - демонстрацинный монитор") | bold,
                           text("Зелёный: ответ | Красный: тайм-аут | "
                                "Жёлтый: ошибка | Серый: нет измерения"),
                           text(model.paused
                                    ? "Состояние: пауза"
                                    : "Состояние: демонстрация"),
                           separator(),

                           renderTable(model) | vscroll_indicator | frame | flex,

                           separator(),
                           renderDetails(model),
                           separator(),
                           text("↑/↓: выбор | n: шаг | p: пауза | c: очистить | q: выход"),
                       }) |
                       border;
            });

        auto root = CatchEvent(renderer, [&](Event event)
                               {
                                if (event == Event::Character('q'))
                                {
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

        std::jthread ticker([&screen](std::stop_token stop)
                            {
            using namespace std::chrono_literals;

            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(1s);

                if (stop.stop_requested()){
                    break;
                }

                screen.PostEvent(Event::Custom);
            } });

        screen.Loop(root);
        ticker.request_stop();

        return 0;
    }
} // namespace ui
