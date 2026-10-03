# ATAK reference review

Review date: October 3, 2026. The reference is [ATAK-CIV](https://github.com/TAK-Product-Center/atak-civ/tree/9f6893dd657feacc35ec5de03dad721c2e44170e) at commit `9f6893dd657feacc35ec5de03dad721c2e44170e`. SWAK uses factual format descriptions from this review. No ATAK source files, icons, fonts, or other assets enter SWAK.

The reviewed `CustomMobacMapSource.java` describes coordinate placeholders, inverted tile rows, and quadkeys. A quadkey encodes tile coordinates as a string of digits. `MobacMapSourceFactory.java` recognizes custom image sources, WMS sources, and combined layers. Both files reside under `takkernel/engine/src/main/java/com/atakmap/map/layer/raster/mobac/`.

SWAK now supports north-origin XYZ, south-origin TMS, and quadkey image addressing through the existing image plugin. These conventions identify images on the same Web Mercator grid. They do not require separate network clients or renderers. SWAK retains its own JSON source records and placeholder syntax. The implementation uses no code from the reference repository.

ATAK uses `{$z}`, `{$x}`, `{$y}`, and `{$q}`. SWAK uses `{z}`, `{x}`, `{y}`, or `{q}`. SWAK does not import ATAK XML files, server rotation, combined layers, scripts, or geographic tile grids. Existing WMTS, WMS, and MVT plugins remain available. [Microsoft documents quadkey addressing](https://learn.microsoft.com/en-us/bingmaps/articles/bing-maps-tile-system), including tile 3,5 at zoom 3 as `213`.

The reviewed `takkernel/shared/src/main/java/gov/tak/platform/client/eud/MapSources.java` reads catalogs from a configured service URL. Catalog entries reference downloadable source files. That service can require authentication. The inspected files did not supply a reusable public catalog. SWAK does not add private catalog URLs or credentials.

The USGS Topo preset comes from separate [USGS service metadata](https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer?f=pjson). It is not a copied ATAK preset. The service describes public-domain topographic data, 256-pixel images, EPSG:3857, and mixed PNG/JPEG tiles. Its URL uses level, row, and column order. The preset now limits zoom to 8 after the user reported missing tiles at higher levels. The published maximum map scale corresponds to level 16. The service metadata lists additional cache levels, but does not establish useful coverage at every level.

The preset links its attribution to the [service credits](https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer). It uses HTTPS with certificate validation and remains inactive until selected with online maps enabled. No Bing or Google service is added. Addressing support does not grant access to a commercial service.

ATAK includes MIL-STD-2525C, D, and E symbol code. Its `Icon2525cTypeResolver.java` relates CoT types to 2525C identifiers through `CotUtils`. That file resides under `atak/ATAK/app/src/main/java/com/atakmap/android/icons/`. SWAK uses the public [MIL-STD-2525C specification](https://worldwind.arc.nasa.gov/milstd2525c/Mil-STD-2525C.pdf) for its initial subset.

Section 5.3.1.1 defines identity frames. SWAK draws friendly ground units as rectangles, hostile units as diamonds, neutral units as squares, and unknown units as four-lobed frames. Appendix A identifies infantry, armor, and medical functions. SWAK draws crossed lines, an oval, and a cross for these functions. Plain frames represent generic ground units.

All paths use original C++ geometry. SWAK neither traces nor loads reference artwork. Sixteen symbols cover four identities and four ground-unit roles. The local editor offers these symbols alongside the existing circle. Received CoT events use the same drawing functions for these exact types:

- `a-{identity}-G-U` identifies a generic ground unit.
- `a-{identity}-G-U-C-I` identifies infantry.
- `a-{identity}-G-U-C-A` identifies armor.
- `a-{identity}-G-U-S-M` identifies a medical unit.

The identity field accepts `f`, `h`, `n`, or `u`. Unsupported types and subtypes retain a circle. Saved unknown symbol identifiers remain intact for later versions. Local symbols use the existing seven-pixel radius. Received events retain their six-pixel radius and eight-pixel selected radius. Qt paints these paths at the display scale.

This subset does not implement the full standard, modifiers, equipment, air symbols, or all identity states. It does not claim MIL-STD-2525 compliance. Local color overrides can depart from standard identity colors. The user can keep the existing orange circle when military symbols are unsuitable.
