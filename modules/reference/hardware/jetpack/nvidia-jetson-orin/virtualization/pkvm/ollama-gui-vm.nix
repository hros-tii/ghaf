# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
#
# TEMPORARY: Ollama with CUDA in gui-vm, for the g2g-ollama demo, until Hugo's
# ollama-cuda lands in Ghaf. The package is his jetpack-nixos expression, with
# jetpack-nixos' `self.cudaPackages` read as `pkgs.nvidia-jetpack.cudaPackages`.
# Ollama listens only on 127.0.0.1:11434; app VMs reach it through g2g-proxy
# (g2g-ollama.nix).
{
  lib,
  pkgs,
  ...
}:
let
  inherit (pkgs.nvidia-jetpack) cudaPackages;
  cross = pkgs.stdenv.buildPlatform != pkgs.stdenv.hostPlatform;

  ollama-cuda = (pkgs.ollama-cuda.override { inherit cudaPackages; }).overrideAttrs (
    prevAttrs:
    {
      meta = prevAttrs.meta // {
        broken = false;
      };
      # Only the Orin GPU (compute capability 8.7). The package passes the CUDA 13
      # desktop list, without 8.7, straight to cmake in preBuild, so Ollama
      # skipped the GPU.
      preBuild =
        let
          archs = "-DCMAKE_CUDA_ARCHITECTURES='75;80;86;89;90;100;103;120;121'";
        in
        assert lib.assertMsg (lib.hasInfix archs prevAttrs.preBuild)
          "ollama-cuda: the CUDA architecture list in preBuild changed";
        builtins.replaceStrings [ archs ] [ "-DCMAKE_CUDA_ARCHITECTURES='87'" ] prevAttrs.preBuild;
    }
    // lib.optionalAttrs cross {
      # llama/server/CMakeLists.txt bundles cudart/cublas via
      # `install(TARGETS ggml-cuda RUNTIME_DEPENDENCIES ...)`, which CMake
      # rejects when cross-compiling. The nixpkgs wrapper already puts the CUDA
      # libraries on LD_LIBRARY_PATH, and libggml-cuda is installed by a separate
      # install(CODE).
      postPatch = (prevAttrs.postPatch or "") + ''
        substituteInPlace llama/server/CMakeLists.txt \
          --replace-fail "if(GGML_CUDA AND CUDAToolkit_FOUND)" \
                         "if(GGML_CUDA AND CUDAToolkit_FOUND AND NOT CMAKE_CROSSCOMPILING)"
      '';

      # llama.cpp is a nested ExternalProject build that does not get
      # CMAKE_SYSTEM_NAME/PROCESSOR, so it would build for the build machine's
      # CPU (x86 -mavx512*, -mamx-*). A toolchain file in the environment is
      # read by the nested build too.
      CMAKE_TOOLCHAIN_FILE = pkgs.writeText "ollama-cross-toolchain.cmake" ''
        set(CMAKE_SYSTEM_NAME ${pkgs.stdenv.hostPlatform.uname.system})
        set(CMAKE_SYSTEM_PROCESSOR ${pkgs.stdenv.hostPlatform.uname.processor})
      '';

      # nvcc comes from buildPackages, so its setup hook points CUDAHOSTCXX at
      # the build platform's c++, which cannot find the host platform's CUDA
      # headers (cuda_runtime.h).
      CUDAHOSTCXX =
        let
          inherit (cudaPackages.backendStdenv) cc;
        in
        "${cc}/bin/${cc.targetPrefix}c++";
    }
  );
in
{
  services.ollama = {
    enable = true;
    package = ollama-cuda;
    host = "127.0.0.1";
    port = 11434;
  };

  # The Jetson GPU is /dev/nvhost-* and /dev/nvmap, group video, which the
  # module's DeviceAllow list (made for desktop GPUs) does not cover. With any
  # DeviceAllow entry systemd allows only the listed devices, so the list is
  # emptied and DevicePolicy=auto then allows all of them.
  systemd.services.ollama.serviceConfig = {
    SupplementaryGroups = [ "video" ];
    DeviceAllow = lib.mkForce [ ];
    DevicePolicy = lib.mkForce "auto";
    PrivateDevices = lib.mkForce false;
    # The Jetson driver's libcuda.so.1 (the module's wrapper adds the CUDA
    # libraries in front of this).
    Environment = [ "LD_LIBRARY_PATH=/run/current-system/sw/lib" ];
  };
}
