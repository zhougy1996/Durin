#pragma once

#include "LevelEditorAPI.h"
#include "LevelEditorTransformTargets.h"
#include "SceneView.h"
#include "Modules/ModularFeature.h"

namespace Durin::Editor
{
	class FTransactionManager;
}

namespace Durin::Editor::Level
{
	class FLevelEditorViewportClient;
	struct FLevelEditorContext;
	struct FLevelEditorViewportInput;

	class ILevelViewportEditMode
	{
	public:
		virtual ~ILevelViewportEditMode() = default;
		virtual auto Enter(FLevelEditorContext&) -> void {}
		virtual auto Exit(FLevelEditorContext&, bool) -> void {}
		virtual auto Tick(FLevelEditorContext&, FLevelEditorViewportClient&, const FSceneView&, FLevelEditorViewportInput&, ::Durin::Editor::FTransactionManager*) -> bool = 0;
		virtual auto GetGizmoTargets(const FLevelEditorContext&) const -> FTransformGizmoTargetSet { return {}; }
		// Defers self-requested mode switches until Tick has returned to the manager.
		virtual auto ShouldExit() const -> bool { return false; }
	};

	struct FLevelViewportEditModeDescriptor
	{
		std::string Id;
		std::string DisplayName;
		int32 Priority = 0;
		std::function<bool(const FLevelEditorContext&)> CanActivate;
		std::function<std::unique_ptr<ILevelViewportEditMode>()> Factory;
	};

	struct FLevelViewportEditModeHandle
	{
		uint64 Id = 0;
		explicit operator bool() const { return Id != 0; }
	};

	// Stores immutable mode descriptors; live mode state belongs to a workspace manager.
	class FLevelViewportEditModeRegistry
	{
	public:
		LEVELEDITOR_API static auto Get() -> FLevelViewportEditModeRegistry&;
		LEVELEDITOR_API auto Register(
			FLevelViewportEditModeDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate = {}) -> FLevelViewportEditModeHandle;
		LEVELEDITOR_API auto Unregister(FLevelViewportEditModeHandle Handle) -> bool;
		LEVELEDITOR_API auto Find(std::string_view Id) const -> const FLevelViewportEditModeDescriptor*;
		LEVELEDITOR_API auto GetAvailable(const FLevelEditorContext& Context) const -> std::vector<const FLevelViewportEditModeDescriptor*>;

	private:
		uint64 NextHandleId = 1;
		struct FEntry
		{
			uint64 HandleId = 0;
			FModuleOwnedResourceLease OwnerResource;
			FModuleOwnedCallbackGate OwnerGate;
			FLevelViewportEditModeDescriptor Descriptor;
		};
		auto FindEntry(std::string_view Id) const -> const FEntry*;
		std::vector<FEntry> Entries;
		friend class FLevelViewportEditModeManager;
	};

	class FLevelViewportEditModeManager
	{
	public:
		LEVELEDITOR_API FLevelViewportEditModeManager();
		LEVELEDITOR_API ~FLevelViewportEditModeManager();
		LEVELEDITOR_API auto Activate(std::string_view Id, FLevelEditorContext& Context) -> bool;
		LEVELEDITOR_API auto Synchronize(FLevelEditorContext& Context) -> void;
		LEVELEDITOR_API auto Tick(FLevelEditorContext& Context, FLevelEditorViewportClient& Client, const FSceneView& View,
			FLevelEditorViewportInput& Input, ::Durin::Editor::FTransactionManager* Transactions) -> bool;
		LEVELEDITOR_API auto Shutdown(FLevelEditorContext* Context = nullptr) -> void;
		auto GetActiveModeId() const -> std::string_view { return ActiveId; }
		auto GetActiveMode() const -> ILevelViewportEditMode* { return ActiveMode.get(); }

	private:
		std::string ActiveId;
		FModuleOwnedResourceLease ActiveOwnerResource;
		FModuleOwnedCallbackGate ActiveOwnerGate;
		std::unique_ptr<ILevelViewportEditMode> ActiveMode;
		FLevelEditorContext* LastContext = nullptr;
	};
} // namespace Durin::Editor::Level
