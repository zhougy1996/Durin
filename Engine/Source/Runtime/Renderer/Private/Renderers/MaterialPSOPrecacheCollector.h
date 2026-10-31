#pragma once

#include "CoreMinimal.h"
#include "PSOPrecacheMaterial.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"
#include "Renderers/MeshPipelineDescription.h"

namespace Durin
{
	namespace RendererPrivate
	{
		class FMeshPassProcessor;
		struct FMeshPassShaders;
		// Consumption-only scope around the UE-shaped append interface. Ready native
		// resources are borrowed from the session; no async work starts inside it.
		struct FPreparedMeshPSOCollectorResources
		{
			const FMeshPassShaders* Shaders = nullptr;
			FRHIVertexDeclaration* Declaration = nullptr;
			EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;
			std::span<const FMeshPSOPrecacheVariant> Variants;
			std::string Error;
		};
		class FPreparedMeshPSOCollectorScope
		{
		public:
			RENDERER_API explicit FPreparedMeshPSOCollectorScope(FPreparedMeshPSOCollectorResources& Resources);
			RENDERER_API ~FPreparedMeshPSOCollectorScope();
			RENDERER_API static auto Get() -> FPreparedMeshPSOCollectorResources&;
			FPreparedMeshPSOCollectorScope(const FPreparedMeshPSOCollectorScope&) = delete;
			auto operator=(const FPreparedMeshPSOCollectorScope&) -> FPreparedMeshPSOCollectorScope& = delete;
		private:
			FPreparedMeshPSOCollectorResources* Previous;
		};
		enum class EPSOPrecacheCollectionPhase : uint8 { Required, Optional };
		class FMeshPSOCollectionSession final
		{
		public:
			RENDERER_API FMeshPSOCollectionSession(FMeshPassProcessor& Processor, const FMaterialPSOPrecacheParams& Params,
				const FRenderResourceGeneration& Generation);
			RENDERER_API ~FMeshPSOCollectionSession();
			RENDERER_API auto CollectPSOInitializers(EPSOPrecacheCollectionPhase Phase, FPSOPrecacheDataArray& Out)
				-> std::expected<bool, std::string>;
		private:
			struct FState;
			std::unique_ptr<FState> State;
		};
		RENDERER_API auto CollectMaterialPSOs(const FMaterialPSOPrecacheParams& Params,
			FMaterialPSOPrecacheCollection& Collection, const FRenderResourceGeneration& Generation)
			-> std::expected<bool, std::string>;
	}
}
