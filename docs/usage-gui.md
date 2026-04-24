# GUI Usage

AppAlchemist's GUI is intended for the normal single-package conversion flow.

## Convert A `.deb` Package

1. Start AppAlchemist.
2. Choose the `.deb` file.
3. Select the output directory.
4. Start conversion.
5. Review the conversion log.
6. Run the generated AppImage from a terminal for a first smoke test.

## Convert A `.rpm` Package

Use the same flow as `.deb` conversion. RPM packages can vary more by distribution, so check the generated launcher and AppDir contents if startup fails.

## Convert An Archive

Supported archive inputs include `.tar.gz`, `.tar.xz` and `.zip` when they contain a usable Linux application layout.

Archive conversion works best when the archive includes one or more of:

- a desktop launcher
- a clear executable entry point
- an icon
- a conventional `usr`, `opt`, or application root layout

## Choose Output Directory

Select a writable directory where generated AppImages should be placed. A dedicated directory such as `~/AppImages` keeps converted artifacts easy to inspect and remove.

## Read Conversion Logs

The conversion log is the first place to check when a package fails. Look for:

- missing executable detection
- missing or invalid `.desktop` metadata
- missing icon files
- dependency copy failures
- AppDir packaging errors

## Run Generated AppImage

Run the generated AppImage directly from a terminal:

```bash
./dist/example-x86_64.AppImage
```

If your system cannot mount AppImages through FUSE:

```bash
APPIMAGE_EXTRACT_AND_RUN=1 ./dist/example-x86_64.AppImage
```
