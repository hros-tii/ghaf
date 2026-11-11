# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
{ stdenv }:
stdenv.mkDerivation {
  pname = "g2g-demo";
  version = "0.1";
  src = ./.;
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    $CC -O2 -Wall -Wextra -o g2g-demo g2g-demo.c
    runHook postBuild
  '';
  installPhase = ''
    install -Dm755 g2g-demo $out/bin/g2g-demo
  '';
}
