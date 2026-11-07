# Keyboard and controller input

AnyPS4 reads the keyboard and an XInput controller for The Witch and the Hundred Knight: Revival Edition (CUSA02799). The keyboard mapping is edited in the launcher, which saves it as `anyps4/pad.ini` beside the generated game executable. Set `ORBIS_PAD_CONFIG` to use a file at another path.

## XInput controller

The controller in slot 1 is used while it is connected; a disconnected controller is checked again every two seconds. A/B/X/Y map to Cross/Circle/Square/Triangle, LB/RB to L1/R1, and the triggers to L2/R2 (digital above a threshold of 30, with their analog value). Start maps to Options, Back to the touch pad, and the thumb clicks to L3/R3. The sticks use a dead zone of 7849. DirectInput is not implemented.

`xinput=0` in `pad.ini` turns the controller off.

## Keyboard mapping

`pad.ini` is a standard INI file with a `[pad]` section. Each line is `action=KEY`. The actions are `cross`, `circle`, `square`, `triangle`, `up`, `down`, `left`, `right`, `l1`, `r1`, `l2`, `r2`, `l3`, `r3`, `options`, `touchpad`, `ls_up`, `ls_down`, `ls_left`, `ls_right`, `rs_up`, `rs_down`, `rs_left` and `rs_right`.

A key is a letter or digit (`Z`, `1`), a name from [tools/orbis_impl/PadKeyNames.hpp](../../tools/orbis_impl/PadKeyNames.hpp) (`UP`, `RETURN`, `SPACE`, `LSHIFT`, `F5`, `NUMPAD4`, `COMMA`...), `NONE` for no key, or `VK:<code>` for a Windows virtual-key code. Names are case-insensitive.

The default mapping:

```ini
[pad]
xinput=1
cross=Z
circle=X
square=C
triangle=V
up=UP
down=DOWN
left=LEFT
right=RIGHT
l1=Q
r1=E
l2=1
r2=3
l3=F
r3=G
options=RETURN
touchpad=T
ls_up=W
ls_down=S
ls_left=A
ls_right=D
rs_up=I
rs_down=K
rs_left=J
rs_right=L
```
