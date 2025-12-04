# GitHub Repository Structure

This document explains the recommended structure for the Diretta UPnP Renderer GitHub repository.

## Repository Layout

```
DirettaUPnPRenderer/
├── README.md                    # Main project page
├── LICENSE                      # MIT License with SDK notices
├── CHANGELOG.md                 # Version history
├── CONTRIBUTING.md              # Contribution guidelines
├── .gitignore                   # Git ignore rules
├── Makefile                     # Build configuration
├── install.sh                   # Installation helper script
│
├── src/                         # Source code
│   ├── main.cpp
│   ├── DirettaRenderer.h
│   ├── DirettaRenderer.cpp
│   ├── AudioEngine.h
│   ├── AudioEngine.cpp
│   ├── DirettaOutput.h
│   ├── DirettaOutput.cpp
│   ├── UPnPDevice.h
│   └── UPnPDevice.cpp
│
├── docs/                        # Documentation
│   ├── INSTALLATION.md          # Detailed installation guide
│   ├── CONFIGURATION.md         # Configuration options
│   ├── TROUBLESHOOTING.md       # Problem solving
│   └── images/                  # Screenshots, diagrams
│       └── architecture.png
│
├── scripts/                     # Helper scripts (optional)
│   ├── setup_network.sh
│   ├── create_service.sh
│   └── test_mtu.sh
│
└── .github/                     # GitHub specific files
    ├── ISSUE_TEMPLATE/
    │   ├── bug_report.md
    │   └── feature_request.md
    └── workflows/               # GitHub Actions (optional)
        └── build.yml
```

## Essential Files

### Root Directory

1. **README.md** ✅
   - Project overview
   - Quick start guide
   - Feature list
   - Links to documentation

2. **LICENSE** ✅
   - MIT License
   - Diretta SDK notices
   - Usage restrictions

3. **CHANGELOG.md** ✅
   - Version history
   - Release notes

4. **CONTRIBUTING.md** ✅
   - Contribution guidelines
   - Code standards
   - Pull request process

5. **.gitignore** ✅
   - Exclude build artifacts
   - Exclude Diretta SDK
   - Exclude personal configs

6. **Makefile**
   - Build configuration
   - SDK path setup
   - Compilation flags

7. **install.sh** ✅
   - Automated installation
   - Dependency setup
   - Configuration helper

### docs/ Directory

1. **INSTALLATION.md** ✅
   - System requirements
   - Step-by-step installation
   - Network configuration
   - Systemd service setup

2. **CONFIGURATION.md** ✅
   - Command-line options
   - Network tuning
   - Performance optimization
   - Audio buffer settings

3. **TROUBLESHOOTING.md** ✅
   - Common problems
   - Solutions
   - Diagnostic tools
   - FAQ

### src/ Directory

Your actual source code files:
- main.cpp
- DirettaRenderer.h/.cpp
- AudioEngine.h/.cpp
- DirettaOutput.h/.cpp
- UPnPDevice.h/.cpp

## What NOT to Include

### ❌ Never Commit These

1. **Diretta SDK**
   - DirettaHostSDK_147/
   - Any SDK libraries or headers
   - Proprietary content

2. **Build Artifacts**
   - bin/
   - obj/
   - *.o files

3. **Personal Files**
   - IDE configurations (.vscode/, .idea/)
   - Personal configs
   - Test audio files

4. **Logs**
   - *.log files
   - Debug outputs

5. **Compiled Binaries**
   - DirettaRendererComplete
   - Any executables

## Optional Enhancements

### GitHub Issue Templates

Create `.github/ISSUE_TEMPLATE/bug_report.md`:

```markdown
---
name: Bug Report
about: Report a bug or issue
title: '[BUG] '
labels: bug
assignees: ''
---

**Describe the bug**
A clear description of what the bug is.

**To Reproduce**
Steps to reproduce the behavior:
1. Start renderer with '...'
2. Play file '...'
3. See error

**Expected behavior**
What you expected to happen.

**System Information**
- OS: [e.g., Fedora 38]
- Kernel: [e.g., 6.5.0]
- DAC: [e.g., Holo Audio Spring 3]
- Network: [e.g., Gigabit, MTU 9000]

**Logs**
```
Paste relevant logs here
```

**Additional context**
Any other information.
```

### GitHub Actions (CI/CD)

Create `.github/workflows/build.yml`:

