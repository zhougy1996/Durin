#pragma once

#include "LevelEditorAPI.h"

namespace Durin
{
	class DLevel;
	class DSkyBoxComponent;
}

namespace Durin::Editor::Level
{
	class IObjectDetailsCustomization;

	// Describes one visible registered skybox and whether scene ordering selects it.
	struct FSkyBoxConflictEntry
	{
		DSkyBoxComponent* Component = nullptr;
		std::string ActorName;
		std::string ObjectPath;
		bool bActive = false;
	};

	// Mirrors scene skybox ordering for nonblocking editor conflict diagnostics.
	class FSkyBoxConflictModel
	{
	public:
		LEVELEDITOR_API explicit FSkyBoxConflictModel(DLevel* Level);

		auto HasConflict() const -> bool { return Entries.size() > 1; }
		auto GetEntries() const -> std::span<const FSkyBoxConflictEntry> { return Entries; }
		auto GetActive() const -> const FSkyBoxConflictEntry* { return Entries.empty() ? nullptr : &Entries.front(); }

	private:
		std::vector<FSkyBoxConflictEntry> Entries;
	};

	LEVELEDITOR_API auto CreateSkyBoxDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
}
