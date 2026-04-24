# CLI Usage

Use the CLI when you need repeatable conversion, logs in a terminal, or batch processing.

## Basic Conversion

```bash
appalchemist --convert ./example.deb
```

The positional form is also accepted and is used by desktop file handlers:

```bash
appalchemist ./example.deb
```

## Output Directory

```bash
appalchemist --convert ./example.rpm --output ./dist
```

If no output directory is provided, AppAlchemist uses its default AppImage output location.

## Disable Auto Launch

Use `--no-launch` when you only want to create the AppImage:

```bash
appalchemist --convert ./example.tar.gz --output ./dist --no-launch
```

## Batch Conversion

```bash
appalchemist --batch ./one.deb ./two.rpm ./three.tar.gz --output ./dist --no-launch
```

Batch mode validates each input path and skips files that are missing.

## Help And Version

```bash
appalchemist --help
appalchemist --version
```

## Exit Codes

The CLI returns `0` when conversion succeeds. It returns a non-zero code when no valid input is provided, the input package is missing, or the conversion backend reports a failure.