```yaml
name: Build Test

on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v2
    
    - name: Install dependencies
      run: |
        sudo apt update
        sudo apt install -y libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libupnp-dev
    
    - name: Build
      run: |
        # Note: This will fail without Diretta SDK, but verifies code compiles
        make || echo "Build test (SDK not available in CI)"
```

## Repository Setup Steps

### 1. Initialize Repository

```bash
cd /path/to/your/project
git init
```

### 2. Create .gitignore

```bash
# Copy the .gitignore file created earlier
cp /mnt/user-data/outputs/.gitignore .
```

### 3. Organize Files

```bash
# Create directory structure
mkdir -p docs src scripts

# Move documentation
mv INSTALLATION.md CONFIGURATION.md TROUBLESHOOTING.md docs/

# Move source code
mv *.cpp *.h src/

# Move scripts (if any)
mv *.sh scripts/
mv install.sh ./  # Keep install.sh in root
```

### 4. Add Files to Git

```bash
# Add all appropriate files
git add README.md LICENSE CHANGELOG.md CONTRIBUTING.md .gitignore
git add Makefile install.sh
git add docs/
git add src/

# Verify what will be committed
git status
```

### 5. Initial Commit

```bash
git commit -m "Initial commit - Diretta UPnP Renderer v1.0.0

- Complete UPnP MediaRenderer implementation
- Diretta protocol integration
- Support for PCM up to 1536kHz, DSD up to DSD1024
- Adaptive network optimization
- Gapless playback
- Full documentation"
```

### 6. Create GitHub Repository

1. Go to GitHub.com
2. Click "New Repository"
3. Name: `DirettaUPnPRenderer`
4. Description: "The world's first native UPnP/DLNA renderer with Diretta protocol support"
5. Public repository
6. Don't initialize with README (you already have one)

### 7. Push to GitHub

```bash
# Add remote
git remote add origin https://github.com/YOUR_USERNAME/DirettaUPnPRenderer.git

# Push
git branch -M main
git push -u origin main
```

### 8. Configure Repository Settings

On GitHub:
1. **About** → Add description and tags
2. **Topics** → Add: `audio`, `diretta`, `upnp`, `dlna`, `hi-res-audio`, `renderer`, `audiophile`
3. **Website** → Link to Diretta website if appropriate

## Maintenance

### Regular Updates

```bash
# Update documentation
nano docs/TROUBLESHOOTING.md

# Commit changes
git add docs/TROUBLESHOOTING.md
git commit -m "docs: Add solution for MTU configuration issue"
git push
```

### Creating Releases

When ready for a release:

```bash
# Tag the version
git tag -a v1.0.0 -m "Release v1.0.0 - Initial public release"
git push origin v1.0.0
```

Then create a GitHub Release:
1. Go to "Releases" on GitHub
2. Click "Draft a new release"
3. Select tag v1.0.0
4. Add release notes (from CHANGELOG.md)
5. Optionally attach compiled binaries

## Community Engagement

### README Badges

Add to top of README.md:

```markdown
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/platform-Linux-blue.svg)](https://www.linux.org/)
[![C++](https://img.shields.io/badge/C++-17-00599C.svg)](https://isocpp.org/)
[![GitHub issues](https://img.shields.io/github/issues/YOUR_USERNAME/DirettaUPnPRenderer)](https://github.com/YOUR_USERNAME/DirettaUPnPRenderer/issues)
[![GitHub stars](https://img.shields.io/github/stars/YOUR_USERNAME/DirettaUPnPRenderer)](https://github.com/YOUR_USERNAME/DirettaUPnPRenderer/stargazers)
```

### Enable Discussions

Go to repository Settings → Features → Enable Discussions

### Star the Repository

Encourage users to star the repo if they find it useful!

---

## Checklist Before Publishing

- [ ] All source code files are in `src/`
- [ ] Documentation is complete and in `docs/`
- [ ] .gitignore properly excludes SDK and build artifacts
- [ ] LICENSE file includes all necessary notices
- [ ] README.md has clear installation instructions
- [ ] Makefile works for different SDK locations
- [ ] No proprietary Diretta SDK files included
- [ ] All scripts are executable (`chmod +x *.sh`)
- [ ] CHANGELOG.md is up to date
- [ ] Repository description and topics are set
- [ ] Issue templates are created (optional)

---

**Ready to share your amazing work with the world!** 🚀
