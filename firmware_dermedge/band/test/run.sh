#!/bin/sh
# Builds and runs the host tests for the band's plain-C logic (scratch detector, touch, state machine)
set -e
cd "$(dirname "$0")"
# The newest macOS SDK can be ahead of the Command Line Tools linker ("unknown architecture
# arm64e.x1"); use the newest SDK that links.
for sdk in "" /Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk /Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk; do
    if [ -z "$sdk" ] || [ -d "$sdk" ]; then
        if echo 'int main(void){return 0;}' | cc ${sdk:+-isysroot "$sdk"} -x c -o /tmp/dermedge_sdk_probe - 2>/dev/null; then
            SYSROOT=${sdk:+-isysroot $sdk}
            break
        fi
    fi
done
cc $SYSROOT -std=c11 -Wall -Wextra -Werror -O1 -o /tmp/dermedge_band_test test_host.c ../main/scratch.c ../main/touch.c ../main/app_sm.c -lm
/tmp/dermedge_band_test
