# atomupd-daemon

Daemon that exposes a D-Bus IPC that allows unprivileged processes (e.g. the Steam Client)
to perform system updates without the need of any privilege escalation.

When asked to perform the update operations, this daemon is expected to launch, as a subprocess, the Python
script `steamos-atomupd-client`.

For testing and development, also a CLI `atomupd-manager` is available that makes the same D-Bus calls
as the one outlined above.

If you want to use a custom configuration file for development, you can either put your
configuration in `/etc/steamos-atomupd/client-dev.conf`, or you can launch the daemon
with the `--config` option. Both options will override the default fallback
`/etc/steamos-atomupd/client.conf`.
