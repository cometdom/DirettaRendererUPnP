/**
 * @file main.cpp
 * @brief Main entry point for Diretta UPnP Renderer
 */

#include "DirettaRenderer.h"
#include "DirettaOutput.h"
#include <iostream>
#include <csignal>
#include <memory>
#include <thread>
#include <chrono>

// Version information
#define RENDERER_VERSION "1.0.6"
#define RENDERER_BUILD_DATE __DATE__
#define RENDERER_BUILD_TIME __TIME__
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
 // Variable globale pour le mode verbose
   bool g_verbose = false;

// List available Diretta targets
void listTargets() {
    std::cout << "════════════════════════════════════════════════════════\n"
              << "  🔍 Scanning for Diretta Targets...\n"
              << "════════════════════════════════════════════════════════\n" << std::endl;
    
    DirettaOutput output;
    output.listAvailableTargets();
    
    std::cout << "\n💡 Usage Examples:\n";
    std::cout << "   To use target #1: " << "sudo ./bin/DirettaRendererUPnP --target 1\n";
    std::cout << "   To use target #2: " << "sudo ./bin/DirettaRendererUPnP --target 2\n";
    std::cout << "   Interactive mode: " << "sudo ./bin/DirettaRendererUPnP\n";
    std::cout << std::endl;
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
        else if ((arg == "--target" || arg == "-t") && i + 1 < argc) {
            config.targetIndex = std::atoi(argv[++i]) - 1;  // Convert to 0-based index
            if (config.targetIndex < 0) {
                std::cerr << "❌ Invalid target index. Must be >= 1" << std::endl;
                exit(1);
            }
        }
        else if (arg == "--list-targets" || arg == "-l") {
            listTargets();
            exit(0);
        }
        else if (arg == "--version" || arg == "-v") {
             std::cout << "═══════════════════════════════════════════════════════" << std::endl;
             std::cout << "  Diretta UPnP Renderer - Version " << RENDERER_VERSION << std::endl;
             std::cout << "═══════════════════════════════════════════════════════" << std::endl;
             std::cout << "Build: " << RENDERER_BUILD_DATE << " " << RENDERER_BUILD_TIME << std::endl;
             std::cout << "Author: Dominique COMET (with Yu Harada - Diretta protocol)" << std::endl;
             std::cout << "MIT License" << std::endl;
             std::cout << "═══════════════════════════════════════════════════════" << std::endl;
            exit(0);
        }       
        else if (arg == "--verbose" || arg == "-v") {
            // ⭐ NOUVEAU: Option verbose
            g_verbose = true;
            std::cout << "✓ Verbose mode enabled" << std::endl;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Diretta UPnP Renderer\n\n"
                      << "Usage: " << argv[0] << " [options]\n\n"
                      << "Options:\n"
                      << "  --name, -n <name>     Renderer name (default: Diretta Renderer)\n"
                      << "  --port, -p <port>     UPnP port (default: auto)\n"
                      << "  --uuid <uuid>         Device UUID (default: auto-generated)\n"
                      << "  --no-gapless          Disable gapless playback\n"
                      << "  --buffer, -b <secs>   Buffer size in seconds (default: 10)\n"
                      << "  --target, -t <index>  Select Diretta target by index (1, 2, 3...)\n"
                      << "  --list-targets, -l    List available Diretta targets and exit\n"
                      << "  --verbose, -v         Enable verbose debug output\n"
                      << "  --version, -V         Show version information\n"
                      << "  --help, -h            Show this help\n"
                      << "\nTarget Selection:\n"
                      << "  First, scan for targets:  " << argv[0] << " --list-targets\n"
                      << "  Then, use specific target: " << argv[0] << " --target 1\n"
                      << "  Or use interactive mode:   " << argv[0] << " (prompts if multiple targets)\n"
                      << "  Or use interactive mode:   " << argv[0] << " (prompts if multiple targets)\n"
                      << "\nDebug Mode:\n"  // ⭐ NOUVEAU
                      << "  Normal mode (clean output): " << argv[0] << " --target 1\n"
                      << "  Verbose mode (all logs):    " << argv[0] << " --target 1 --verbose\n"                     
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
