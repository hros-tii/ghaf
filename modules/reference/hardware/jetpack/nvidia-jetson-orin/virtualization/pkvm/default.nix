# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
{
  config,
  lib,
  ...
}:
{
  _file = ./default.nix;

  imports = [
    ./orin-pkvm-host.nix
  ];

  options = {
    ghaf.host.kernel.hardening = {
      hypervisor.enable = lib.mkEnableOption "support for protected guests on Orin AGX";

      allowedPassthroughDevices = lib.mkOption {
        type = lib.types.listOf lib.types.str;
        default = [ ];
        apply = lib.unique;
        description = "Platform devices assignable to virtual guests on a pKVM-enabled system";
      };
    };

    ghaf.guest.hardening = {
      protected.enable = lib.mkEnableOption "protected mode for VMs on Orin AGX";
    };
  };

  config = lib.mkIf config.ghaf.host.kernel.hardening.hypervisor.enable {
    # Disable appvms that depend on desktop graphics. Display passthrough is
    # not ported yet.
    ghaf.reference.appvms = {
      chromium.enable = lib.mkForce false;
      flatpak.enable = lib.mkForce false;
    };

    # Apply the protected guest config to all Orin VMs
    ghaf.virtualization.vmConfig =
      let
        guestConfig = _: { imports = [ ./orin-pkvm-guest.nix ]; };
      in
      lib.mkIf config.ghaf.guest.hardening.protected.enable {
        sysvms = {
          # pVMs need more memory
          netvm.mem = 4096;
          adminvm.mem = 4096;
          # On protected VM, mem size needs to match the size of the 1:1 mapping.
          # crosvm rejects a RAM file-backed mapping that isn't a subset of one region,
          # and any leftover sliver would be ordinary non-identity memory.
          guivm.mem = 13312;

          netvm.extraModules = [
            guestConfig
            (import ./pvmfw-vm.nix "net-vm")
            {
              microvm.kernelParams = [
                "ignore_loglevel"
                "earlycon=uart8250,mmio,0x3f8"
                "keep_bootcon"
              ];
            }
          ];
          adminvm.extraModules = [
            guestConfig
            (import ./pvmfw-vm.nix "admin-vm")
            (import ./g2g-ollama.nix "client")
            {
              microvm.kernelParams = [
                "ignore_loglevel"
                "earlycon=uart8250,mmio,0x3f8"
                "keep_bootcon"
              ];
            }
          ];
          guivm.extraModules = [
            guestConfig
            (import ./pvmfw-vm.nix "gui-vm")
            (import ./g2g-ollama.nix "server")
            ./ollama-gui-vm.nix
            {
              microvm.kernelParams = [
              ];
            }
          ];
        };
        appvms = {
          chromium.extraModules = [ guestConfig ];
          flatpak.extraModules = [ guestConfig ];
        };
      };
  };
}
