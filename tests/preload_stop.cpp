#include "AudioEngine.h"
#include "LogLevel.h"
#include <chrono>
#include <iostream>

LogLevel g_logLevel = LogLevel::INFO;
bool g_prefetchEnabled = true;

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    AudioEngine engine;
    engine.setAudioCallback([](auto const&, auto, auto, auto, auto) { return true; });
    engine.setCurrentURI("tone.wav", "");
    if (!engine.play()) return 3;
    engine.setNextURI(argv[1]);
    std::thread audio([&] {
        for (int i = 0; i < 2000 && engine.getState() == AudioEngine::State::PLAYING; ++i) {
            engine.process(512);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::string command;
    std::getline(std::cin, command);
    auto start = std::chrono::steady_clock::now();
    engine.stop();
    const auto trackAtStop = engine.getTrackNumber();
    audio.join();
    auto elapsed = std::chrono::steady_clock::now() - start;
    if (engine.getState() != AudioEngine::State::STOPPED || engine.getTrackNumber() != trackAtStop) return 4;
    if (trackAtStop != 1 && !(command == "release" && trackAtStop == 2)) return 6;
    engine.setCurrentURI("tone.wav", "", true);
    if (!engine.play() || !engine.process(512)) return 5;
    std::cout << "BOUNDARY_OK " << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " track=" << trackAtStop << std::endl;
}
