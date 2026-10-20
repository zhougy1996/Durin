# Durin agent skills

This directory contains distributable skill sources. Copy an entire skill folder
into a client's supported skills directory; copying a skill does not configure
an MCP connection or start the editor.

Engine development uses the repository MCP documentation directly and does not
require an installed copy under the engine's `.agents/skills` directory.

## Install Durin MCP instructions

For external Codex users, run this from the Durin repository root to copy the
skill to the personal skills directory:

```powershell
$ErrorActionPreference = 'Stop'
$destination = Join-Path $HOME '.agents/skills'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath 'Tools/AgentSkills/durin-mcp' -Destination $destination -Recurse -Force
```

Alternatively, copy the `durin-mcp` folder into an external project's
`.agents/skills` directory, or another client's supported skills directory. Users
who receive just the skill folder can copy it directly without an engine checkout.
The resulting layout must be `<skills-directory>/durin-mcp/SKILL.md`.

If Codex does not discover the installed skill, restart it. MCP connection setup
is described in the skill and in the repository's
[local MCP guide](../../Documentation/Editor/Guides/LocalMCP.md).

Maintain the source under `Tools/AgentSkills/durin-mcp` and copy it again after
updates. Keep credentials and machine-specific client configuration out of this
directory. The skill does not depend on relative paths back to the engine tree.

## Startup before a release exists

Durin does not yet provide a release distribution or a stable release launch
procedure. The skill therefore connects to an already enabled editor, or refers
source users to the checkout's build/run and MCP enablement guides. It does not
invent a packaged executable path or launch command. A user without either a
running endpoint or a usable source checkout needs an editor instance from its
operator before connecting.

When a release workflow is implemented, update the skill's startup section from
that release's documented procedure. Keep connection and tool usage independent
of the distribution layout.
