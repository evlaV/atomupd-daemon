# atomupd-daemon

Daemon that exposes a D-Bus IPC that allows unprivileged processes (e.g. the Steam Client)
to perform system updates without the need of any privilege escalation.

When asked to perform the update operations, this daemon is expected to launch, as a subprocess, the Python
script `steamos-atomupd-client`.

For testing and development, also a CLI `atomupd-manager` is available.

### Custom configuration

If you want to use a custom configuration file for development, you can either put your
configuration in `/etc/steamos-atomupd/client-dev.conf`, or you can launch the daemon
with the `--config` option. Both options will override the default fallback

For example, if you want to test a steamdeck build from your local server, you can
create a `client-dev.conf` file similar to this one:
```ini
[Server]
QueryUrl = http://example.home.arpa/updates
ImagesUrl = http://example-images.home.arpa/
MetaUrl = http://example.home.arpa/meta
Variants = rel;beta;main
Username = example
Password = hunter2
```

To preserve the config file across updates use:
```shell
echo "/etc/steamos-atomupd/client-dev.conf" > /etc/atomic-update.conf.d/dev.conf
```
