#include "CookBuildProviders.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Package.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	auto AssetPrivate::MakeObjectCookContext(const FCookDependencyRequest& Request) -> FObjectCookContext
	{
		FObjectCookContext Context;
		Context.Target = {std::string(GetTargetPlatformName(Request.TargetPlatform)), std::string(GetTargetProfileName(Request.TargetProfile))};
		Context.Package = Request.Package.ToString();
		Context.bCooking = true;
		Context.bRetainEditorOnlyData = Request.bRetainEditorOnlyData;
		const auto Shader = std::as_bytes(std::span(Request.ShaderBuildIdentity));
		if (!Shader.empty()) Context.RequestValues.emplace("shader-build", FByteBuffer(Shader.begin(), Shader.end()));
		FBinaryWriter Writer;
		Writer.WriteU32(static_cast<uint32>(Request.MaterialConfigurations.size()));
		for (const auto& Configuration : Request.MaterialConfigurations)
		{
			Writer.WriteU32(static_cast<uint32>(Configuration.Quality));
			Writer.WriteU32(static_cast<uint32>(Configuration.FeatureLevel));
		}
		Context.RequestValues.emplace("material/configurations", Writer.TakeBytes());
		return Context;
	}

	auto AssetPrivate::AppendNativeCookSchema(FObjectCookContext& Context, std::string_view Name, uint32 Version,
		FCookDependencyDeclarationFunction Declare) -> void
	{
		FBinaryWriter VersionBytes;
		VersionBytes.WriteU32(Version);
		Context.Dependencies.push_back({ECookBuildDependencyKind::SchemaProducerVersion,
			"class/" + std::string(Name), {}, VersionBytes.TakeBytes()});
		if (!Declare) return;
		const auto Bytes = Context.FindRequestValue("material/configurations");
		std::vector<FMaterialCookConfiguration> Configurations;
		if (!Bytes.empty())
		{
			FBinaryReader Reader(Bytes);
			uint32 Count = 0;
			if (!Reader.ReadU32(Count) || Count > 6) { Context.Result = {EObjectCookError::InvalidInput, "Invalid material Cook configuration count."}; return; }
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				uint32 Quality = 0, Feature = 0;
				if (!Reader.ReadU32(Quality) || !Reader.ReadU32(Feature)) { Context.Result = {EObjectCookError::InvalidInput, "Truncated material Cook configurations."}; return; }
				Configurations.push_back({static_cast<EMaterialQualityLevel>(Quality), static_cast<ERHIFeatureLevel>(Feature)});
			}
			if (!Reader.IsAtEnd()) { Context.Result = {EObjectCookError::InvalidInput, "Trailing material Cook configuration bytes."}; return; }
		}
		FPackagePath Package;
		if (!Context.Package.empty() && !FPackagePath::TryCreate(Context.Package, Package)) { Context.Result = {EObjectCookError::InvalidInput, "Invalid Cook schema package."}; return; }
		const auto Shader = Context.FindRequestValue("shader-build");
		const FCookDependencyRequest Request{Package,
			ParseTargetPlatform(Context.Target.Platform).value_or(ECookTargetPlatform::Invalid),
			ParseTargetProfile(Context.Target.Profile).value_or(ECookTargetProfile::Invalid), Context.bRetainEditorOnlyData,
			std::string_view(reinterpret_cast<const char*>(Shader.data()), Shader.size()), Configurations};
		if (auto Result = Declare(Request, Context.Dependencies); !Result)
			Context.Result = {Result.Status == ECookContributionStatus::Canceled ? EObjectCookError::Cancelled : EObjectCookError::InvalidInput,
				FormatCookContributionError(Result)};
	}
	auto FCookPreparationContext::GetTargetPlatform() const -> ECookTargetPlatform { return Options.TargetPlatform; }
	auto FCookPreparationContext::GetTargetProfile() const -> ECookTargetProfile { return Options.TargetProfile; }
	auto FCookPreparationContext::ReadDeclaredInput(ECookBuildDependencyKind Kind, std::string_view Name, FByteBuffer& Out) const -> FCookInputResult
	{
		Out.clear();
		return Options.ReadCookInput ? Options.ReadCookInput(Kind, Name, Out)
			: FCookInputFailure{.Error = ECookInputError::NoReader, .Kind = Kind, .Name = std::string(Name)}.ToInputResult();
	}
	auto FormatCookContributionError(const FCookContributionResult& Result) -> std::string
	{
		std::string_view Reason;
		switch (Result.Error)
		{
		case ECookContributionError::None: return {};
		case ECookContributionError::Preparation: return Result.Diagnostic;
		case ECookContributionError::EmptyRuntimeRoot: return "Cook root has no runtime exports: " + Result.VirtualPath;
		case ECookContributionError::SourceMutation: Reason = "Cook cannot settle authored source mutation after capture"; break;
		case ECookContributionError::RecipeProvider: return std::format("Cook recipe provider '{}' is unavailable for '{}'.", Result.Provider, Result.VirtualPath);
		case ECookContributionError::ShaderInputs: return std::format("Material Cook requires declared '{}' inputs for '{}'.", Result.Provider, Result.VirtualPath);
		case ECookContributionError::Target: Reason = "The object does not support this Cook target"; break;
		case ECookContributionError::PlatformData: Reason = "Texture platform data is unavailable"; break;
		case ECookContributionError::Contract: Reason = "Material target or pass contract is incompatible"; break;
		case ECookContributionError::Plan: return Result.PlanCause ? FormatCookPlanError(*Result.PlanCause) : "Cook plan admission failed.";
		}
		return std::format("{}: {}", Reason, Result.ObjectPath);
	}

	auto SaveCookPackage(DPackage* Package, std::string_view Path, FCookContext& Context) -> FCookContributionResult
	{
		if (GetTargetPlatformName(Context.GetTargetPlatform()).empty() || Context.GetTargetProfile() != ECookTargetProfile::Game)
			return {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::Target, .VirtualPath = std::string(Path), .TargetPlatform = Context.GetTargetPlatform(), .TargetProfile = Context.GetTargetProfile()};
		const auto Added = Context.AddPackage(std::string(Path), Package);
		if (!Added)
		{
			if (!Context.GetPreparationFailure()) return Context.GetPreparationFailure();
			return {.Status = ECookContributionStatus::Error, .Error = ECookContributionError::Plan, .VirtualPath = std::string(Path), .PlanCause = Added.Error};
		}
		return {};
	}
}
