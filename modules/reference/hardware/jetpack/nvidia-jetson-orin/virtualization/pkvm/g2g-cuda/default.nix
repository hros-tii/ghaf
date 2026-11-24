# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# g2g-cuda: the CUDA Driver API over libg2g.
#   bin/g2g-cuda-server           runs forwarded calls on this VM's GPU
#   lib/g2g-cuda/libcuda.so.1     stand-in libcuda for a VM without a GPU
#   bin/g2g-vecadd                test: vector add through whatever libcuda.so.1 it finds
# Dynamic (glibc): the server loads the real libcuda.so.1, a glibc library.
{ stdenv, openssl }:
stdenv.mkDerivation {
  pname = "g2g-cuda";
  version = "0.1";
  src = ./.;
  buildInputs = [ openssl ];
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    L=${../libg2g}
    CFLAGS="-O2 -Wall -Wextra -I$L"
    $CC $CFLAGS -fPIC -fvisibility=hidden -c $L/g2g.c -o g2g.o
    $CC $CFLAGS -o g2g-cuda-server server.c g2g.o -lcrypto -ldl
    $CC $CFLAGS -shared -fPIC -fvisibility=hidden -Wl,-soname,libcuda.so.1 \
      -o libcuda.so.1 shim.c g2g.o -lcrypto -lpthread
    $CC $CFLAGS -o g2g-vecadd vecadd.c -ldl
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 g2g-cuda-server $out/bin/g2g-cuda-server
    install -Dm755 g2g-vecadd $out/bin/g2g-vecadd
    install -Dm755 libcuda.so.1 $out/lib/g2g-cuda/libcuda.so.1
    ln -s libcuda.so.1 $out/lib/g2g-cuda/libcuda.so
    runHook postInstall
  '';
  # After the fixup phase shrinks RPATHs: libcuda.so.1 is dlopen()ed, so the
  # driver directory would otherwise be dropped.
  postFixup = ''
    patchelf --add-rpath /run/opengl-driver/lib $out/bin/g2g-vecadd
  '';
}
