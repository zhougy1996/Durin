#pragma once

#include "CoreMinimal.h"
#include "StaticMesh/StaticMeshBuild.h"
#if DURIN_WITH_EDITOR && DURIN_WITH_EDITORONLY_DATA
#include "DerivedDataBuildSession.h"
#include "DerivedDataBuildFunction.h"

namespace Durin::StaticMeshPrivate
{
	ENGINE_API auto RegisterBuildFunction() -> void;
	ENGINE_API auto MakeRenderBuildFunction() -> std::shared_ptr<const DerivedData::IBuildFunction>;
	ENGINE_API auto MakeRenderInputResolver(const FStaticMeshBuildRequest& Request) -> std::shared_ptr<const DerivedData::IBuildInputResolver>;
	auto OpenBuildSessions() -> void;
	auto ShutdownBuildSessions() -> void;
	using FRenderBuildResult = std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string>;
	// Detached inputs only; cancellation is owned across cache callbacks.
	auto DispatchRenderBuild(FStaticMeshBuildRequest& Request, uint64 BuilderVersion,
		DerivedData::IRequestOwner& Owner, FCancellationToken Cancellation, uint64 MaximumWorkingSetBytes,
		DerivedData::FBuildCompletionCallback Completion) -> std::expected<void, std::string>;
	// CPU consumption runs on a worker after the build callback has retired.
	auto ConsumeRenderBuild(const FStaticMeshBuildSettings& Settings, DerivedData::FBuildCompleteParams Completion,
		const FAssetBuildTaskContext& Control) -> FRenderBuildResult;
}
#endif
