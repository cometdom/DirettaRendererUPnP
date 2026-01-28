## Contributing

**Note:** Contributions are welcome but there is no guarantee they will be reviewed or merged.

---

## Credits

### Original Author

**Dominique COMET** ([@cometdom](https://github.com/cometdom)) - Original development

### Fork Maintainer

**SwissMontainsBear** ([@SwissMontainsBear](https://github.com/SwissMontainsBear))

### Third-Party Components

- [Diretta Host SDK](https://www.diretta.link) - Proprietary (personal use only)
- [FFmpeg](https://ffmpeg.org) - LGPL/GPL
- [libupnp](https://pupnp.sourceforge.io/) - BSD License

---

## License

This project is licensed under the **MIT License** - see the [LICENSE](LICENSE) file for details.

**IMPORTANT**: The Diretta Host SDK is proprietary software by Yu Harada and is licensed for **personal use only**. Commercial use is prohibited.

---

## Disclaimer

**THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND.**

The maintainer ([@SwissMontainsBear](https://github.com/SwissMontainsBear)):

- **Does NOT provide technical support**
- **Does NOT guarantee updates, bug fixes, or maintenance**
- **Does NOT accept liability** for any issues arising from use of this software
- **Does NOT guarantee responses** to issues or pull requests
- Shares this code **purely as a courtesy** to the community

**Use this software entirely at your own risk.**

The maintainer is not responsible for any:

- Hardware damage
- Data loss
- Audio equipment damage
- Any other issues that may arise

For questions about the Diretta protocol, contact [diretta.link](https://www.diretta.link).

---

**Enjoy bit-perfect audio streaming! 🎵**

*This fork is provided as-is for the audiophile community.*

```
---

## 3. CONTRIBUTING.md

```markdown
# Contributing to Diretta UPnP Renderer (Community Fork)

> ⚠️ **Important Notice**
> 
> This is a personal project shared with the community as a courtesy.
> 
> - **No guarantee** that contributions will be reviewed or merged
> - **No guarantee** that the project will be actively maintained
> - **No support** is provided
> 
> Contribute with the understanding that this is a best-effort, hobby project.

---

Thank you for your interest in contributing! 🎵

## Table of Contents

1. [How Can I Contribute?](#how-can-i-contribute)
2. [Development Setup](#development-setup)
3. [Coding Standards](#coding-standards)
4. [Submitting Changes](#submitting-changes)
5. [Reporting Bugs](#reporting-bugs)
6. [Suggesting Enhancements](#suggesting-enhancements)

---

## How Can I Contribute?

### 1. Testing and Bug Reports

- Test with different audio formats (FLAC, ALAC, DSD, etc.)
- Test with different control points (JPlay, BubbleUPnP, etc.)
- Test on different Linux distributions
- Report any issues you find

### 2. Documentation

- Improve README, installation guides, or troubleshooting docs
- Add examples of working configurations
- Create tutorials

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

# Download Diretta SDK from https://www.diretta.link/hostsdk.html
# Extract to ~/DirettaHostSDK_147/
```

### Fork and Clone

```bash
git clone https://github.com/SwissMontainsBear/DirettaRendererUPnP-X.git
cd DirettaRendererUPnP-X

git remote add upstream https://github.com/SwissMontainsBear/DirettaRendererUPnP-X.git
```

### Build and Test

```bash
make clean
make

# Test manually
sudo ./bin/DirettaRendererUPnP --port 4005 --buffer 2.0
```

### Keep Your Fork Updated

```bash
git fetch upstream
git checkout main
git merge upstream/main
```

---

## Coding Standards

### C++ Style (C++17)

#### Naming Conventions

```cpp
class AudioEngine { };           // Classes: PascalCase
void processAudio();             // Functions: camelCase
std::string m_currentURI;        // Members: m_ prefix
const int MAX_BUFFER_SIZE;       // Constants: UPPER_SNAKE_CASE
namespace diretta { }            // Namespaces: lowercase
```

#### Formatting

- **Indentation**: 4 spaces (no tabs)
- **Braces**: Opening brace on same line
- **Line length**: Max 120 characters

#### Best Practices

```cpp
// Use smart pointers
std::unique_ptr<AudioEngine> m_audioEngine;

// Use const where possible
void process(const AudioBuffer& buffer);

// Use named constants
const uint32_t CD_SAMPLE_RATE = 44100;
```

### Logging Format

```cpp
std::cout << "[ComponentName] Message" << std::endl;
std::cerr << "[ComponentName] Error: " << details << std::endl;
```

---

## Submitting Changes

### Branch Naming

```bash
git checkout -b feature/add-volume-control
git checkout -b fix/audio-dropout-issue
git checkout -b docs/improve-installation
```

### Commit Messages

```
Short summary (50 chars or less)

Detailed explanation if needed. Wrap at 72 characters.

- Use present tense: "Add feature" not "Added feature"
- Reference issues: "Fixes #123"
```

### Pull Request Checklist

- [ ] Code compiles without warnings
- [ ] Tested with real audio playback
- [ ] Documentation updated (if needed)
- [ ] No Diretta SDK files included

---

## Reporting Bugs

> ⚠️ **No guarantee of response or fix.**

### Before Submitting

1. Check existing issues
2. Try the latest version
3. Read troubleshooting docs

### Bug Report Template

```markdown
## Bug Description
[Clear description]

## Steps to Reproduce
1. 
2. 
3. 

## System Information
- OS: 
- Audio format: 
- DAC: 

## Logs
[paste logs]
```

---

## Suggesting Enhancements

```markdown
## Feature Description
[What feature would you like?]

## Use Case
[Why would this be useful?]

## Proposed Implementation
[Optional: How could this work?]
```

---

## Important Reminders

- **Do not include** the Diretta SDK in commits (it's proprietary)
- **Personal use only** - commercial use prohibited due to SDK restrictions
- **Test thoroughly** before submitting PRs

---

## No Support Guarantee

Please understand:

|      |                                                        |
| ---- | ------------------------------------------------------ |
| ❌    | Pull requests may not be reviewed (or reviewed slowly) |
| ❌    | Issues may not receive responses                       |
| ❌    | No obligation to accept contributions                  |
| ❌    | No obligation to provide feedback                      |

This is a hobby project maintained in spare time.

**If you need a feature urgently, consider maintaining your own fork.**

---

## Thank You! 🎵

Your contributions are appreciated, even if response times are slow or unpredictable.

```
---

## 4. .github/ISSUE_TEMPLATE/bug_report.md

Create the directory structure `.github/ISSUE_TEMPLATE/` and add this file:

```markdown
---
name: Bug Report
about: Report a bug (no guarantee of response or fix)
title: '[BUG] '
labels: 'bug'
assignees: ''
---

## ⚠️ Please Read First

**This project is provided AS-IS without support.**

- The maintainer may not respond to this issue
- There is no guarantee this will be fixed
- Response times may be very slow

By submitting this issue, you acknowledge the above.

---

## Bug Description

[Clear description of the bug]

## Steps to Reproduce

1. 
2. 
3. 

## Expected Behavior

[What should happen]

## Actual Behavior

[What actually happens]

## System Information

- **OS**: 
- **Kernel**: 
- **Network card**: 
- **MTU**: 
- **DAC**: 
- **Diretta Target**: 
- **Audio format**: 

## Logs

```

[Paste relevant logs here]

```
## Additional Context

[Any other information]
```

---

## 5. .github/ISSUE_TEMPLATE/feature_request.md

```markdown
---
name: Feature Request
about: Suggest a feature (no guarantee of implementation)
title: '[FEATURE] '
labels: 'enhancement'
assignees: ''
---

## ⚠️ Please Read First

**This project is provided AS-IS without support.**

- Feature requests may not be implemented
- There is no roadmap or timeline
- Consider forking if you need a feature urgently

By submitting this request, you acknowledge the above.

---

## Feature Description

[What feature would you like?]

## Use Case

[Why would this be useful? Who would benefit?]

## Proposed Implementation

[Optional: How could this be implemented?]

## Alternatives Considered

[Optional: Other approaches you considered]
```

---

## 6. .github/ISSUE_TEMPLATE/config.yml

```yaml
blank_issues_enabled: false
contact_links:
  - name: ⚠️ Read Before Opening Issues
    url: https://github.com/SwissMontainsBear/DirettaRendererUPnP-X#disclaimer
    about: |
      This project is provided AS-IS without support. 
      Please read the disclaimer before opening issues.
  - name: Original Project
    url: https://github.com/cometdom/DirettaRendererUPnP
    about: For the original project by Dominique COMET.
  - name: Diretta Protocol
    url: https://www.diretta.link
    about: For questions about the Diretta protocol itself.
```

---

## 
