#pragma once

#include "CoreMinimal.h"
#include "Console/ConsoleCommand.h"
#include "Tools/EditorToolSubsystem.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin::Editor::MainFrame
{
	// GameThread adapter. Legacy text stays in its parser; tool text uses schema-directed conversion.
	class FConsoleToolAdapter final
	{
	public:
		explicit FConsoleToolAdapter(DEditorEngine& Editor, FConsoleCommandRegistry& Legacy = FConsoleCommandRegistry::Get());
		~FConsoleToolAdapter();
		FConsoleToolAdapter(const FConsoleToolAdapter&) = delete;
		auto operator=(const FConsoleToolAdapter&) -> FConsoleToolAdapter& = delete;
		auto Execute(std::string_view Line) -> FConsoleCommandResult;
		auto FindCompletions(std::string_view Prefix) -> std::vector<std::string>;
		auto Poll() -> std::vector<FConsoleCommandResult>;
	private:
		struct FEntry
		{
			std::string Name, Description, Usage;
			std::optional<FToolAddress> Address;
			bool Collision = false;
		};
		auto Entries() -> std::vector<FEntry>;
		auto ExecuteInternal(std::string_view Line) -> FConsoleCommandResult;
		auto ResolveSubsystem() const -> DEditorToolSubsystem*;
		TWeakObjectPtr<DEditorEngine> Host;
		FConsoleCommandRegistry& Legacy;
		std::shared_ptr<FEditorToolService> Endpoint;
		std::vector<FEditorToolInvocationId> Pending;
	};
}
