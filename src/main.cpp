/**
 * @file main.cpp
 * @brief Main entry point for Diretta UPnP Renderer
 */

#include "DirettaRenderer.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <thread>
#include <chrono>

// Global renderer instance for signal handler
std::unique_ptr<DirettaRenderer> g_renderer;

// Signal handler for clean shutdown
void signalHandler(int signal) {
    std::cout << "\n⚠️  Signal " << signal << " received, shutting down..." << std::endl;
    if (g_renderer) {
        g_renderer->stop();
    }
    exit(0);
}

// Parse command line arguments
DirettaRenderer::Config parseArguments(int argc, char* argv[]) {
    DirettaRenderer::Config config;
    
    // Defaults
    config.name = "Diretta Renderer";
    config.port = 0;  // 0 = auto
    config.gaplessEnabled = true;
    config.bufferSeconds =10;  // ⭐ 4 secondes minimum (essentiel pour DSD!)
    
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
        else if ((arg == "--buffer" || arg == "-b") && i + 1 < argc) {
            config.bufferSeconds = std::atof(argv[++i]);  // ⭐ atof pour supporter décimales
            if (config.bufferSeconds < 10) {
                std::cerr << "⚠️  Warning: Buffer < 2 seconds may cause issues with DSD/Hi-Res!" << std::endl;
            }
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Diretta UPnP Renderer\n\n"
                      << "Usage: " << argv[0] << " [options]\n\n"
                      << "Options:\n"
                      << "  --name, -n <name>     Renderer name (default: Diretta Renderer)\n"
                      << "  --port, -p <port>     UPnP port (default: auto)\n"
                      << "  --uuid <uuid>         Device UUID (default: auto-generated)\n"
                      << "  --no-gapless          Disable gapless playback\n"
                      << "  --buffer, -b <secs>   Buffer size in seconds (default: 4)\n"
                      << "  --help, -h            Show this help\n"
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
    // Setup signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    std::cout << "═══════════════════════════════════════════════════════\n"
              << "  🎵 Diretta UPnP Renderer - Complete Edition\n"
              << "═══════════════════════════════════════════════════════\n"
              << std::endl;
    
    // Parse arguments
    DirettaRenderer::Config config = parseArguments(argc, argv);
    
    // Display configuration
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Name:        " << config.name << std::endl;
    std::cout << "  Port:        " << (config.port == 0 ? "auto" : std::to_string(config.port)) << std::endl;
    std::cout << "  Gapless:     " << (config.gaplessEnabled ? "enabled" : "disabled") << std::endl;
    std::cout << "  Buffer:      " << config.bufferSeconds << " seconds" << std::endl;
    std::cout << "  UUID:        " << config.uuid << std::endl;
    std::cout << std::endl;
    
    try {
        // Create renderer
        g_renderer = std::make_unique<DirettaRenderer>(config);
        
        std::cout << "🚀 Starting renderer..." << std::endl;
        
        // Start renderer
        if (!g_renderer->start()) {
            std::cerr << "❌ Failed to start renderer" << std::endl;
            return 1;
        }
        
        std::cout << "✓ Renderer started successfully!" << std::endl;
        std::cout << std::endl;
        std::cout << "📡 Waiting for UPnP control points..." << std::endl;
        std::cout << "   (Press Ctrl+C to stop)" << std::endl;
        std::cout << std::endl;
        
        // Main loop - just wait
        while (g_renderer->isRunning()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "❌ Exception: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "\n✓ Renderer stopped" << std::endl;
    
    return 0;
}
