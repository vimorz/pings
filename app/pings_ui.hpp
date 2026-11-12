#pragma once

#include <vector>

#include <pings/ping_target.hpp>

namespace ui
{
    struct AppModel;

    enum class UiMode {
        Demo,
        Config,
    };

    // Пользовательский интерфейс
    int ui(AppModel model, UiMode mode, std::vector<pings::PingTarget> targets = {});

} // namespace ui