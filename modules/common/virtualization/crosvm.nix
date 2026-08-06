# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Module to customize the crosvm package used by microvm and its compilation
# options.
#
{
  lib,
  pkgs,
  ...
}:
{
  _file = ./crosvm.nix;

  options.ghaf.virtualization.crosvm = {
    package = lib.mkOption {
      type = lib.types.package;
      default = pkgs.crosvm;
      defaultText = lib.literalExpression "pkgs.crosvm";
      description = "The crosvm package used across Ghaf modules.";
    };

    features = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      description = "Additional features to compile in crosvm binary";
    };
  };
}
