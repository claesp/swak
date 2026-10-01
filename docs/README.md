# SWAK

SWAK is a native Linux workspace for Cursor-on-Target (CoT) events. It uses C++20, Qt 6 Widgets, and OpenGL. CMake generates the build instructions, and Ninja builds the application.

A plugin adds a feature through a defined interface. Six built-in plugins provide the CoT format, the map view, land outlines, event markers, UDP reception, and the event editor. Qt is the only direct application dependency.

The application receives CoT XML datagrams and imports individual XML files. You can generate events, inspect their positions, and export the XML preview. Interface preferences survive restarts and missing plugins. The application does not send network traffic or download maps.

The offline map contains schematic land outlines and a coordinate grid. These outlines are hand-authored reference shapes, not surveyed map data. Do not use this map for navigation.

Read [Build and run](build.md) for setup. Read the [test report](testing.md) for tested platforms and limits. Read [Use the workspace](usage.md) for controls. Read [Plugin architecture](architecture.md) and [Security boundaries](security.md) before adding features.

This first version supports a bounded subset of CoT XML 2.0. It does not implement TAK server connections, encrypted transport, multicast, raster tiles, terrain, or military symbol standards. The plugin interfaces provide places to add these features.
