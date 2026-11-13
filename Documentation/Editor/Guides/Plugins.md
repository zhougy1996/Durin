# Change Project Plugins

Summary: Select the next-start plugin set, save it, and restart the editor safely.
Modules: MainFrame, Projects

Last reviewed: 2026-11-13

Open a project and choose **Edit > Plugins...**. Search by name or plugin ID,
then use the independent checkboxes to select the next-start requests. Hover a
row to inspect its running state and build availability. A selected plugin with
missing or incompatible build artifacts shows **Will be skipped on restart**; hover
that status for the specific diagnostic, or copy the window's build diagnostic.
The running editor
keeps its current modules, mounts and defaults until restart.

A checked plugin can require another plugin. Unchecking its prerequisite leaves
the dependent request checked and displays a warning; that dependent and its
consumers stay inactive on the next start. Check the required dependency to use
them. Changing one checkbox never changes the other checkboxes.

Project-required plugins and their dependencies are marked **Required by project**
and cannot be unchecked. Choose **Save** to persist optional requests to the
project's `Configs/Plugins.json`; `.dproject` and the build graph are unchanged. **Cancel** restores the saved choices
without accessing disk. **Refresh** rescans descriptors and reloads saved choices
and build availability, discarding unsaved checkbox changes. Failed saves retain
your choices and show a diagnostic; a read-only or occupied project cannot be
partially overwritten.

After saving, choose **Restart editor...** when available. The normal unsaved-work
prompt still applies. Cancelling that prompt keeps the current editor running;
the saved next-start choices remain. Restart preserves the explicitly selected
project. Missing, incompatible or stale native artifacts cause affected plugins and their
consumers to be skipped at startup, with diagnostics; the Editor stays available.
Build them explicitly to activate them. A failed project-required plugin withholds
project module/content initialization while preserving the Editor. The window
does not build plugins. Compatible enable/disable changes need only a restart.

Source builds compile all discovered target-compatible Engine/project plugins,
regardless of optional startup requests. `.dproject` `PluginDependencies` declares
required project providers. For
descriptor/resource authoring, follow the [plugin contract](../../Workspace/Plugins.md).
Deploying the saved Game selection additionally requires a new Cook and Stage;
see [Game staging](../../Development/Tooling/DurinDevTool.md#game-staging).
