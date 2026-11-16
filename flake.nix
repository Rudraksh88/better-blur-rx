{
  description = "Better Blur DX fork for KDE Plasma 6 on Wayland: blur shaped by window alpha and by shapes clients publish over D-Bus";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, ... }@inputs: inputs.utils.lib.eachSystem [
    "x86_64-linux" "aarch64-linux"
  ] (system: let
    pkgs = import nixpkgs {
      inherit system;
    };
  in rec {
    packages = {
      default = pkgs.kdePackages.callPackage ./nix/package.nix { };
      x11 = pkgs.kdePackages.callPackage ./nix/package-x11.nix { };
    };

    devShells.default = pkgs.mkShell {
      inputsFrom = [ packages.default ];
    };
  });
}
