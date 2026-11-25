# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Ollama for app VMs over g2g: an app VM reaches the Ollama in the GPU VM as if
# it ran locally, on 127.0.0.1:11434, with no network between the two VMs.
#
#   role = "server": the GPU VM. g2g-proxy connects each channel from a listed
#                    peer to the local Ollama, 127.0.0.1:11434.
#   role = "client": an app VM. g2g-proxy listens on 127.0.0.1:11434 and
#                    carries each connection to the GPU VM.
#
# g2g-proxy carries the bytes as they are, so every Ollama client (ollama run,
# curl, chat UIs, the Python library) works unchanged. Ollama itself is set up
# separately in the GPU VM. Not used together with g2g-cuda.nix: the g2g mailbox
# is one per VM.
role:
{
  lib,
  pkgs,
  ...
}:
let
  libg2g = pkgs.pkgsStatic.callPackage ./libg2g { };
  port = toString 11434;
  server = "gui-vm";
in
{
  imports = [ ./g2g-access.nix ];

  config = {
    users.groups.g2g-proxy = { };
    users.users.g2g-proxy = {
      isSystemUser = true;
      group = "g2g-proxy";
      extraGroups = [ "g2g" ];
    };

    systemd.services.g2g-ollama = {
      description =
        if role == "server" then
          "Ollama for app VMs over g2g"
        else
          "Ollama in ${server} over g2g, on 127.0.0.1:${port}";
      wantedBy = [ "multi-user.target" ];
      after = [ "network.target" ] ++ lib.optional (role == "server") "ollama.service";
      serviceConfig = {
        ExecStart =
          if role == "server" then
            "${libg2g}/bin/g2g-proxy server ${port}"
          else
            "${libg2g}/bin/g2g-proxy client ${server} ${port}";
        User = "g2g-proxy";
        Group = "g2g-proxy";
        SupplementaryGroups = [ "g2g" ];
        Restart = "always";
        RestartSec = 2;
        NoNewPrivileges = true;
        ProtectSystem = "strict";
        ProtectHome = true;
        PrivateTmp = true;
      };
    };
  };
}
