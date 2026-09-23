#pragma once

#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMeshBuildTypes.h"

namespace Durin
{
	// Fixed Developer module contract; implementations return detached CPU data only.
	class IMeshBuilderModule : public IModuleInterface
	{
	public:
		// The builder version is immutable for this module generation.
		virtual auto GetRenderBuilderVersion() const -> uint32 = 0;
		virtual auto BuildRender(const FStaticMeshRenderBuildRequest& Request,
			const FAssetBuildTaskContext& Control = {})
			-> std::expected<FStaticMeshRenderBuildProduct, FStaticMeshRenderBuildError> = 0;
	};

	// Acquire on the module-control thread, then copy into worker-owned state.
	// Shutdown/unload is rejected until all sessions have been released. Consumers
	// must stop admission and drain their work before shutting down the module.
	class FStaticMeshBuildSession
	{
	public:
		ENGINE_API static auto Acquire() -> FStaticMeshBuildSession;
		explicit operator bool() const { return Module != nullptr && CodeLease != nullptr; }
		auto GetModule() const -> IMeshBuilderModule& { return *Module; }
		auto GetGeneration() const -> uint64 { return Generation; }

	private:
		std::shared_ptr<void> CodeLease;
		IMeshBuilderModule* Module = nullptr;
		uint64 Generation = 0;
	};
}
