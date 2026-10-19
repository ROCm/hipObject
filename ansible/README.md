# Ansible: MinIO S3-over-RDMA server and hipObject client

This directory provisions two classes of hosts: a **MinIO RDMA server**
(AIStor `.deb` URL in `group_vars/minio_rdma_servers.yml`) and an **AMD GPU client**
that applies the `rdma_setup` and `rocm_setup` roles (see
[What the client play changes](#what-the-client-play-changes)),
builds hipObject with `HIPOBJ_MINIO_CLIENT`, and runs the
`minio-getput-rdma` example against GPU memory per
[integrations/minio-cpp/TESTING.md](../integrations/minio-cpp/TESTING.md).

The two roles come from the `sbates130272.batesste` collection on Ansible
Galaxy, a personal collection that's maintained outside the ROCm
organization ([source][batesste-src]).

## Prerequisites

- ansible-core 2.18 or newer on the controller, plus SSH access to targets.
  The collection needs `community.general` 13.4 or newer, which needs
  ansible-core 2.18.
- Ubuntu 24.04 or 26.04 LTS targets.
- Install the collection before the first playbook run (see Quick start).

The [ansible.cfg](ansible.cfg) file sets `collections_path` so Galaxy
installs the collection into `collections/`, which is gitignored; use the
Quick start `ansible-galaxy` command with `-p collections` after cloning.
[requirements.yml](requirements.yml) accepts any version of the collection
from 1.0.0 on, so `ansible-galaxy` installs the newest release, including a
new major version.

## Quick start

```bash
cd ansible
ansible-galaxy collection install -r requirements.yml -p collections
cp inventory/hosts.example.yml inventory/hosts.yml
# Edit inventory/hosts.yml: hosts, users, endpoint, and secrets.
ansible-playbook playbooks/site.yml -i inventory/hosts.yml
```

Store `hipobj_minio_access_key` and `hipobj_minio_secret_key` with
[Ansible Vault][ansible-vault] or `ansible-playbook ... -e @secrets.yml`.
Keep private files untracked (see [.gitignore](.gitignore)).

## Layout

| Path | Role |
| ---- | ---- |
| [requirements.yml](requirements.yml) | Galaxy collection `sbates130272.batesste`. |
| [inventory/hosts.example.yml](inventory/hosts.example.yml) | Example two-node inventory. |
| [group_vars/minio_rdma_servers.yml](group_vars/minio_rdma_servers.yml) | `.deb` URL, checksum, RDMA extras. |
| [group_vars/hipobj_clients.yml](group_vars/hipobj_clients.yml) | Clone URL and directories, CMake flags, role settings, `mc` download, smoke size. |
| [playbooks/minio_rdma_server.yml](playbooks/minio_rdma_server.yml) | Download and install server `.deb`. |
| [playbooks/hipobj_client.yml](playbooks/hipobj_client.yml) | ROCm/RDMA roles plus hipObject build. |
| [playbooks/hipobj_smoke_test.yml](playbooks/hipobj_smoke_test.yml) | MinIO `mc` client + bucket + `minio-getput-rdma`. |
| [playbooks/site.yml](playbooks/site.yml) | Ordered import of the three plays above. |

## What the client play changes

[playbooks/hipobj_client.yml](playbooks/hipobj_client.yml) runs both roles
on every client, with the settings in
[group_vars/hipobj_clients.yml](group_vars/hipobj_clients.yml). Between
them, they:

- Run `apt upgrade` (the safe variant) and `apt autoremove`, which updates
  every package on the host. `rdma_setup` does this unless
  `rdma_setup_skip_system_upgrade` is true, and the group variables don't
  set it. (They set `rocm_setup_skip_system_upgrade`, which is `rocm_setup`'s
  own setting.)
- Install the headers for the running kernel, its `linux-modules-extra`
  package when there is one, the RDMA userspace and development packages,
  and `perftest`, and copy an `rdma-detect` script to `/usr/local/bin`.
- On Ubuntu 24.04, add a third-party PPA (`ppa:crazyguitar/rdmatop`) and
  install `rdmatop` from it, unless `rdma_setup_install_rdmatop` is false.
- Add AMD's ROCm package repository for ROCm 10 and later
  (stable.repo.amd.com), install ROCm and `amdgpu-dkms`, write
  `/etc/profile.d/rocm.sh`, and add ROCm's libraries to the loader cache.
- Remove any ROCm installed from repo.radeon.com, ROCm's older package
  repository: the packages named `rocm` followed by a digit, `hsa-rocr*`,
  `amdgpu-install`, and `rock-dkms`. `rocm_setup` does this unless
  `rocm_setup_purge_other_stream` is false.

Neither role reboots the host with these settings. Reboot it yourself if
the run installed or upgraded `amdgpu-dkms` or the kernel. The group
variables turn off the roles' GPU checks, AMD Device Metrics Exporter,
`rdma_exporter`, and RDMA MOTD; the roles' own READMEs describe the rest of
their settings.

## Variables worth tuning

- `hipobj_source_dir` and `hipobj_build_dir`: where the client clones and
  builds hipObject. The clone goes to `~/Projects/hipobject` on the client
  by default, and the build to `build-ansible` inside it.
- `hipobj_repo_version`: the branch, tag, or commit to build (`main` by
  default).
- `hipobj_cmake_hipobj_bnxt` / `hipobj_cmake_hipobj_ionic`: which RDMA
  backends to build. The defaults build the BNXT backend only; turn on
  ionic for a client with an AMD Pensando NIC.
- `hipobj_mc_endpoint_url` defaults to `https://` plus `hipobj_s3_endpoint_host`
  for `mc`. The shipped `minio-getput-rdma` example uses HTTP in
  `BaseUrl`; match your lab or adjust client code if you require TLS on
  the control plane.
- `hipobj_minio_manage_service` / `hipobj_minio_systemd_unit`: keep false
  until you confirm the unit name from `dpkg -L` on an installed host.
- `hipobj_server_install_rdma_stack`: set false if the server image already
  includes RDMA userspace libraries.
- `hipobj_minio_mc_download_url` / `hipobj_minio_mc_install_path` /
  `hipobj_minio_mc_checksum`: MinIO Client (`mc`) install for smoke tests
  (defaults to the linux-amd64 GitHub release asset named by
  `hipobj_minio_mc_release`, checked against its SHA256; not the distro `mc`
  package). Override the checksum along with the URL.

## Licensing

MinIO AIStor terms and download policy are described on the vendor site;
see [MinIO download][minio-dl].

## Out of scope (documented gaps)

These items appear in lab docs but are **not** automated by these
playbooks: **cuObjServer**, the **RC-to-DC adapter**, RoCEv2 tuning
(PFC/ECN), firewalls, and unknown post-install service wiring inside the
edge `.deb`. Extend playbooks after you capture unit names and env files
from a reference host.

## Syntax check

```bash
cd ansible
ansible-galaxy collection install -r requirements.yml -p collections
ansible-playbook --syntax-check playbooks/site.yml -i inventory/hosts.example.yml
```

GitHub Actions runs the same Galaxy install, syntax-check on all
playbooks, and `ansible-lint` when files under `ansible/` change (see
[hipobject-ansible.yml][ansible-ci]). Full `site.yml` is not executed in CI
because `rocm_setup`, `rdma_setup`, and GPU-backed S3 smoke tests require
lab hardware.

<!-- References -->

[ansible-ci]: ../.github/workflows/hipobject-ansible.yml
[ansible-vault]: https://docs.ansible.com/ansible/latest/vault_guide/index.html
[batesste-src]: https://github.com/sbates130272/batesste-ansible
[minio-dl]: https://www.min.io/download#
