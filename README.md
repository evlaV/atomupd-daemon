# atomupd-daemon

Daemon that exposes a D-Bus IPC that allows unprivileged processes (e.g. the Steam Client)
to perform system updates without the need of any privilege escalation.

Currently, this is still a WIP and only the structure of the project is in place.

The following properties, methods and signals are exposed under the `com.steampowered.Atomupd1` interface:

- `Version`: property that exposes an unsigned 32-bit integer representing the version of this daemon

- `IsPreparationInProgress`: property that exposes a boolean that is `true` is an update preparation is in progress

- `IsUpdateInProgress`: property that exposes a boolean that is `true` is an update is in progress

- `IsUpdateAvailable`: method that asks the server if an update is available and returns a boolean
  to indicate if there is an update, a string containing the update version and another string with
  the changelog. In input, it takes a `Branch` string value that can be used to switch between the
  available update channels, e.g. `stable`, `beta`, `dev` etc...

- `EstimateUpdateSize`: method that estimates an update download size. It returns an unsigned
  64-bit integer that represent the amount of estimated Bytes that will be required to download from
  the remote server

- `PauseUpdate`: method that pauses the update that is currently in progress, if any.

- `ResumeUpdate`: method that resumes a previously paused download, if any.

- `StartPreparingUpdate`: method that starts the preparation phase of an update. Usually this involves
  downloading the RAUC update bundle file and re-generating the seed index file, if necessary.
  To know when the preparation ends, subscribe to the `PreparationProgress` signal before calling
  this method.

- `StartUpdate`: method that starts the update. This is expected to be called after the preparation
  phase ended. To know when the update ends, subscribe to the `UpdateProgress` signal before calling
  this method.

- `PreparationProgress`: signal that emits the progress percentage of an update preparation, if any,
  and a `Completed` boolean that indicates whether this phase ended or not.

- `UpdateProgress`: signal that emits the progress percentage of an update, if any, and a `Completed`
  boolean that indicates whether this phase ended or not.


When asked to perform the update operations, this daemon is expected to launch, as a subprocess, the Python
script `steamos-atomupd-client`.

For testing and development, also a CLI is available that makes the same D-Bus calls as the one outlined
above (yet to be implemented).
