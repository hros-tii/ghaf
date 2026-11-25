# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Who may use g2g in this VM: the members of the group g2g.
#
# /dev/pkvm-g2g (mailbox and shared pages) and /dev/open-dice0 (this VM's
# identity) go to the group, open-dice0 read-only: a write to it wipes the DICE
# secrets. The initrd stages the peers file (the whitelist) root-only, so it is
# made readable by the group too. The mailbox is one per VM: only one process
# in the VM can use g2g at a time.
_: {
  users.groups.g2g = { };
  services.udev.extraRules = ''
    KERNEL=="pkvm-g2g", GROUP="g2g", MODE="0660"
    KERNEL=="open-dice0", GROUP="g2g", MODE="0640"
  '';
  systemd.tmpfiles.rules = [ "z /run/g2g/peers 0440 root g2g -" ];
}
