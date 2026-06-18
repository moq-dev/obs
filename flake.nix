{
  description = "obs-moq - OBS Studio plugin for Media over QUIC";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
      ...
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Lint tools
        lintDeps = with pkgs; [
          clang-tools
          gersemi
        ];

        # Build tools
        buildDeps = with pkgs; [
          cmake
          ninja
          pkg-config
          just
        ];

        # Build dependencies
        libDeps =
          with pkgs;
          [
            ffmpeg
          ]
          ++ lib.optionals stdenv.hostPlatform.isLinux [
            obs-studio
          ];
      in
      {
        devShells.default = pkgs.mkShellNoCC {
          packages = lintDeps ++ buildDeps ++ libDeps;
        };
      }
    );
}
