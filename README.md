# DarwinCTL

DarwinCTL is a lightweight init-style controller for macOS, inspired by systemd/rc.d, but simplified for user-defined units. It provides unit management, dependency resolution, and boot initialization via launchd.

## Features

- Unit definitions in TOML (~/DarwinUnits/*.toml by default).

- Dependency resolution with proper topological ordering.

- Single-boot guard: core_init can only run once per boot (further invocations return already booted).

- Launchd integration (`com.darwinctl.core.plist`) for automatic execution at macOS startup.


## Commands
```bash
darwinctl map [<root>]
```
This command prints the dependency tree. If a root unit is specified, the tree starts from that unit; if omitted, it defaults to `rootinit`. It is typically used to visualize startup order and confirm that dependencies are correctly linked.


```bash
darwinctl start <unit>
```
This command launches the specified unit together with all of its dependencies. Units are always started in correct topological order, which guarantees that every dependency is running before the dependent unit is started. For example, running darwinctl start `rootinit` will start `rootinit` and every unit that depends on it.


```bash
darwinctl edit <unit>
```
This command opens a unit file in the editor defined by the `$EDITOR` environment variable (defaulting to nano if not set). When editing finishes, the file is validated; if the new content is invalid, the old version is restored. The command also takes care of immutability flags, especially for `rootinit.toml`, which is protected by default. Editing through this command is the recommended and safe way to modify unit definitions.
