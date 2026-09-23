# amdgpu DKMS patches for the emulated-GPU CI lane

These apply to the amdgpu DKMS tree (`/usr/src/amdgpu-7.1.9-*`) shipped in the
rocjitsu guest image. `ernic-rocjitsu-gpu` scp's them into the guest and runs
`patch -p1` before rebuilding DKMS.

They live here only because the rocjitsu guest image does not carry them yet.
Their real home is that image's build. Delete this directory, and the
`Patch and rebuild the amdgpu DKMS module` step, once it does.

| Patch | Needed when | Why |
| --- | --- | --- |
| `0001-amdkfd-fail-closed-ptrace-gate-7.2` | guest kernel >= 7.2 | 7.2 moved dumpability and the exec-time `user_ns` off `mm_struct` onto `task->exec_state`, and neither `task_exec_state_get_dumpable()` nor `task_exec_state_rcu()` is exported, so an out-of-tree module cannot read either. The gate fails closed instead: require `CAP_SYS_PTRACE` unconditionally. Version-gated, so <= 7.1 keeps the upstream logic verbatim. Strictly no weaker than the in-tree check. |
| `0002-amdkcl-probe-panel-type-separately` | any mainline kernel >= 6.19 | `amdgpu_dm_set_panel_type()` is gated on `HAVE_DRM_DISPLAY_INFO_AMD_VSDB`, whose probe cites a commit that *did* merge at v6.19 — but the function body uses `display_info->panel_type` and `DRM_MODE_PANEL_TYPE_LCD`, which are AMD-downstream only and never merged. Adds two honest probes and guards both uses. A plain bug, worth sending to AMD regardless of this lane. |
| `0003-amdgpu-ras-guard-vbios-query` | always, under rocjitsu | rocjitsu serves no option ROM (`rombar=0`), so `adev->mode_info.atom_context` is NULL. `amdgpu_ras_query_ras_capablity_from_vbios()` dereferences it unconditionally and the probe oopses in `amdgpu_atom_parse_data_header+0x9`. |

Patch 3 is what previously parked this lane. It was recorded as a "hang in
`amdgpu_ras_init`"; it is a NULL-pointer oops, and the second failure behind it
was `ip_block_mask=0x3f` dropping MES — this DKMS build enumerates an extra
`ras_v1_0` at index 5, so MES sits at 6 and the mask has to be `0x7f`.
