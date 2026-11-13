"""Place a default water surface in the current writable Level.

Enable Water in the project's Plugins settings, build its plugin inventory and
restart the editor first. With Play stopped and a writable Level open, right-click
this file in the Content Browser and choose Run Python Script.

The surface is 32 by 32 local units, centered at the origin, with animated waves
and the plugin's default material. Adjust its Water settings in Details.
Ctrl+Z undoes the operation; Ctrl+Y restores it. Save the Level when ready.
"""

import durin


def add_water_surface():
    water_type = getattr(durin, "WaterSurfaceActor", None)
    if water_type is None:
        raise RuntimeError(
            'Water is unavailable. Enable "Plugins": {"Water": true} in the '
            "project's .dproject, build its <Project>PluginBuild target and restart the editor."
        )

    actors = durin.get_editor_subsystem(durin.EditorActorSubsystem)
    level = actors.get_editor_level()
    if level is None:
        raise RuntimeError("Open a writable Level and stop Play before running this example.")

    marker = durin.Name("Durin.Examples.AddWaterSurface")
    if any(marker in actor.get_editor_property("tags")
           for actor in actors.get_all_level_actors(level)):
        raise RuntimeError(
            "The example water surface is already present. Undo its creation or remove it before running again."
        )

    with durin.ScopedEditorTransaction("Python: Add water surface"):
        water = actors.spawn_actor(level, water_type, durin.Transform(), label="Example_WaterSurface")
        if water is None:
            raise RuntimeError("Could not create water; check Level write access and Play state.")
        water.set_editor_property("tags", [marker])

    print("Created Example_WaterSurface at the origin (32 x 32). Ctrl+Z undoes it; Ctrl+Y restores it.")
    return water


add_water_surface()
