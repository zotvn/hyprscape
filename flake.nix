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

    version = "0.2.0";
  in {
    # A Hyprland plugin is ABI-locked to one exact Hyprland build -- the loader refuses anything
    # else -- so the derivation takes the Hyprland package rather than pinning one. Pass the very
    # same Hyprland your session runs:
    #
    #   hyprscape = inputs.hyprscape.lib.mkHyprscape {
    #     inherit pkgs;
    #     hyprland = inputs.hyprland.packages.${pkgs.stdenv.hostPlatform.system}.hyprland;
    #   };
    #
    # If you run the Hyprland from nixpkgs, `inputs.hyprscape.packages.${system}.hyprscape` is
    # already exactly that and needs no arguments.
    lib.mkHyprscape = {
      pkgs,
      hyprland ? pkgs.hyprland,
    }:
      pkgs.stdenv.mkDerivation (finalAttrs: {
        pname = "hyprscape";
        inherit version;

        src = lib.fileset.toSource {
          root = ./.;
          fileset = lib.fileset.unions [./src ./Makefile];
        };

        nativeBuildInputs = [pkgs.pkg-config] ++ hyprland.nativeBuildInputs;
        buildInputs = [hyprland] ++ hyprland.buildInputs;

        # Hyprland's nativeBuildInputs drag in cmake, whose setup hook would otherwise take over
        # the configure phase and look for a CMakeLists.txt we do not have.
        dontUseCmakeConfigure = true;
        dontConfigure = true;

        # The Makefile's series check reads `pkg-config --modversion hyprland`, which here is
        # whatever `hyprland` above provides -- so a mismatched Hyprland fails at build time with
        # a message rather than at load time with a broken session.
        makeFlags = ["all"];

        installPhase = ''
          runHook preInstall
          mkdir -p $out/lib
          cp libhyprscape.so $out/lib/libhyprscape.so
          runHook postInstall
        '';

        passthru.hyprlandPackage = hyprland;

        meta = {
          description = "A niri-style zoom-out overview for Hyprland's scrolling layout";
          homepage = "https://github.com/cybergaz/hyprscape";
          license = lib.licenses.bsd3;
          platforms = lib.platforms.linux;
          maintainers = [];
        };
      });

    # For `nixpkgs.overlays = [inputs.hyprscape.overlays.default];`
    overlays.default = final: prev: {
      hyprscape = self.lib.mkHyprscape {
        pkgs = final;
        hyprland = final.hyprland;
      };
    };

    packages = forSystems (system: pkgs: rec {
      hyprscape = self.lib.mkHyprscape {inherit pkgs;};
      default = hyprscape;
    });

    devShells = forSystems (system: pkgs: {
      default = pkgs.mkShell {
        inputsFrom = [pkgs.hyprland];
        packages = [pkgs.pkg-config pkgs.clang-tools pkgs.gnumake];
      };
    });

    formatter = forSystems (system: pkgs: pkgs.alejandra);
  };
}
