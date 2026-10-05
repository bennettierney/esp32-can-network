# Source this in a terminal: . ./env.sh
# Uses Apple's python3 (has lzma) and runs as x86_64 to match brew's cmake and ninja.
if [ "$(uname -m)" != "x86_64" ]; then
    echo "Re-run under Rosetta: arch -x86_64 /bin/zsh, then . ./env.sh"
    return 1
fi
export PATH=/usr/bin:$PATH
. "$HOME/esp/esp-idf/export.sh"
