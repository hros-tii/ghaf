# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Boot admin-vm through pvmfw with verified boot. pvmfw expects one vbmeta, as an
# AVB footer on the kernel (partition "boot"), carrying the initrd's hash. The
# initrd is described as "initrd_debug" so the VM stays debuggable and pvmfw
# passes the NixOS command line (init=...) through unfiltered.
#
# TEST ONLY: testkey_rsa4096.pem is AOSP's public AVB test key, the one
# pvmfw-standalone embeds by default. Anyone can sign with it.
{ config, pkgs, ... }:
let
  avbtool = ./avb/avbtool.py;
  key = ./avb/testkey_rsa4096.pem;
  # Fixed salt so the signed kernel is reproducible.
  salt = builtins.hashString "sha256" "ghaf-admin-vm-avb";

  signedKernel =
    pkgs.runCommand "admin-vm-kernel-avb"
      {
        nativeBuildInputs = [
          pkgs.buildPackages.python3
          pkgs.buildPackages.openssl
        ];
      }
      ''
        mkdir -p $out
        install -m 0644 ${config.boot.kernelPackages.kernel.out}/Image $out/Image
        install -m 0644 ${config.microvm.initrdPath} initrd

        # The initrd's hash descriptor; the initrd itself is not modified.
        python3 ${avbtool} add_hash_footer \
          --image initrd --partition_name initrd_debug --dynamic_partition_size \
          --do_not_append_vbmeta_image --output_vbmeta_image initrd_debug.vbmeta \
          --salt ${salt}

        # The kernel's footer: its own hash plus the initrd's, signed.
        python3 ${avbtool} add_hash_footer \
          --image $out/Image --partition_name boot --dynamic_partition_size \
          --algorithm SHA256_RSA4096 --key ${key} \
          --include_descriptors_from_image initrd_debug.vbmeta \
          --salt ${salt}

        python3 ${avbtool} info_image --image $out/Image
      '';
in
{
  ghaf.virtualization.microvm.protected-vm.pkvmFirmware = true;
  microvm.kernel = signedKernel;
}
