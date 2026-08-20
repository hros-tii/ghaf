# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
{
  config,
  lib,
  ...
}:
let
  cfg = config.ghaf.hardware.nvidia.orin;
in
{
  _file = ./orin-pkvm-guest.nix;

  config = lib.mkIf (cfg.kernelVersion == "stable-6-18-pkvm") {
    ghaf.virtualization.vmConfig.sysvms = {
      netvm.extraModules = [
        {
          ghaf.virtualization.microvm.protected-vm.enable = true;
        }
      ];

      adminvm.extraModules = [
        {
          ghaf.virtualization.microvm.protected-vm.enable = true;
        }
      ];
    };
  };
}
