# xiaopai

Monorepo for the Xiaopai OpenClaw skills and Stack Chan firmware/server work.

## Projects

- `openclaw-skills/` - OpenClaw skill plugins and OpenSpec documents.
- `stack-chan/` - Stack Chan firmware and companion server.

## Submodules

The Stack Chan project keeps upstream third-party sources as Git submodules:

```sh
git submodule update --init --recursive
```

## Morrow startup modes

The three Morrow configurations must be paired with the correct runtime mode. Stop the current Morrow server before switching modes because these commands listen on port `3000`.

### Morrow API key

Provider credentials are not stored in the repository TOML files. Every mode reads the key from the current Ubuntu
user's standard configuration directory:

```text
${XDG_CONFIG_HOME:-$HOME/.config}/xiaopai/morrow-api-key
```

Create the directory and move your key file there without `sudo`:

```sh
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/xiaopai"
mv /path/to/your/key-file "${XDG_CONFIG_HOME:-$HOME/.config}/xiaopai/morrow-api-key"
chmod 600 "${XDG_CONFIG_HOME:-$HOME/.config}/xiaopai/morrow-api-key"
```

The file may contain the raw key or a single assignment such as `OPENAI_API_KEY="..."`. It is parsed as data and
never executed as a shell script. Override the location with `MORROW_API_KEY_FILE=/another/path` when needed.

Use the controller script to select a mode:

```sh
/home/myendless/xiaopai/start_morrow.sh nolark
/home/myendless/xiaopai/start_morrow.sh lark
/home/myendless/xiaopai/start_morrow.sh demo
```

### Scripted demo mode

This mode loads `config-demo.toml` without `--robot`. It is a scripted, tool-free mode for the final-event reception request, weather card, and urgent approval scenes.

```sh
morrow --config /home/myendless/xiaopai/morrow/config-demo.toml \
  server --host 0.0.0.0 --port 3000
```

### Final-event Q&A mode

This mode loads `config-final-event.toml` for on-site conversation and project Q&A only. Do not add `--robot`; without that flag, Morrow does not register the Feishu, weather, or map tools.

```sh
morrow --config /home/myendless/xiaopai/morrow/config-final-event.toml \
  server --host 0.0.0.0 --port 3000
```

### Full robot mode

This mode loads `config-full.toml` and enables the robot toolset, including Feishu tools such as `lark_calendar_list`.

```sh
morrow --config /home/myendless/xiaopai/morrow/config-full.toml \
  server --robot --host 0.0.0.0 --port 3000
```

The runtime flag is the hard boundary: `config-demo.toml` and `config-final-event.toml` must run without `--robot`, while `config-full.toml` must run with `--robot` when Feishu access is required. The system prompt alone does not disable tool registration.
