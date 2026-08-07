# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Wire the Ghaf-patched QEMU into microvm.nix so all VMs use it.
#
{ config, inputs, ... }:
{
  imports = [ inputs.self.nixosModules.ghaf-qemu ];

  microvm.qemu.package = config.ghaf.virtualization.qemu.package;

  microvm.crosvm.package = config.ghaf.virtualization.crosvm.package.overrideAttrs (oldAttrs: {
    buildFeatures = oldAttrs.buildFeatures ++ config.ghaf.virtualization.crosvm.features;
  });

  # The host runs VMMs as the unprivileged `microvm` user. Crosvm's
  # multiprocess minijail needs CAP_SYS_ADMIN to create PID and mount
  # namespaces, so retain the unprivileged service boundary and use
  # single-process mode until a capability-scoped sandbox is wired.
  microvm.crosvm.extraArgs = [ "--disable-sandbox" ];
}
