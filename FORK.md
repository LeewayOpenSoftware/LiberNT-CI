# LiberNT-LDW: the Leeway Open Software development fork of LiberNT

This repository, **LiberNT-LDW** (Leeway Development Works, https://github.com/LeewayOpenSoftware/LiberNT-LDW), is the Leeway Open Software development fork of LiberNT (upstream: https://github.com/LiberNT/LiberNT). LDW is a provisional designation for Leeway's engineering environment, not a separate operating system: the OS is and remains LiberNT. `main-nt10` is a pure mirror of upstream, synced often; our fixes and improvements live on topic branches rebased onto it. Part of this work comes from our research in a parallel project, Aureline - Asterion (our Wine and ReactOS forks).

| Branch | What it changes | Modules | Status |
|---|---|---|---|
| `fix/ndis-deregister-uaf` | Removing an NDIS 5 miniport that registered a control device no longer bugchecks (0x7E): the symbolic link name is copied and read before the device is deleted. | ndis | Bug reproduced on real hardware (HP dx2400); fix reviewed and built. |
| `diag/ndis6-start-logging` | Every silent failure on the NDIS 6 start path is logged; checked builds count interrupts, DPCs, receives, sends and OIDs. | ndis | Built; logging exercised on real hardware. |
| `fix/acpi-bridge-prt` | `_PRT` tables on PCI-PCI bridges and PCIe root ports are honoured; pci.sys logs the interrupt route it picks. | acpi, pci | Built; virtual machine verification pending; not published yet. |
| `feature/pnp-auto-install` | Network drivers install from driver folders without reboot loops or duplicate connections. | newdev, setupapi, netcfgx | Built; virtual machine test pending; not published yet. |
| `leeway/readme` | This file and the README section. | docs | Done. |

Everything here is licensed as upstream LiberNT (GPL 3.0, see COPYING).
