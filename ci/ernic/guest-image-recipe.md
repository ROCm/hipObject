# The guest image that carries both ionic_rdma and amdgpu

hipObject's CI needs one VM that has both an emulated AMD Pensando ionic RDMA
NIC (served by `rocm-ernic`) and an emulated gfx1250 (served by `rocjitsu`).
The `gpu-direct` lane in
[`.github/workflows/hipobject-hardware-test-gpu-direct.yml`](../../.github/workflows/hipobject-hardware-test-gpu-direct.yml)
used to derive that guest at the start of every run — boot bare, install a
kernel, patch and rebuild amdgpu, shut down, boot again with both vfio-user
functions — at about fifteen minutes a run for the same result every time.

That is now a published flavour, `ubuntu-qcow2-gen-ernic-rocjitsu`, and the
lane boots it once. This page records what the flavour carries, so a probe
failure can be attributed to the image or to the lane without re-deriving the
history. The recipe it was built from was verified green in CI at hipObject
`f193923`, through `amdgpu 0000:00:05.0` binding, `kfd kfd: added device
1002:75c1`, `ionic` at `PORT_ACTIVE`, and a successful `hipMalloc` on the
emulated device.

Pin a dated tag; `GUEST_ARTIFACT_TAG` in the lane is the one in use.

## Kernel

Mainline **v7.2.4**. 7.2 is a hard floor, not a preference: `ionic_rdma` calls
`ib_umem_get_va`, a `static inline` that exists in 7.2.4 and does not exist in
7.0 or 7.1.13.

## ionic / ionic_rdma

In-tree from 7.2.4, and the two `rocm-ernic` patches apply unmodified. The
guest supplies the kernel, headers and toolchain; building `ionic-ernic` as a
DKMS module stays with the lane, which does it through the ernic Ansible
collection.

## amdgpu

DKMS 7.1.9 does not build on 7.2.4 as shipped. Three patches are applied in
the image build, to `/usr/src/amdgpu-*` so they survive a later
`dkms autoinstall`:

| Patch | What it does |
| --- | --- |
| `0001-amdkfd-fail-closed-ptrace-gate-7.2` | Version-gates the dumpability check in `kfd_process_queue_manager.c`; on >= 7.2 it fails closed on `ns_capable(CAP_SYS_PTRACE)`. **Still wants a human sign-off** — it changes a security check, and it was written to build, not reviewed as policy. The guest records `"kfd_ptrace_gate_reviewed": false` in `/etc/ernic-rocjitsu-guest.json`; read that field rather than assuming the set is settled. |
| `0002-amdkcl-probe-panel-type-separately` | Adds `AC_AMDGPU_DRM_DISPLAY_INFO_PANEL_TYPE` and `AC_AMDGPU_DRM_MODE_PANEL_TYPE_LCD` configure probes and guards both uses. |
| `0003-amdgpu-ras-guard-vbios-query` | Guards the RAS vbios query on `adev->mode_info.atom_context`, which is NULL under emulation. |

The copies this repo used to carry in `ci/ernic/patches/amdgpu/` are gone; the
image build owns them, and `/etc/ernic-rocjitsu-guest.json` lists each applied
patch as `name:sha256[0:16]`.

## Firmware

The `vfio_guest_firmware.py --set gap` output — `gc_12_1_0_imu.bin`,
`gc_12_1_0_mes.bin`, `gc_12_1_0_mes1.bin` and a `rocjitsu-gap-manifest.json`
— is baked into `/lib/firmware/updates/amdgpu`. That set is static: the
generator's fixtures do not depend on which rocjitsu build serves the socket.

`ip_discovery.bin` is **not** baked in and cannot be, because it describes the
device the socket serves. It has to come from the same rocjitsu build the lane
runs (`ROCJITSU_IMAGE_GPU`), via `rj-ip-discovery gfx1250`; a mismatch makes
probe fail at -22 or -2. Generating and staging it is the lane's one remaining
piece of in-guest setup.

## Probe parameters

The guest ships them as `/usr/local/bin/amdgpu-probe`, and autoload is
blacklisted, so nothing loads the driver until the lane calls that helper:

```
emu_mode=1 discovery=2 fw_load_type=0 ip_block_mask=0x7f \
vm_update_mode=3 gpu_recovery=0 vramlimit=1024
```

Two of these are easy to get wrong:

- `ip_block_mask=0x7f`, not `0x3f`. This DKMS build enumerates an extra
  `ras_v1_0` at index 5, which pushes MES to 6. Masking it off makes
  `gfx_v12_1_xcc_cp_resume` dereference a NULL ring.
- `vramlimit=1024`. Not a performance knob: it is the budget ROCr provisions
  queue scratch from, and 256 runs scratch-free kernels fine while making a
  private-segment dispatch wait forever for an allocation that never arrives.
  Unrelated to `vram_aperture_bytes` in the rocjitsu profile, which is the BAR
  window.

`amdgpu-probe` refuses, loudly, when amdgpu is already resident with different
parameters — `modprobe` returns 0 in that case and silently discards
everything you passed it.

## Device node groups

`/dev/kfd` and `/dev/dri/render*` are `root:render` 0660. The image puts the
login user in `render` and `video`; without that the HIP runtime enumerates no
agent and `hipMalloc` returns 100 (`hipErrorNoDevice`) with a healthy KFD node
three lines up the log.

## Two things about the guest that are not the image's fault

- **>= 4 vCPUs.** `IONIC_EQ_COUNT_MIN` is 4, and below that `ionic_rdma` never
  registers the ibdev.
- **The guest cannot be warm-rebooted with the rocjitsu function attached** —
  the device reset wedges it. Nothing in the lane reboots; a step that starts
  to is a bug, not a slow path.
