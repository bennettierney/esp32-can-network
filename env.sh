# Source this in a terminal: . ./env.sh
# Native arm64. Uses Apple's python3 (has lzma) and ESP-IDF's own cmake and ninja.
if [ "$(uname -m)" != "arm64" ]; then
    echo "Run this in a native arm64 shell, not Rosetta."
    return 1
fi
export PATH=/usr/bin:$PATH
. "$HOME/esp/esp-idf/export.sh"
