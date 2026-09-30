#!/usr/bin/env bash
# Create the Linux VM used for all kernel work (run on the macOS host).
# Requires Homebrew. The workspace directory is mounted writable at the same
# path inside the guest.
set -euo pipefail
WORKSPACE=${WORKSPACE:-$HOME/developer/embedded}
brew list lima >/dev/null 2>&1 || brew install lima
limactl create --name=kdev --vm-type=vz --cpus=6 --memory=8 --disk=80 \
	--set ".mounts=[{\"location\":\"$WORKSPACE\",\"writable\":true}]" \
	--tty=false template:ubuntu-24.04
limactl start kdev
limactl shell kdev -- bash -c 'sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq &&
	sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential \
	"linux-headers-$(uname -r)" python3 sparse flex bison bc libssl-dev libelf-dev dwarves git'
echo "VM ready: limactl shell kdev"
