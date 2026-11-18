#include "LevelEditorViewportEditing.h"

namespace Durin::LevelEditor
{
	auto FLevelViewportEditModeRegistry::Get() -> FLevelViewportEditModeRegistry&
	{
		static FLevelViewportEditModeRegistry Registry;
		return Registry;
	}

	auto FLevelViewportEditModeRegistry::Register(
		FLevelViewportEditModeDescriptor Descriptor) -> FLevelViewportEditModeHandle
	{
		if (Descriptor.Id.empty() || Descriptor.DisplayName.empty() || !Descriptor.Factory || Find(Descriptor.Id)) return {};
		const uint64 Id = NextHandleId++;
		Entries.push_back({Id, std::move(Descriptor)});
		return {Id};
	}

	auto FLevelViewportEditModeRegistry::Unregister(FLevelViewportEditModeHandle Handle) -> bool
	{
		const auto It = std::ranges::find_if(Entries, [Handle](const FEntry& Entry) { return Entry.HandleId == Handle.Id; });
		if (It == Entries.end()) return false;
		Entries.erase(It);
		return true;
	}

	auto FLevelViewportEditModeRegistry::Find(std::string_view Id) const -> const FLevelViewportEditModeDescriptor*
	{
		const auto It = std::ranges::find_if(Entries,
			[Id](const FEntry& Entry) { return Entry.Descriptor.Id == Id; });
		return It == Entries.end() ? nullptr : &It->Descriptor;
	}

	auto FLevelViewportEditModeRegistry::GetAvailable(const FLevelEditorContext& Context) const -> std::vector<const FLevelViewportEditModeDescriptor*>
	{
		std::vector<const FLevelViewportEditModeDescriptor*> Result;
		for (const FEntry& Entry : Entries)
		{
			if (!Entry.Descriptor.CanActivate || Entry.Descriptor.CanActivate(Context))
				Result.push_back(&Entry.Descriptor);
		}
		std::ranges::sort(Result, [](const auto* A, const auto* B) { return A->Priority > B->Priority; });
		return Result;
	}

	FEditorModeTools::FEditorModeTools()
	{
	}

	FEditorModeTools::~FEditorModeTools()
	{
		try { DeactivateAllModes(); }
		catch (const std::exception& Error) { DURIN_ERROR("Editor mode shutdown failed: {}", Error.what()); }
	}

	auto FEditorModeTools::Activate(std::string_view Id, FLevelEditorContext& Context) -> bool
	{
		const FLevelViewportEditModeDescriptor* Descriptor =
			FLevelViewportEditModeRegistry::Get().Find(Id);
		if (!Descriptor) return false;
		if (Descriptor->CanActivate && !Descriptor->CanActivate(Context)) return false;
		if (ActiveId == Id && ActiveMode) return true;
		if (ActiveMode)
		{
			ActiveMode->Exit(Context, false);
		}
		ActiveMode = Descriptor->Factory();
		if (!ActiveMode) { ActiveId.clear(); return false; }
		ActiveId = Descriptor->Id;
		LastContext = &Context;
		ActiveMode->Enter(Context);
		return true;
	}

	auto FEditorModeTools::Synchronize(FLevelEditorContext& Context) -> void
	{
		LastContext = &Context;
		const FLevelViewportEditModeDescriptor* Active =
			FLevelViewportEditModeRegistry::Get().Find(ActiveId);

		if (ActiveMode && (!Active
			|| (Active->CanActivate && !Active->CanActivate(Context))))
		{
			ActiveMode->Exit(Context, true);
			ActiveMode.reset();
			ActiveId.clear();
		}
		if (!ActiveMode) Activate("Select", Context);
	}

	auto FEditorModeTools::Tick(FLevelEditorContext& Context, FLevelEditorViewportClient& Client, const FSceneView& View,
		FLevelEditorViewportInput& Input, DTransactor* Transactions) -> bool
	{
		Synchronize(Context);
		if (!ActiveMode) return false;
		const bool bHandled = ActiveMode->Tick(Context, Client, View, Input, Transactions);
		if (ActiveMode && ActiveMode->ShouldExit()) Activate("Select", Context);
		return bHandled;
	}

	auto FEditorModeTools::DeactivateAllModes(FLevelEditorContext* Context) -> void
	{
		if (ActiveMode && (Context || LastContext)) ActiveMode->Exit(Context ? *Context : *LastContext, true);
		ActiveMode.reset();
		ActiveId.clear();
		LastContext = nullptr;
	}
} // namespace Durin::LevelEditor
