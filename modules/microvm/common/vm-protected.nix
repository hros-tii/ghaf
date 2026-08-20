# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
{
  config,
  lib,
  pkgs,
  ...
}:
let
  jetsonKernelDrv = pkgs.linux_6_18_jetson_pkvm.override {
    argsOverride.defconfig = "guest_defconfig";
  };
in
{
  _file = ./pkvm-guest.nix;

  options.ghaf.virtualization.microvm.protected-vm = {
    enable = lib.mkEnableOption "this guest to be run as a protected VM under the pKVM hypervisor.";
  };

  config = lib.mkIf config.ghaf.virtualization.microvm.protected-vm.enable {

    microvm.crosvm.extraArgs = [
      "--protected-vm-without-firmware"
      "--unmap-guest-memory-on-fork"
      "--smccc-trng"
    ];

    boot.kernelPackages = pkgs.linuxPackagesFor jetsonKernelDrv;

    assertions = [
      {
        assertion = pkgs.stdenv.hostPlatform.isAarch64;
        message = "ghaf.virtualization.microvm.protected-vm expected Jetson aarch64 platform; got ${pkgs.stdenv.hostPlatform.system}.";
      }
    ];
  };
}
