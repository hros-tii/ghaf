# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# g2g-cuda as a service: the GPU VM serves CUDA to app VMs over libg2g.
#
#   role = "server": the GPU VM. g2g-cuda-server runs as a systemd service,
#                    as its own user, not root.
#   role = "client": an app VM. The g2g-cuda shim is this VM's libcuda.so.1,
#                    so every CUDA application loads it with no setup.
#
# Both roles give /dev/pkvm-g2g (mailbox and shared pages) and /dev/open-dice0
# (this VM's identity) to the group g2g. open-dice0 is read-only for the
# group: a write to it wipes the DICE secrets.
role:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  g2gCuda = pkgs.callPackage ./g2g-cuda { };
in
{
  config = lib.mkMerge [
    {
      users.groups.g2g = { };
      # The initrd stages the peers file (the whitelist) root-only.
      systemd.tmpfiles.rules = [ "z /run/g2g/peers 0440 root g2g -" ];
      services.udev.extraRules = ''
        KERNEL=="pkvm-g2g", GROUP="g2g", MODE="0660"
        KERNEL=="open-dice0", GROUP="g2g", MODE="0640"
      '';
    }

    (lib.mkIf (role == "server") {
      users.groups.g2g-cuda = { };
      users.users.g2g-cuda = {
        isSystemUser = true;
        group = "g2g-cuda";
        extraGroups = [
          "g2g"
          "video"
        ];
      };

      systemd.services.g2g-cuda-server = {
        description = "CUDA for app VMs over g2g, on this VM's GPU";
        wantedBy = [ "multi-user.target" ];
        after = [ "systemd-modules-load.service" ];
        serviceConfig = {
          ExecStart = "${g2gCuda}/bin/g2g-cuda-server";
          User = "g2g-cuda";
          Group = "g2g-cuda";
          SupplementaryGroups = [
            "g2g"
            "video"
          ];
          # The GPU driver and /dev/pkvm-g2g may come up after us: retry.
          Restart = "always";
          RestartSec = 2;
          Environment = [
            "G2G_LIBCUDA=/run/current-system/sw/lib/libcuda.so.1"
            "CUDA_CACHE_PATH=/var/cache/g2g-cuda"
          ];
          CacheDirectory = "g2g-cuda";
          NoNewPrivileges = true;
          ProtectHome = true;
          PrivateTmp = true;
        };
      };
    })

    (lib.mkIf (role == "client") {
      # CUDA applications on NixOS look for the driver in /run/opengl-driver/lib.
      systemd.tmpfiles.rules = [
        "d /run/opengl-driver/lib 0755 root root -"
        "L+ /run/opengl-driver/lib/libcuda.so.1 - - - - ${g2gCuda}/lib/g2g-cuda/libcuda.so.1"
        "L+ /run/opengl-driver/lib/libcuda.so - - - - ${g2gCuda}/lib/g2g-cuda/libcuda.so.1"
      ];
      users.users.ghaf.extraGroups = [ "g2g" ];
    })
  ];
}
