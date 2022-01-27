# wtype
xdotool type for wayland

[![Packaging status](https://repology.org/badge/vertical-allrepos/wtype.svg)](https://repology.org/project/wtype/versions)

## Building

```
meson build
ninja -C build
sudo ninja -C build install
```

## Usage

```
# Type unicode characters
wtype ∇⋅∇ψ = ρ
```

To press/release modifiers, `-M`/`-m` can be used respectively.

```
# Press Ctrl+C
wtype -M ctrl c -m ctrl
```

To alter delay between keystrokes, `-d`.

```
# delay of 0 when typing "foo", 120ms on "bar"
wtype foo -d 120 bar

# also applied on stdin
echo everything | wtype -d 12 -
```

To press/release a named key (as given by [xkb_keysym_get_name](https://xkbcommon.org/doc/current/group__keysyms.html)),
`-P`/`-p` can be used.

```
# Press and release the Left key
wtype -P left -p left
```

Note that when wtype terminates, all the pressed keys/modifiers get released, as the compositor destroys the associated
virtual keyboard object. To help performing a more complicated sequence of key presses, `-s` can be used to insert delays into the stream of key events.

```
# Hold the Right key for 1000ms
wtype -P right -s 1000 -p right
```
