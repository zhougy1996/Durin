#include "Asset/Cook.h"
#include "Asset/PackageSerialization.h"
#include "DObject/Package.h"

namespace Durin
{
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
