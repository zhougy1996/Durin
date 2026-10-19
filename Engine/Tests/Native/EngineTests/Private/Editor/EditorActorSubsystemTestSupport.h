#pragma once

#include "CoreMinimal.h"
#include "Actors/EditorActorSubsystem.h"
#include "DObject/StrongObjectPtr.h"
#include "Editor/EditorEngine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "NativeDObjectTestSupport.h"

namespace Durin
{
	struct FEditorEngineTestAccess
	{
		static auto InitializeActorEditor(DEditorEngine& Editor, DWorld& World) -> FSubsystemResult
		{
			Editor.EditorWorld = &World;
			Editor.SetWorld(&World);
			return Editor.ActivateEditor() ? Editor.InitializeEditorSubsystems() : FSubsystemResult{ESubsystemError::InvalidState, "Editor activation failed."};
		}
	};
}
namespace Durin::Tests
{
	// Real editor-owned services without application, renderer or window startup.
	class FActorEditorFixture
	{
	public:
		FActorEditorFixture()
		{
			Testing::InitializeDObjectSystemForTests();
			Editor = NewObject<DEditorEngine>(nullptr, "ActorTestEditor");
			World = NewObject<DWorld>(Editor.Get(), "ActorTestWorld");
			if (!World->SetWorldType(EWorldType::Editor) || !World->InitializeSubsystems()
				|| !World->SetCurrentLevel(NewObject<DLevel>(World.Get(), "ActorTestLevel"))
				|| !FEditorEngineTestAccess::InitializeActorEditor(*Editor, *World))
				throw std::runtime_error("Actor editor fixture initialization failed.");
		}
		~FActorEditorFixture()
		{
			Shutdown();
			MarkObjectHierarchyAsGarbage(Editor.Get());
			World = nullptr;
			Editor = nullptr;
			CollectGarbage();
		}
		auto Shutdown() -> void { Editor->PrepareForShutdown(); }
		auto GetEditor() const -> DEditorEngine* { return Editor.Get(); }
		auto GetWorld() const -> DWorld* { return World.Get(); }
		auto GetActorEditor() const -> DEditorActorSubsystem* { return Editor->GetEditorSubsystem<DEditorActorSubsystem>(); }
		auto GetTransactions() const -> DTransBuffer* { return Cast<DTransBuffer>(Editor->GetTransactor()); }
	private:
		TStrongObjectPtr<DEditorEngine> Editor;
		TStrongObjectPtr<DWorld> World;
	};
}
