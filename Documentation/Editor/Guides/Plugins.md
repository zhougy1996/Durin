# Change Project Plugins

Summary: Select the next-start plugin set, save it, and restart the editor safely.
Modules: MainFrame, Projects

Last reviewed: 2026-11-13

Open a project and choose **Edit > Plugins...**. Search by name or plugin ID,
then use the independent checkboxes to select the next-start requests. Hover a
row to inspect its running state and build availability. The running editor
keeps its current modules, mounts and defaults until restart.

A checked plugin can require another plugin. Unchecking its prerequisite leaves
the dependent request checked and displays a warning; that dependent and its
consumers stay inactive on the next start. Check the required dependency to use
them. Changing one checkbox never changes the other checkboxes.

Choose **Save** to persist valid requests. **Cancel** restores the saved choices
without accessing disk. **Refresh** rescans descriptors and reloads saved choices
and build availability, discarding unsaved checkbox changes. Failed saves retain
your choices and show a diagnostic; a read-only or occupied project cannot be
partially overwritten.

After saving, choose **Restart editor...** when available. The normal unsaved-work
prompt still applies. Cancelling that prompt keeps the current editor running;
the saved next-start choices remain. Restart preserves the explicitly selected
project. Missing, incompatible or stale native artifacts require an explicit
build first; the window does not build them. Compatible enable/disable changes
need only a restart.

Source builds compile all eligible discovered plugins even while disabled. For
descriptor/resource authoring, follow the [plugin contract](../../Workspace/Plugins.md).
Deploying the saved Game selection additionally requires a new Cook and Stage;
see [Game staging](../../Development/Tooling/DurinDevTool.md#game-staging).
