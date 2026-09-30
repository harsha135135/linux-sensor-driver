#!/usr/bin/env bash
# Build and install a debug kernel for lockdep/KASAN validation (inside the VM).
#
# Source: kernel.org 6.8.12, the last 6.8.y release (the stock guest runs
# Ubuntu 6.8.0-x). Config: the running kernel's config, trimmed to the modules
# currently loaded (localmodconfig) so the build takes minutes, not hours,
# with debug options added. The stock kernel stays the GRUB default; the debug
# kernel is selected for ONE boot with grub-reboot.
set -euo pipefail
VER=6.8.12
SRC=$HOME/kbuild/linux-$VER
mkdir -p "$HOME/kbuild"
cd "$HOME/kbuild"
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential bc bison flex \
	libssl-dev libelf-dev dwarves rsync kmod cpio debhelper-compat >/dev/null
[ -d "$SRC" ] || { wget -q "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VER.tar.xz"; tar xf "linux-$VER.tar.xz"; }
cd "$SRC"
cp "/boot/config-$(uname -r)" .config
yes '' | make localmodconfig >/dev/null
./scripts/config \
	--set-str LOCALVERSION "-vsdebug" \
	-e PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP -e DEBUG_LIST \
	-e DEBUG_OBJECTS -e DEBUG_OBJECTS_HRTIMERS -e DEBUG_OBJECTS_TIMERS \
	-e KASAN -e KASAN_GENERIC -e KASAN_INLINE \
	-e DEBUG_KMEMLEAK -e DEBUG_KMEMLEAK_AUTO_SCAN \
	-e UBSAN -e UBSAN_BOUNDS \
	-d DEBUG_INFO_BTF -d DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT -e DEBUG_INFO_NONE \
	--set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS "" \
	-d MODULE_SIG_ALL -d SECURITY_LOCKDOWN_LSM
make olddefconfig >/dev/null
echo "debug options in final .config:"
grep -E '^CONFIG_(PROVE_LOCKING|DEBUG_ATOMIC_SLEEP|KASAN|KASAN_GENERIC|DEBUG_LIST|DEBUG_OBJECTS_HRTIMERS|DEBUG_KMEMLEAK|UBSAN)=' .config
make -j"$(nproc)" bindeb-pkg >"$HOME/kbuild/build.log" 2>&1
cd "$HOME/kbuild"
sudo dpkg -i linux-image-${VER}-vsdebug_*.deb linux-headers-${VER}-vsdebug_*.deb
# One-shot boot into the debug kernel; the stock kernel remains the default.
sudo sed -i 's/^GRUB_DEFAULT=.*/GRUB_DEFAULT=saved/' /etc/default/grub
sudo update-grub >/dev/null 2>&1
sudo grub-set-default 0
ENTRY=$(sudo grep -oE "menuentry '[^']*${VER}-vsdebug'" /boot/grub/grub.cfg | head -1 | cut -d"'" -f2)
sudo grub-reboot "Advanced options for Ubuntu>${ENTRY}"
echo "installed; next boot (only) will use: ${ENTRY}"
