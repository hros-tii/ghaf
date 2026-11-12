# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Per-VM pvmfw state on the host: instance.img (pvmfw's rollback record,
# encrypted and authenticated with the sealing CDI) and instance_id.dtbo (the
# VM's identity salt). One pair per VM, never shared.
{ lib, pkgs, ... }:
let
  vms = [ "admin-vm" "net-vm" "gui-vm" ];
  mk = vm: lib.nameValuePair "pvmfw-instance-${vm}" {
    description = "pvmfw instance.img and instance_id.dtbo for ${vm}";
    wantedBy = [ "microvm@${vm}.service" ];
    before = [ "microvm@${vm}.service" ];
    unitConfig.RequiresMountsFor = "/persist";
    serviceConfig = {
      Type = "oneshot";
      RemainAfterExit = true;
    };
    path = [ pkgs.gptfdisk pkgs.dtc pkgs.coreutils ];
    script = ''
      set -eu
      d=/persist/pvmfw/${vm}
      install -d -o microvm -g kvm -m 0750 "$d"

      # The identity salt: made once and kept, or the VM gets new secrets.
      if [ ! -s "$d/instance_id.dtbo" ]; then
        id=$(od -An -tx1 -N64 /dev/urandom | tr -s ' \n' ' ')
        printf '/dts-v1/;\n/plugin/;\n/ { fragment@0 { target-path = "/"; __overlay__ {\n  avf { untrusted { instance-id = [%s]; }; };\n}; }; };\n' "$id" > "$d/instance_id.dts"
        dtc -I dts -O dtb -o "$d/instance_id.dtbo" "$d/instance_id.dts"
        rm "$d/instance_id.dts"
      fi

      # pvmfw records the first boot and rejects a different payload or pvmfw
      # afterwards, so start a fresh instance.img when either changes.
      stamp="$(readlink -f /var/lib/microvms/${vm}/current) $(sha256sum /boot/EFI/pvmfw/pvmfw.bin 2>/dev/null | cut -d' ' -f1)"
      if [ ! -s "$d/instance.img" ] || [ "$(cat "$d/stamp" 2>/dev/null)" != "$stamp" ]; then
        rm -f "$d/instance.img"
        truncate -s 2M "$d/instance.img"
        sgdisk -n 1:2048:0 -c 1:vm-instance "$d/instance.img"
        printf 'Android-VM-instance\001\000' | dd of="$d/instance.img" bs=512 seek=2048 conv=notrunc status=none
        printf '%s' "$stamp" > "$d/stamp"
      fi
      chown microvm:kvm "$d/instance.img" "$d/instance_id.dtbo"
      chmod 0660 "$d/instance.img"
      chmod 0640 "$d/instance_id.dtbo"
    '';
  };
in
{
  systemd.services = lib.listToAttrs (map mk vms);
}
