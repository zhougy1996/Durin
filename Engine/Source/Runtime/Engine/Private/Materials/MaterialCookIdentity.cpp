#include "Asset/CookBuildProviders.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialCookedProgram.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::AssetPrivate
{
	auto DeclareMaterialCookInputs(const FCookDependencyRequest& Request, std::vector<FCookDependencyDeclaration>& Out) -> FCookContributionResult
	{
		FBinaryWriter Recipe;
		Recipe.WriteU64(GetMaterialBuildVersion());
		Recipe.WriteU32(GetMaterialCompileAdapterVersion());
		Recipe.WriteU32(MIR::CurrentVersion);
		Recipe.WriteU32(MaterialCookedProgramPayloadSchemaVersion);
		Recipe.WriteU32(CurrentMaterialPassContractVersion);
		Out.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "recipe/material", {}, Recipe.TakeBytes()});
		if (Request.ShaderBuildIdentity.empty()) return {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::ShaderInputs,
			.VirtualPath = Request.Package.ToString(), .TargetPlatform = Request.TargetPlatform,
			.TargetProfile = Request.TargetProfile, .Provider = "shader-build"};
		const auto Bytes = std::as_bytes(std::span(Request.ShaderBuildIdentity));
		Out.push_back({ECookBuildDependencyKind::SchemaProducerVersion, "shader-build", {}, FByteBuffer(Bytes.begin(), Bytes.end())});
		const auto Configurations = ResolveMaterialCookConfigurations(Request.MaterialConfigurations);
		if (!Configurations) return {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::Contract, .VirtualPath = Request.Package.ToString()};
		FBinaryWriter Policy;
		Policy.WriteU32(1);
		Policy.WriteU32(static_cast<uint32>(Configurations->size()));
		for (const auto& Configuration : *Configurations)
		{
			Policy.WriteU32(static_cast<uint32>(Configuration.Quality));
			Policy.WriteU32(static_cast<uint32>(Configuration.FeatureLevel));
		}
		Out.push_back({ECookBuildDependencyKind::ConfigurationValue, "material/configurations", {}, Policy.TakeBytes()});
		return {};
	}
}

namespace Durin
{
	auto DMaterialInterface::AppendToClassSchema(FObjectCookContext& Context) -> void
	{
		(void)Context.RequireRequestValue("shader-build");
		if (Context.Result) AssetPrivate::AppendNativeCookSchema(Context, "material", 5, AssetPrivate::DeclareMaterialCookInputs);
	}
}
