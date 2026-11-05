# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
{ stdenv, gnu-efi }:
stdenv.mkDerivation {
  pname = "pvmfw-loader";
  version = "0.1";
  src = ./.;
  buildInputs = [ gnu-efi ];
  dontConfigure = true;
  buildPhase = ''
    runHook preBuild
    $CC -I${gnu-efi}/include/efi -I${gnu-efi}/include/efi/aarch64 \
      -fpic -fshort-wchar -ffreestanding -fno-stack-protector -fno-strict-aliasing \
      -O2 -Wall -c pvmfw-loader.c -o pvmfw-loader.o
    $LD -nostdlib -znocombreloc -shared -Bsymbolic \
      -T ${gnu-efi}/lib/elf_aarch64_efi.lds \
      ${gnu-efi}/lib/crt0-efi-aarch64.o pvmfw-loader.o \
      -L${gnu-efi}/lib -lefi -lgnuefi -o pvmfw-loader.so
    $OBJCOPY -j .text -j .sdata -j .data -j .dynamic -j .rodata -j .rel \
      -j .rela -j '.rel.*' -j '.rela.*' -j .reloc \
      -O efi-bsdrv-aarch64 pvmfw-loader.so pvmfw-loader.efi
    runHook postBuild
  '';
  installPhase = ''
    install -Dm644 pvmfw-loader.efi $out/pvmfw-loader.efi
  '';
}
