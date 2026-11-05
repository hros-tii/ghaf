# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
{
  config,
  lib,
  pkgs,
  ...
}:
{
  _file = ./vm-protected.nix;

  options.ghaf.virtualization.microvm.protected-vm = {
    enable = lib.mkEnableOption "this guest to be run as a protected VM under the pKVM hypervisor.";

    firmware = lib.mkOption {
      type = lib.types.nullOr lib.types.path;
      default = null;
      description = "pvmfw image to boot this protected VM through. If null, the VM boots without firmware.";
    };

    pkvmFirmware = lib.mkEnableOption ''
      booting this protected VM through the pvmfw that pKVM owns (--protected-vm),
      rather than one the VMM loads from a host file'';
  };

  config = lib.mkIf config.ghaf.virtualization.microvm.protected-vm.enable {
    assertions = [
      {
        assertion = pkgs.stdenv.hostPlatform.isAarch64;
        message = "ghaf.virtualization.microvm.protected-vm expects ARM64 platform; got ${pkgs.stdenv.hostPlatform.system}.";
      }
    ];

    microvm.hypervisor = "crosvm";

    microvm.crosvm.extraArgs = [
    ]
    ++ (
      if config.ghaf.virtualization.microvm.protected-vm.pkvmFirmware then
        [ "--protected-vm" ]
      else if config.ghaf.virtualization.microvm.protected-vm.firmware == null then
        [ "--protected-vm-without-firmware" ]
      else
        [
          "--protected-vm-with-firmware"
          "${config.ghaf.virtualization.microvm.protected-vm.firmware}"
        ]
    )
    ++ [
      "--unmap-guest-memory-on-fork"
      "--disable-sandbox"
      "--smccc-trng"
      "--swiotlb"
      "128"
    ];

    microvm.kernelParams = [
      "sysctl.fs.fuse.max_pages_limit=64"
      "swiotlb=65536,4"
    ];

    microvm.crosvm.vfioIommu = "pkvm-iommu";
  };
}
