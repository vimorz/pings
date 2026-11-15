#include "demo_source.hpp"

namespace ui
{

    // Сюда перенеси существующие определения:
    //
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

} // namespace ui