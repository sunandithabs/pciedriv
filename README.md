# pciedriv

Linux PCIe driver for QEMU's `edu` educational device (`1234:11e8`). Covers BAR/MMIO mapping, MSI interrupts, coherent DMA under a 28-bit mask, and a misc character device, with a self-contained QEMU test harness.

**v2.0**

## Features

- MMIO access to the device's factorial unit, with completion signalled by MSI (falls back to INTx)
- `read()`/`write()` on `/dev/edu`: each call does a DMA transfer to/from the device's 4 KB buffer, with offsets via `lseek`/`pread`/`pwrite`
- `ioctl` interface: factorial, DMA round-trip self-test, device info, optional crash trigger
- `debugfs` stats at `/sys/kernel/debug/edu/stats` (irqs, fact ops, DMA bytes, timeouts)
- `timeout_ms` module parameter (runtime-tunable)
- Mutex-serialized device access; IRQs masked on remove
- Test suite (11 checks incl. 4-process concurrency) that runs in a throwaway QEMU guest

## Build and run

Needs: `qemu-system-x86_64`, static `busybox`, kernel headers for the running kernel, `cpio`, a readable `/boot/vmlinuz-$(uname -r)`.

```bash
./run.sh          # build, boot guest, run tests, power off
./run.sh shell    # drop to a guest shell (gdb on :1234 via -s, ftrace, oops practice)
make              # build module + test binary only
```

## Interface

| ioctl | Description |
|---|---|
| `EDU_IOC_FACT` | in: n, out: n! (mod 2^32) |
| `EDU_IOC_DMA_TEST` | 4 KB DMA round trip, 0 = ok |
| `EDU_IOC_INFO` | `struct edu_info`: id, irq, msi, buf_size |
| `EDU_IOC_OOPS` | NULL deref for crash analysis; needs `allow_oops=1` |

| module param | Default | Description |
|---|---|---|
| `allow_oops` | 0 | enable `EDU_IOC_OOPS` |
| `timeout_ms` | 1000 | per-operation timeout |

## Crash-analysis practice

```bash
EXTRA="MODARGS=allow_oops=1" ./run.sh shell   # then: /test oops
```
Map the `edu_ioctl+0x..` offset from the oops to a source line with `gdb edu_drv.ko` -> `list *(edu_ioctl+0x..)`, or `addr2line`.

## Layout

`edu_drv.c` driver, `edu_uapi.h` userspace ABI, `test.c` tests, `run.sh`/`init` QEMU harness.

## Changelog

- **2.0**: read/write DMA path, `EDU_IOC_INFO`, debugfs stats, `timeout_ms`, IRQ masking on remove, expanded tests.
- **1.0**: MMIO, MSI, DMA self-test, misc device, oops trigger.

GPL-2.0
