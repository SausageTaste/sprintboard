# Sprintboard

Browse and manage your ComfyUI images from a desktop or phone browser.
Run the server on the computer that stores your images, then connect over your
local network.

<p>
  <img src="docs/images/IMG_6695.avif" width="48%" alt="Sprintboard mobile gallery" />
  <img src="docs/images/IMG_6697.avif" width="48%" alt="Sprintboard image viewer" />
</p>

- Browse folders and view images on desktop or mobile.
- Search ComfyUI prompts and model names.
- Add searchable AI tags with an optional local tagging service.
- Generate smaller AVIF copies while keeping the original images.

## Quick start

1. Download a prebuilt binary or [build from source](#build-from-source).
2. Run `sprintboard.exe` from a folder containing `dist` and the required DLLs.
3. Open **http://127.0.0.1:8787** and select **Image View**.
4. Edit the `dir_bindings` section in the generated `server_configs.json` to point
   to your images:

```json
{
  "dir_bindings": {
    "My images": {
      "local_dir": "C:\\Users\\you\\Pictures\\ComfyUI"
    }
  }
}
```

The folder name appears in the gallery. Directory settings reload automatically.
See [configuration](docs/configuration.md) for all options.

**On your phone:** connect to the same Wi-Fi, set `server_host` to your PC's LAN
address or `0.0.0.0`, and restart the server. Open `http://<PC-IP>:8787` on your
phone—for example, `http://192.168.0.201:8787`. For access outside your network,
use a VPN instead of exposing the server port directly.

## Search

Separate terms with commas. For example:

```text
1girl, -blue hair, model:animagine, dim:ver
```

| Filter | Meaning |
| --- | --- |
| `text` | Include images whose prompts or AI tags contain the text |
| `-text` | Exclude images whose prompts or AI tags contain the text |
| `model:text` | Match part of a model name |
| `dim:ver` / `dim:hor` | Show portrait / landscape images |

Commas always separate terms, including inside parentheses.

## Optional features

**AI tags:** start a [local tagging service](docs/tagging.md), then set
`tagger_enabled` to `true`.

**AVIF copies:** set `avif_gen` to `true`. A source such as `image.png` produces
`image.png.sprintboard.avif`, preserving its ComfyUI metadata. Originals are not
automatically removed. Gallery deletion removes both the source and its managed
copy; see [deletion behavior](docs/storage-and-indexing.md#deleting-images).

## When images update

At startup, recent date folders appear first, followed by a full library scan.
After that, Sprintboard discovers folders every **5 minutes** and checks contents
as you browse, with a **60-second** minimum between scans of the same folder.
Unvisited folders can stay out of date; manual refresh scans the whole library.

See [indexing and storage](docs/storage-and-indexing.md) for scan settings,
backups, cloud sync, and migrations. Back up each collection's hidden
`.sprintboard` folder too—it stores your AI tags.

## Build from source

Clone the repository with its submodules:

```sh
git clone --recurse-submodules https://github.com/SausageTaste/sprintboard.git
cd sprintboard
```

Build the frontend with Node.js and npm:

```sh
npm ci
npm run build
```

For the backend, install CMake, vcpkg, and a compatible C++ compiler
(Visual Studio or Build Tools on Windows). Set `VCPKG_ROOT` and build the CMake
target **`sprintboard`**. Run the executable from a directory containing `dist`
and its required runtime libraries.

After updating, rebuild **both** the frontend and backend, then reload your
browser. Use `npm run dev` for frontend development.
