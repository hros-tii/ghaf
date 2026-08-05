# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# tiiuae fork of crosvm to bring pKVM patches
{ prev }:
let
  version = "0-develop-2026-07-23";
  src = prev.fetchFromGitHub {
    owner = "tiiuae";
    repo = "crosvm";
    rev = "0b294d1c87ba3bcd7127df4706c6caf092c516ab"; # develop
    fetchSubmodules = true;
    hash = "sha256-+aM0ifaxwWLtIk9YbCETixZWh/4fFWCcoCtA7XOpE9Y=";
  };
  cargoHash = "sha256-NEmMsCuiEOkanGwT/Oib9yhP+UeT+bCwGI9I3DCWyWU=";
in
prev.crosvm.overrideAttrs (old: {
  inherit version src cargoHash;
  # buildRustPackage extendDrvArgs closes over the original cargoHash from
  # nixpkgs when building cargoDeps, so overriding only cargoHash is not
  # picked up by the vendor build
  cargoDeps = prev.rustPlatform.fetchCargoVendor {
    inherit (old) pname;
    inherit src version;
    hash = cargoHash;
  };
})
