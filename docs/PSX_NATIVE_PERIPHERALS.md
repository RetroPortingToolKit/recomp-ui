# PSX native peripheral choices

A PSX host can advertise native Mouse, GunCon and neGcon capabilities separately
in `RecompLauncherCGameInfo`. The launcher hides unsupported choices. Consumers
must rebuild against the matching header; appended fields are not an ABI promise
for an older binary.

Mouse and GunCon are distinct input sources. Only one visible seat may use the
host pointer. A one-player Mouse title also exposes the second console port, so
the player can keep a keyboard/gamepad there and choose the Mouse's port. Device
names, GUIDs, live instances and controller settings move together.

NeGcon extends the public PSX profile's Auto selector when the host supports it.
Auto retains its existing behavior, including keyboard input and title locks.
Other consoles keep their own mode lists.

The matching psxrecomp runtime provides native SIO devices and settings
persistence. Its default Mouse capture toggle is Ctrl+F10; its GunCon controls
are configured in `keybinds.ini`'s `[guncon]` section. Merely selecting a PSX
profile does not assert that a host implements either device.
