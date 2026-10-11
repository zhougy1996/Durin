#pragma once

#include "CoreMinimal.h"
#include "StaticMesh/StaticMeshBuild.h"
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
#include "DerivedDataBuildSession.h"

namespace Durin::StaticMeshPrivate
{
	using FRenderBuildResult = std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>;
	// Detached inputs only; cancellation is owned across cache callbacks.
	auto DispatchRenderBuild(FStaticMeshBuildRequest& Request, uint64 BuilderVersion,
		DerivedData::FBuildRequestOwner& Owner, std::function<bool()> ShouldCancel, uint64 MaximumWorkingSetBytes,
		DerivedData::FBuildCompletionCallback Completion) -> std::expected<void, FStaticMeshBuildFailure>;
	// CPU consumption runs on a worker after the build callback has retired.
	auto ConsumeRenderBuild(const FStaticMeshBuildSettings& Settings, DerivedData::FBuildCompleteParams Completion,
		const FAssetBuildTaskContext& Control) -> FRenderBuildResult;
}
#endif
