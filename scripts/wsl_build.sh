#!/usr/bin/env bash
# WSL build helper for POSEIDON. Sets PATH and jumps into project.
set -e
export PATH="$HOME/.local/bin:$PATH"
cd /mnt/c/Users/D/poseidon
exec "$@"
