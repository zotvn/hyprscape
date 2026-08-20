{
  description = "hyprscape - a niri-style zoom-out overview for Hyprland's scrolling layout";

  # Only one input, so a consumer that already has nixpkgs pays nothing extra:
  #   inputs.hyprscape.inputs.nixpkgs.follows = "nixpkgs";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = {
    self,
    nixpkgs,
    ...
  }: let
    inherit (nixpkgs) lib;
    systems = ["x86_64-linux" "aarch64-linux"];
    forSystems = f: lib.genAttrs systems (system: f system nixpkgs.legacyPackages.${system});
  in {
    # The plugin ABI is tied to one exact Hyprland build, so the derivation is parameterised by
    # the Hyprland package instead of pinning one. Pass the very same Hyprland your session runs:
    #
    #   hyprscape = inputs.hyprscape.lib.mkHyprscape {
    #     inherit pkgs;
    #     hyprland = inputs.hyprland.packages.${system}.hyprland;
    #   };
    lib.mkHyprscape = {
      pkgs,
      hyprland ? pkgs.hyprland,
    }:
      pkgs.stdenv.mkDerivation {
        pname = "hyprscape";
        version = "0.1";
        src = ./.;

        nativeBuildInputs = [pkgs.pkg-config] ++ hyprland.nativeBuildInputs;
        buildInputs = [hyprland] ++ hyprland.buildInputs;

        buildPhase = ''
          runHook preBuild
          g++ -std=c++23 -shared -fPIC --no-gnu-unique -O2 \
            -Wall -Wno-narrowing -Wno-unused-parameter -Wno-unused-variable \
            $(pkg-config --cflags hyprland pixman-1 libdrm pangocairo libinput libudev wayland-server xkbcommon) \
            -Isrc \
            -o libhyprscape.so $(find src -name '*.cpp' | sort)
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          mkdir -p $out/lib
          cp libhyprscape.so $out/lib/libhyprscape.so
          runHook postInstall
        '';

        meta = with lib; {
          description = "A niri-style zoom-out overview for Hyprland's scrolling layout";
          license = licenses.bsd3;
          platforms = platforms.linux;
        };
      };

    packages = forSystems (system: pkgs: rec {
      hyprscape = self.lib.mkHyprscape {inherit pkgs;};
      default = hyprscape;
    });

    devShells = forSystems (system: pkgs: {
      default = pkgs.mkShell {
        inputsFrom = [pkgs.hyprland];
        packages = [pkgs.pkg-config pkgs.clang-tools];
      };
    });
  };
}
