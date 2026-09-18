{
  description = "Logos monerod_module: a Monero node, in-process.";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # TODO: github:logos-co/logos-monero-nix once that repo is published.
    logos-monero-nix = {
      url = "git+file:///Users/dlipicar/repos/logos-monero-nix";
      inputs.logos-nix.follows = "logos-module-builder/logos-nix";
      inputs.nixpkgs.follows = "logos-module-builder/nixpkgs";
    };
  };

  outputs = inputs@{ logos-module-builder, logos-monero-nix, ... }:
    let
      lib = logos-module-builder.inputs.nixpkgs.lib;
      # x86_64-windows is cross-built on x86_64-linux; libmonerod_c comes from contrib/depends.
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" "x86_64-windows" ];
      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        externalLibInputs.monerod_c = {
          input = logos-monero-nix;
          packages.default = "monerod-c";
        };
      };
    in
    { packages = lib.genAttrs systems (system: module.packages.${system}); };
}
