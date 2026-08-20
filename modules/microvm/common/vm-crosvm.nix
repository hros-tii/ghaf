# SPDX-FileCopyrightText: 2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.ghaf.virtualization.crosvm;

  crosvmPackage = cfg.package.overrideAttrs (oldAttrs: {
    buildFeatures = oldAttrs.buildFeatures ++ cfg.features;
    patches = (oldAttrs.patches or [ ]) ++ cfg.patches;
  });

  # `--log-level` is a global crosvm option, so argh only accepts it before the
  # `run` subcommand.  microvm.nix appends `crosvm.extraArgs` after `run`, and
  # crosvm ignores RUST_LOG, which leaves wrapping the binary as the only way to
  # set the log level of the `microvm@<vm>.service` units.
  crosvmWithLogLevel = pkgs.symlinkJoin {
    name = "${crosvmPackage.name}-log-level";
    paths = [ crosvmPackage ];
    nativeBuildInputs = [ pkgs.makeWrapper ];
    postBuild = ''
      wrapProgram $out/bin/crosvm --add-flags "--log-level=${cfg.logLevel}"
    '';
    meta = (crosvmPackage.meta or { }) // {
      mainProgram = "crosvm";
    };
  };
in
{
  _file = ./vm-crosvm.nix;

  boot.kernelPatches = [
    {
      name = "Additional virt guest config";
      patch = null;
      structuredExtraConfig = with lib.kernel; {
        VIRTIO = yes;
        VSOCKETS = yes;
        VSOCKETS_LOOPBACK = yes;
        VIRTIO_VSOCKETS = yes;
        VIRTIO_BALLOON = yes;
        VIRTIO_FS = module;
        SCSI_VIRTIO = module;
      };
    }
  ]
  ++ lib.optionals (config.microvm.hypervisor == "crosvm" && pkgs.stdenv.hostPlatform.isx86_64) [
    # Crosvm's virtual IOMMU must be available before PCI enumeration.  Loading
    # it as a module lets passthrough drivers race ahead of the IOMMU supplier;
    # the late registration then leaves those devices without an IOMMU group and
    # DMA-backed drivers cannot probe reliably.
    {
      name = "crosvm-virtio-iommu-builtin";
      patch = null;
      structuredExtraConfig = with lib.kernel; {
        VIRTIO = yes;
        VIRTIO_PCI = yes;
        VIRTIO_IOMMU = yes;
      };
    }
  ];

  hardware.enableAllHardware = false;
  boot.initrd.includeDefaultModules = false;
  boot.initrd.availableKernelModules = [
    "virtiofs"
    "virtio_net"
    "virtio_pci"
    "virtio_mmio"
    "virtio_blk"
    "virtio_scsi"
    "virtio_balloon"
    "virtio_console"
    "vsock"
  ];

  microvm.crosvm.package = if cfg.logLevel == null then crosvmPackage else crosvmWithLogLevel;

  microvm.crosvm.extraArgs = lib.optionals pkgs.stdenv.hostPlatform.isAarch64 [
    "--core-scheduling"
    "false"
  ];
}
