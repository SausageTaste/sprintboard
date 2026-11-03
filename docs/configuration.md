# Configuration

[Back to README](../README.md)

Sprintboard creates `server_configs.json` when it first starts. Each entry in
`dir_bindings` names a gallery folder and maps it to one physical directory.
Use a nonempty `local_dir` string; paths can be absolute or relative to the server's
working directory. In JSON, write Windows backslashes as `\\`.

## Example

```json
{
  "cache_dir": ".sprintboard/cache",
  "avif_gen": false,
  "avif_gen_remove_src": false,
  "avif_quality": 70.0,
  "avif_speed": 4,
  "tagger_enabled": false,
  "tagger_host": "127.0.0.1",
  "tagger_port": 8790,
  "tagger_batch_size": 4,
  "tagger_poll_interval_seconds": 30.0,
  "image_index_folder_discovery_interval_seconds": 300.0,
  "image_index_visit_refresh_interval_seconds": 60.0,
  "dir_bindings": {
    "example": {
      "local_dir": "./fixtures/images"
    }
  },
  "server_host": "127.0.0.1",
  "server_port": 8787,
  "tls-certfile": "",
  "tls-keyfile": ""
}
```

## Network settings

Restart the server after changing these settings.

| Key | Description |
| --- | --- |
|`server_host` |This is the IP address used by the server. Change it to something like `192.168.x.x` if you want to access the web app from your phone. |
|`server_port` |The port number to bind to. Pick any available port you like (for example, `8787`). |
|`tls-certfile` |The file path to the TLS certificate file used to enable HTTPS mode. |
|`tls-keyfile` |The file path to the TLS key file used to enable HTTPS mode. |

## Library settings

These settings reload automatically.

| Key | Description |
| --- | --- |
|`avif_gen` |Generate an AVIF proxy for each PNG source. A source such as `img.png` produces `img.png.sprintboard.avif`. When tagging is enabled, current analysis is required before a new proxy is encoded. |
|`avif_gen_remove_src` |Retained for configuration compatibility but not implemented. Sources are removed only by an explicit gallery delete action. |
|`avif_quality` |Quality option for AVIF encoder. |
|`avif_speed` |Speed option for AVIF encoder. |
|`tagger_enabled` |Analyze gallery images through the local tagging service and make its general and character tags searchable. |
|`tagger_host` |Host running the tagging service. The default is `127.0.0.1`. |
|`tagger_port` |Port used by the tagging service. The default is `8790`. |
|`tagger_batch_size` |Maximum number of image paths submitted in one analysis request. This must not exceed the service's `--batch-size`. |
|`tagger_poll_interval_seconds` |Minimum delay between checks for missing or stale analyses. |
|`cache_dir` |External fallback cache directory, used when an image root cannot host a writable database. Defaults to `.sprintboard/cache`; relative paths resolve against the server working directory. Must be a nonempty string. |
|`dir_bindings` |Add folder entries here. Each key will appear as a folder in the root directory, and all contents in `local_dir` will be placed inside it. You can use both absolute and relative paths for `local_dir`. Each binding maps to exactly one local directory. |
|`image_index_folder_discovery_interval_seconds` |Seconds between folder discovery passes (default: `300`). |
|`image_index_visit_refresh_interval_seconds` |Minimum seconds between scans of a visited folder (default: `60`). |

Both scan intervals must be finite and at least one second. See
[indexing behavior](storage-and-indexing.md#indexing-and-refresh) for how they work.

## Older configurations

Replace `"local_dirs": ["path"]` with `"local_dir": "path"`. Split bindings with
multiple directories into separate entries; `local_dirs` is no longer supported.

`image_index_full_refresh_interval_seconds` and
`image_index_date_refresh_interval_seconds` remain readable for compatibility but
no longer schedule recurring scans. Use the discovery and visit settings above.

## Troubleshooting

- **Config won't load:** check commas, quotes, and escaped backslashes. The server
  logs JSON parsing errors.
- **Phone can't connect:** check that both devices are on the same network, set
  `server_host` to your PC's LAN address or `0.0.0.0`, and restart the server.
- **Empty gallery after an update:** rebuild both the frontend and backend, then
  reload the browser. The C++ build does not update `dist`.

For VS Code debugging, set the working directory in `.vscode/settings.json`:

```json
{
  "cmake.debugConfig": {
    "cwd": "${workspaceFolder}"
  }
}
```
