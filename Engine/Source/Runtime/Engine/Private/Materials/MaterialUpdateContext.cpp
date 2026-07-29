#include "Materials/MaterialUpdateContext.h"

#include "Components/StaticMeshComponent.h"
#include "CoreGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/ObjectLifecycle.h"
#include "Logging/LogMacros.h"
#include "Materials/MaterialInterface.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
	namespace
	{
		bool GIsFlushingMaterialUpdates = false;
		FMaterialUpdateCounters GLastMaterialUpdateCounters;

		auto HandlesEqual(FObjectHandle Left, FObjectHandle Right) -> bool
		{
			return Left.Index == Right.Index && Left.Generation == Right.Generation;
		}

		auto HandleLess(FObjectHandle Left, FObjectHandle Right) -> bool
		{
			return Left.Index < Right.Index
				|| (Left.Index == Right.Index && Left.Generation < Right.Generation);
		}

		auto CheckMaterialUpdateThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		struct FScopedMaterialUpdateFlush
		{
			FScopedMaterialUpdateFlush()
			{
				checkf(!GIsFlushingMaterialUpdates, "Material update context flush cannot be re-entered.");
				GIsFlushingMaterialUpdates = true;
			}

			~FScopedMaterialUpdateFlush()
			{
				GIsFlushingMaterialUpdates = false;
			}
		};

		struct FAffectedMaterial
		{
			FObjectHandle Handle;
			EMaterialRenderDirtyFlags DirtyFlags = EMaterialRenderDirtyFlags::None;
		};
	}

	FMaterialUpdateContext::FMaterialUpdateContext() = default;

	auto GetLastMaterialUpdateCounters() -> FMaterialUpdateCounters
	{
		CheckMaterialUpdateThread();
		return GLastMaterialUpdateCounters;
	}

	FMaterialUpdateContext::~FMaterialUpdateContext()
	{
		checkf(ChangedRoots.empty(), "FMaterialUpdateContext must be flushed before destruction.");
	}

	auto FMaterialUpdateContext::AddMaterial(
		DMaterialInterface* Material,
		EMaterialRenderDirtyFlags DirtyFlags
	) -> void
	{
		CheckMaterialUpdateThread();
		if (DirtyFlags == EMaterialRenderDirtyFlags::None) return;
		if (!IsValid(Material))
		{
			DURIN_DEBUG_CATEGORY("Materials", "Ignoring an invalid material update root.");
			return;
		}

		const FObjectHandle Handle = MakeObjectHandle(Material);
		if (IsObjectHandleNull(Handle))
		{
			DURIN_DEBUG_CATEGORY("Materials", "Ignoring a material update root without a live object handle.");
			return;
		}

		const auto It = std::ranges::find_if(ChangedRoots, [Handle](const FChangedRoot& Root) {
			return HandlesEqual(Root.Handle, Handle);
		});
		if (It != ChangedRoots.end())
		{
			It->DirtyFlags |= DirtyFlags;
			return;
		}
		ChangedRoots.push_back({.Handle = Handle, .DirtyFlags = DirtyFlags});
	}

	auto FMaterialUpdateContext::Flush() -> void
	{
		CheckMaterialUpdateThread();
		if (ChangedRoots.empty()) return;
		FScopedMaterialUpdateFlush FlushScope;

		std::ranges::sort(ChangedRoots, [](const FChangedRoot& Left, const FChangedRoot& Right) {
			return HandleLess(Left.Handle, Right.Handle);
		});
		std::erase_if(ChangedRoots, [](const FChangedRoot& Root) {
			auto* Material = Cast<DMaterialInterface>(ResolveObjectHandle(Root.Handle));
			if (IsValid(Material)) return false;
			DURIN_DEBUG_CATEGORY("Materials", "Ignoring a stale material update root during flush.");
			return true;
		});

		Counters = {};
		Counters.RootCount = static_cast<uint64>(ChangedRoots.size());
		if (ChangedRoots.empty()) return;

		const std::vector<DObject*> Objects = GDObjectArray.Snapshot();
		Counters.ObjectSnapshotCount = 1;
		Counters.ScannedObjectCount = static_cast<uint64>(Objects.size());

		std::vector<FObjectHandle> MaterialHandles;
		std::vector<FObjectHandle> ComponentHandles;
		for (DObject* Object : Objects)
		{
			if (!IsValid(Object)) continue;
			const FObjectHandle Handle = MakeObjectHandle(Object);
			if (IsObjectHandleNull(Handle)) continue;
			if (Cast<DMaterialInterface>(Object)) MaterialHandles.push_back(Handle);
			else if (Cast<DStaticMeshComponent>(Object)) ComponentHandles.push_back(Handle);
		}
		std::ranges::sort(MaterialHandles, HandleLess);
		std::ranges::sort(ComponentHandles, HandleLess);

		std::vector<FAffectedMaterial> AffectedMaterials;
		for (FObjectHandle MaterialHandle : MaterialHandles)
		{
			auto* Material = Cast<DMaterialInterface>(ResolveObjectHandle(MaterialHandle));
			if (!IsValid(Material)) continue;
			++Counters.TestedMaterialCount;

			EMaterialRenderDirtyFlags MergedFlags = EMaterialRenderDirtyFlags::None;
			for (const FChangedRoot& Root : ChangedRoots)
			{
				auto* RootMaterial = Cast<DMaterialInterface>(ResolveObjectHandle(Root.Handle));
				if (!IsValid(RootMaterial) || !Material->IsDependent(RootMaterial)) continue;
				MergedFlags |= Root.DirtyFlags;
				if (Material != RootMaterial) MergedFlags |= EMaterialRenderDirtyFlags::ParentChain;
			}
			if (MergedFlags != EMaterialRenderDirtyFlags::None)
			{
				AffectedMaterials.push_back({.Handle = MaterialHandle, .DirtyFlags = MergedFlags});
			}
		}

		Counters.AffectedMaterialCount = static_cast<uint64>(AffectedMaterials.size());
		for (const FAffectedMaterial& Affected : AffectedMaterials)
		{
			auto* Material = Cast<DMaterialInterface>(ResolveObjectHandle(Affected.Handle));
			if (IsValid(Material)) ++Material->RenderStateVersion;
		}

		for (FObjectHandle ComponentHandle : ComponentHandles)
		{
			auto* Component = Cast<DStaticMeshComponent>(ResolveObjectHandle(ComponentHandle));
			if (!IsValid(Component)) continue;
			++Counters.ScannedComponentCount;

			const uint32 SlotCount = Component->GetNumMaterials();
			for (uint32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
			{
				DMaterialInterface* SlotMaterial = Component->GetMaterial(SlotIndex);
				if (!IsValid(SlotMaterial)) continue;
				const FObjectHandle SlotMaterialHandle = MakeObjectHandle(SlotMaterial);
				const auto It = std::ranges::find_if(
					AffectedMaterials,
					[SlotMaterialHandle](const FAffectedMaterial& Affected) {
						return HandlesEqual(Affected.Handle, SlotMaterialHandle);
					});
				if (It == AffectedMaterials.end()) continue;
				// Preserve the legacy matched-slot counter until scan-oriented
				// counters retire; content reaches the slot through its retained proxy.
				++Counters.UpdatedSlotCount;
			}
		}

		ChangedRoots.clear();
		GLastMaterialUpdateCounters = Counters;
	}
}
