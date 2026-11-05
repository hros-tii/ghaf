# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Stable Linux 6.18 kernel with pKVM patches for arm64.
# The same kernel is used for both host and guests on pvm target, only with different defconfigs.
#
{
  nvidia-jetpack7,
  fetchFromGitHub,
  applyPatches,
  structuredExtraConfig ? { },
  argsOverride ? { },
  ...
}@args:
nvidia-jetpack7.kernel.override {
  inherit structuredExtraConfig;

  argsOverride =
    args
    // {
      pname = "linux-pkvm-jetson";
      version = "6.18.58";
      extraMeta.branch = "pkvm-stable-dev";

      src = applyPatches {
        name = "linux-pkvm-jetson-src";
        patches = [
          ./0004-DEBUG-iommu-arm-smmu-v2-pkvm-Log-failed-idmap-maps.patch
        ];
        src = fetchFromGitHub {
          owner = "tiiuae";
          repo = "linux-pkvm-jetson";
          rev = "aa7e08d26d531fbe31448026041b8e6d2d531c64"; # pkvm-stable-dev - 22-10-2026
          hash = "sha256-LWKNKQkwIdg4LjegI/3wasmP3rg3THmwr5RAT/u7SBo=";
        };
      };
    }
    // argsOverride;
}
