#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include <memory>

namespace Durin
{
	class DObject;

	enum class EMaterialGraphNodeChange : uint8
	{
		None = 0, Added = 1 << 0, Removed = 1 << 1, Content = 1 << 2,
		Interface = 1 << 3, Inputs = 1 << 4, Position = 1 << 5,
	};
	ENUM_CLASS_FLAGS(EMaterialGraphNodeChange);
	enum class EMaterialGraphChange : uint8
	{
		None = 0, Reset = 1 << 0,
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

	// Owning-thread observation. Checkpoints exist only while subscribed, contain
	// exact reflected values, and do not retain graph/resource objects.
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
		ENGINE_API auto BeginBatch() -> void;
		ENGINE_API auto EndBatch(DObject& Owner) -> void;
	private:
		auto Flush(DObject& Owner) -> void;
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	// Called after asset replacement has committed and rewritten external references.
	ENGINE_API auto RefreshMaterialGraphObservers() -> void;
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
