#pragma once

#include "AssetBuild/BuildFunction.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

namespace Durin::Asset::Build::Private
{
	extern const FBuildFunctionIdentity StaticMeshFunctionIdentity;
	extern const FBuildFunctionIdentity StaticMeshCollisionFunctionIdentity;
	inline constexpr std::string_view StaticMeshInputName = "StaticMeshBuildInput";
	inline constexpr std::string_view StaticMeshValueName = "StaticMeshPayload";
	inline constexpr std::string_view CollisionInputName = "StaticMeshCollisionBuildInput";
	inline constexpr std::string_view CollisionValueName = "StaticMeshCollisionPayload";

	auto BuildCollisionGeometryHash(
		std::span<const FVector3f> Positions,
		std::span<const uint32> Indices) -> FXxHash128;
	auto EncodeStaticMeshRenderData(
		const FStaticMeshRenderData& RenderData, FBuildValue& OutValue,
		std::string& OutError) -> bool;
	auto DecodeStaticMeshRenderData(
		const FBuildValue& Value,
		std::unique_ptr<FStaticMeshRenderData>& OutRenderData,
		std::string& OutError) -> bool;
	auto EncodeStaticMeshCollisionInput(
		std::span<const FVector3f> Positions,
		std::span<const uint32> Indices,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy) -> std::vector<uint8>;
	auto DecodeStaticMeshCollisionValue(
		const FBuildValue& Value,
		EBodySetupCollisionSourceMode Mode,
		EBodySetupCollisionQueryPolicy Policy,
		FCollisionGeometryRef& OutGeometry,
		std::string& OutError) -> bool;

	auto CreateStaticMeshBuildFunction() -> std::shared_ptr<IBuildFunction>;
	auto CreateStaticMeshCollisionBuildFunction() -> std::shared_ptr<IBuildFunction>;
}
