#include "TickableEditorObject.h"
#include "Editor/EditorEngine.h"
#include "Threading/RunnableThread.h"
namespace Durin
{
	namespace
	{
		struct FEntry { FTickableEditorObject* Object; };
		auto Entries() -> std::vector<std::shared_ptr<FEntry>>& { static auto* Value = new std::vector<std::shared_ptr<FEntry>>; return *Value; }
	}
	FTickableEditorObject::FTickableEditorObject() { Entries().push_back(std::make_shared<FEntry>(this)); }
	FTickableEditorObject::~FTickableEditorObject()
	{
		for (auto& Entry : Entries()) if (Entry->Object == this) Entry->Object = nullptr;
		std::erase_if(Entries(), [](const auto& Entry) { return !Entry->Object; });
	}
	auto FTickableEditorObject::TickObjects(DEditorEngine& Editor, float DeltaSeconds) -> void
	{
		CheckGameThread();
		static bool Dispatching = false;
		requiref(!Dispatching, "Editor tickable dispatch cannot be nested.");
		Dispatching = true;
		struct FGuard { bool& Value; ~FGuard() { Value = false; } } Guard{Dispatching};
		const auto Snapshot = Entries();
		for (const auto& Entry : Snapshot)
		{
			if (Editor.IsShutdownRequested()) break;
			auto* Object = Entry->Object;
			if (!Object || Object->GetTickableEditor() != &Editor || Entry->Object != Object) continue;
			if (!Object->IsAllowedToTick() || Entry->Object != Object) continue;
			const auto Type = Object->GetTickableTickType();
			if (Entry->Object != Object || Type == ETickableTickType::Never) continue;
			if (Type != ETickableTickType::Always && (!Object->IsTickable() || Entry->Object != Object)) continue;
			Object->Tick(DeltaSeconds);
		}
	}
}
