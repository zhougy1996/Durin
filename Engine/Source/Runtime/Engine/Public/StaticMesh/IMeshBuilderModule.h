#pragma once

#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMeshBuildTypes.h"

namespace Durin
{
	// Fixed Developer module contract; implementations return detached CPU data only.
	class IMeshBuilderModule : public IModuleInterface
	{
	public:
		// Borrow the active implementation. Consumers drain work before editor shutdown.
		ENGINE_API static auto Get() -> IMeshBuilderModule*;
		// The builder version is immutable for the editor lifetime.
		virtual auto GetRenderBuilderVersion() const -> uint32 = 0;
		virtual auto BuildRender(const FStaticMeshRenderBuildRequest& Request,
			const FAssetBuildTaskContext& Control = {})
			-> std::expected<FStaticMeshRenderBuildProduct, FStaticMeshRenderBuildError> = 0;
	};
}
