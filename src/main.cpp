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
#include <iomanip>    // ⭐ v1.3.0: Pour std::fixed, std::setprecision
#include "TimestampedLogger.h"

// Version information
#define RENDERER_VERSION "1.3.1"    // ⭐ v1.3.1: Timestamped logging
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

// ⭐ NOUVEAU: Variable globale pour l'interface réseau (multi-homed support)
std::string g_networkInterface = "";

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
    config.bufferSeconds = 2.0f;  // Default 2 seconds (v1.0.9)
    
    // ⭐ v1.3.0: Transfer mode default
    config.transferMode = TransferMode::VarMax;
    
    // ⭐ NEW: Advanced Diretta SDK settings
    config.threadMode = 1;        // Default: Critical only
    config.cycleTime = 10000;     // Default: 10ms
    config.cycleMinTime = 333;    // Default: 333µs
    config.infoCycle = 100000;      // Default: 100ms
    config.mtuOverride = 0;       // 0 = auto-detect
    
    // ⭐ NEW: Network interface (empty = auto-detect)
    config.networkInterface = "";
    
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
            if (config.bufferSeconds < 1.0f) {
                std::cerr << "⚠️  Warning: Buffer < 1 second may cause issues with DSD/Hi-Res!" << std::endl;
            }
        }
        else if ((arg == "--target" || arg == "-t") && i + 1 < argc) {
            config.targetIndex = std::atoi(argv[++i]) - 1;  // Convert to 0-based index
            if (config.targetIndex < 0) {
                std::cerr << "❌ Invalid target index. Must be >= 1" << std::endl;
                exit(1);
            }
        }
        else if ((arg == "--thread-mode") && i + 1 < argc) {
            config.threadMode = std::atoi(argv[++i]);
        }
        else if ((arg == "--cycle-time") && i + 1 < argc) {
            config.cycleTime = std::atoi(argv[++i]);
            if (config.cycleTime < 333 || config.cycleTime > 10000) {
                std::cerr << "⚠️  Warning: cycle-time should be between 333-10000 µs" << std::endl;
            }
        }
        else if ((arg == "--cycle-min-time") && i + 1 < argc) {
            config.cycleMinTime = std::atoi(argv[++i]);
        }
        else if ((arg == "--info-cycle") && i + 1 < argc) {
            config.infoCycle = std::atoi(argv[++i]);
        }
        else if ((arg == "--mtu") && i + 1 < argc) {
            config.mtuOverride = std::atoi(argv[++i]);
            if (config.mtuOverride > 0 && config.mtuOverride < 1500) {
                std::cerr << "⚠️  Warning: MTU < 1500 may cause issues" << std::endl;
            }
        }
        // ⭐ v1.3.0: Transfer mode option
        else if (arg == "--transfer-mode" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "varmax") {
                config.transferMode = TransferMode::VarMax;
            } else if (mode == "fix") {
                config.transferMode = TransferMode::Fix;
            } else {
                std::cerr << "❌ Invalid transfer mode: " << mode << std::endl;
                std::cerr << "   Valid values: varmax, fix" << std::endl;
                exit(1);
            }
        }
        // ⭐ NOUVEAU: Options pour interface réseau
        else if (arg == "--interface" && i + 1 < argc) {
            config.networkInterface = argv[++i];
            std::cout << "✓ Will bind to interface: " << config.networkInterface << std::endl;
        }
        else if (arg == "--bind-ip" && i + 1 < argc) {
            config.networkInterface = argv[++i];
            std::cout << "✓ Will bind to IP: " << config.networkInterface << std::endl;
        }
        // Fin des nouvelles options
        else if (arg == "--list-targets" || arg == "-l") {
            listTargets();
            exit(0);
        }
        else if (arg == "--version" || arg == "-V") {  // ← FIX: -V au lieu de -v
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
            // ⭐ Option verbose
            g_verbose = true;
            std::cout << "✓ Verbose mode enabled" << std::endl;
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Diretta UPnP Renderer\n\n"
                      << "Usage: " << argv[0] << " [options]\n\n"
                      << "Options:\n"
                      << "  --name, -n <n>     Renderer name (default: Diretta Renderer)\n"
                      << "  --port, -p <port>     UPnP port (default: auto)\n"
                      << "  --uuid <uuid>         Device UUID (default: auto-generated)\n"
                      << "  --buffer, -b <secs>   Buffer size in seconds (default: 2.0)\n"
                      << "  --target, -t <index>  Select Diretta target by index (1, 2, 3...)\n"
                      << "  --list-targets, -l    List available Diretta targets and exit\n"
                      << "  --verbose, -v         Enable verbose debug output\n"
                      << "  --version, -V         Show version information\n"
                      << "  --help, -h            Show this help\n"
                      << "\n"
                      << "Network Interface Options (for multi-homed systems):\n"  // ⭐ NOUVEAU
                      << "  --interface <n>    Network interface to bind (e.g., eth0, eno1)\n"
                      << "  --bind-ip <ip>        IP address to bind (e.g., 192.168.1.10)\n"
                      << "\n"
                      << "  For systems with multiple network interfaces (3-tier architecture):\n"
                      << "    Control network: 192.168.1.x on eth0\n"
                      << "    Diretta network: 192.168.2.x on eth1\n"
                      << "\n"
                      << "    " << argv[0] << " --interface eth0 --target 1\n"
                      << "\n"
                      << "Transfer Mode Options:\n"
                      << "  --transfer-mode <mode>  Transfer timing mode (default: varmax)\n"
                      << "                          varmax = Adaptive timing (optimal bandwidth)\n"
                      << "                          fix    = Fixed timing (precise cycle control)\n"
                      << "\n"
                      << "  When using 'fix' mode, you MUST specify --cycle-time:\n"
                      << "    Example: --transfer-mode fix --cycle-time 1893\n"
                      << "             (1893 µs = 528 Hz - precise audiophile timing)\n"
                      << "\n"
                      << "Advanced Diretta SDK Options:\n"
                      << "  --thread-mode <value>   Thread mode bitmask (default: 1)\n"
                      << "                          1=Critical, 2=NoShortSleep, 4=NoSleep4Core,\n"
                      << "                          8=SocketNoBlock, 16=OccupiedCPU, 32/64/128=FEEDBACK,\n"
                      << "                          256=NOFASTFEEDBACK, 512=IDLEONE, 1024=IDLEALL,\n"
                      << "                          2048=NOSLEEPFORCE, 4096=LIMITRESEND,\n"
                      << "                          8192=NOJUMBOFRAME, 16384=NOFIREWALL, 32768=NORAWSOCKET\n"
                      << "  --cycle-time <µs>       Transfer packet cycle time (default: 10000)\n"
                      << "                          VarMax mode: Maximum cycle time (optional)\n"
                      << "                          Fix mode: Fixed cycle time (REQUIRED)\n"
                      << "                          Examples: 1893 (528 Hz), 2000 (500 Hz)\n"
                      << "  --cycle-min-time <µs>   Transfer packet cycle min time (default: 333)\n"
                      << "  --info-cycle <µs>       Information packet cycle time (default: 100000)\n"
                      << "  --mtu <bytes>           Override MTU (default: auto-detect)\n"
                      << "\n"                     
                      << "Target Selection:\n"
                      << "  First, scan for targets:  " << argv[0] << " --list-targets\n"
                      << "  Then, use specific target: " << argv[0] << " --target 1\n"
                      << "  Or use interactive mode:   " << argv[0] << " (prompts if multiple targets)\n"
                      << "\n"
                      << "Debug Mode:\n"
                      << "  Normal mode (clean output): " << argv[0] << " --target 1\n"
                      << "  Verbose mode (all logs):    " << argv[0] << " --target 1 --verbose\n"
                      << "\n"
                      << "Multi-homed Examples:\n"  // ⭐ NOUVEAU
                      << "  List network interfaces:     ip link show\n"
                      << "  Bind to specific interface:  " << argv[0] << " --interface eth0\n"
                      << "  Bind to specific IP:         " << argv[0] << " --bind-ip 192.168.1.10\n"
                      << std::endl;
            exit(0);
        }
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            std::cerr << "Use --help for usage information" << std::endl;
            exit(1);
        }
    }
    
    // ═══════════════════════════════════════════════════════════════
    // ⭐ v1.3.0: Validate Fix mode requires explicit cycle-time
    // ═══════════════════════════════════════════════════════════════
    if (config.transferMode == TransferMode::Fix) {
        // Check if user explicitly set cycle-time
        bool cycleTimeWasSet = false;
        for (int j = 1; j < argc; j++) {
            if (std::string(argv[j]) == "--cycle-time") {
                cycleTimeWasSet = true;
                break;
            }
        }
        
        if (!cycleTimeWasSet) {
            std::cerr << "\n❌ Error: --transfer-mode fix requires --cycle-time\n" << std::endl;
            std::cerr << "Fix mode uses a precise, fixed cycle time that YOU must specify." << std::endl;
            std::cerr << "\nExample usage:" << std::endl;
            std::cerr << "  " << argv[0] << " --target 1 --transfer-mode fix --cycle-time 1893" 
                      << std::endl;
            std::cerr << "\nThe cycle-time value determines the fixed timing:" << std::endl;
            std::cerr << "  1893 µs = 528 Hz  (audiophile frequency)" << std::endl;
            std::cerr << "  2000 µs = 500 Hz" << std::endl;
            std::cerr << "  1000 µs = 1000 Hz" << std::endl;
            std::cerr << "\nNote: In VarMax mode (default), --cycle-time is optional." << std::endl;
            exit(1);
        }
    }
    
    return config;
}

