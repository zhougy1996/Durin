#include "CookBuildProviders.h"
#include "CookRegistry.h"
#include "Asset/PackageSerialization.h"
#include "Asset/AssetCompilingManager.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

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
	namespace
	{
		auto PrepareBuiltin(DObject& Object, std::string_view Path, FCookPreparationContext& Context) -> FCookContributionResult
		{
			auto Reject = [&](ECookContributionError Error, std::string Diagnostic = {}) -> FCookContributionResult {
				return {.Error = Error, .ObjectPath = Object.GetObjectPath(), .VirtualPath = std::string(Path),
					.TargetPlatform = Context.GetTargetPlatform(), .TargetProfile = Context.GetTargetProfile(), .Diagnostic = std::move(Diagnostic)};
			};
			const auto& Options = Context.Options;
			const FAssetBuildTaskContext Control{.Cancellation = FCancellationToken([Check = Options.IsCancelled] { return Check && Check(); })};
			if (auto* Texture = Cast<DTexture>(&Object))
			{
				if (Texture->IsAsyncCacheComplete()) Texture->BeginCachePlatformData();
				if (!Texture->FinishCachePlatformData() || !Texture->HasPlatformData()) return Reject(ECookContributionError::PlatformData);
			}
			else if (auto* Mesh = Cast<DStaticMesh>(&Object))
			{
				if (HasPendingStaticMeshSourceMutation(*Mesh)) return Reject(ECookContributionError::SourceMutation);
				auto Built = Mesh->PrepareCookedPayloads(Options.TargetPlatform, Context.Primary, Context.Collision, Control);
				if (!Built) return Reject(ECookContributionError::Preparation, std::move(Built.error()));
			}
			else if (auto* Material = Cast<DMaterialInterface>(&Object); Material && !Material->IsDynamicInstance())
			{
				auto Built = Material->PrepareCookedProgram(Options.TargetPlatform, Options.MaterialConfigurations, Control);
				if (!Built) return Reject(ECookContributionError::Preparation, std::move(Built.error()));
				Context.Primary = std::move(*Built);
			}
			return {};
		}
		auto RegisterBuiltins(std::vector<FCookContributorHandle>& Handles, bool OnlyMissing) -> FCookContributorRegistrationResult
		{
			const auto First = Handles.size();
			auto Add = [&](DClass* Class, FCookContributorRegistration Registration) {
				auto Result = RegisterCookContributor(Class, std::move(Registration));
				if (!Result && OnlyMissing && Result.Error == ECookContributorRegistrationError::DuplicateClass) return FCookContributorRegistrationResult{};
				if (Result) Handles.push_back(Result.Handle);
				return Result;
			};
			auto EmptyDeclaration = [](const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FCookContributionResult { return {}; };
			FCookContributorRegistrationResult Result;
			if ((Result = Add(DTexture2D::StaticClass(), {"texture2d", 2, PrepareBuiltin, {}, AssetPrivate::DeclareTexture2DCookInputs}))
				&& (Result = Add(DTextureCube::StaticClass(), {"texture-cube", 2, PrepareBuiltin, {}, AssetPrivate::DeclareTextureCubeCookInputs}))
				&& (Result = Add(DVolumeTexture::StaticClass(), {"volume-texture", 2, PrepareBuiltin, {}, AssetPrivate::DeclareVolumeTextureCookInputs}))
				&& (Result = Add(DStaticMesh::StaticClass(), {"static-mesh", 2, PrepareBuiltin, {}, AssetPrivate::DeclareStaticMeshCookInputs}))
				&& (Result = Add(DMaterial::StaticClass(), {"material", 5, PrepareBuiltin, {}, AssetPrivate::DeclareMaterialCookInputs}))
				&& (Result = Add(DMaterialInstance::StaticClass(), {"material-instance", 5, PrepareBuiltin, {}, AssetPrivate::DeclareMaterialCookInputs}))
				&& (Result = Add(DMaterialFunctionInterface::StaticClass(), {"material-function-source", 1, {}, {}, EmptyDeclaration, ECookObjectDisposition::SourceOnly}))
				&& (!OnlyMissing || (Result = Add(DObject::StaticClass(), {"generic-package", 1, {}, {}, EmptyDeclaration})))) return {};
			for (size_t Index = First; Index < Handles.size(); ++Index) UnregisterCookContributor(Handles[Index]);
			Handles.resize(First);
			return Result;
		}
	}
	auto FormatCookContributionError(const FCookContributionResult& Result) -> std::string
	{
		std::string_view Reason;
		switch (Result.Error)
		{
		case ECookContributionError::None: return {};
		case ECookContributionError::Preparation: return Result.Diagnostic;
		case ECookContributionError::EmptyRuntimeRoot: return "Cook root has no runtime exports: " + Result.VirtualPath;
		case ECookContributionError::AuthoringOnly: Reason = "Material functions are authoring-only build dependencies"; break;
		case ECookContributionError::TypeMismatch: return std::format("Cook contributor expected class '{}' but received '{}' for '{}'.", Result.ExpectedClass, Result.ActualClass, Result.ObjectPath);
		case ECookContributionError::SourceMutation: Reason = "Cook cannot settle authored source mutation after capture"; break;
		case ECookContributionError::RecipeProvider: return std::format("Cook recipe provider '{}' is unavailable for '{}'.", Result.Provider, Result.VirtualPath);
		case ECookContributionError::ShaderInputs: return std::format("Material Cook requires declared '{}' inputs for '{}'.", Result.Provider, Result.VirtualPath);
		case ECookContributionError::Target: Reason = "The contributor does not support this cook target"; break;
		case ECookContributionError::PlatformData: Reason = "Texture platform data is unavailable"; break;
		case ECookContributionError::RenderData: Reason = "Static mesh render data is unavailable"; break;
		case ECookContributionError::Revision: return std::format("Material '{}' revision {} has no complete latest target result.", Result.ObjectPath, Result.AuthoredRevision);
		case ECookContributionError::Contract: Reason = "Material target or pass contract is incompatible"; break;
		case ECookContributionError::FunctionDependencies: Reason = "Material function dependencies are stale"; break;
		case ECookContributionError::UnsupportedClass: Reason = "No Engine family Cook contribution exists for the object class"; break;
		case ECookContributionError::Plan: return Result.PlanCause ? FormatCookPlanError(*Result.PlanCause) : "Cook plan admission failed.";
		}
		return std::format("{}: {}", Reason, Result.ObjectPath);
	}

	auto SaveCookPackage(DPackage* Package, std::string_view Path, FCookContext& Context) -> FCookContributionResult
	{
		if (GetTargetPlatformName(Context.GetTargetPlatform()).empty() || Context.GetTargetProfile() != ECookTargetProfile::Game)
			return {.Error = ECookContributionError::Target, .VirtualPath = std::string(Path), .TargetPlatform = Context.GetTargetPlatform(), .TargetProfile = Context.GetTargetProfile()};
		const auto Added = Context.AddPackage(std::string(Path), Package);
		if (!Added)
		{
			if (!Context.GetPreparationFailure()) return Context.GetPreparationFailure();
			return {.Error = ECookContributionError::Plan, .VirtualPath = std::string(Path), .PlanCause = Added.Error};
		}
		return {};
	}
	auto RegisterEngineCookContributors(std::vector<FCookContributorHandle>& Handles) -> FCookContributorRegistrationResult
	{ return RegisterBuiltins(Handles, false); }
	AssetPrivate::FEngineCookRegistrationScope::FEngineCookRegistrationScope() { Result = RegisterBuiltins(Handles, true); }
	AssetPrivate::FEngineCookRegistrationScope::~FEngineCookRegistrationScope() { for (auto Handle : Handles) UnregisterCookContributor(Handle); }
}
