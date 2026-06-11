# Ansible: MinIO S3-over-RDMA server and hipObject client

This directory provisions two classes of hosts: a **MinIO RDMA server**
(AIStor `.deb` URL from [STUFF.md](../STUFF.md)) and an **AMD GPU client**
that applies `sbates130272.batesste` roles (`rdma_setup`, `rocm_setup`),
builds hipObject with `HIPOBJ_MINIO_CLIENT`, and runs the
`minio-getput-rdma` example against GPU memory per
[integrations/minio-cpp/TESTING.md](../integrations/minio-cpp/TESTING.md).

## Prerequisites

- Ansible 2.14 or newer on the controller, plus SSH access to targets.
- Ubuntu 24.04 or 26.04 LTS targets (aligned with the batesste collection).
- Install the collection before the first playbook run (see Quick start).

The [ansible.cfg](ansible.cfg) file sets `collections_path` so Galaxy
installs the collection into `collections/`, which is gitignored; use the
Quick start `ansible-galaxy` command with `-p collections` after cloning.

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
| [group_vars/hipobj_clients.yml](group_vars/hipobj_clients.yml) | Clone URL, CMake flags, smoke size. |
| [playbooks/minio_rdma_server.yml](playbooks/minio_rdma_server.yml) | Download and install server `.deb`. |
| [playbooks/hipobj_client.yml](playbooks/hipobj_client.yml) | ROCm/RDMA roles plus hipObject build. |
| [playbooks/hipobj_smoke_test.yml](playbooks/hipobj_smoke_test.yml) | MinIO `mc` client + bucket + `minio-getput-rdma`. |
| [playbooks/site.yml](playbooks/site.yml) | Ordered import of the three plays above. |

## Variables worth tuning

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
  (defaults to linux-amd64 from dl.min.io; not the distro `mc` package).

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
[ansible-check.yml][ansible-ci]). Full `site.yml` is not executed in CI
because `rocm_setup`, `rdma_setup`, and GPU-backed S3 smoke tests require
lab hardware.

<!-- References -->

[ansible-ci]: ../.github/workflows/ansible-check.yml
[ansible-vault]: https://docs.ansible.com/ansible/latest/vault_guide/index.html
[minio-dl]: https://www.min.io/download#
