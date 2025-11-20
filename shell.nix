(import <nixpkgs> {}).swaylock.overrideAttrs({ mesonFlags ? [], ... }: {
  mesonFlags = mesonFlags ++ [
  ];
})
