#!/bin/sh
# get-key.sh - fetch the public TI-84 Plus OS signing key (0A) used to sign
# OS upgrades.  The key was factored in 2009 and is distributed by KnightOS;
# it is not part of this repository.
set -e
mkdir -p keys
curl -fsSL -o keys/0A.key https://raw.githubusercontent.com/KnightOS/kernel/master/keys/0A.key
echo "keys/0A.key"
