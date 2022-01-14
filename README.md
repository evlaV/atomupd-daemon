# atomupd-daemon

Daemon that exposes a D-Bus IPC that allows unprivileged processes (e.g. the Steam Client)
to perform system updates without the need of any privilege escalation.

Currently, this is still a WIP and only the structure of the project is in place.

When asked to perform the update operations, this daemon is expected to launch, as a subprocess, the Python
script `steamos-atomupd-client`.

For testing and development, also a CLI is available that makes the same D-Bus calls as the one outlined
above (yet to be implemented).
