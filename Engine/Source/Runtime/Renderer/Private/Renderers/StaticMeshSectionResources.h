#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialRenderTypes.h"
#include "RenderResourceCreation.h"

namespace Durin
{
	class FStaticMeshRenderer;
	struct FMeshDrawDescription;
	struct FResolvedMeshPipeline;

	enum class EStaticMeshSectionResourcePolicy : uint8
	{
		SamplersOnly,
		Forward,
		Shadow
	};

	// Serial render-thread state for one resource-preparation invocation. Owners
	// survive cache-slot replacement; view and instance bindings stay downstream.
	class FStaticMeshSectionResourceContext
	{
	public:
		FStaticMeshSectionResourceContext() = default;
		FStaticMeshSectionResourceContext(const FStaticMeshSectionResourceContext&) = delete;
		auto operator=(const FStaticMeshSectionResourceContext&) -> FStaticMeshSectionResourceContext& = delete;

	private:
		friend class FStaticMeshRenderer;
		struct FKey
		{
			const FMeshDrawDescription* Description = nullptr;
			EStaticMeshSectionResourcePolicy Policy = EStaticMeshSectionResourcePolicy::SamplersOnly;
			auto operator==(const FKey&) const -> bool = default;
		};
		struct FKeyHash
		{
			auto operator()(const FKey& Key) const -> size_t
			{
				const auto Hash = std::hash<const FMeshDrawDescription*>{}(Key.Description);
				return Hash ^ (static_cast<size_t>(Key.Policy) + size_t{0x9e3779b9} + (Hash << 6) + (Hash >> 2));
			}
		};
		struct FEntry
		{
			// Description owns GeometryOwner, which retains the immutable binding/declaration.
			std::shared_ptr<const FMeshDrawDescription> Description;
			FMaterialRenderBinding Material;
			std::shared_ptr<const FResolvedMeshPipeline> Pipeline;
		};
		std::unordered_map<FKey, FEntry, FKeyHash> Entries;
		const FStaticMeshRenderer* Renderer = nullptr;
		FRenderResourceGeneration Generation;
		uint64 ReleaseEpoch = 0;
		uint64 SurfaceRevision = 0;

		auto Synchronize(const FStaticMeshRenderer* InRenderer, const FRenderResourceGeneration& InGeneration,
			uint64 InReleaseEpoch, uint64 InSurfaceRevision) -> void
		{
			if (Renderer == InRenderer && Generation == InGeneration && ReleaseEpoch == InReleaseEpoch
				&& SurfaceRevision == InSurfaceRevision) return;
			Entries.clear();
			Renderer = InRenderer;
			Generation = InGeneration;
			ReleaseEpoch = InReleaseEpoch;
			SurfaceRevision = InSurfaceRevision;
		}
	};
}
