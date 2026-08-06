# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
{
  config,
  lib,
  ...
}:
let
  inherit (lib)
    mkOption
    types
    optionals
    optionalAttrs
    ;
in
{
  _file = ./crosvm.nix;

  options.ghaf.crosvm = {
    guivm = mkOption {
      type = types.attrs;
      default = { };
      description = "Extra crosvm arguments for GuiVM";
    };
    audiovm = mkOption {
      type = types.attrs;
      default = { };
      description = "Extra crosvm arguments for AudioVM";
    };
    netvm = mkOption {
      type = types.attrs;
      default = { };
      description = "Extra crosvm arguments for NetVM";
    };
  };

  config = {
    ghaf.crosvm = {
      guivm = optionalAttrs (config.ghaf.type == "host") {
        microvm.crosvm.extraArgs =
          optionals (config.ghaf.hardware.definition.type == "laptop") [
            "--battery"
            # TODO port these to crosvm
            # "-device"
            # "battery,use-qmp=false,enable-sysfs=true,probe_interval=20000"
            # # Lid Button
            # "-device"
            # "button,use-qmp=false,enable-procfs=true,probe_interval=2000"
            # # AC adapter
            # "-device"
            # "acad,use-qmp=false,enable-sysfs=true,probe_interval=5000"
          ]
          ++ (config.ghaf.hardware.passthrough.crosvmExtraArgs.gui-vm or [ ]);
      };
      audiovm = optionalAttrs (config.ghaf.type == "host") {
        microvm.crosvm.extraArgs =
          optionals (config.ghaf.hardware.definition.type == "laptop") [
            "--battery"
          ]
          ++ optionals (config.ghaf.hardware.definition.audio.acpiPath != null) [
            "--acpi-table"
            "${config.ghaf.hardware.definition.audio.acpiPath}"
          ];
      };
      netvm = optionalAttrs (config.ghaf.type == "host") {
        microvm.crosvm.extraArgs = config.ghaf.hardware.passthrough.crosvmExtraArgs.net-vm or [ ];
      };
    };
  };
}
