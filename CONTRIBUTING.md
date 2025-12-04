# Contributing to Diretta UPnP Renderer

Thank you for your interest in contributing to the Diretta UPnP Renderer project! 🎉

This document provides guidelines for contributing to the project.

## Table of Contents

1. [Code of Conduct](#code-of-conduct)
2. [How Can I Contribute?](#how-can-i-contribute)
3. [Development Setup](#development-setup)
4. [Coding Standards](#coding-standards)
5. [Submitting Changes](#submitting-changes)
6. [Reporting Bugs](#reporting-bugs)
7. [Suggesting Enhancements](#suggesting-enhancements)

---

## Code of Conduct

### Our Standards

- Be respectful and inclusive
- Welcome newcomers and help them learn
- Focus on constructive criticism
- Prioritize audio quality and user experience
- Respect the personal-use-only nature of the Diretta SDK

### Unacceptable Behavior

- Harassment or discriminatory language
- Trolling or inflammatory comments
- Publishing others' private information
- Distributing the Diretta SDK (it's proprietary)

---

## How Can I Contribute?

### 1. Testing and Bug Reports

- Test with different audio formats (FLAC, ALAC, DSD, etc.)
- Test with different control points (JPlay, BubbleUPnP, etc.)
- Test on different Linux distributions
- Report any issues you find (see [Reporting Bugs](#reporting-bugs))

### 2. Documentation

- Improve README, installation guides, or troubleshooting docs
- Add examples of working configurations
- Translate documentation to other languages
- Create video tutorials or blog posts

### 3. Code Contributions

- Fix bugs
- Add new features
- Improve performance
- Enhance code quality

### 4. Hardware Testing

- Test with different DACs
- Test with different network configurations
- Document compatibility findings

---

## Development Setup

### Prerequisites

```bash
# Install dependencies (Fedora example)
sudo dnf install git gcc-c++ make ffmpeg-devel libupnp-devel

# Download Diretta SDK from https://www.diretta.link
# Extract to ~/DirettaHostSDK_147/
```

### Fork and Clone

```bash
# Fork the repository on GitHub, then:
git clone https://github.com/YOUR_USERNAME/DirettaUPnPRenderer.git
cd DirettaUPnPRenderer

# Add upstream remote
git remote add upstream https://github.com/ORIGINAL_OWNER/DirettaUPnPRenderer.git
```

### Build and Test

```bash
# Build
make clean
make

# Run tests (when available)
make test

# Test manually
sudo ./bin/DirettaRendererComplete --port 4005 --buffer 2.0
```

### Keep Your Fork Updated

```bash
git fetch upstream
git checkout main
git merge upstream/main
```

---

## Coding Standards

### C++ Style

We follow modern C++ best practices (C++17):

#### Naming Conventions

```cpp
// Classes: PascalCase
class AudioEngine { };

// Functions/methods: camelCase
void processAudio();

// Member variables: m_ prefix + camelCase
std::string m_currentURI;

// Constants: UPPER_SNAKE_CASE
const int MAX_BUFFER_SIZE = 8192;

// Namespaces: lowercase
namespace diretta { }
```

#### Formatting

- **Indentation**: 4 spaces (no tabs)
- **Braces**: Opening brace on same line
```cpp
void function() {
    // code here
}
```
- **Line length**: Aim for 100 characters, max 120
- **Comments**: Use `//` for single-line, `/* */` for multi-line

#### Best Practices

```cpp
// ✅ Good: Use smart pointers
std::unique_ptr<AudioEngine> m_audioEngine;

// ✅ Good: Use const where possible
void process(const AudioBuffer& buffer);

// ✅ Good: Clear variable names
uint32_t sampleRate;

// ❌ Avoid: Raw pointers for ownership
AudioEngine* m_audioEngine;

// ❌ Avoid: Magic numbers
if (value > 44100) { }  // What does 44100 mean?

// ✅ Better: Named constants
const uint32_t CD_SAMPLE_RATE = 44100;
if (value > CD_SAMPLE_RATE) { }
```

### Error Handling

```cpp
// ✅ Check return values
if (!m_audioEngine->open(uri)) {
    std::cerr << "[Error] Failed to open: " << uri << std::endl;
    return false;
}

// ✅ Use exceptions for truly exceptional cases
try {
    parseConfig(configFile);
} catch (const std::exception& e) {
    std::cerr << "Configuration error: " << e.what() << std::endl;
}
```

### Logging

```cpp
// Use consistent logging format
std::cout << "[ComponentName] Message" << std::endl;
std::cerr << "[ComponentName] ⚠️  Warning: " << details << std::endl;
std::cerr << "[ComponentName] ❌ Error: " << details << std::endl;
std::cout << "[ComponentName] ✓ Success" << std::endl;
```

### Comments

```cpp
// Explain WHY, not WHAT (code should be self-explanatory)

// ❌ Bad comment (obvious)
// Increment counter
counter++;

// ✅ Good comment (explains reasoning)
// Use smaller packets for 16bit/44.1kHz to avoid fragmentation
// even though jumbo frames are available
if (isLowBitrate) {
    m_syncBuffer->configTransferAuto(...);
}
```

---

## Submitting Changes

### Branch Naming

```bash
# Feature branches
git checkout -b feature/add-volume-control

# Bug fix branches
git checkout -b fix/audio-dropout-issue

# Documentation
git checkout -b docs/improve-installation-guide
```

### Commit Messages

Follow this format:

```
Short summary (50 chars or less)

More detailed explanation if needed. Wrap at 72 characters.
Explain the problem this commit solves and why this approach
was chosen.

- Bullet points are okay
- Use present tense: "Add feature" not "Added feature"
- Reference issues: "Fixes #123" or "Related to #456"
```

Examples:

```
Add support for volume control via UPnP

Implements the RenderingControl service to allow volume
adjustment from control points. Uses ALSA mixer API.

Fixes #42
```

```
Fix audio dropouts with 16bit/44.1kHz files

Previously, jumbo frames were used for all formats,
causing fragmentation with low-bitrate files. Now
automatically switches to smaller packets.

Related to #56, #78
```

### Pull Request Process

1. **Update documentation** if adding features
2. **Add tests** if applicable (when test framework exists)
3. **Ensure code compiles** without warnings
4. **Test your changes** with real audio playback
5. **Update CHANGELOG** (if exists)

#### Pull Request Template

```markdown
## Description
Brief description of what this PR does.

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Documentation update
- [ ] Performance improvement

## Testing
How was this tested? Include:
- Audio formats tested (FLAC, DSD, etc.)
- Control points tested (JPlay, BubbleUPnP, etc.)
- Linux distributions tested

## Checklist
- [ ] Code compiles without warnings
- [ ] Tested with real audio playback
- [ ] Documentation updated (if needed)
- [ ] No Diretta SDK files included
```

---

## Reporting Bugs

### Before Submitting a Bug Report

1. **Check existing issues** - your bug may already be reported
2. **Try the latest version** - it may already be fixed
3. **Read troubleshooting docs** - TROUBLESHOOTING.md

### Bug Report Template

```markdown
## Bug Description
Clear description of what the bug is.

## Steps to Reproduce
1. Start renderer with command: `sudo ./bin/...`
2. Play file: `artist - track.flac`
3. Observe behavior: audio cuts out after 30 seconds

## Expected Behavior
Audio should play smoothly without interruptions.

## Actual Behavior
Audio cuts out after 30 seconds, then resumes.

## System Information
- OS: Fedora 38
- Kernel: 6.5.0-rt (RT kernel)
- Network card: Intel I219-V
- MTU: 9000
- DAC: Holo Audio Spring 3
- Audio format: FLAC 24bit/192kHz

## Logs
```
[paste relevant logs here]
```

## Additional Context
Using dedicated audio network. Switch supports jumbo frames.
```

---

## Suggesting Enhancements

We welcome feature suggestions! Please include:

### Enhancement Template

```markdown
## Feature Description
What feature would you like to see?

## Use Case
Why would this feature be useful? Who would benefit?

## Proposed Implementation
(Optional) How could this be implemented?

## Alternatives Considered
(Optional) What other approaches did you consider?

## Additional Context
Any other relevant information.
```

### Examples of Good Enhancement Requests

- "Add playlist support for queue management"
- "Implement volume control via UPnP RenderingControl"
- "Add web UI for configuration"
- "Support for multiple Diretta targets"

---

## Development Tips

### Debugging

```bash
# Build with debug symbols
make clean
CXXFLAGS="-g -DDEBUG" make

# Run with GDB
sudo gdb ./bin/DirettaRendererComplete
(gdb) run --port 4005 --buffer 2.0
```

### Testing Different Formats

Keep test files in `test_audio/` (git-ignored):
```
test_audio/
├── cd_quality/
│   └── test_16bit_44.1kHz.flac
├── hi_res/
│   └── test_24bit_192kHz.flac
└── dsd/
    └── test_dsd64.dsf
```

### Network Testing

```bash
# Simulate packet loss
sudo tc qdisc add dev enp4s0 root netem loss 1%

# Remove simulation
sudo tc qdisc del dev enp4s0 root

# Monitor packets
sudo tcpdump -i enp4s0 -w capture.pcap
```

---

## Code Review Process

1. Maintainers will review your PR
2. Address feedback and update PR
3. Once approved, your PR will be merged
4. Your contribution will be acknowledged!

---

## Recognition

Contributors will be:
- Listed in CONTRIBUTORS.md
- Credited in release notes
- Acknowledged in README (for significant contributions)

---

## Questions?

- **General questions**: Open a GitHub Discussion
- **Bug reports**: Open a GitHub Issue
- **Security issues**: Email (to be determined)

---

## Thank You! 🎵

Your contributions help make high-quality audio streaming accessible to everyone!

---

*This contributing guide may be updated. Please check back periodically.*
