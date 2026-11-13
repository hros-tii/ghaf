# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Build with pkgsStatic so the binary needs nothing from the (unauthenticated)
# guest rootfs at run time.
{ stdenv }:
stdenv.mkDerivation {
  pname = "g2g-agent";
  version = "0.1";
  src = ./.;
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    $CC -O2 -Wall -Wextra -o g2g-agent g2g-agent.c
    runHook postBuild
  '';
  installPhase = ''
    install -Dm755 g2g-agent $out/bin/g2g-agent
  '';
}
