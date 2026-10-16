#include "Asset/CookBuildProviders.h"
#include "StaticMesh/StaticMesh.h"
#include "Collision/CollisionGeometry.h"
#include "Serialization/BinaryFormat.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "Physics/PhysicsCookDerivedDataKey.h"
#include "DerivedDataBuildDefinition.h"
#endif

namespace Durin::AssetPrivate
{
	auto DeclareStaticMeshCookInputs(const FCookDependencyRequest& Request, std::vector<FCookDependencyDeclaration>& Out) -> FCookContributionResult
	{
#if DURIN_WITH_EDITORONLY_DATA
		const auto* Module = IMeshBuilderModule::Get();
		const auto Version = Module ? Module->GetBuildVersion() : 0;
		if (Version)
		{
			FBinaryWriter Recipe;
			Recipe.WriteU64(Version);
			Recipe.WriteU32(StaticMeshRenderBuildFunctionVersion);
			Recipe.WriteU32(StaticMeshRenderConstantsSchemaVersion);
			Recipe.WriteU32(StaticMeshRenderOutputSchemaVersion);
			Recipe.WriteU32(StaticMeshPayloadSchemaVersion);
			const auto Physics = GetPhysicsCookBuildDescriptor();
			Recipe.WriteU64(FCollisionCookedData::GetBuildVersion());
			Recipe.WriteU32(Physics.Version);
			Recipe.WriteU32(Physics.ConstantsSchema);
			Recipe.WriteU32(Physics.OutputSchema);
			Recipe.WriteU32(PhysicsCollisionPayloadSchemaVersion);
			Out.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "recipe/static-mesh", {}, Recipe.TakeBytes()});
			return {};
		}
#endif
		return {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::RecipeProvider, .VirtualPath = Request.Package.ToString(),
			.TargetPlatform = Request.TargetPlatform, .TargetProfile = Request.TargetProfile, .Provider = "static-mesh"};
	}
}

namespace Durin
{
	auto DStaticMesh::AppendToClassSchema(FObjectCookContext& Context) -> void
	{ AssetPrivate::AppendNativeCookSchema(Context, "static-mesh", 2, AssetPrivate::DeclareStaticMeshCookInputs); }
}
