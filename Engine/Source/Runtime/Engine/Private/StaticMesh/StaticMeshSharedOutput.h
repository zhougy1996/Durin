#pragma once
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildFunction.h"
#include "StaticMesh/StaticMeshBuildTypes.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin::StaticMeshPrivate
{
	ENGINE_API auto MakeSharedOutput(FStaticMeshRenderBuildProduct Product, uint32 MaterialSlotCount,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<DerivedData::FBuildOutput, std::string>;
	ENGINE_API auto MakeSharedOutputForBuild(FStaticMeshRenderBuildProduct Product, uint32 MaterialSlotCount,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<DerivedData::FBuildOutput, std::string>;
	ENGINE_API auto ValidateSharedOutput(const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<void, std::string>;
	ENGINE_API auto ValidateSharedOutputWithReceipt(const DerivedData::FBuildOutput& Output,
		uint32 ExpectedMaterialSlotCount, const std::function<bool()>& ShouldCancel = {})
		-> std::expected<std::shared_ptr<const DerivedData::FBuildValidationReceipt>, std::string>;
	ENGINE_API auto AssembleSharedOutput(const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {},
		const DerivedData::FBuildValidationReceipt* Receipt = nullptr)
		-> std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string>;
}
#endif
