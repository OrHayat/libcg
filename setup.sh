#!/bin/bash
set -e

echo "=== libcg setup ==="

if [ "$(uname -s)" != "Darwin" ]; then
    echo "This setup script is for macOS only."
    exit 1
fi

# Xcode CLI tools
if ! xcode-select -p &>/dev/null; then
    echo "Installing Xcode Command Line Tools..."
    xcode-select --install
    echo "Re-run this script after installation completes."
    exit 0
else
    echo "Xcode CLI tools: OK"
fi

# Homebrew
if ! command -v brew &>/dev/null; then
    echo "Installing Homebrew..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
else
    echo "Homebrew: OK"
fi

# graphite CLI
if ! command -v gt &>/dev/null; then
    echo "Installing Graphite CLI..."
    brew install withgraphite/tap/graphite
else
    echo "gt: OK"
fi

echo "=== Done ==="
