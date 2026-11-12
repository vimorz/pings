#include <exception>
#include <filesystem>
#include <iostream>
#include <utility>

#include "pings_ui.hpp"
#include "tui/app_model.hpp"
#include "tui/demo_source.hpp"

#include <pings/config/config_loader.hpp>
#include <pings/config/config_mapping.hpp>
#include <pings/config/config_validation.hpp>

int main(int argc, char *argv[])
{
    try
    {
        if (argc == 1)
        {
            return ui::ui(ui::makeDemoModel(), ui::UiMode::Demo);
        }

        if (argc != 2)
        {
            std::cerr << "Usage: pings_tui [config.yml]\n";
            return 2;
        }

        const std::filesystem::path path{argv[1]};

        const auto loaded = pings::config::loadConfig(path);

        if (!loaded.success())
        {
            std::cerr << "Cannot load config: " << path << '\n';

            for (const auto &error : loaded.errors)
            {
                std::cerr << "  [" << pings::config::toString(error.code)
                          << "] ";

                if (error.line != 0)
                {
                    std::cerr << error.line << ':' << error.column << ": ";
                }

                std::cerr << error.message << '\n';
            }

            return 1;
        }

        const auto &config = *loaded.config;
        const auto validation = pings::config::validate(config);

        if (!validation.valid())
        {
            std::cerr << "Invalid config: " << path << '\n';

            for (const auto &error : validation.errors)
            {
                std::cerr << "  " << error.field << ": "
                          << error.message << '\n';
            }

            return 1;
        }

        ui::AppModel model;
        model.hosts.reserve(config.hosts.size());

        for (const auto &host : config.hosts)
        {
            model.hosts.push_back({
                host.name.empty() ? host.host : host.name,
                host.host,
                {},
                host.enabled,
            });
        }

        return ui::ui(std::move(model), ui::UiMode::Config, pings::config::makePingTargets(config));
    }

    catch (const std::exception &error)
    {
        std::cerr << "TUI error: " << error.what() << '\n';
        return 1;
    }
}