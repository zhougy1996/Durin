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
		// Explicit ready inputs shared by collection and the pass initializer builder.
		struct FPreparedMeshPSOCollectorResources
		{
			const FMeshPassShaders* Shaders = nullptr;
			FRHIVertexDeclaration* Declaration = nullptr;
			EPSOPrecachePriority Priority = EPSOPrecachePriority::Medium;
			std::span<const FMeshPSOPrecacheVariant> Variants;
		};
		enum class EPSOPrecacheCollectionPhase : uint8 { Required, Optional };
		class FMeshPSOCollectionSession final
		{
		public:
			RENDERER_API FMeshPSOCollectionSession(FMeshPassProcessor& Processor, const FMaterialPSOPrecacheParams& Params,
				const FRenderResourceGeneration& Generation);
			RENDERER_API ~FMeshPSOCollectionSession();
			RENDERER_API auto Prepare(EPSOPrecacheCollectionPhase Phase) -> std::expected<EPSOCollectorPreparation, std::string>;
			RENDERER_API auto AppendPrepared(FPSOPrecacheDataArray& Out) -> std::expected<void, std::string>;
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
