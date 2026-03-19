# Experimental GTK Frontend

This directory is reserved for the future GNOME-first frontend built with `GTK4` and `libadwaita`.

The migration strategy is:

1. keep the current conversion backend
2. move GUI state orchestration into shared controllers
3. attach a new GTK frontend to those controllers

Until the build integration lands, the implementation plan is documented in:

- `docs/gtk4-libadwaita-ui-plan.md`
