#!/bin/bash
# usage: ./run.sh            -> build, run tests, auto power off
#        ./run.sh shell      -> drop to shell (gdb/ftrace/oops practice)
set -e
cd "$(dirname "$0")"
KIMG=${KIMG:-/boot/vmlinuz-$(uname -r)}
[ -r "$KIMG" ] || { echo "cannot read $KIMG (sudo chmod a+r it, or set KIMG=)"; exit 1; }
BB=$(command -v busybox) || { echo "install busybox (static)"; exit 1; }
make
rm -rf rootfs; mkdir -p rootfs/{bin,proc,sys,dev}
cp "$BB" rootfs/bin/busybox
for a in sh mount insmod rmmod dmesg poweroff cat ls grep uname echo; do ln -sf busybox rootfs/bin/$a; done
cp edu_drv.ko test rootfs/; cp init rootfs/init; chmod +x rootfs/init
(cd rootfs && find . | cpio -o -H newc 2>/dev/null | gzip) > initramfs.gz
qemu-system-x86_64 -M q35 -m 512 -nographic -accel kvm -accel tcg \
  -kernel "$KIMG" -initrd initramfs.gz \
  -append "console=ttyS0 nokaslr $1 $EXTRA" -device edu -s
