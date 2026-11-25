# SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
# SPDX-License-Identifier: Apache-2.0
{ prev }:
let
  inherit (prev.nvidia-jetpack) cudaPackages;
in
(prev.ollama-cuda.override {
  inherit cudaPackages;
  cudaArches = [ "sm_87" ];
}).overrideAttrs
  (
    prevAttrs:
    {
      meta = prevAttrs.meta // {
        broken = false;
      };
    }
    // prev.lib.optionalAttrs (prev.stdenv.buildPlatform != prev.stdenv.hostPlatform) {
      # llama/server/CMakeLists.txt bundles cudart/cublas via
      # `install(TARGETS ggml-cuda RUNTIME_DEPENDENCIES ...)`, which CMake
      # rejects when cross-compiling.
      # We can remove the bundling because the nixpkgs wrapper already puts
      # the CUDA libraries on LD_LIBRARY_PATH, and libggml-cuda is installed
      # by a separate install(CODE).
      postPatch = (prevAttrs.postPatch or "") + ''
        substituteInPlace llama/server/CMakeLists.txt \
          --replace-fail "if(GGML_CUDA AND CUDAToolkit_FOUND)" \
                         "if(GGML_CUDA AND CUDAToolkit_FOUND AND NOT CMAKE_CROSSCOMPILING)"
      '';

      # Ollama configures llama.cpp as a nested ExternalProject build that does not
      # forward CMAKE_SYSTEM_NAME and CMAKE_SYSTEM_PROCESSOR. The nested CMake then
      # treats the build as native and uses the build machine's CPU (e.g.
      # x86_64), so ggml enables x86 CPU variants (-mavx512*, -mamx-*).
      # Solve by setting CMAKE_TOOLCHAIN_FILE in env which is read in nested builds.
      CMAKE_TOOLCHAIN_FILE = prev.writeText "ollama-cross-toolchain.cmake" ''
        set(CMAKE_SYSTEM_NAME ${prev.stdenv.hostPlatform.uname.system})
        set(CMAKE_SYSTEM_PROCESSOR ${prev.stdenv.hostPlatform.uname.processor})
      '';

      # nvcc comes from buildPackages so its setup hook exports CUDAHOSTCXX
      # from the build platform c++. This compiler cannot find the host platform's
      # CUDA headers (cuda_runtime.h).
      CUDAHOSTCXX =
        let
          inherit (cudaPackages.backendStdenv) cc;
        in
        "${cc}/bin/${cc.targetPrefix}c++";
    }
  )
