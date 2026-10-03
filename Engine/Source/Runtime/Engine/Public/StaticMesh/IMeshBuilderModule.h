#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMeshBuildTypes.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	// Fixed Developer module contract; implementations return detached CPU data only.
	class IMeshBuilderModule : public IModuleInterface
	{
	public:
		ENGINE_API auto StartupModule() -> void override;
		// Borrow the active implementation. Consumers drain work before editor shutdown.
		ENGINE_API static auto Get() -> IMeshBuilderModule*;
		// The builder version is immutable for the editor lifetime.
		virtual auto GetRenderBuilderVersion() const -> uint32 = 0;
		virtual auto BuildRender(const FStaticMeshRenderBuildRequest& Request,
			const FAssetBuildTaskContext& Control = {})
			-> std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshRenderBuildError> = 0;
	};
}

#endif
