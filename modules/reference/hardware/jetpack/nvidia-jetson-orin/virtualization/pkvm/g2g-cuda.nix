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
# Not used together with g2g-ollama.nix: the g2g mailbox is one per VM.
role:
{
  lib,
  pkgs,
  ...
}:
let
  g2gCuda = pkgs.callPackage ./g2g-cuda { };
in
{
  imports = [ ./g2g-access.nix ];

  config = lib.mkMerge [
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
