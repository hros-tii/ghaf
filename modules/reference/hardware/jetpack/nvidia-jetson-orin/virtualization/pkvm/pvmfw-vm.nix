# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Boot a VM through pKVM's pvmfw with verified boot. `name` is its
# com.android.virt.name, from which pvmfw derives its g2g identity. pvmfw expects one vbmeta, as an
# AVB footer on the kernel (partition "boot"), carrying the initrd's hash. The
# initrd is described as "initrd_debug" so the VM stays debuggable and pvmfw
# passes the NixOS command line (init=...) through unfiltered.
#
# TEST ONLY: testkey_rsa4096.pem is AOSP's public AVB test key, the one
# pvmfw-standalone embeds by default. Anyone can sign with it.
name:
{ config, pkgs, ... }:
let
  avbtool = ./avb/avbtool.py;
  key = ./avb/testkey_rsa4096.pem;
  # Fixed salt so the signed kernel is reproducible.
  salt = builtins.hashString "sha256" "ghaf-${name}-avb";

  signedKernel =
    pkgs.runCommand "${name}-kernel-avb"
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
          --prop com.android.virt.name:${name} \
          --include_descriptors_from_image initrd_debug.vbmeta \
          --salt ${salt}

        python3 ${avbtool} info_image --image $out/Image
      '';

  # Guest-to-guest agent and its whitelist. Both ride in the initrd, which the
  # AVB footer covers; the rootfs is not authenticated. Static, so it loads
  # nothing from the guest's /nix/store at run time.
  g2gAgent = pkgs.pkgsStatic.callPackage ./g2g-agent { };
  g2gPeers = pkgs.writeText "g2g-peers" ''
    # The VMs this one exchanges guest-to-guest messages with.
    admin-vm
    net-vm
    gui-vm
  '';
in
{
  ghaf.virtualization.microvm.protected-vm.pkvmFirmware = true;
  microvm.kernel = signedKernel;
  # pvmfw's per-VM state, made on the host by pvmfw-instances.nix.
  microvm.crosvm.extraArgs = [
    "--block"
    "/persist/pvmfw/${name}/instance.img,o_direct=false,ro=false"
    "--device-tree-overlay"
    "/persist/pvmfw/${name}/instance_id.dtbo"
  ];
  environment.systemPackages = [ (pkgs.callPackage ./g2g-demo { }) ];

  # Stage 1 copies the agent and the peers out of the signed initrd into /run,
  # which systemd keeps across switch-root.
  boot.initrd.systemd.storePaths = [
    "${g2gAgent}/bin/g2g-agent"
    g2gPeers
    "${pkgs.coreutils}/bin/install"
  ];
  boot.initrd.systemd.services.g2g-stage = {
    description = "Copy the g2g agent and its peers out of the signed initrd";
    wantedBy = [ "initrd.target" ];
    before = [ "initrd-switch-root.target" ];
    unitConfig.DefaultDependencies = false;
    serviceConfig = {
      Type = "oneshot";
      ExecStart = [
        "${pkgs.coreutils}/bin/install -D -m 0500 ${g2gAgent}/bin/g2g-agent /run/g2g/g2g-agent"
        "${pkgs.coreutils}/bin/install -D -m 0400 ${g2gPeers} /run/g2g/peers"
      ];
    };
  };

  systemd.services.g2g-agent = {
    description = "Guest-to-guest agent";
    wantedBy = [ "multi-user.target" ];
    unitConfig.ConditionPathExists = "/run/g2g/g2g-agent";
    serviceConfig = {
      ExecStart = "/run/g2g/g2g-agent /run/g2g/peers";
      Restart = "on-failure";
      RestartSec = 5;
    };
  };
}
