{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.boot.uki;

  inherit (pkgs.stdenv.hostPlatform) efiArch;

  format = pkgs.formats.ini { };
in

{
  options = {

    boot.uki = {
      name = lib.mkOption {
        type = lib.types.str;
        description = "Name of the UKI";
      };

      version = lib.mkOption {
        type = lib.types.nullOr lib.types.str;
        default = config.system.image.version;
        defaultText = lib.literalExpression "config.system.image.version";
        description = "Version of the image or generation the UKI belongs to";
      };

      tries = lib.mkOption {
        type = lib.types.nullOr lib.types.ints.unsigned;
        default = null;
        description = ''
          Number of boot attempts before this UKI is considered bad.

          If no tries are specified (the default) automatic boot assessment remains inactive.

          See documentation on [Automatic Boot Assessment](https://systemd.io/AUTOMATIC_BOOT_ASSESSMENT/) and
          [boot counting](https://uapi-group.org/specifications/specs/boot_loader_specification/#boot-counting)
          for more information.
        '';
      };

      settings = lib.mkOption {
        type = format.type;
        description = ''
          The configuration settings for ukify. These control what the UKI
          contains and how it is built.
        '';
      };

      configFile = lib.mkOption {
        type = lib.types.path;
        description = ''
          The configuration file passed to {manpage}`ukify(1)` to create the UKI.

          By default this configuration file is created from {option}`boot.uki.settings`.
        '';
      };
    };

    system.boot.loader.ukiFile = lib.mkOption {
      type = lib.types.str;
      internal = true;
      description = "Name of the UKI file";
    };

  };

  config = {

    boot.uki.name = lib.mkOptionDefault (
      if config.system.image.id != null then config.system.image.id else "nixos"
    );

    boot.uki.settings = {
      UKI =
        {
          Linux = lib.mkOptionDefault "${config.boot.kernelPackages.kernel}/${config.system.boot.loader.kernelFile}";
          Initrd = lib.mkOptionDefault "${config.system.build.initialRamdisk}/${config.system.boot.loader.initrdFile}";
          Cmdline = lib.mkOptionDefault "init=@toplevel@/init ${toString config.boot.kernelParams}";
          Stub = lib.mkOptionDefault "${pkgs.systemd}/lib/systemd/boot/efi/linux${efiArch}.efi.stub";
          Uname = lib.mkOptionDefault "${config.boot.kernelPackages.kernel.modDirVersion}";
          OSRelease = lib.mkOptionDefault "@${config.system.build.etc}/etc/os-release";
          # This is needed for cross compiling.
          EFIArch = lib.mkOptionDefault efiArch;
        }
        // lib.optionalAttrs (config.hardware.deviceTree.enable && config.hardware.deviceTree.name != null)
          {
            DeviceTree = lib.mkOptionDefault "${config.hardware.deviceTree.package}/${config.hardware.deviceTree.name}";
          };
    };

    boot.uki.configFile = lib.mkOptionDefault (format.generate "ukify.conf" cfg.settings);

    system.boot.loader.ukiFile =
      let
        name = config.boot.uki.name;
        version = config.boot.uki.version;
        versionInfix = if version != null then "_${version}" else "";
        triesInfix = if cfg.tries != null then "+${builtins.toString cfg.tries}" else "";
      in
      name + versionInfix + triesInfix + ".efi";

    system.build.uki = pkgs.runCommand config.system.boot.loader.ukiFile { } ''
      mkdir -p $out
      ${pkgs.buildPackages.systemdUkify}/lib/systemd/ukify build \
        --config=${cfg.configFile} \
        --output="$out/${config.system.boot.loader.ukiFile}"
    '';

    boot.loader.grub.enable = lib.mkDefault false;
    # boot.loader.systemd-boot.enable = lib.mkForce false;

    # Replace systemd-boot bootloader install script (systemd-boot-builder.py)
    #
    # Note that running `nixos-rebuild switch` on a Secure Boot system will cause the signed kernel to be overwritten 
    # by the new (unsigned) UKI.
    # `nixos-rebuild test` and similar variants that don't modify the boot entries can still be used.
    boot.loader.external = {
      enable = true;
      installHook = pkgs.writeShellScript "systemd-boot-uki-installer" ''
        TOPLEVEL="$1"
        EFI_MOUNT="${config.boot.loader.efi.efiSysMountPoint}"
        SYSTEMD_BOOT="${pkgs.systemd}/lib/systemd/boot/efi"

        mkdir -p "$EFI_MOUNT/loader"
        mkdir -p "$EFI_MOUNT/EFI/BOOT"
        mkdir -p "$EFI_MOUNT/EFI/systemd"
        mkdir -p "$EFI_MOUNT/EFI/Linux"

        cp "$SYSTEMD_BOOT/systemd-boot${efiArch}.efi" "$EFI_MOUNT/EFI/systemd/"
        cp "$SYSTEMD_BOOT/systemd-boot${efiArch}.efi" "$EFI_MOUNT/EFI/BOOT/BOOT${lib.toUpper efiArch}.efi"

        sed -e "s%@toplevel@%$TOPLEVEL%" ${cfg.configFile} > "$EFI_MOUNT/loader/ukify.conf"
        ${pkgs.buildPackages.systemdUkify}/lib/systemd/ukify build \
          --config="$EFI_MOUNT/loader/ukify.conf" \
          --output="$EFI_MOUNT/EFI/Linux/${config.system.boot.loader.ukiFile}"
      '';
    };

    system.boot.loader.id = "systemd-boot";
    system.requiredKernelConfig = with config.lib.kernelConfig; [
      (isYes "EFI_STUB")
    ];
  };
}
