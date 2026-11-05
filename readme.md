# pciedriv

linux pcie driver for qemu's `edu` educational device (`1234:11e8`). covers bar/mmio mapping, msi interrupts, coherent dma under a 28-bit mask, and a misc character device, with a self-contained qemu test harness.

**v2.0**

## features

- mmio access to the device's factorial unit, with completion signalled by msi (falls back to intx)
- `read()`/`write()` on `/dev/edu`: each call does a dma transfer to/from the device's 4 kb buffer, with offsets via `lseek`/`pread`/`pwrite`
- `ioctl` interface: factorial, dma round-trip self-test, device info, optional crash trigger
- `debugfs` stats at `/sys/kernel/debug/edu/stats` (irqs, fact ops, dma bytes, timeouts)
- `timeout_ms` module parameter (runtime-tunable)
- mutex-serialized device access; irqs masked on remove
- test suite (11 checks incl. 4-process concurrency) that runs in a throwaway qemu guest

## build and run

needs: `qemu-system-x86_64`, static `busybox`, kernel headers for the running kernel, `cpio`, a readable `/boot/vmlinuz-$(uname -r)`.

```bash
./run.sh          # build, boot guest, run tests, power off
./run.sh shell    # drop to a guest shell (gdb on :1234 via -s, ftrace, oops practice)
make              # build module + test binary only
```

## interface

| ioctl | description |
|---|---|
| `EDU_IOC_FACT` | in: n, out: n! (mod 2^32) |
| `EDU_IOC_DMA_TEST` | 4 kb dma round trip, 0 = ok |
| `EDU_IOC_INFO` | `struct edu_info`: id, irq, msi, buf_size |
| `EDU_IOC_OOPS` | null deref for crash analysis; needs `allow_oops=1` |

| module param | default | description |
|---|---|---|
| `allow_oops` | 0 | enable `EDU_IOC_OOPS` |
| `timeout_ms` | 1000 | per-operation timeout |

## crash-analysis practice

```bash
EXTRA="MODARGS=allow_oops=1" ./run.sh shell   # then: /test oops
```
map the `edu_ioctl+0x..` offset from the oops to a source line with `gdb edu_drv.ko` -> `list *(edu_ioctl+0x..)`, or `addr2line`.

## layout

`edu_drv.c` driver, `edu_uapi.h` userspace abi, `test.c` tests, `run.sh`/`init` qemu harness.

## changelog

- **2.0**: read/write dma path, `EDU_IOC_INFO`, debugfs stats, `timeout_ms`, irq masking on remove, expanded tests.
- **1.0**: mmio, msi, dma self-test, misc device, oops trigger.

gpl-2.0
