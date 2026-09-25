# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Module which adds option ghaf.boot.loader.systemd-boot-dtb.enable
#
# By setting this option to true, device tree file gets copied to
# /boot-partition, and gets added to systemd-boot's entry.
#
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.ghaf.hardware.aarch64.systemd-boot-dtb;
  inherit (lib) mkEnableOption mkIf;

  # A DTB reached through a `devicetree` line is installed into the EFI
  # configuration table by systemd-boot, which first runs it through the
  # firmware's EFI_DT_FIXUP_PROTOCOL. That appends /chosen and reserved-memory
  # nodes in place, so a blob whose totalsize equals its used size comes back
  # corrupt and the EFI stub rejects it with "Invalid header detected on UEFI
  # supplied FDT, ignoring", then boots with an empty DTB. Ship it with slack,
  # as sdimage.nix does for the ESP copy it builds into the image.
  paddedDtb =
    pkgs.runCommand "padded-dtb-${config.hardware.deviceTree.name}"
      {
        nativeBuildInputs = [ pkgs.dtc ];
      }
      ''
        mkdir -p "$(dirname "$out/${config.hardware.deviceTree.name}")"
        dtc -I dtb -O dtb -p 65536 \
          -o "$out/${config.hardware.deviceTree.name}" \
          "${config.hardware.deviceTree.package}/${config.hardware.deviceTree.name}"
      '';
  paddedDtbPath = "${paddedDtb}/${config.hardware.deviceTree.name}";
in
{
  _file = ./systemd-boot-dtb.nix;

  options.ghaf.hardware.aarch64.systemd-boot-dtb = {
    enable = mkEnableOption "systemd-boot-dtb";
  };

  config = mkIf cfg.enable {
    boot.loader.systemd-boot = {
      extraFiles."dtbs/${config.hardware.deviceTree.name}" = paddedDtbPath;
      extraInstallCommands = ''
        # Find out the latest generation from loader.conf
        default_cfg=$(${pkgs.coreutils}/bin/cat /boot/loader/loader.conf | ${pkgs.gnugrep}/bin/grep default | ${pkgs.gawk}/bin/awk '{print $2}')
        FILEHASH=$(${pkgs.coreutils}/bin/sha256sum "${paddedDtbPath}" | ${pkgs.coreutils}/bin/cut -d ' ' -f 1)
        FILENAME="/dtbs/$FILEHASH.dtb"
        ${pkgs.coreutils}/bin/cp -fv "${paddedDtbPath}" "/boot$FILENAME"
        echo "devicetree $FILENAME" >> /boot/loader/entries/$default_cfg
      '';
    };
  };
}
