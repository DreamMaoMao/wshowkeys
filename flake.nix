{
  description = "wshowkeys - show your key presses on screen";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-utils,
    }:
    flake-utils.lib.eachDefaultSystem (
      system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in
      {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "wshowkeys";
          version = "0.1.0";

          src = ./.;

          strictDeps = true;

          nativeBuildInputs = with pkgs; [
            meson
            pkg-config
            wayland-scanner
            ninja
          ];

          buildInputs = with pkgs; [
            cairo
            libinput
            pango
            wayland
            wayland-protocols
            libxkbcommon
          ];

          meta = with pkgs.lib; {
            description = "Displays keys being pressed on a Wayland session";
            longDescription = ''
              Displays keypresses on screen on supported Wayland compositors
              (requires wlr_layer_shell_v1 support).
              Note: this tool requires root permissions to read input events,
              but these permissions are dropped after startup. The NixOS
              module exposed by this flake provides such a setuid binary
              (use `programs.wshowkeys.enable = true;`).
            '';
            homepage = "https://github.com/DreamMaoMao/wshowkeys";
            license = with licenses; [
              gpl3Only
              mit
            ];
            platforms = platforms.linux;
            mainProgram = "wshowkeys";
          };
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.default ];
          packages = with pkgs; [
            meson
            ninja
            pkg-config
          ];
        };

        # exposes module, not reccomended for use, see README for instructions
        nixosModules.default =
          {
            config,
            lib,
            pkgs,
            ...
          }:
          let
            cfg = config.programs.wshowkeys;
          in
          {
            options.programs.wshowkeys = {
              enable = lib.mkEnableOption "wshowkeys";
              package = lib.mkPackageOption pkgs "wshowkeys" {
                default = self.packages.${pkgs.system}.default;
              };
            };

            config = lib.mkIf cfg.enable {
              environment.systemPackages = [ cfg.package ];

              security.wrappers.wshowkeys = {
                setuid = true;
                owner = "root";
                group = "root";
                source = lib.getExe cfg.package;
              };
            };
          };
      }
    );
}
