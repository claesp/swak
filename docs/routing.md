# Route planning

Open Tools > Plugins > Route planning. Choose a provider and travel mode. Select Pick start, then click the map. Select Pick end, then click the map. You can also enter coordinates directly. Select Calculate to request a route. Cancel / clear stops the request and removes the preview. Escape cancels point placement.

The first version supports driving, walking, running, and cycling. Running uses pedestrian access rules and the configured running speed. The preview shows distance and estimated time. The route stays until you clear it, change its inputs, or close the application. Endpoints and configuration survive restart. Routes are not saved as editable map objects in this version.

Route providers uses the same table and Add, Edit, Delete ... workflow as the other provider managers. Radio buttons select one active provider. New entries start with an empty name and URL. File > Settings > Route providers opens the same controls. Width and running speed apply immediately.

Calculate sends the two coordinates to the selected server. The bundled FOSSGIS demo entry has fair-use limits and no application availability guarantee. No request runs at startup, during panning, or after a configuration change. Use a private server for regular work. HTTPS certificates remain checked. Plain HTTP is allowed only for literal loopback addresses, such as 127.0.0.1.

# Research and design

OpenFreeMap publishes tiles for map display. It uses the OpenMapTiles schema. Its road layer describes rendered road classes, geometry, and selected attributes. It does not expose a complete routing graph with all road connections, access rules, and turn restrictions. SWAK must not infer legal routes from intersections of drawn lines. Bridges and tunnels demonstrate why that approach fails.

Sources: [OpenFreeMap](https://openfreemap.org/) and [OpenMapTiles transportation schema](https://raw.githubusercontent.com/openmaptiles/openmaptiles/master/layers/transportation/transportation.yaml).

OpenStreetMap data supplies nodes, ways, relations, and road tags. A routing engine processes these records into connected edges and permitted transitions. Geofabrik publishes regional OSM extracts in PBF format. PBF is a compact binary format for OSM data. These extracts provide a path to a private routing service without dependence on map display tiles. OpenStreetMap data uses the ODbL license and requires attribution.

Sources: [Geofabrik downloads](https://download.geofabrik.de/), [Geofabrik formats](https://download.geofabrik.de/technical.html), and [OpenStreetMap copyright](https://www.openstreetmap.org/copyright).

Valhalla is an open-source routing engine. Its API supports motor vehicles, pedestrians, and bicycles. The first provider requests distance-based routing. It also requests disabled hierarchy pruning for driving. The server can impose distance and computation limits, so SWAK does not claim a globally shortest route. Running uses pedestrian routing with a different speed. The server returns a GeoJSON LineString inside its OSRM-compatible JSON response.

Sources: [Valhalla route API](https://valhalla.github.io/valhalla/api/route/api-reference/) and [Valhalla project and public demo policy](https://github.com/valhalla/valhalla).

# Provider contract and limits

RouteProvider constructs the request body and decodes the response. RouteSources manages saved entries and asynchronous network requests. The route planner supplies endpoints and draws the result through MapTool. The client does not link a routing engine or import a new application library.

The client permits one request at a time, with a two-second request interval and a twenty-second deadline. Replies cannot exceed 4 MiB or 50000 coordinates. The parser requires finite, bounded coordinates and plausible distance and time values. Redirects are rejected. Cancellation disconnects callbacks before aborting the reply. Provider entries remain stored when their format plugin is absent.

The initial provider URL is https://valhalla1.openstreetmap.de/route. A private Valhalla installation uses its own /route endpoint. The request uses two locations, a costing model, distance-based costing, the OSRM JSON format, and GeoJSON geometry. Responses require code Ok and one route. Errors, absent roads, and unsupported formats leave no old route on the map.

# Next versions

First, qualify private Valhalla deployments with regional PBF extracts and known route fixtures. Cover access restrictions, one-way streets, turn restrictions, bridges, and disconnected roads for each mode. Compare routes against the same engine build and data revision.

Next, add waypoint editing, route saving, route import and export, and selectable alternatives. Add instruction tables and source timestamps before navigation features. Route matching, traffic, transit schedules, accessibility, and closures need their own data and qualification. The current preview does not account for local Restricted objects or replace current road signs.

A later offline mode can manage a separate local routing process and its graph packages. This keeps large graph storage and preprocessing outside the Qt interface. It needs disk limits, data update rules, process supervision, and reproducible regional packages. Display MBTiles alone cannot supply this graph.
