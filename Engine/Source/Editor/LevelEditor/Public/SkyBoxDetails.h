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

	// Describes one visible registered skybox in an invalid multi-skybox level.
	struct FSkyBoxConflictEntry
	{
		DSkyBoxComponent* Component = nullptr;
		std::string ActorName;
		std::string ObjectPath;
	};

	// Reports an invalid multi-skybox level without assigning an active candidate.
	class FSkyBoxConflictModel
	{
	public:
		LEVELEDITOR_API explicit FSkyBoxConflictModel(DLevel* Level);

		auto HasConflict() const -> bool { return Entries.size() > 1; }
		auto GetEntries() const -> std::span<const FSkyBoxConflictEntry> { return Entries; }
	private:
		std::vector<FSkyBoxConflictEntry> Entries;
	};

	LEVELEDITOR_API auto CreateSkyBoxDetailsCustomization() -> std::shared_ptr<IObjectDetailsCustomization>;
}