int main(int argc, char* argv[]) {
        // ⭐ v1.3.1: Install timestamped logging (MUST BE FIRST!)
    TimestampedStreambuf* coutBuf = nullptr;
    TimestampedStreambuf* cerrBuf = nullptr;
    installTimestampedLogging(coutBuf, cerrBuf);

    // Setup signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    std::cout << "═══════════════════════════════════════════════════════\n"
              << "  🎵 Diretta UPnP Renderer v" << RENDERER_VERSION << "\n"
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
    
    // ⭐ v1.3.0: Display transfer mode
    std::cout << "  Transfer:    " 
              << (config.transferMode == TransferMode::VarMax ? "VarMax (adaptive)" : "Fix (precise)") 
              << std::endl;
    
    // ⭐ NOUVEAU: Afficher interface réseau
    if (!config.networkInterface.empty()) {
        std::cout << "  Network:     " << config.networkInterface << " (specific interface)" << std::endl;
    } else {
        std::cout << "  Network:     auto-detect (first available)" << std::endl;
    }
    
    std::cout << "  UUID:        " << config.uuid << std::endl;
    
    // ⭐ Display advanced settings only if modified from defaults OR if Fix mode
    if (config.threadMode != 1 || config.cycleTime != 10000 || 
        config.cycleMinTime != 333 || config.infoCycle != 100000 || 
        config.mtuOverride != 0 || config.transferMode == TransferMode::Fix) {
        std::cout << "\nAdvanced Diretta Settings:" << std::endl;
        if (config.threadMode != 1)
            std::cout << "  Thread Mode: " << config.threadMode << std::endl;
        // ⭐ v1.3.0: Always show cycle time in Fix mode with frequency
        if (config.transferMode == TransferMode::Fix) {
            double freq_hz = 1000000.0 / config.cycleTime;
            std::cout << "  Cycle Time:  " << config.cycleTime << " µs (" 
                      << std::fixed << std::setprecision(2) << freq_hz << " Hz - FIXED)" 
                      << std::endl;
        } else if (config.cycleTime != 10000) {
            std::cout << "  Cycle Time:  " << config.cycleTime << " µs (max)" << std::endl;
        }
        if (config.cycleMinTime != 333)
            std::cout << "  Cycle Min:   " << config.cycleMinTime << " µs" << std::endl;
        if (config.infoCycle != 100000)
            std::cout << "  Info Cycle:  " << config.infoCycle << " µs" << std::endl;
        if (config.mtuOverride != 0)
            std::cout << "  MTU:         " << config.mtuOverride << " bytes" << std::endl;
    }
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
        // ⭐ v1.3.1: Cleanup
    if (coutBuf) delete coutBuf;
    if (cerrBuf) delete cerrBuf;
    
    return 0;
}
