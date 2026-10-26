# GazCast Clean Record for OBS Studio

A free OBS Studio plugin that records a second, **clean** copy of your show:
the same scenes and the same sound, but without the overlays. Your stream and
your normal recording still show everything.

Use it to get a programme recording you can re-edit later, with new graphics
or none at all.

Requires OBS Studio 30 or later (64-bit). Windows x64 is the supported target;
the template's macOS and Linux build paths are kept and should also work.

## Install (Windows)

1. Download the installer:
   https://github.com/GAZMEDIA/gazcast-clean-record/releases/latest/download/gazcast-clean-record-windows-x64-setup.exe
   (always the newest version; older ones are on the
   [Releases](https://github.com/GAZMEDIA/gazcast-clean-record/releases) page).
2. Close OBS and run the setup file. If Windows shows "Windows protected your
   PC", click **More info**, then **Run anyway** (the installer is not signed).
3. Start OBS and turn the dock on from **Docks → GazCast Clean Record**.

Prefer no installer? Download the `.zip` instead, unzip it and copy the
`gazcast-clean-record` folder to `C:\ProgramData\obs-studio\plugins\`.

To uninstall, use **Settings → Apps** in Windows (GazCast Clean Record for OBS
Studio), or delete `C:\ProgramData\obs-studio\plugins\gazcast-clean-record`.

## Use

1. The **GazCast Clean Record** dock lists every scene and source in OBS, all
   ticked. Untick the ones to leave out of the clean recording (your overlays,
   logos, text...). Untick a scene to leave out everything in it, for example
   a "Graphics" scene you place inside your other scenes. Sources that look like overlays (browser sources,
   GazCast and other web overlays, images, slideshows, text) are labelled
   "(overlay)" to help you choose, but nothing is unticked for you.
2. Press **Start Recording** in OBS as usual. With **Start and stop together
   with OBS's Start Recording button** ticked (the default), the clean copy
   starts and stops with it and is saved next to your recording as
   `<recording name> (clean).mkv` (or `.mp4` if you record MP4).
3. You can also use the dock's own **Start clean recording** button; that file
   is called `GazCast clean <date> <time>` and goes in your recording folder.

Unticking a scene or source adds a filter called **Hide in GazCast clean
recording** to it, so you can also add or remove it from its **Filters**
window. It is saved with your scene collection.

The clean copy uses the same video encoder and settings as your normal
recording (or the first hardware encoder it finds, then x264), so it costs
about as much as a second recording. MP4 files are written fragmented so they
survive a crash.

## Known limits

Overlays can show in the clean copy for the second or so a scene transition
runs, and in scenes that contain an item using **Crop to bounds**. The stream
is never affected.

## How it works

- The **Hide in GazCast clean recording** filter (`gazcast_clean_hide`) passes
  its source through untouched, except while the clean mix is rendering, when
  it draws nothing.
- While a clean recording runs, a private source draws OBS's program scene
  into its own video mix (`obs_view`). It walks the scene's items and nested
  scenes itself, with its own textures for cropped, scaled or blended items,
  so nothing drawn for the stream is reused. That mix gets its own
  encoders and an `ffmpeg_muxer` output, with the normal recording's first
  audio track.
- Settings live in
  `%APPDATA%\obs-studio\plugin_config\gazcast-clean-record\clean-record.json`.

## Building

Push to this repo (or run the workflow by hand) and GitHub Actions builds the
Windows plugin with the obs-plugintemplate scripts in `.github/scripts`; each
build is saved as a release.

## Reporting problems

Open an issue: https://github.com/GAZMEDIA/gazcast-clean-record/issues

## License

GPL-2.0-or-later, like OBS Studio. See `LICENSE`.
