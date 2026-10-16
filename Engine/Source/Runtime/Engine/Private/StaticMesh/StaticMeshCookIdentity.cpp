#include "Asset/CookSchema.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCustomVersion.h"
#include "Collision/CollisionGeometry.h"
#include "Serialization/BinaryFormat.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "Physics/PhysicsCookDerivedDataKey.h"
#include "DerivedDataBuildDefinition.h"
#endif

namespace Durin
{
	auto DStaticMesh::AppendToClassSchema(FObjectCookContext& Context) -> void
	{
		Context.VersionCoveredClasses.insert("Durin::DStaticMesh");
		Context.UsedCustomVersions.insert(FStaticMeshSourceVersion::Guid);
		AssetPrivate::AppendCookSchemaVersion(Context, "static-mesh", 2);
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
			Context.Dependencies.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "recipe/static-mesh", {}, Recipe.TakeBytes()});
			return;
		}
#endif
		Context.Result = {EObjectCookError::InvalidInput, "Cook recipe provider 'static-mesh' is unavailable."};
	}
}
