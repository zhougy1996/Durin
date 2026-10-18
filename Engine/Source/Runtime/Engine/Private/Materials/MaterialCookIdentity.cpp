#include "Asset/CookSchema.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialCustomVersion.h"
#include "Materials/MaterialCookedProgram.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	auto DMaterialInterface::AppendToClassSchema(FObjectCookContext& Context) -> void
	{
		Context.VersionCoveredClasses.insert("Durin::DMaterialInterface");
		Context.UsedCustomVersions.insert(FMaterialGraphVersion::Guid);
		Context.UsedCustomVersions.insert(FMaterialOutputVersion::Guid);
		Context.UsedCustomVersions.insert(FMaterialFunctionVersion::Guid);
		Context.UsedCustomVersions.insert(FMaterialInstanceVersion::Guid);
		const auto Shader = Context.RequireRequestValue("shader-build");
		(void)Context.RequireRequestValue("material/configurations");
		if (!Context.Result) return;
		if (Shader.empty()) {
			Context.Result = {EObjectCookError::InvalidInput, "Material Cook requires a shader-build identity."}; return;
		}
		AssetPrivate::AppendCookSchemaVersion(Context, "material", 7);
		FBinaryWriter Recipe;
		Recipe.WriteU64(GetMaterialBuildVersion());
		Recipe.WriteU32(GetMaterialCompileAdapterVersion());
		Recipe.WriteU32(MIR::CurrentVersion);
		Recipe.WriteU32(MaterialCookedProgramPayloadSchemaVersion);
		Recipe.WriteU32(CurrentMaterialPassContractVersion);
		Context.Dependencies.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "recipe/material", {}, Recipe.TakeBytes()});
	}
	auto DMaterial::AppendToClassSchema(FObjectCookContext& Context) -> void
	{ Context.VersionCoveredClasses.insert("Durin::DMaterial"); }
	auto DMaterialInstance::AppendToClassSchema(FObjectCookContext& Context) -> void
	{ Context.VersionCoveredClasses.insert("Durin::DMaterialInstance"); }

}
