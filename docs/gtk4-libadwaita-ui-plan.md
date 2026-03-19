# AppAlchemist GNOME UI Plan

## Goal

Move the desktop UI toward a GNOME-first design language without rewriting the conversion backend.

The target stack is:

- `GTK4`
- `libadwaita`
- existing conversion pipeline and CLI reused as-is

## Design Direction

Use the same visual principles as the torrent client and video player references:

- calm dark surfaces instead of heavy gradients
- one accent color taken from GNOME system accent
- minimal chrome
- wide spacing and strong content grouping
- motion used only for transitions and state feedback

The application should look native inside GNOME rather than themed manually.

## Layout

Primary window structure:

1. `AdwApplicationWindow`
2. `AdwHeaderBar`
3. `AdwNavigationSplitView`
4. left `AdwNavigationPage` sidebar
5. main content page with status banner, drop card, options group, activity log

### Sidebar sections

- `Convert`
- `Repository`
- `History`
- `Settings`

### Convert page

Top:

- large drop target card
- secondary button for file picker
- helper text for supported formats

Middle:

- grouped conversion settings in `AdwPreferencesGroup`
- output directory row
- optimize size switch
- resolve dependencies switch
- compression combo row

Bottom:

- inline progress row
- collapsible log panel
- success banner with “Open Output Folder”

## Component Mapping

Current Qt widget to GNOME replacement:

- `QMainWindow` -> `AdwApplicationWindow`
- custom top controls -> `AdwHeaderBar`
- custom cards -> `AdwBin` with CSS classes
- `QLabel` status text -> `AdwBanner`
- `QProgressBar` -> `GtkProgressBar`
- `QTextEdit` log -> `GtkTextView` in `AdwExpanderRow` or bottom sheet
- settings row layout -> `AdwActionRow` and `AdwComboRow`

## State Model

The frontend should be driven by a shared controller, not by widget-local state.

Core states:

- idle
- files selected
- converting
- waiting for sudo password
- success
- partial success
- failed
- cancelled

The new `ConversionController` should remain the source for:

- current item index
- total item count
- progress text
- log events
- success and error events
- batch completion

## Accent Color Strategy

Rules:

- do not hardcode purple or blue gradients
- use libadwaita defaults for buttons, switches and selection
- only add a small custom CSS layer for spacing, card radius and transparency
- if dark mode is active, keep surfaces warm and low-contrast

## Motion

Allowed:

- content fade on page switch
- progress reveal
- success banner slide-in
- subtle hover on drop zone

Avoid:

- pulsing glows
- heavy shadows
- animated gradients

## Migration Plan

Phase 1:

- extract UI-independent conversion state into shared controller
- keep Qt frontend working

Phase 2:

- build GTK4/libadwaita window shell
- connect it to shared controller

Phase 3:

- implement Convert page only
- reach feature parity for single and batch conversion

Phase 4:

- migrate repository browser and settings

Phase 5:

- remove or freeze legacy Qt GUI

## First GTK Files

Recommended initial source tree:

```text
frontend/gtk/
  CMakeLists.txt
  main.cpp
  app.cpp
  app_window.cpp
  app_window.h
  app_window.ui
  style.css
```

## Immediate Implementation Notes

- backend should stay in C++ and Qt-free where possible
- GTK frontend should avoid re-implementing conversion logic
- password prompts should be handled in the frontend, while the controller emits a request event
