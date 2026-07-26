# Spline System

Durin's spline support currently provides an editable, persistent spatial-curve
component and its query API. It does not yet provide an end-to-end spline
content system such as spline meshes, viewport point manipulation, or a
gameplay path follower.

## Support Summary

| Area | Current support |
| --- | --- |
| Curve data | Reflected position, arrive/leave tangents, rotation, scale, point type, loop state, and distance-table quality |
| Interpolation | `Linear`, `Constant`, authored cubic Hermite (`Curve`), and automatically tangented cubic Hermite (`CurveAuto`) |
| Runtime queries | Location, tangent, direction, rotation, scale, and transform by parameter; location, tangent, direction, and transform by distance |
| Coordinate spaces | Component-local and transformed world-space results |
| Editing | Details-panel point editing, add/remove/reorder, loop and quality settings, continuous-edit transactions, Undo/Redo, and Cancel |
| Viewport | Sampled curve, point markers, and tangent display; the visualization selects only the owning component |
| Persistence | Reflection, object-graph duplication, level-package save/load, and post-load cache repair |
| Higher-level consumers | None in production code beyond the Level Editor visualization and details customization |

## Runtime Model

`FSplineCurve` owns the reflected control points and transient arc-length cache.
`DSplineComponent` embeds the curve, converts query results through its scene
transform, marks its package dirty after component-level mutations, and exposes
the runtime/editor-facing API. A new curve starts with two `CurveAuto` points at
local positions `(0, 0, 0)` and `(100, 0, 0)`.

Each segment uses the type of its starting control point:

- `Linear` linearly interpolates position.
- `Constant` holds the starting position, rotation, and scale until the segment
  endpoint and returns a zero tangent.
- `Curve` evaluates a cubic Hermite segment from the starting leave tangent and
  ending arrive tangent.
- `CurveAuto` evaluates the same Hermite form but derives one shared tangent at
  each point from neighboring positions. Open-curve endpoints use the adjacent
  chord; interior and closed-loop points use half the previous-to-next chord.

Tangents are derivatives with respect to the segment-local parameter in
`[0, 1]`, not normalized directions. Rotation uses normalized shortest-path
spherical interpolation and scale uses linear interpolation for every
non-constant segment. These values are authored independently: returned
rotations do not automatically align to the spline tangent. A caller that needs
a tangent-aligned frame must construct its own orientation from the returned
direction and its chosen up-vector policy.

The global parameter is the zero-based segment index plus the segment-local
parameter. Open curves clamp queries to `[0, NumSegments]`. Closed curves add
the last-to-first segment and wrap values outside that range. At the closed-loop
seam, parameter `NumSegments` evaluates at the first point, while
`GetDistanceAtParam(NumSegments)` reports the full loop length.

Empty curves return neutral values, and a one-point curve returns that point's
position, rotation, and scale with zero tangent and zero length.

## Distance Queries

The curve builds a fixed-step polyline table in local space and uses linear
interpolation within that table to convert between distance and global
parameter. `ReparamStepsPerSegment` controls its sampling density, defaults to
`10`, and is clamped to `[1, 1024]`. Length and distance-based results are
therefore approximations for curved segments rather than analytic arc length.

Open-curve distance queries clamp to the two endpoints. Closed-curve distances
wrap by the computed loop length. Moving or rotating the component does not
change the reported length. Component scale, including non-uniform scale,
changes world-space locations and tangents but deliberately does not redefine
the local-space length used by distance queries. Consequently, world-space
motion sampled by local distance is not constant-speed after non-uniform scale.

`FSplineCurve` setters invalidate the cache and rebuild it lazily on the next
length or distance query. `DSplineComponent` setters rebuild immediately and
mark the containing package dirty. Archive loading writes reflected fields
directly, so `DSplineComponent::PostLoad` explicitly repairs the cache;
`FSplineCurve::UpdateSpline` is the corresponding explicit repair point for
other archive owners.

## Editor Support

The Level Editor registers both a details customization and a component
visualizer for `DSplineComponent`.

The details view supports:

- component transform, loop state, and reparameterization quality;
- per-point position, rotation, scale, and interpolation type;
- manual arrive and leave tangents for `Curve` points; and
- appending, removing, and reordering points.

Property edits use the shared reflected-property transaction path. Continuous
transform, tangent, and quality edits coalesce into transactions, Escape
cancels an active edit, and structural changes participate in Undo/Redo. Adding
a point copies the last point, moves it `100` local units forward, and changes
its type to `CurveAuto`; an empty curve receives a default point.

The viewport visualizer draws the sampled curve. When the component is selected,
it also draws every control point and displays authored or automatic tangent
lines for curved points. Its hit data identifies the actor and component only.
There is no point-level selection, transform gizmo, tangent-handle hit target,
or direct viewport dragging; point manipulation currently happens in the
details panel.

## Current Boundaries

The repository currently has no production runtime consumer of
`DSplineComponent` outside the Level Editor integration. In particular, the
spline system does not currently include:

- spline-mesh deformation or spline rendering in game/runtime output;
- automatic actor placement, path following, animation, navigation, or physics
  integration;
- nearest-point, curvature, bounds, per-point metadata, or event queries; or
- point-level viewport selection and manipulation.

The implemented layer is therefore suitable for storing, editing, serializing,
and sampling paths from C++. A feature that needs visible geometry or behavior
along the path must provide that consumer separately.

## Verification Coverage

Native tests cover default, empty, and one-point curves; manual Hermite and
automatic tangents; closed-loop wrapping; straight-line distance conversion;
local/world transform behavior; reflection; duplication; transactional editing
and cache rebuilds; level-package round trips; and emission of selectable
viewport visualization lines. Curved-segment arc-length error, degenerate
curves, and interactive viewport point editing do not currently have dedicated
behavior coverage.
