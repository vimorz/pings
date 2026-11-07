#include <exception>
#include <iostream>

#include "pings_ui.hpp"

int main()
{
    try {
        return ui::ui();
    } catch (const std::exception& error) {
        std::cerr << "TUI error: " << error.what() << '\n';
        return 1;
    }
}