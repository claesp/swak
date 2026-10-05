# Marker collections

A collection is a catalog of marker names, colors, and vector icons. It does not contain placed objects or their coordinates. Core contains Generic marker and provides a home for future SWAK symbols. MILSTD2525 contains the existing sixteen military symbols. These symbols remain a limited MIL-STD-2525C subset.

Tools > Marker Collections opens the collection manager. File > Settings > Marker collections opens the same controls. The table sorts names with the OS locale. Checkboxes enable collections for new markers. An unchecked collection still supplies icons for existing markers. These controls do not change received CoT messages.

Add creates a collection from an explicit template choice. Enter a name. Select the collection whose symbols you want to copy. Edit changes the name, description, and version of a user collection. Built-in collections are read-only. Add a copy before you change their metadata. The manager does not include a vector drawing editor.

Delete removes a user collection from the catalog. Placed markers keep their collection and symbol identifiers. A missing icon appears as a circle with the saved color. The editor shows the unavailable collection and symbol. Import the original collection to restore those icons. Disable a built-in collection to remove it from new-marker choices.

After you place a marker, select Collection before Type. Core offers Generic marker. MILSTD2525 offers the military types. Apply stores both identifiers with the marker. The editor remembers the last chosen collection and type for future placement. The next marker uses Core or another enabled collection if the previous choice becomes unavailable.

Existing records without a collection identifier retain their previous meaning. An empty symbol belongs to Core. A nonempty legacy symbol belongs to MILSTD2525. Undo, movement, and restart preserve the collection reference. All-disabled catalogs reject new placement and return the map tool to idle.

# Import and export

Import accepts ZIP, TAR, and gzip-compressed TAR through the existing libarchive dependency. Export creates ZIP or gzip-compressed TAR according to the destination extension. The archive contains exactly one regular file named `collection.json`. Import extracts no files to disk. Imported collections contain data only, without executable plugins, scripts, fonts, raster images, or external resources.

Select a collection row before Export. Select Import to load an archive. The importer rejects duplicate identifiers without replacing the existing collection. Exported built-in collections retain their identifiers. Use Add to create a separate copy for import into a workspace that already contains the built-in collection. Exports replace their destination atomically and use owner-only permissions.

The compressed input limit is one MiB. The expanded JSON limit is 256 KiB. The complete saved catalog has the same 256 KiB limit and permits at most sixteen collections. Each collection contains one through 64 symbols. Imports reject links, encryption, extra entries, and path names other than `collection.json`.

# Archive schema

The JSON root uses `schemaVersion: 1`. Required metadata includes `id`, `name`, `description`, `version`, `enabled`, and `symbols`. Identifiers contain at most 64 letters, digits, periods, or hyphens. Collection identifiers cannot be empty. Names contain at most 100 characters. Descriptions contain at most 1024 characters, and versions contain at most 32 characters. Metadata must contain printable text.

Each symbol supplies `id`, `name`, `color`, `frame`, and `glyph`. Symbol identifiers are unique within their collection. An empty symbol identifier represents the generic type in Core. Colors use `#RRGGBB`. The frame contains a filled vector outline, and the glyph contains optional dark internal lines. The renderer uses the current marker radius, which preserves existing marker sizes.

A vector path is an array of commands. `M` starts a path, `L` adds a line, and `C` adds a cubic curve. Every nonempty path starts with `M`. Each path permits at most 64 commands. Coordinates must be finite numbers between -2 and 2. The resulting bounds must fit the normalized marker square from -1 to 1, with a 0.01 tolerance.

The following complete file defines an original triangle. Put this file at the archive root under the name `collection.json`. Close a frame explicitly with a final line or curve to its starting point. The glyph can be empty.

```json
{
  "schemaVersion": 1,
  "id": "example.triangles",
  "name": "Triangles",
  "description": "Example vector markers.",
  "version": "1.0.0",
  "enabled": true,
  "symbols": [
    {
      "id": "triangle",
      "name": "Triangle",
      "color": "#ffc857",
      "frame": [["M", 0, -1], ["L", 1, 1], ["L", -1, 1], ["L", 0, -1]],
      "glyph": []
    }
  ]
}
```

A cubic command has seven entries: `["C", controlX1, controlY1, controlX2, controlY2, endX, endY]`. Vector data uses logical coordinates, not pixels or geographic positions. The common renderer scales paths for the map and for selection icons. Qt supplies the existing GPU paint engine for map drawing.

Saved catalogs retain unavailable schema records within the storage limits. Unsupported records do not supply selectable or rendered symbols. Valid saved vector definitions still work when their original collection plugin is absent. The host refreshes registered built-in definitions from their plugins while retaining the enabled flags.
