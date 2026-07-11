# Level System

`DLevel` is the persistent scene asset. A packaged level is the main asset of a `.dasset` package and directly owns its actors; actors own their components through the ordinary Outer/Inner graph. A transient startup level may instead be owned directly by a `DWorld` and is not saveable.

`DWorld` is a runtime or editor session container. It activates at most one level, forwards actor APIs to that level, and registers or unregisters the level's components when switching. Package lifetime remains owned by `AssetCore`; switching a world does not implicitly destroy a persistent level package.

Scene persistence stores the actor list, primary camera, component relative transforms, attachment parents, and camera projection settings. Attachment children and world transforms are derived. After package fields are applied, `DLevel::PostLoad` validates ownership and attachment cycles, rebuilds child lists, and recalculates world transforms before the package load is published.

The Level Editor exposes New, Open, and Save using virtual asset paths and the Asset Registry. Dirty package switches require Save, Discard, or Cancel. Static-mesh components persist cross-package references to `DStaticMesh` assets. A static-mesh asset serializes its project-relative source path and build settings; CPU/GPU render data is rebuilt from the copied Content source after load and is never serialized. Version one intentionally supports a single active level and does not include sub-level streaming, PIE cloning, or Save As.
