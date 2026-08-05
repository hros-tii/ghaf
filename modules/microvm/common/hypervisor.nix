{
  config,
  lib,
  pkgs,
  options,
  ...
}:
let
  cfg = config.ghaf.virtualization.microvm.hypervisor;
in
{
  _file = ./hypervisor.nix;

  options.ghaf.virtualization.microvm.hypervisor = lib.mkOption {
    description = "The microvm hypervisor (VMM) to use for this VM.";
    type = lib.types.enum [
      "qemu"
      "crosvm"
    ];
    default = "qemu";
  };

  config.microvm.hypervisor = cfg;
}
