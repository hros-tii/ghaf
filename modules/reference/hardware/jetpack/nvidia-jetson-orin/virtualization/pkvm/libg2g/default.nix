# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# libg2g (static library and headers) and g2g-echo, its test tool. Build with
# pkgsStatic so the binaries need nothing from the unauthenticated guest rootfs.
{ stdenv, openssl }:
stdenv.mkDerivation {
  pname = "libg2g";
  version = "0.1";
  src = ./.;
  buildInputs = [ openssl ];
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    $CC -O2 -Wall -Wextra -c g2g.c -o g2g.o
    $AR rcs libg2g.a g2g.o
    $CC -O2 -Wall -Wextra -o g2g-echo g2g-echo.c libg2g.a -lcrypto
    $CC -O2 -Wall -Wextra -o g2g-proxy g2g-proxy.c libg2g.a -lcrypto
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 g2g-echo $out/bin/g2g-echo
    install -Dm755 g2g-proxy $out/bin/g2g-proxy
    install -Dm644 libg2g.a $out/lib/libg2g.a
    install -Dm644 -t $out/include g2g.h g2g_ring.h pkvm_g2g.h
    runHook postInstall
  '';
}
