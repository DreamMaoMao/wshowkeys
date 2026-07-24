# wshowkeys

Displays keypresses on screen on supported Wayland compositors (requires
`wlr_layer_shell_v1` support).


https://github.com/user-attachments/assets/1a0b201d-7b2b-42cd-b83a-dcb6f31f0cbf


Forked from https://git.sr.ht/~sircmpwn/wshowkeys as Drew has moved onto other thigns.

## Installation

### arch
```bash
yay -S wshowkeys-mao-git

```

### nix
Add as a flake input:
```nix
{
    inputs = {
        # ...
        wshowkeys = {
            url = "github:DreamMaoMao/wshowkeys";
            inputs.nixpkgs.follows = "nixpkgs"; # optional
        };
    }
}
```
Then install using nixpkgs harness in system config:
```nix
programs.wshowkeys = {
    enable = true;
    package = inputs.wshowkeys.packages.${pkgs.stdenv.hostPlatform.system}.default;
};
```
Without the harness, you will get a setuid error and the app won't run.


### other

Dependencies:

- cairo
- libinput
- pango
- udev 
- wayland 
- xkbcommon 

```
$ meson build
$ ninja -C build
# ninja -C build install
# sudo chmod a+s /usr/bin/wshowkeys
```

wshowkeys must be configured as setuid during installation. It requires root
permissions to read input events. These permissions are dropped after startup.

## Usage

```
wshowkeys [-b|-f|-s #RRGGBB[AA]] [-F font] [-t timeout]
    [-a top|left|right|bottom] [-m margin] [-o output]
```

- *-b #RRGGBB[AA]*: set background color
- *-f #RRGGBB[AA]*: set foreground color
- *-s #RRGGBB[AA]*: set color for special keys
- *-F font*: set font (Pango format, e.g. 'monospace 24')
- *-t timeout*: set timeout before clearing old keystrokes(ms)
- *-a top|left|right|bottom*: anchor the keystrokes to an edge. May be specified
  twice.
- *-m margin*: set a margin (in pixels) from the nearest edge
- *-l lenmax*: set the key layer lenmax
- *-o output*: request wshowkeys is shown on the specified output
  (unimplemented)
- *-M*: show modifier keys
- *-U*: show mouse buttons
- *-S*: show scroll direction

example:
```bash
wshowkeys -a bottom -F 'Sans Bold 30' -s '#B5B520ff' -f  '#ecd29cff' -b '#201B1488' -l 60 -t 500 -M -U -S
```
