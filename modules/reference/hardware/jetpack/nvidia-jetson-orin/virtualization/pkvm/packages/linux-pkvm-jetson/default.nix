# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# Stable Linux 6.18 kernel with pKVM patches for arm64.
# The same kernel is used for both host and guests on pvm target, only with different defconfigs.
#
{
  nvidia-jetpack7,
  fetchFromGitHub,
  ccacheStdenv,
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
      version = "6.18.63";
      extraMeta.branch = "pkvm-stable-dev";

      src = fetchFromGitHub {
        owner = "tiiuae";
        repo = "linux-pkvm-jetson";
        rev = "81fc753cba29e002a383b377ae2c363d45f70666"; # pkvm-stable-dev - 26-10-2026
        hash = "sha256-bnLcx27SYJ/jr3vf74ZhbIH4VjAxjPfuuHm1SvjP6RU=";
      };

      kernelPatches = [
        {
          name = "Fix smmu domain realloc";
          patch = ./test-smmu-alloc.patch;
        }
        {
          name = "pkvm guest: MMIO-guard-map earlycon";
          patch = ./pkvm-guest-mmio-guard-earlycon.patch;
        }
      ];

      stdenv = ccacheStdenv;
    }
    // argsOverride;
}
