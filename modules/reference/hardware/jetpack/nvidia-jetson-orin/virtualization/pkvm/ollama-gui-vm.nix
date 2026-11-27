# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Ollama in gui-vm, for the g2g-ollama demo, as a service on the Orin GPU. The
# package is Ghaf's ollama-cuda (overlays/custom-packages/ollama-cuda). Ollama
# listens only on 127.0.0.1:11434; app VMs reach it through g2g-proxy
# (g2g-ollama.nix).
{
  lib,
  pkgs,
  ...
}:
{
  services.ollama = {
    enable = true;
    package = pkgs.ollama-cuda;
    host = "127.0.0.1";
    port = 11434;
  };

  # The Jetson GPU is /dev/nvhost-* and /dev/nvmap, group video, which the
  # module's DeviceAllow list (made for desktop GPUs) does not cover. With any
  # DeviceAllow entry systemd allows only the listed devices, so the list is
  # emptied and DevicePolicy=auto then allows all of them.
  systemd.services.ollama.serviceConfig = {
    SupplementaryGroups = [ "video" ];
    DeviceAllow = lib.mkForce [ ];
    DevicePolicy = lib.mkForce "auto";
    PrivateDevices = lib.mkForce false;
    # The Jetson driver's libcuda.so.1 (the module's wrapper adds the CUDA
    # libraries in front of this).
    Environment = [ "LD_LIBRARY_PATH=/run/current-system/sw/lib" ];
  };
}
