#include "CookBuildProviders.h"
#include "Asset/Cook.h"
#include "Shader/ShaderBuildProvider.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/Class.h"
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
	namespace
	{
		auto RegisterFunctionSource(std::vector<FCookContributorHandle>& Handles) -> FCookContributorRegistrationResult
		{
			// Override generic DObject contributors: function graphs are complete,
			// versioned build inputs, never runtime packages or unversioned recipes.
			const auto Handle = RegisterCookContributor(DMaterialFunctionInterface::StaticClass(),
				{"material-function-source", 1, 1,
					[](DObject& Object, std::string_view Path, FCookContext& Context) -> FAssetResult {
						return FCookContributionResult{.Error = ECookContributionError::AuthoringOnly,
							.ObjectPath = Object.GetObjectPath(), .VirtualPath = std::string(Path),
							.TargetPlatform = Context.GetTargetPlatform(), .TargetProfile = Context.GetTargetProfile()}.ToAssetResult();
					}, {}, {},
					[](const FCookDependencyRequest&, std::vector<FCookDependencyDeclaration>&) -> FAssetResult {
						// Source bytes, reflected schema and transitive package references
						// are already captured by ordinary Cook dependency discovery.
						return {};
					}});
			if (!Handle) return Handle;
			Handles.push_back(Handle.Handle);
			return {};
		}

		template<typename T>
		auto RegisterFamily(
			std::string Name,
			std::vector<FCookContributorHandle>& Handles) -> FCookContributorRegistrationResult
		{
			constexpr uint32 Version = std::is_base_of_v<DMaterialInterface, T> ? 5 : 2;
			const auto Handle = RegisterCookContributor(
				T::StaticClass(), {std::move(Name), Version, Version,
					[](DObject& Object, std::string_view VirtualPath,
						FCookContext& Context) -> FAssetResult {
						if (!Object.IsA(T::StaticClass()))
							return FCookContributionResult{.Error = ECookContributionError::TypeMismatch,
								.ObjectPath = Object.GetObjectPath(), .VirtualPath = std::string(VirtualPath),
								.ExpectedClass = T::StaticClass()->GetQualifiedName().ToString(),
								.ActualClass = Object.GetClass()->GetQualifiedName().ToString()}.ToAssetResult();
						if constexpr (std::is_same_v<T, DStaticMesh>)
						{
							if (HasPendingStaticMeshSourceMutation(static_cast<DStaticMesh&>(Object)))
								return FCookContributionResult{.Error = ECookContributionError::SourceMutation,
									.ObjectPath = Object.GetObjectPath(), .VirtualPath = std::string(VirtualPath)}.ToAssetResult();
						}
						else FAssetCompilingManager::Get().FinishCompilationForObject(Object);
						return ContributeEngineCookAsset(Object, VirtualPath, Context).ToAssetResult();
					},
					[](const DObject&) -> ECookPackageStatus {
						return ECookPackageStatus::Captured;
					}, {},
					[](const FCookDependencyRequest& Request, std::vector<FCookDependencyDeclaration>& Out) -> FAssetResult {
						if constexpr (std::is_same_v<T, DTexture2D> || std::is_same_v<T, DTextureCube>
							|| std::is_same_v<T, DVolumeTexture> || std::is_same_v<T, DStaticMesh>)
						{
							const std::string Family = std::is_same_v<T, DTexture2D> ? "texture2d"
								: std::is_same_v<T, DTextureCube> ? "texture-cube"
								: std::is_same_v<T, DVolumeTexture> ? "volume-texture" : "static-mesh";
							FByteBuffer Value;
							if (!AssetPrivate::GetCookBuildProviderInput(Family, Value))
								return FCookContributionResult{.Error = ECookContributionError::RecipeProvider,
									.VirtualPath = Request.Package.ToString(), .TargetPlatform = Request.TargetPlatform,
									.TargetProfile = Request.TargetProfile, .Provider = Family}.ToAssetResult();
							Out.push_back({ECookBuildDependencyKind::SchemaProducerVersion,
								"recipe/" + Family, {}, std::move(Value)});
						}
						if constexpr (std::is_base_of_v<DMaterialInterface, T>)
						{
							const auto Identity = Request.ShaderBuildIdentity;
							if (Identity.empty()) return FCookContributionResult{.Error = ECookContributionError::ShaderInputs,
								.VirtualPath = Request.Package.ToString(), .TargetPlatform = Request.TargetPlatform,
								.TargetProfile = Request.TargetProfile, .Provider = "shader-build"}.ToAssetResult();
							const auto Bytes = std::as_bytes(std::span(Identity));
							Out.push_back({ECookBuildDependencyKind::SchemaProducerVersion,
								"shader-build", {}, FByteBuffer(Bytes.begin(), Bytes.end())});
						}
						return {};
					}}
			);
			if (!Handle) return Handle;
			Handles.push_back(Handle.Handle);
			return {};
		}
	}

	auto FormatCookContributionError(const FCookContributionResult& Result) -> std::string
	{
		std::string_view Reason;
		switch (Result.Error)
		{
		case ECookContributionError::None: return {};
		case ECookContributionError::AuthoringOnly: Reason = "Material functions are authoring-only build dependencies"; break;
		case ECookContributionError::TypeMismatch: return std::format("Cook contributor expected class '{}' but received '{}' for '{}'.", Result.ExpectedClass, Result.ActualClass, Result.ObjectPath);
		case ECookContributionError::SourceMutation: Reason = "Cook cannot settle authored source mutation after capture"; break;
		case ECookContributionError::RecipeProvider: return std::format("Cook recipe provider '{}' is unavailable for '{}'.", Result.Provider, Result.VirtualPath);
		case ECookContributionError::ShaderInputs: return std::format("Material Cook requires declared '{}' inputs for '{}'.", Result.Provider, Result.VirtualPath);
		case ECookContributionError::Target: Reason = "Only the Win64 game cook target is supported"; break;
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

	auto FCookContributionResult::ToAssetResult() const -> FAssetResult
	{
		if (*this) return {};
		const EAssetError Classification = Error == ECookContributionError::TypeMismatch ? EAssetError::TypeMismatch
			: Error == ECookContributionError::SourceMutation || Error == ECookContributionError::RecipeProvider
				|| Error == ECookContributionError::ShaderInputs ? EAssetError::InUse : EAssetError::UnsupportedProperty;
		FAssetResult Result{Classification, FormatCookContributionError(*this)};
		return Result;
	}

	auto ContributeEngineCookAsset(
		DObject& Object,
		std::string_view VirtualPackagePath,
		FCookContext& Context) -> FCookContributionResult
	{
		if (Object.IsA(DTexture2D::StaticClass())
			|| Object.IsA(DTextureCube::StaticClass())
			|| Object.IsA(DVolumeTexture::StaticClass()))
			return static_cast<DTexture&>(Object).ContributeToCook(
				Context, VirtualPackagePath);
		if (Object.IsA(DStaticMesh::StaticClass()))
			return static_cast<DStaticMesh&>(Object).ContributeToCook(
				Context, VirtualPackagePath);
		if (Object.IsA(DMaterialInterface::StaticClass()))
			return static_cast<DMaterialInterface&>(Object).ContributeToCook(
				Context, VirtualPackagePath);
		return {.Error = ECookContributionError::UnsupportedClass, .ObjectPath = Object.GetObjectPath(), .VirtualPath = std::string(VirtualPackagePath)};
	}

	auto RegisterEngineCookContributors(
		std::vector<FCookContributorHandle>& OutHandles) -> FCookContributorRegistrationResult
	{
		const size_t FirstNewHandle = OutHandles.size();
		FCookContributorRegistrationResult Registered;
		if ((Registered = RegisterFamily<DTexture2D>("texture2d", OutHandles))
			&& (Registered = RegisterFamily<DTextureCube>("texture-cube", OutHandles))
			&& (Registered = RegisterFamily<DVolumeTexture>("volume-texture", OutHandles))
			&& (Registered = RegisterFamily<DStaticMesh>("static-mesh", OutHandles))
			&& (Registered = RegisterFamily<DMaterial>("material", OutHandles))
			&& (Registered = RegisterFamily<DMaterialInstance>("material-instance", OutHandles))
			&& (Registered = RegisterFunctionSource(OutHandles))) return {};

		for (size_t Index = FirstNewHandle; Index < OutHandles.size(); ++Index)
			UnregisterCookContributor(OutHandles[Index]);
		OutHandles.resize(FirstNewHandle);
		return Registered;
	}
}
