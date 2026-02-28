/**
 * @file main.cpp
 * @brief Main entry point for Diretta UPnP Renderer (Simplified Architecture)
 */

#include "DirettaRenderer.h"
#include "DirettaSync.h"
#include "LogLevel.h"
#include "TimestampedLogger.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <thread>
#include <chrono>
#include <iomanip>

#define RENDERER_VERSION "2.0.6"
#define RENDERER_BUILD_DATE __DATE__
#define RENDERER_BUILD_TIME __TIME__

std::unique_ptr<DirettaRenderer> g_renderer;

// Async logging infrastructure (A3 optimization)
// Declared here (before shutdownAsyncLogging) to avoid forward reference
LogRing* g_logRing = nullptr;
std::atomic<bool> g_logDrainStop{false};
std::thread g_logDrainThread;

// Cleanup async logging thread (must be called before exit)
void shutdownAsyncLogging() {
    if (g_logRing) {
        g_logDrainStop.store(true, std::memory_order_release);
        if (g_logDrainThread.joinable()) {
            g_logDrainThread.join();
        }
        delete g_logRing;
        g_logRing = nullptr;
    }
}

void signalHandler(int signal) {
    std::cout << "\nSignal " << signal << " received, shutting down..." << std::endl;
    if (g_renderer) {
        g_renderer->stop();
    }
    shutdownAsyncLogging();
    exit(0);
}

void statsSignalHandler(int /*signal*/) {
    if (g_renderer) {
        g_renderer->dumpStats();
    }
}

bool g_verbose = false;
LogLevel g_logLevel = LogLevel::INFO;

void logDrainThreadFunc() {
    LogEntry entry;
    while (!g_logDrainStop.load(std::memory_order_acquire)) {
        // Drain all pending log entries
        while (g_logRing && g_logRing->pop(entry)) {
            std::cout << "[" << (entry.timestamp_us / 1000) << "ms] "
                      << entry.message << std::endl;
        }
        // Sleep briefly to avoid busy-wait
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Final drain on shutdown
    while (g_logRing && g_logRing->pop(entry)) {
        std::cout << "[" << (entry.timestamp_us / 1000) << "ms] "
                  << entry.message << std::endl;
    }
}

void listTargets() {
    std::cout << "════════════════════════════════════════════════════════\n"
              << "  Scanning for Diretta Targets...\n"
              << "════════════════════════════════════════════════════════\n" << std::endl;

    DirettaSync::listTargets();

    std::cout << "\nUsage:\n";
    std::cout << "   Target #1: sudo ./bin/DirettaRendererUPnP --target 1\n";
    std::cout << "   Target #2: sudo ./bin/DirettaRendererUPnP --target 2\n";
    std::cout << std::endl;
}

DirettaRenderer::Config parseArguments(int argc, char* argv[]) {
    DirettaRenderer::Config config;

    config.name = "Diretta Renderer";
    config.port = 0;
    config.gaplessEnabled = true;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if ((arg == "--name" || arg == "-n") && i + 1 < argc) {
            config.name = argv[++i];
        }
        else if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            config.port = std::atoi(argv[++i]);
        }
        else if (arg == "--uuid" && i + 1 < argc) {
            config.uuid = argv[++i];
        }
        else if (arg == "--no-gapless") {
            config.gaplessEnabled = false;
        }
        else if ((arg == "--target" || arg == "-t") && i + 1 < argc) {
            config.targetIndex = std::atoi(argv[++i]) - 1;
            if (config.targetIndex < 0) {
                std::cerr << "Invalid target index. Must be >= 1" << std::endl;
                exit(1);
            }
        }
        else if (arg == "--interface" && i + 1 < argc) {
            config.networkInterface = argv[++i];
        }
        else if (arg == "--list-targets" || arg == "-l") {
            listTargets();
            exit(0);
        }
        else if (arg == "--version" || arg == "-V") {
            std::cout << "═══════════════════════════════════════════════════════" << std::endl;
            std::cout << "  Diretta UPnP Renderer - Version " << RENDERER_VERSION << std::endl;
            std::cout << "═══════════════════════════════════════════════════════" << std::endl;
            std::cout << "Build: " << RENDERER_BUILD_DATE << " " << RENDERER_BUILD_TIME << std::endl;
            std::cout << "Architecture: Simplified (DirettaSync unified)" << std::endl;
            std::cout << "═══════════════════════════════════════════════════════" << std::endl;
            exit(0);
        }
        else if (arg == "--verbose" || arg == "-v") {
            g_verbose = true;
            g_logLevel = LogLevel::DEBUG;
            std::cout << "Verbose mode enabled (log level: DEBUG)" << std::endl;
        }
        else if (arg == "--quiet" || arg == "-q") {
            g_logLevel = LogLevel::WARN;
            std::cout << "Quiet mode enabled (log level: WARN)" << std::endl;
        }
        // Advanced Diretta SDK settings
        else if (arg == "--thread-mode" && i + 1 < argc) {
            config.threadMode = std::atoi(argv[++i]);
        }
        else if (arg == "--cycle-time" && i + 1 < argc) {
            config.cycleTime = std::atoi(argv[++i]);
            if (config.cycleTime < 333 || config.cycleTime > 10000) {
                std::cerr << "Warning: cycle-time should be between 333-10000 us" << std::endl;
            }
        }
        else if (arg == "--info-cycle" && i + 1 < argc) {
            config.infoCycle = std::atoi(argv[++i]);
        }
        else if (arg == "--cycle-min-time" && i + 1 < argc) {
            config.cycleMinTime = std::atoi(argv[++i]);
        }
        else if (arg == "--transfer-mode" && i + 1 < argc) {
            config.transferMode = argv[++i];
            if (config.transferMode != "auto" && config.transferMode != "varmax" &&
                config.transferMode != "varauto" && config.transferMode != "fixauto" &&
                config.transferMode != "random") {
                std::cerr << "Invalid transfer-mode. Use: auto, varmax, varauto, fixauto, random" << std::endl;
                exit(1);
            }
        }
        else if (arg == "--target-profile-limit" && i + 1 < argc) {
            config.targetProfileLimitTime = std::atoi(argv[++i]);
        }
        else if (arg == "--mtu" && i + 1 < argc) {
            config.mtu = std::atoi(argv[++i]);
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Diretta UPnP Renderer (Simplified Architecture)\n\n"
                      << "Usage: " << argv[0] << " [options]\n\n"
                      << "Options:\n"
                      << "  --name, -n <name>     Renderer name (default: Diretta Renderer)\n"
                      << "  --port, -p <port>     UPnP port (default: auto)\n"
                      << "  --uuid <uuid>         Device UUID (default: auto-generated)\n"
                      << "  --no-gapless          Disable gapless playback\n"
                      << "  --target, -t <index>  Select Diretta target by index (1, 2, 3...)\n"
                      << "  --interface <name>    Network interface to bind (e.g., eth0)\n"
                      << "  --list-targets, -l    List available Diretta targets and exit\n"
                      << "  --verbose, -v         Enable verbose debug output (log level: DEBUG)\n"
                      << "  --quiet, -q           Quiet mode - only errors and warnings (log level: WARN)\n"
                      << "  --version, -V         Show version information\n"
                      << "  --help, -h            Show this help\n"
                      << "\n"
                      << "Advanced Diretta SDK settings:\n"
                      << "  --thread-mode <mode>       SDK thread mode bitmask (default: 1=CRITICAL)\n"
                      << "                             Flags: 1=CRITICAL, 2=NOSHORTSLEEP, 4=NOSLEEP4CORE,\n"
                      << "                             8=SOCKETNOBLOCK, 16=OCCUPIED, 2048=NOSLEEPFORCE,\n"
                      << "                             8192=NOJUMBOFRAME, 16384=NOFIREWALL, 32768=NORAWSOCKET\n"
                      << "  --cycle-time <us>          Max cycle time in microseconds (333-10000, default: auto)\n"
                      << "  --cycle-min-time <us>      Min cycle time in microseconds (random mode only)\n"
                      << "  --info-cycle <us>          Info packet cycle in microseconds (default: 100000)\n"
                      << "  --transfer-mode <mode>     Transfer mode: auto, varmax, varauto, fixauto, random\n"
                      << "  --target-profile-limit <us> Target profile limit time (0=self, default: 200)\n"
                      << "  --mtu <bytes>              MTU override (default: auto-detect)\n"
                      << std::endl;
            exit(0);
        }
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            std::cerr << "Use --help for usage information" << std::endl;
            exit(1);
        }
    }

    return config;
}

