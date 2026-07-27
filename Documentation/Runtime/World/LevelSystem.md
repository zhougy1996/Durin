# Level System

`DLevel` is the persistent scene asset. A packaged level is the main asset of a `.dasset` package. Levels retain actors through reflected `TObjectPtr` arrays, and actors retain their components the same way; their Outer hierarchy separately provides structural containment and object paths.

`DWorld` is a runtime or editor session container. It activates at most one level, forwards actor APIs to that level, and registers or unregisters the level's components when switching. Replacing a transient level structurally owned by the world marks that complete level hierarchy as garbage. A persistent packaged level is structurally owned by its package instead, so switching worlds does not destroy it; an object that must survive a transient world must likewise be explicitly reparented before world retirement.

A world starts without an active level. Actor operations safely return empty or fail until a level is activated. The editor supports this empty state: scene panels remain available, while level-dependent editing actions are disabled.

Scene persistence stores the actor list, primary camera, component relative transforms, attachment parents, and camera projection settings. Attachment children and world transforms are derived. After package fields are applied, `DLevel::PostLoad` validates ownership and attachment cycles, rebuilds child lists, and recalculates world transforms before the package load is published.

The Level Editor exposes New, Open, and Save using virtual asset paths and the
Asset Registry. Dirty package switches require Save, Discard, or Cancel.
Static-mesh components persist cross-package references to `DStaticMesh`
assets. A static-mesh asset serializes an optional mounted `FSourcePath`, exact
source identity, and build settings; CPU/GPU render data is restored from DDC or
rebuilt from the SourceAssets domain and is never serialized in the authored
package. Version one intentionally supports a single active level and does not
include sub-level streaming, PIE cloning, or Save As.

At startup the editor opens the project's optional `Editor.DefaultLevel`. Projects without a default level start with an empty editor; levels are otherwise opened directly from the Content Browser. Missing or invalid defaults and failed level loads are non-fatal.
