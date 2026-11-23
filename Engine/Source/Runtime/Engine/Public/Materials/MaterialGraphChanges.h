#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "EngineAPI.h"

namespace Durin
{
	class DObject;

	enum class EMaterialGraphNodeChange : uint8
	{
		None = 0, Added = 1 << 0, Removed = 1 << 1, Content = 1 << 2,
		Interface = 1 << 3, Inputs = 1 << 4, Position = 1 << 5, ParameterValue = 1 << 6,
	};
	ENUM_CLASS_FLAGS(EMaterialGraphNodeChange);
	enum class EMaterialGraphChange : uint8
	{
		None = 0, Reset = 1 << 0, RootInputs = 1 << 1, RootPosition = 1 << 2, ParameterDefinitions = 1 << 3,
	};
	ENUM_CLASS_FLAGS(EMaterialGraphChange);
	struct FMaterialGraphNodeChange
	{
		FGuid NodeId;
		EMaterialGraphNodeChange Flags = EMaterialGraphNodeChange::None;
	};

	// Invalidation hints for rereading current owner data, not an undo log.
	// Removed then Added is a replacement; Added then Removed cancels within a batch.
	struct FMaterialGraphChangeSet
	{
		std::vector<FMaterialGraphNodeChange> Nodes;
		EMaterialGraphChange Flags = EMaterialGraphChange::None;
		ENGINE_API auto MarkNode(FGuid Id, EMaterialGraphNodeChange Change) -> void;
		ENGINE_API auto MarkGraph(EMaterialGraphChange Change) -> void;
		ENGINE_API auto Merge(const FMaterialGraphChangeSet& Other) -> void;
		auto IsEmpty() const -> bool { return Nodes.empty() && Flags == EMaterialGraphChange::None; }
		auto Has(EMaterialGraphChange Change) const -> bool { return (Flags & Change) != EMaterialGraphChange::None; }
	};

	// Called after asset replacement has committed and rewritten external references.
	ENGINE_API auto RefreshMaterialGraphObservers() -> void;

	// Owning-thread observation. Comparison data exists only while subscribed, contains
	// exact reflected values, and does not retain graph or resource objects.
	class FMaterialGraphChangeSource
	{
	public:
		ENGINE_API FMaterialGraphChangeSource();
		ENGINE_API ~FMaterialGraphChangeSource();
		FMaterialGraphChangeSource(const FMaterialGraphChangeSource&) = delete;
		auto operator=(const FMaterialGraphChangeSource&) -> FMaterialGraphChangeSource& = delete;
		ENGINE_API auto Subscribe(DObject& Owner, std::function<void(const FMaterialGraphChangeSet&)> Callback) -> FDelegateHandle;
		ENGINE_API auto Unsubscribe(FDelegateHandle Handle) -> void;
		// Called only after successful owner publication; no-op publications are silent.
		ENGINE_API auto Publish(DObject& Owner) -> void;
		ENGINE_API auto PublishPresentation(DObject& Owner) -> void;
		// Starts tracking authored publications even without a graph observer.
		ENGINE_API auto GetEditRevision() -> uint64;
		ENGINE_API auto BeginBatch() -> void;
		ENGINE_API auto EndBatch(DObject& Owner) -> void;
	private:
		friend ENGINE_API auto RefreshMaterialGraphObservers() -> void;
		auto Refresh(DObject& Owner) -> void;
		auto Flush(DObject& Owner) -> void;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	ENGINE_API auto GetMaterialGraphChangeSource(DObject& Owner) -> FMaterialGraphChangeSource&;
	class FScopedMaterialGraphChange
	{
	public:
		ENGINE_API explicit FScopedMaterialGraphChange(DObject& Owner);
		ENGINE_API ~FScopedMaterialGraphChange();
		FScopedMaterialGraphChange(const FScopedMaterialGraphChange&) = delete;
		auto operator=(const FScopedMaterialGraphChange&) -> FScopedMaterialGraphChange& = delete;
	private:
		DObject& Owner;
	};
}

#endif
