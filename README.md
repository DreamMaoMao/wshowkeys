# wshowkeys

Displays keypresses on screen on supported Wayland compositors (requires
`wlr_layer_shell_v1` support).




https://github.com/DreamMaoMao/myshowkey/assets/30348075/1849c30a-9970-4d79-825d-3cfed05795a9





Forked from https://git.sr.ht/~sircmpwn/wshowkeys as Drew has moved onto other thigns.

## Installation

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

example:
```
wshowkeys -a bottom -F 'Sans Bold 40' -t 1000 -s '#73e155' -f  '#ecd29c' -l 60
```
