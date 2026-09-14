#include "app/App.h"
#include "core/Log.h"

#include <exception>

int main(int argc, char** argv) {
    try {
        space::App app(argc, argv);
        return app.run();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: {}", e.what());
        return 1;
    }
}
