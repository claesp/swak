# SWAK

SWAK is a native Linux workspace for Cursor-on-Target (CoT) events. It uses C++20, Qt 6 Widgets, and OpenGL. CMake generates the build instructions, and Ninja builds the application.

A plugin adds a feature through a defined interface. Built-in plugins provide event reception, generation, map rendering, data feeds, and local markers. All plugins share core interfaces and declare their roles in embedded manifests. Read [dependencies](dependencies.md) for third-party components.

The application receives CoT XML datagrams and imports individual XML files. You can generate events, inspect positions, and export XML. Interface preferences survive restarts and missing plugins. Map and data downloads require explicit configuration.

Map providers support Web Mercator raster and vector tiles. Data providers load bounded KML and KMZ feeds. A plain green background marks missing map data. Maps and feeds can contain incomplete or outdated information.

Local markers support editable text, colors, and a limited set of original military vector symbols. Received CoT events use the same symbols for supported types. Read [the ATAK reference review](atak-reference-review.md) for provenance and symbol limits. This subset does not establish military-standard compliance.

Read [Build and run](build.md) for setup. Read [Development diagnostics](quality.md) and [the handoff](HANDOFF.md) for tests and current limits. Read [Use the workspace](usage.md) for controls. Read [Plugin architecture](architecture.md) and [Security boundaries](security.md) before adding features.
