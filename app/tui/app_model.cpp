#include "app_model.hpp"

#include <algorithm>

namespace ui
{

    void appendSample(HostRow &host, PingSample sample)
    {
        host.history.push_back(sample);

        if (host.history.size() > historyCapacity)
        {
            host.history.pop_front();
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

    HistorySummary summarizeHistory(const HostRow &host)
    {
        HistorySummary summary;

        for (const auto &sample : host.history)
        {
            switch (sample.status)
            {
            case SampleStatus::Reply:
                ++summary.replies;
                break;

            case SampleStatus::Timeout:
                ++summary.timeouts;
                break;

            case SampleStatus::Error:
                ++summary.errors;
                break;
            }
        }

        return summary;
    }
} // namespace ui