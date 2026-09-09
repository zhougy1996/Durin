# Road Geometry

Summary: Define final-curve road authority, explicit plane/sphere authoring, fixed planet ownership, and legacy scene conversion.

Modules: RoadWeaver, RoadWeaverEditor, Engine

## Authority and coordinates

`FRoad.ReferenceLine` is the sole persisted final geometry. Its ordinary
three-dimensional `FSplineCurve` represents road elevation as well as horizontal
alignment. Nodes, lane stations, editing, saves, queries and preview deformation
refer to that curve. No runtime projection or radius-dependent position change
occurs during preview rebuilding.

`FDefinition.Planet` identifies one fixed planet and stores its center and radius
in network-local double-precision meters. New networks receive a planet GUID at
creation. In the absence of a planet actor/asset abstraction in these modules,
this small value is the binding; it introduces no new planet framework. Legacy
networks without identity use the reserved default planet binding. Applications
creating several networks for the same planet supply the same binding.

Actor placement is a rigid network-to-world transform: it transforms roads and
planet center together. Scale must be exactly one. Planet identity, center and
radius cannot change through ordinary asset edits once roads exist. Radius
change tooling and reassignment are outside this contract.

The creation dialog selects **Plane** or **Sphere** and a fixed planet radius.
Its network origin is the north pole and its center is `(0, 0, -radius)`.
Plane creates the road on local Z=0; sphere explicitly fits the initial route to
the spherical surface. The initial spherical arc must be shorter than half a
circumference. The submitted length is a creation target; subsequent lengths
are measured from the accepted final cubic curve. Neither option constrains
future edits to remain on that surface. Bridges, tunnels and elevation changes
can be expressed by the same curve without implementing their tooling here.

`bRadialUp` and `ReferenceUp` control orientation only. Radial up uses the vector
from the planet center; fixed up uses the stored reference normal. Radius never
enters position evaluation. Singular tangent/up configurations fail preview
construction with a diagnostic.

## Explicit fitting and publication

`FitRoadDefinition` operates on a complete detached candidate. `DRoadNet::FitToSurface`
combines it with asset publication. Plane fitting projects positions and
derivatives; sphere fitting radially projects source evaluations and fits
manual-broken Hermite intervals. Both publish the resulting Cartesian curve,
not just projected control points or an instruction to project later.

Sphere fits certify the squared radius of every complete cubic using the
convex hull of its degree-six Bernstein coefficients. The accepted radial
error is at most 1e-3 m. Position and orientation probes at quarter intervals
also compare the fit with projected source evaluations (1e-3 m and 1e-3 radians).
The radial certificate covers between probes; the source-position/frame probes
are numerical checks, not a general analytic bound. Fits allow depth 16 and at
most 65536 intervals per curve. Exhaustion, center crossings, collapsed
projections, nonfinite values and degenerate frames are diagnosed. Antipodal
routes need intermediate controls; the fitter chooses no arbitrary great circle.

Coordinates and tangent components are bounded to +/-1e7 m. Base and effective
sphere radii must be within [1, 1e7] m. Curves are open and each segment must have
positive length greater than 1e-6 m. This is a cubic approximation to a sphere,
not an exact analytic spherical curve or production terrain fitting.

The complete operation preserves road/node/lane/junction/connection IDs and
existing curve-point IDs. Added fit points receive new IDs that persist on save.
It projects nodes and authored connectors as part of the same candidate and
remaps every section boundary by its normalized distance on the old road.
Lane identities, widths, directions, speeds and transition mappings are retained.
Validation requires endpoints within 1e-4 m, contiguous nonempty sections from
zero, and final station coverage within max(1e-4 m, 1e-6 times curve length).
Regular curve edits submit reconciled nodes/stations explicitly; they do not
silently invoke fitting. Failed candidates leave asset data, notifications and
Dirty state unchanged.

## Derived preview and queries

`FRoadAlignment` retains the authoritative spline evaluation and distance table.
Preview intervals restrict its cubic polynomials exactly using evaluated
positions and parameter-scaled derivatives. Subdivision can improve frame
approximation but cannot change centerline geometry or query length. Samples
use the same interval frame as `FSplineMeshDeformer`, so mesh and query orientation
agree as well as position. Lane station selection uses the final distance table.
The Engine spline table additionally resolves nonuniform collinear and inflected
cubics; see [Spline System](SplineSystem.md).

Preview meshes currently require constant left/right widths across sections.
Generated `DSplineMeshComponent` objects remain transient, keyed derived output.
All candidates preflight before output reuse. Failed reconstruction retains a
last valid snapshot as Stale, or reports Error when none exists. A preview does
not mark packages Dirty merely by regenerating.

## Compatibility

Schema 3 introduces final-curve authority and planet binding. Schema 1/2 assets,
including older records omitting the then-default version, retain Cartesian
geometry and stable graph IDs. Valid legacy station ranges are normalized to
the curve length before admission. Invalid topology or station ordering is not
repaired by guessing: load logs the entity diagnostic and retains the original
data for explicit repair. Unsupported future versions cannot preview as Ready.

Legacy Actor `Surface` is hidden from editing and retained solely as a serialized
conversion input. `GeometryVersion` gates conversion in `PostLoad`, before native
construction. A constrained legacy instance is converted into an Actor-owned
persistent `DRoadNet` subobject, and its `RoadNet` reference is replaced only
after complete validation. This preserves distinct instance shapes without
modifying a shared external asset. After success the legacy descriptor is cleared;
all further previews derive from the private final curve. No second active
geometry definition exists. Save persists the replacement graph and version;
reload does not fit it again. Unconstrained instances retain their shared asset.

Failure preserves the source reference, legacy parameters and version, reports
an actionable diagnostic and prevents fallback to incorrectly shaped raw geometry.
Repair the original graph/legacy descriptor and reload, or assign a validated
replacement asset. Migration itself does not dirty packages or write files;
an explicit save persists it. The checked-in `NewRoadNet` and `L_RoadNet` remain
legacy regression inputs and migrate on opening.

The transient subtree exclusion, legacy Generated component flag restoration,
and GC attachment-cleanup Dirty suppression from commit `85cb90399` remain intact.
Road integration tests inspect saved exports, round-trip converted geometry,
open a sandbox copy of the checked-in level and collect obsolete generated
components without setting Dirty. They do not substitute for interactive picking
or full editor transaction replay qualification in the broader foundation plan.
