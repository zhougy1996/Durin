"""Create a small scripting test scene in the current writable Level.

Right-click this file in the Content Browser and choose Run Python Script.
Undo/Redo restores the whole example as one history entry. Geometry assets are
not loaded; this example exercises Actor creation, lighting and property edits.
"""

import durin


def create_test_scene():
    actors = durin.get_editor_subsystem(durin.EditorActorSubsystem)
    level = actors.get_editor_level()
    if level is None:
        raise RuntimeError("Open a writable Level and stop Play before running this example.")

    # Internal object names can change after Undo while history retains old objects.
    # A persistent tag identifies this example independently of names and labels.
    marker = durin.Name("Durin.Examples.CreateTestScene")
    if any(marker in actor.get_editor_property("tags")
           for actor in actors.get_all_level_actors(level)):
        raise RuntimeError("The example is already present. Undo its creation or remove its Actors before running again.")

    def spawn(actor_type, name, position=(0.0, 0.0, 0.0)):
        pose = durin.Transform()
        translation = pose.translation
        translation.x, translation.y, translation.z = position
        pose.translation = translation
        actor = actors.spawn_actor(level, actor_type, pose, label=name)
        if actor is None:
            raise RuntimeError(f"Could not create {name}; check Level write access and Play state.")
        actor.set_editor_property("tags", [marker])
        return actor

    def root(actor):
        return actors.get_actor_info(level, actor).root_component

    with durin.ScopedEditorTransaction("Python: Create test scene"):
        spawn(durin.Actor, "Example_Origin")
        camera = spawn(durin.CameraActor, "Example_Camera", (-8.0, 0.0, 3.0))
        sun = spawn(durin.DirectionalLightActor, "Example_Sun", (0.0, 0.0, 6.0))
        spawn(durin.ProceduralSkyActor, "Example_Sky")
        fill = spawn(durin.PointLightActor, "Example_FillLight", (2.0, -2.0, 3.0))
        spawn(durin.PlayerStart, "Example_PlayerStart", (0.0, 0.0, 1.0))

        root(sun).set_editor_property("intensity", 3.0)
        root(fill).set_editor_property("intensity", 8.0)
        root(fill).set_editor_property("range", 12.0)

        # Struct reads are detached copies; assign nested changes back explicitly.
        pose = root(camera).get_editor_property("relative_transform")
        position = pose.translation
        position.z = 4.0
        pose.translation = position
        root(camera).set_editor_property("relative_transform", pose)

    print("Created six Actors with Example_* labels. Ctrl+Z undoes the scene; Ctrl+Y restores it.")


create_test_scene()