int main(int argc, char* argv[]) {
    // Install timestamped logging (MUST BE FIRST!)
    TimestampedStreambuf* coutBuf = nullptr;
    TimestampedStreambuf* cerrBuf = nullptr;
    installTimestampedLogging(coutBuf, cerrBuf);

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGUSR1, statsSignalHandler);

    std::cout << "═══════════════════════════════════════════════════════\n"
              << "  Diretta UPnP Renderer v" << RENDERER_VERSION << "\n"
              << "═══════════════════════════════════════════════════════\n"
              << std::endl;

    DirettaRenderer::Config config = parseArguments(argc, argv);

    // Initialize async logging ring buffer (A3 optimization)
    // Only active in verbose mode to avoid overhead in production
    if (g_verbose) {
        g_logRing = new LogRing();
        g_logDrainThread = std::thread(logDrainThreadFunc);
    }

    std::cout << "Configuration:" << std::endl;
    std::cout << "  Name:     " << config.name << std::endl;
    std::cout << "  Port:     " << (config.port == 0 ? "auto" : std::to_string(config.port)) << std::endl;
    std::cout << "  Gapless:  " << (config.gaplessEnabled ? "enabled" : "disabled") << std::endl;
    if (!config.networkInterface.empty()) {
        std::cout << "  Network:  " << config.networkInterface << std::endl;
    }
    std::cout << "  UUID:     " << config.uuid << std::endl;
    std::cout << std::endl;

    try {
        g_renderer = std::make_unique<DirettaRenderer>(config);

        std::cout << "Starting renderer..." << std::endl;

        if (!g_renderer->start()) {
            std::cerr << "Failed to start renderer" << std::endl;
            shutdownAsyncLogging();
            return 1;
        }

        std::cout << "Renderer started!" << std::endl;

        std::cout << std::endl;
        std::cout << "Waiting for UPnP control points..." << std::endl;
        std::cout << "(Press Ctrl+C to stop)" << std::endl;
        std::cout << std::endl;

        while (g_renderer->isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        shutdownAsyncLogging();
        return 1;
    }

    std::cout << "\nRenderer stopped" << std::endl;
    shutdownAsyncLogging();

    return 0;
}
