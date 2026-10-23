# GazCast Clean Record for OBS Studio

A free OBS Studio plugin that records a second, **clean** copy of your show:
the same scenes and the same sound, but without the overlays. Your stream and
your normal recording still show everything.

Use it to get a programme recording you can re-edit later, with new graphics
or none at all.

Requires OBS Studio 30 or later (64-bit). Windows x64 is the supported target;
the template's macOS and Linux build paths are kept and should also work.

## Install (Windows)

1. Download `gazcast-clean-record-<version>-windows-x64-setup.exe` from this
   repo's [Releases](https://github.com/GAZMEDIA/gazcast-clean-record/releases).
2. Close OBS and run the setup file. If Windows shows "Windows protected your
   PC", click **More info**, then **Run anyway** (the installer is not signed).
3. Start OBS and turn the dock on from **Docks → GazCast Clean Record**.

Prefer no installer? Download the `.zip` instead, unzip it and copy the
`gazcast-clean-record` folder to `C:\ProgramData\obs-studio\plugins\`.

To uninstall, use **Settings → Apps** in Windows (GazCast Clean Record for OBS
Studio), or delete `C:\ProgramData\obs-studio\plugins\gazcast-clean-record`.

## Use

1. In the **GazCast Clean Record** dock, check the list of sources. Every
   overlay is ticked for you the first time the plugin sees it: browser
   sources (GazCast and any other web overlay), images, slideshows and text.
   Cameras, screen and window captures, media files and colour backgrounds stay
   in. Untick anything you want to keep, tick anything else to leave out.
2. Press **Start Recording** in OBS as usual. With **Start and stop together
   with OBS's Start Recording button** ticked (the default), the clean copy
   starts and stops with it and is saved next to your recording as
   `<recording name> (clean).mkv` (or `.mp4` if you record MP4).
3. You can also use the dock's own **Start clean recording** button; that file
   is called `GazCast clean <date> <time>` and goes in your recording folder.

Ticking a source adds a filter called **Hide in GazCast clean recording** to
it, so you can also add or remove it from the source's **Filters** window. It
is saved with your scene collection.

The clean copy uses the same video encoder and settings as your normal
recording (or the first hardware encoder it finds, then x264), so it costs
about as much as a second recording. MP4 files are written fragmented so they
survive a crash.

## Known limits

OBS draws a few things once per frame and reuses them. An overlay still shows
in the clean copy if it sits inside a scene item that is cropped, has a scale
filter or a blending mode set, and during the second or so a scene transition
runs. The stream is never affected.

## How it works

- The **Hide in GazCast clean recording** filter (`gazcast_clean_hide`) passes
  its source through untouched, except while the clean mix is rendering, when
  it draws nothing.
- While a clean recording runs, a private source renders OBS's program output
  (output channel 0) into its own video mix (`obs_view`). That mix gets its own
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
