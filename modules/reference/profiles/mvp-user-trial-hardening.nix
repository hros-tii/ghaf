# Copyright 2022-2024 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
{ config, lib, pkgs, ... }:

let
  cfg = config.ghaf.reference.profiles.mvp-user-trial-hardening;
in
{
  imports = [ ./mvp-user-trial.nix ];

  options.ghaf.reference.profiles.mvp-user-trial-hardening = {
    enable = lib.mkEnableOption "Enable the mvp configuration for security features";
  };

  config = lib.mkIf cfg.enable {
    users.users.microvm.extraGroups = 
      lib.mkIf (config.security.tpm2.enable && config.security.tpm2.tssGroup != null) [
        config.security.tpm2.tssGroup
      ];

    systemd.tmpfiles.rules = [
      "f /tmp/cancel 0770 microvm kvm -"
    ];

    ghaf = {
      users.admin.createHome = true; # TEMP: testing

      reference = {
        profiles = {
          mvp-user-trial.enable = true;
        };
      };

      storage.encryption.enable = false;

      virtualization.microvm = let
        tpmRmPassthroughModule = {
          security.tpm2.enable = true;

          microvm.qemu.extraArgs = [
            "-tpmdev"
            "passthrough,id=tpmrm0,path=/dev/tpmrm0,cancel-path=/tmp/cancel"
            "-device"
            "tpm-tis,tpmdev=tpmrm0"
          ];

          environment.systemPackages = [
            pkgs.tpm2-tools
            pkgs.tpm2-tss
          ];
        };
      in {
        guivm.extraModules = [ tpmRmPassthroughModule ];
        netvm.extraModules = [ tpmRmPassthroughModule ];
        adminvm.extraModules = [ tpmRmPassthroughModule ];
      };

      # disable plymouth: not integrated yet with LUKS PIN prompt
      graphics.boot.enable = false;

      profiles.graphics = {
        idleManagement.enable = false; ## TEMP: testing
      };
    };
  };
}
