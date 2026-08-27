#include "AssetForge/Builtins/SceneImport.h"

#include "Animation/AnimationClip.h"
#include "DObject/Package.h"
#include "AssetForge/Builtins/ImportedScene.h"
#include "Asset.h"
#include "Image/ImageDecoder.h"
#include "HAL/PlatformProcess.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SceneImportInternal.h"
#include "Skeletal/SkeletalBuildOperations.h"
#include "SkeletalMesh/SkeletalDerivedData.h"
#include "SkeletalMesh/SkeletalMesh.h"
#include "SkeletalMesh/Skeleton.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "StaticMeshImportAdapter.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	using namespace Durin::Asset;
	namespace
	{

		auto MakeMaterialSamplerState(
			const FImportedSampler& Sampler) -> FMaterialSamplerState
		{
			FMaterialSamplerState Result;
			Result.MinFilter = static_cast<EMaterialSamplerMinFilter>(Sampler.MinFilter);
			Result.MagFilter = Sampler.MagFilter == EImportedSamplerFilter::Nearest
				? EMaterialSamplerMagFilter::Nearest
				: EMaterialSamplerMagFilter::Linear;
			auto ConvertAddress = [](EImportedSamplerWrap Wrap) {
				switch (Wrap)
				{
				case EImportedSamplerWrap::MirroredRepeat:
					return EMaterialSamplerAddressMode::MirroredRepeat;
				case EImportedSamplerWrap::ClampToEdge:
					return EMaterialSamplerAddressMode::ClampToEdge;
				case EImportedSamplerWrap::Repeat:
				default:
					return EMaterialSamplerAddressMode::Repeat;
				}
			};
			Result.AddressU = ConvertAddress(Sampler.WrapU);
			Result.AddressV = ConvertAddress(Sampler.WrapV);
			return Result;
		}

		auto AddDiagnostic(
			std::vector<FImportDiagnostic>& Diagnostics,
			EImportDiagnosticCategory Category,
			std::string_view Phase,
			std::string_view Message,
			std::string_view SourceIdentity = {}) -> void
		{
			Diagnostics.push_back({
				.Severity = EImportDiagnosticSeverity::Error,
				.Category = Category,
				.Phase = std::string(Phase),
				.SourceIdentity = std::string(SourceIdentity),
				.Message = std::string(Message)});
		}

		class FSceneDiagnosticScope
		{
		public:
			FSceneDiagnosticScope(
				bool& InSucceeded,
				std::string& InMessage,
				std::vector<FImportDiagnostic>& InDiagnostics,
				std::string_view InPhase)
				: bSucceeded(InSucceeded), Message(InMessage), Diagnostics(InDiagnostics),
				  Phase(InPhase) {}
			~FSceneDiagnosticScope()
			{
				if (!bSucceeded && Diagnostics.empty() && !Message.empty())
					AddDiagnostic(Diagnostics, EImportDiagnosticCategory::TranslationFailure,
						Phase, Message, "root");
				FinalizeImportDiagnostics(Diagnostics, Phase, "root", "request");
			}
		private:
			bool& bSucceeded;
			std::string& Message;
			std::vector<FImportDiagnostic>& Diagnostics;
			std::string Phase;
		};

		auto ImportAxisVector(EStaticMeshImportAxis Axis) -> FVector3f
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: return {1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeX: return {-1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveY: return {0.0f, 1.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeY: return {0.0f, -1.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveZ: return {0.0f, 0.0f, 1.0f};
			case EStaticMeshImportAxis::NegativeZ: return {0.0f, 0.0f, -1.0f};
			}
			return {};
		}

		auto MakeMeshImportOptions(
			const FStaticMeshImportSettings& Settings,
			std::string RootSourcePath) -> FMeshImportOptions
		{
			const FVector3f Forward = ImportAxisVector(Settings.ForwardAxis);
			const FVector3f Right = ImportAxisVector(Settings.RightAxis);
			const FVector3f Up = ImportAxisVector(Settings.UpAxis);
			FMeshImportOptions Options;
			for (uint32 Component = 0; Component < 3; ++Component)
			{
				Options.SourceToEngine[Component][0] = Forward[Component];
				Options.SourceToEngine[Component][1] = Right[Component];
				Options.SourceToEngine[Component][2] = Up[Component];
			}
			Options.RootSourcePath = std::move(RootSourcePath);
			return Options;
		}

		auto FoldAscii(std::string Value) -> std::string
		{
			std::ranges::transform(Value, Value.begin(), [](const char Character) {
				return static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
			});
			return Value;
		}

		auto SanitizeAssetName(
			std::string_view Value,
			std::string_view Fallback) -> std::string
		{
			std::string Result;
			bool bLastWasSeparator = false;
			for (const char Character : Value)
			{
				const bool bValid = std::isalnum(static_cast<unsigned char>(Character)) != 0;
				if (bValid)
				{
					Result.push_back(Character);
					bLastWasSeparator = false;
				}
				else if (!Result.empty() && !bLastWasSeparator)
				{
					Result.push_back('_');
					bLastWasSeparator = true;
				}
			}
			while (!Result.empty() && Result.back() == '_') Result.pop_back();
			return Result.empty() ? std::string(Fallback) : Result;
		}

		auto MakeUniqueName(
			std::string_view Requested,
			std::string_view Fallback,
			std::unordered_set<std::string>& UsedNames) -> std::string
		{
			const std::string Base = SanitizeAssetName(Requested, Fallback);
			std::string Candidate = Base;
			for (uint32 Suffix = 2; !UsedNames.insert(FoldAscii(Candidate)).second; ++Suffix)
				Candidate = std::format("{}_{}", Base, Suffix);
			return Candidate;
		}

		auto MakeSceneOutputPath(
			const FAssetPath& DestinationDirectory,
			std::string_view DirectoryName,
			std::string_view Leaf,
			FAssetPath& OutPath,
			std::string& OutError) -> bool
		{
			std::filesystem::path OutputPath(DestinationDirectory.ToString());
			if (!DirectoryName.empty()) OutputPath /= DirectoryName;
			OutputPath /= Leaf;
			return FAssetPath::TryCreate(
				OutputPath.generic_string(), OutPath, &OutError);
		}

		auto StableSuffix(std::string_view Value) -> std::string
		{
			return FXxHash128::HashBuffer(std::as_bytes(std::span(Value))).ToString();
		}

		template<typename FVisitor>
		auto VisitGltfUris(
			std::span<const std::byte> Bytes,
			FVisitor&& Visitor) -> bool
		{
			const std::string Text(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
			size_t Cursor = 0;
			uint32 Index = 0;
			while ((Cursor = Text.find("\"uri\"", Cursor)) != std::string::npos)
			{
				const size_t Colon = Text.find(':', Cursor + 5);
				const size_t Quote = Colon == std::string::npos
					? std::string::npos : Text.find('"', Colon + 1);
				const size_t End = Quote == std::string::npos
					? std::string::npos : Text.find('"', Quote + 1);
				if (End == std::string::npos) return false;
				std::string Uri = Text.substr(Quote + 1, End - Quote - 1);
				Cursor = End + 1;
				if (Uri.starts_with("data:")) continue;
				if (Uri.find("\\u") != std::string::npos
					|| Uri.find(':') != std::string::npos) return false;
				for (size_t Slash = 0; (Slash = Uri.find("\\/", Slash)) != std::string::npos;)
					Uri.replace(Slash, 2, "/");
				if (!Visitor(Uri, Index++)) return false;
			}
			return true;
		}

		auto DiscoverGltfUris(
			std::span<const std::byte> Bytes,
			FDependencyRequestSink& Sink) -> bool
		{
			return VisitGltfUris(Bytes,
				[&](std::string_view Uri, uint32 Index) {
					return Sink.AddRelative("root",
						std::format("scene-dependency:{}", Index), Uri);
				});
		}

		class FTemporarySceneFiles
		{
		public:
			~FTemporarySceneFiles()
			{
				if (Root.empty()) return;
				std::error_code Error;
				std::filesystem::remove_all(Root, Error);
			}

			auto Stage(const FSourceSnapshot& Snapshot, std::string& OutError) -> bool
			{
				const FSourceSnapshotEntry* RootSource = Snapshot.FindSource("root");
				if (!RootSource || RootSource->Filename.empty())
				{
					OutError = "Scene snapshot has no logical root source.";
					return false;
				}
				static std::atomic<uint64> Serial = 0;
				Root = std::filesystem::temp_directory_path()
					/ std::format("DurinSceneImport_{}_{}_{}",
						RootSource->ContentHash.ToString(),
						FPlatformProcess::CurrentProcessId(), ++Serial);
				const std::filesystem::path SourceRoot(RootSource->Filename);
				const std::filesystem::path SourceParent = SourceRoot.parent_path();
				for (const FSourceSnapshotEntry& Source : Snapshot.GetSources())
				{
					if (Source.Filename.empty()) continue;
					const std::filesystem::path SourcePath(Source.Filename);
					std::filesystem::path Relative = SourcePath.lexically_relative(SourceParent);
					if (Relative.empty() || Relative.is_absolute()
						|| std::ranges::find(Relative, std::filesystem::path("..")) != Relative.end())
					{
						OutError = std::format(
							"Scene source {} escapes its logical root.", Source.Filename);
						return false;
					}
					const std::filesystem::path Target = Root / Relative;
					std::error_code Error;
					std::filesystem::create_directories(Target.parent_path(), Error);
					if (Error || !FFileHelper::SaveArrayToFile(
						std::as_bytes(Source.GetBytes()), Target))
					{
						OutError = std::format("Failed to stage captured Scene source {}.",
							Source.StableIdentity);
						return false;
					}
					if (Source.StableIdentity == "root") PhysicalRoot = Target;
				}
				return !PhysicalRoot.empty();
			}

			auto GetRoot() const -> const std::filesystem::path& { return PhysicalRoot; }

		private:
			std::filesystem::path Root;
			std::filesystem::path PhysicalRoot;
		};

		auto DecodeSceneSnapshot(
			const FSourceSnapshot& Snapshot,
			const FStaticMeshImportSettings& Settings,
			FImportedSceneData& OutScene,
			std::string& OutError) -> bool
		{
			FTemporarySceneFiles Files;
			if (!Files.Stage(Snapshot, OutError)) return false;
			const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
			if (!ImportFromFile(Files.GetRoot().generic_string(), OutScene,
				MakeMeshImportOptions(Settings, Root->Filename)))
			{
				OutError = "Captured Scene sources could not be decoded.";
				return false;
			}
			return true;
		}
	}

	auto BuildScenePlan(
		const FSourceSnapshot& Snapshot,
		const FAssetPath& DestinationDirectory,
		const FStaticMeshImportSettings& Settings,
		FSceneImportPlan& OutPlan,
		std::vector<FImportOutputSummary>& OutOutputs,
		std::vector<FImportDiagnostic>& OutDiagnostics,
		std::string& OutError) -> bool
	{
		OutError = "Scene translation failed.";
		auto CheckCanceled = [&]() -> bool {
			if (!::Durin::AssetForge::Builtins::Private::IsSceneImportCancellationRequested())
				return false;
			AddDiagnostic(OutDiagnostics, EImportDiagnosticCategory::Canceled,
				"scene-parse", "Scene import preparation was canceled.", "root");
			return true;
		};
		if (CheckCanceled()) return false;
		std::string Error;
		if (!DestinationDirectory.IsValid() || !Settings.IsValid(&Error))
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticCategory::InvalidPlan,
				"scene-plan", Error.empty()
					? "Scene import plan settings are invalid." : Error);
			return false;
		}
		OutPlan = {};
		OutOutputs.clear();
		OutPlan.MeshSettings = Settings;
		if (!DecodeSceneSnapshot(Snapshot, Settings, OutPlan.Scene, Error))
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticCategory::TranslationFailure,
				"scene-parse", Error, "root");
			return false;
		}
		if (CheckCanceled()) return false;
		const FSourceSnapshotEntry* RootSource = Snapshot.FindSource("root");
		if (!RootSource)
		{
			AddDiagnostic(OutDiagnostics, EImportDiagnosticCategory::InvalidSource,
				"scene-plan", "Scene snapshot has no root source.", "root");
			return false;
		}
		const std::string SceneName = SanitizeAssetName(
			std::filesystem::path(RootSource->Filename)
				.stem().generic_string(), "Scene");
		std::unordered_set<std::string> SkeletonNames;
		for (uint32 SkeletonIndex = 0;
			SkeletonIndex < OutPlan.Scene.Skeletons.size(); ++SkeletonIndex)
		{
			if (CheckCanceled()) return false;
			const FImportedSkeletonData& Skeleton =
				OutPlan.Scene.Skeletons[SkeletonIndex];
			FAssetPath SkeletonPath;
			if (!MakeSceneOutputPath(DestinationDirectory, "Skeletons",
				MakeUniqueName(Skeleton.SuggestedName,
					std::format("Skeleton_{}", SkeletonIndex), SkeletonNames),
				SkeletonPath, Error)) return false;
			OutOutputs.push_back({
				.StableIdentity = Skeleton.StableIdentity,
				.Role = "Skeleton",
				.AssetPath = SkeletonPath,
				.AssetClassName = "Durin::DSkeleton"});
			OutPlan.Outputs.push_back({
				.StableIdentity = Skeleton.StableIdentity,
				.Kind = ESceneOutputKind::Skeleton,
				.SourceIndex = SkeletonIndex});
		}
		FAssetPath MeshPath;
		if (!MakeSceneOutputPath(DestinationDirectory, "Meshes",
			SceneName, MeshPath, Error)) return false;
			OutOutputs.push_back({
			.StableIdentity = "scene:mesh:combined",
			.Role = "StaticMesh",
			.AssetPath = MeshPath,
			.AssetClassName = "Durin::DStaticMesh"});
		OutPlan.Outputs.push_back({
			.StableIdentity = "scene:mesh:combined",
			.Kind = ESceneOutputKind::StaticMesh});

		std::unordered_set<uint32> UsedMaterialIndices;
		std::vector<uint32> MaterialIndices;
		for (const FImportedMaterialSlot& Slot : OutPlan.Scene.MaterialSlots)
			if (UsedMaterialIndices.insert(Slot.SourceMaterialIndex).second)
				MaterialIndices.push_back(Slot.SourceMaterialIndex);
		for (const FImportedSkeletalMeshData& Mesh : OutPlan.Scene.SkeletalMeshes)
			for (const FMeshMaterialSlotDefinition& Slot : Mesh.MaterialSlots)
				if (UsedMaterialIndices.insert(Slot.SourceMaterialIndex).second)
					MaterialIndices.push_back(Slot.SourceMaterialIndex);
		std::unordered_map<std::string, uint32> MaterialNameCounts;
		for (const FImportedMaterial& Material : OutPlan.Scene.Materials)
			++MaterialNameCounts[FoldAscii(Material.SourceName)];
		std::unordered_set<std::string> MaterialNames;
		std::unordered_set<std::string> TextureNames;
		std::unordered_map<std::string, std::string> TextureByKey;
		for (const uint32 MaterialIndex : MaterialIndices)
		{
			if (CheckCanceled()) return false;
			const auto Material = std::ranges::find(
				OutPlan.Scene.Materials, MaterialIndex,
				&FImportedMaterial::SourceMaterialIndex);
			if (Material == OutPlan.Scene.Materials.end()) return false;
			const std::string MaterialKey = !Material->SourceName.empty()
				&& MaterialNameCounts[FoldAscii(Material->SourceName)] == 1
				? std::string("name:") + FoldAscii(Material->SourceName)
				: std::format("index:{}", MaterialIndex);
			const std::string MaterialIdentity =
				std::string("scene:material:") + StableSuffix(MaterialKey);
			FAssetPath MaterialPath;
			if (!MakeSceneOutputPath(DestinationDirectory, "Materials",
				MakeUniqueName(Material->SourceName, "Material", MaterialNames),
				MaterialPath, Error)) return false;
			FSceneOutputData MaterialOutput{
				.StableIdentity = MaterialIdentity,
				.Kind = ESceneOutputKind::MaterialInstance,
				.SourceIndex = MaterialIndex};
			auto AddTexture = [&](const FImportedTextureBinding& Binding,
				uint32 MaterialRole, std::string_view Role, ETextureUsage Usage,
				ESceneTextureDerivation Derivation = ESceneTextureDerivation::None,
				float DerivationScale = 1.0f,
				const FVector3f& DerivationColorScale = FVector3f(1.0f)) -> bool {
				if (Binding.ImageIndex >= OutPlan.Scene.Images.size()) return false;
				const FImportedImage& Image = OutPlan.Scene.Images[Binding.ImageIndex];
				const std::string TextureKey = std::format("{}:{}:{}:{}:{}:{}:{}",
					Image.StableIdentity, Role, static_cast<uint32>(Derivation),
					std::bit_cast<uint32>(DerivationScale),
					std::bit_cast<uint32>(DerivationColorScale.x),
					std::bit_cast<uint32>(DerivationColorScale.y),
					std::bit_cast<uint32>(DerivationColorScale.z));
				auto Texture = TextureByKey.find(TextureKey);
				if (Texture == TextureByKey.end())
				{
					const std::string TextureIdentity =
						std::string("scene:texture:") + StableSuffix(TextureKey);
					FAssetPath TexturePath;
					if (!MakeSceneOutputPath(DestinationDirectory, "Textures",
						MakeUniqueName(Image.SuggestedName + "_" + std::string(Role),
							"Image_" + std::string(Role), TextureNames), TexturePath, Error)) return false;
					OutOutputs.push_back({
						.StableIdentity = TextureIdentity,
						.Role = "Texture2D." + std::string(Role),
						.AssetPath = TexturePath,
						.AssetClassName = "Durin::DTexture2D"});
					OutPlan.Outputs.push_back({
						.StableIdentity = TextureIdentity,
						.Kind = ESceneOutputKind::Texture2D,
						.SourceIndex = Binding.ImageIndex,
						.TextureUsage = Usage,
						.TextureDerivation = Derivation,
						.TextureDerivationScale = DerivationScale,
						.TextureDerivationColorScale = DerivationColorScale});
					Texture = TextureByKey.emplace(TextureKey, TextureIdentity).first;
				}
				MaterialOutput.TextureBindings.push_back({
					.MaterialRole = MaterialRole,
					.TextureIdentity = Texture->second,
					.Binding = Binding});
				return true;
			};
			for (const FImportedTextureBinding& Binding : Material->TextureBindings)
			{
				switch (Binding.Semantic)
				{
				case EImportedTextureSemantic::BaseColor:
					if (!AddTexture(Binding, 0, "BaseColor", ETextureUsage::Color)) return false;
					if (Material->AlphaMode == EImportedAlphaMode::Mask
						|| Material->AlphaMode == EImportedAlphaMode::Blend)
						if (!AddTexture(Binding,
							Material->AlphaMode == EImportedAlphaMode::Mask ? 7u : 6u,
							Material->AlphaMode == EImportedAlphaMode::Mask
								? "OpacityMask" : "Opacity",
							ETextureUsage::DataMask, ESceneTextureDerivation::Alpha)) return false;
					break;
				case EImportedTextureSemantic::MetallicRoughness:
					if (!AddTexture(Binding, 2, "Metallic", ETextureUsage::DataMask,
						ESceneTextureDerivation::Blue)
						|| !AddTexture(Binding, 3, "Roughness", ETextureUsage::DataMask,
							ESceneTextureDerivation::Green)) return false;
					break;
				case EImportedTextureSemantic::Normal:
					if (!AddTexture(Binding, 1, "Normal", ETextureUsage::Normal,
						Binding.Strength == 1.0f ? ESceneTextureDerivation::None
							: ESceneTextureDerivation::ScaledNormal,
						Binding.Strength)) return false;
					break;
				case EImportedTextureSemantic::Occlusion:
					if (!AddTexture(Binding, 4, "AmbientOcclusion", ETextureUsage::DataMask,
						ESceneTextureDerivation::Red)) return false;
					break;
				case EImportedTextureSemantic::Emissive:
					if (!AddTexture(Binding, 5, "Emissive", ETextureUsage::Color,
						ESceneTextureDerivation::ScaledColor, 1.0f,
						Material->EmissiveFactor)) return false;
					break;
				}
			}
			OutOutputs.push_back({
				.StableIdentity = MaterialIdentity,
				.Role = "MaterialInstance",
				.AssetPath = MaterialPath,
				.AssetClassName = "Durin::DMaterialInstance"});
			OutPlan.Outputs.push_back(std::move(MaterialOutput));
		}

		std::unordered_set<std::string> SkeletalMeshNames;
		for (uint32 MeshIndex = 0;
			MeshIndex < OutPlan.Scene.SkeletalMeshes.size(); ++MeshIndex)
		{
			if (CheckCanceled()) return false;
			const FImportedSkeletalMeshData& Mesh =
				OutPlan.Scene.SkeletalMeshes[MeshIndex];
			if (!Mesh.Payload || Mesh.SkeletonIndex >= OutPlan.Scene.Skeletons.size())
				return false;
			const FImportedSkeletonData& Skeleton =
				OutPlan.Scene.Skeletons[Mesh.SkeletonIndex];
			FAssetPath MeshPath;
			if (!MakeSceneOutputPath(DestinationDirectory, "SkeletalMeshes",
				MakeUniqueName(Mesh.SuggestedName,
					std::format("SkeletalMesh_{}", MeshIndex), SkeletalMeshNames),
				MeshPath, Error)) return false;
			OutOutputs.push_back({
				.StableIdentity = Mesh.StableIdentity,
				.Role = "SkeletalMesh",
				.AssetPath = MeshPath,
				.AssetClassName = "Durin::DSkeletalMesh"});
			OutPlan.Outputs.push_back({
				.StableIdentity = Mesh.StableIdentity,
				.Kind = ESceneOutputKind::SkeletalMesh,
				.SourceIndex = MeshIndex,
				.SkeletonIdentity = Skeleton.StableIdentity});
		}

		std::unordered_set<std::string> AnimationNames;
		for (uint32 ClipIndex = 0;
			ClipIndex < OutPlan.Scene.AnimationClips.size(); ++ClipIndex)
		{
			if (CheckCanceled()) return false;
			const FImportedAnimationClipData& Clip =
				OutPlan.Scene.AnimationClips[ClipIndex];
			if (!Clip.Payload || Clip.SkeletonIndex >= OutPlan.Scene.Skeletons.size())
				return false;
			const FImportedSkeletonData& Skeleton =
				OutPlan.Scene.Skeletons[Clip.SkeletonIndex];
			FAssetPath ClipPath;
			if (!MakeSceneOutputPath(DestinationDirectory, "Animations",
				MakeUniqueName(Clip.SuggestedName,
					std::format("Animation_{}", ClipIndex), AnimationNames),
				ClipPath, Error)) return false;
			OutOutputs.push_back({
				.StableIdentity = Clip.StableIdentity,
				.Role = std::format("AnimationClip.{:.6g}s", Clip.Payload->DurationSeconds),
				.AssetPath = ClipPath,
				.AssetClassName = "Durin::DAnimationClip"});
			OutPlan.Outputs.push_back({
				.StableIdentity = Clip.StableIdentity,
				.Kind = ESceneOutputKind::AnimationClip,
				.SourceIndex = ClipIndex,
				.SkeletonIdentity = Skeleton.StableIdentity});
		}
		for (const FSceneImportDiagnostic& Diagnostic : OutPlan.Scene.Diagnostics)
		{
			if (Diagnostic.Severity != EImportDiagnosticSeverity::Warning) continue;
			OutPlan.Warnings.push_back(Diagnostic.Message);
			EImportDiagnosticCategory Category = EImportDiagnosticCategory::TranslationFailure;
			if (Diagnostic.Category == ESceneImportDiagnosticCategory::MissingDependency)
				Category = EImportDiagnosticCategory::MissingDependency;
			else if (Diagnostic.Category == ESceneImportDiagnosticCategory::UnsafeDependencyPath)
				Category = EImportDiagnosticCategory::UnsafeDependency;
			else if (Diagnostic.Category == ESceneImportDiagnosticCategory::ResourceLimitExceeded)
				Category = EImportDiagnosticCategory::ResourceLimitExceeded;
			OutDiagnostics.push_back({
				.Severity = EImportDiagnosticSeverity::Warning,
				.Category = Category,
				.Phase = "scene-parse",
				.SourceIdentity = Diagnostic.SourceIdentity.empty()
					? "root" : Diagnostic.SourceIdentity,
				.OutputIdentity = Diagnostic.Subject.empty()
					? "scene" : Diagnostic.Subject,
				.Message = Diagnostic.Message});
		}
		if (CheckCanceled()) return false;
		OutError.clear();
		return true;
	}

	namespace
	{
		auto FindSnapshotImageBytes(
			const FSourceSnapshot& Snapshot,
			const FImportedSceneData& Scene,
			const FImportedImage& Image,
			std::span<const std::byte>& OutBytes,
			std::string& OutFilename) -> bool
		{
			if (!Image.EmbeddedEncodedBytes.empty())
			{
				OutBytes = Image.EmbeddedEncodedBytes;
				const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
				if (!Root) return false;
				OutFilename = Root->Filename;
				return true;
			}
			if (!Image.ExternalDependencyIndex
				|| *Image.ExternalDependencyIndex >= Scene.Dependencies.size()) return false;
			const std::string& Dependency =
				Scene.Dependencies[*Image.ExternalDependencyIndex].SourcePath;
			const auto Source = std::ranges::find_if(
				Snapshot.GetSources(), [&](const FSourceSnapshotEntry& Entry) {
					return Entry.Filename == Dependency;
				});
			if (Source == Snapshot.GetSources().end()) return false;
			OutBytes = Source->GetBytes();
			OutFilename = Source->Filename;
			return true;
		}

		auto EmbeddedImageExtension(EImportedImageEncoding Encoding)
			-> std::string_view
		{
			switch (Encoding)
			{
			case EImportedImageEncoding::Png: return ".png";
			case EImportedImageEncoding::Jpeg: return ".jpg";
			case EImportedImageEncoding::Bmp: return ".bmp";
			case EImportedImageEncoding::Tga: return ".tga";
			}
			return ".image";
		}

		auto MakeEmbeddedImageSourcePath(
			std::string_view RootSource,
			const FImportedImage& Image,
			std::string_view TextureIdentity,
			std::span<const std::byte> Bytes) -> std::string
		{
			const std::filesystem::path RootPath(RootSource);
			const std::string FileName = std::format(
				"{}_{}{}",
				StableSuffix(TextureIdentity),
				FXxHash128::HashBuffer(Bytes).ToString(),
				EmbeddedImageExtension(Image.Encoding));
			return (RootPath.parent_path()
				/ (RootPath.stem().generic_string() + "_Embedded")
				/ FileName).generic_string();
		}

		auto BuildDerivedTextureBytes(
			std::span<const std::byte> EncodedBytes,
			ESceneTextureDerivation Derivation,
			float Scale,
			const FVector3f& ColorScale,
			std::vector<std::byte>& OutBytes,
			std::string& OutError) -> bool
		{
			Image::FDecodedImage Image;
			if (!Image::DecodeImageFromMemory(EncodedBytes, Image, OutError)) return false;
			if (Image.Width > std::numeric_limits<uint16>::max()
				|| Image.Height > std::numeric_limits<uint16>::max())
			{
				OutError = "Derived Scene texture exceeds the TGA dimension limit.";
				return false;
			}
			std::vector<std::byte> Pixels(Image.Pixels.size());
			for (size_t Offset = 0; Offset < Image.Pixels.size(); Offset += 4)
			{
				uint8 Red = std::to_integer<uint8>(Image.Pixels[Offset + 0]);
				uint8 Green = std::to_integer<uint8>(Image.Pixels[Offset + 1]);
				uint8 Blue = std::to_integer<uint8>(Image.Pixels[Offset + 2]);
				if (Derivation == ESceneTextureDerivation::Red
					|| Derivation == ESceneTextureDerivation::Green
					|| Derivation == ESceneTextureDerivation::Blue
					|| Derivation == ESceneTextureDerivation::Alpha)
				{
					const size_t Channel = Derivation == ESceneTextureDerivation::Red ? 0
						: Derivation == ESceneTextureDerivation::Green ? 1
						: Derivation == ESceneTextureDerivation::Blue ? 2 : 3;
					Red = Green = Blue = std::to_integer<uint8>(Image.Pixels[Offset + Channel]);
				}
				else if (Derivation == ESceneTextureDerivation::ScaledNormal)
				{
					float X = (static_cast<float>(Red) / 255.0f * 2.0f - 1.0f) * Scale;
					float Y = (static_cast<float>(Green) / 255.0f * 2.0f - 1.0f) * Scale;
					const float LengthSquared = X * X + Y * Y;
					if (LengthSquared > 1.0f)
					{
						const float InverseLength = 1.0f / std::sqrt(LengthSquared);
						X *= InverseLength;
						Y *= InverseLength;
					}
					Red = static_cast<uint8>(std::lround((X * 0.5f + 0.5f) * 255.0f));
					Green = static_cast<uint8>(std::lround((Y * 0.5f + 0.5f) * 255.0f));
					Blue = 255;
				}
				else if (Derivation == ESceneTextureDerivation::ScaledColor)
				{
					auto ScaleSrgb = [](uint8 Value, float Factor) -> uint8 {
						const float Srgb = static_cast<float>(Value) / 255.0f;
						const float Linear = Srgb <= 0.04045f ? Srgb / 12.92f
							: std::pow((Srgb + 0.055f) / 1.055f, 2.4f);
						const float Scaled = std::clamp(Linear * Factor, 0.0f, 1.0f);
						const float Encoded = Scaled <= 0.0031308f ? Scaled * 12.92f
							: 1.055f * std::pow(Scaled, 1.0f / 2.4f) - 0.055f;
						return static_cast<uint8>(std::lround(Encoded * 255.0f));
					};
					Red = ScaleSrgb(Red, ColorScale.x);
					Green = ScaleSrgb(Green, ColorScale.y);
					Blue = ScaleSrgb(Blue, ColorScale.z);
				}
				Pixels[Offset + 0] = static_cast<std::byte>(Blue);
				Pixels[Offset + 1] = static_cast<std::byte>(Green);
				Pixels[Offset + 2] = static_cast<std::byte>(Red);
				Pixels[Offset + 3] = std::byte{255};
			}
			OutBytes.assign(18, std::byte{0});
			OutBytes[2] = std::byte{2};
			OutBytes[12] = static_cast<std::byte>(Image.Width & 0xff);
			OutBytes[13] = static_cast<std::byte>((Image.Width >> 8) & 0xff);
			OutBytes[14] = static_cast<std::byte>(Image.Height & 0xff);
			OutBytes[15] = static_cast<std::byte>((Image.Height >> 8) & 0xff);
			OutBytes[16] = std::byte{32};
			OutBytes[17] = std::byte{0x28};
			OutBytes.insert(OutBytes.end(), Pixels.begin(), Pixels.end());
			OutError.clear();
			return true;
		}

		auto MakeDerivedImageSourcePath(
			std::string_view RootSource,
			std::string_view TextureIdentity,
			std::span<const std::byte> Bytes) -> std::string
		{
			const std::filesystem::path RootPath(RootSource);
			return (RootPath.parent_path()
				/ (RootPath.stem().generic_string() + "_Derived")
				/ std::format("{}_{}.tga", StableSuffix(TextureIdentity),
					FXxHash128::HashBuffer(Bytes).ToString())).generic_string();
		}
	}

	auto DiscoverSceneImportDependencies(
		std::span<const FSourceSnapshotEntry> Sources,
		FDependencyRequestSink& Sink,
		std::vector<FImportDiagnostic>& OutDiagnostics) -> bool
	{
		const auto Root = std::ranges::find(
			Sources, std::string_view("root"), &FSourceSnapshotEntry::StableIdentity);
		if (Root == Sources.end()) return false;
		const std::string Extension = FoldAscii(
			std::filesystem::path(Root->Filename).extension().generic_string());
		if (Extension != ".gltf") return true;
		if (DiscoverGltfUris(Root->GetBytes(), Sink)) return true;
		AddDiagnostic(OutDiagnostics, EImportDiagnosticCategory::UnsafeDependency,
			"dependency-discovery", "glTF contains an unsupported or unsafe URI.", "root");
		return false;
	}

	auto DecodeSceneSnapshotForImport(
		const FSourceSnapshot& Snapshot,
		const FStaticMeshImportSettings& Settings,
		FImportedSceneData& OutScene,
		std::string& OutError) -> bool
	{
		return DecodeSceneSnapshot(Snapshot, Settings, OutScene, OutError);
	}

	auto BuildSceneImportTextureProduct(
		const FSourceSnapshot& Snapshot,
		const FSceneImportPlan& Data,
		const FSceneOutputData& Descriptor,
		const std::function<bool()>& IsCancellationRequested,
		FSceneTextureBuildProduct& OutProduct,
		std::string& OutError) -> bool
	{
		if (Descriptor.Kind != ESceneOutputKind::Texture2D
			|| Descriptor.SourceIndex >= Data.Scene.Images.size())
		{
			OutError = "Scene texture product mapping is invalid.";
			return false;
		}
		const FImportedImage& Image = Data.Scene.Images[Descriptor.SourceIndex];
		std::span<const std::byte> Bytes;
		std::string SourceFilename;
		if (!IsSceneSurfaceImageEncodingSupported(Image.Encoding)
			|| !FindSnapshotImageBytes(Snapshot, Data.Scene, Image, Bytes, SourceFilename))
		{
			OutError = "Scene image snapshot mapping is invalid.";
			return false;
		}
		OutProduct = {};
		if (Descriptor.TextureDerivation != ESceneTextureDerivation::None)
		{
			if (!BuildDerivedTextureBytes(Bytes, Descriptor.TextureDerivation,
				Descriptor.TextureDerivationScale, Descriptor.TextureDerivationColorScale,
				OutProduct.GeneratedSourceBytes, OutError)) return false;
			Bytes = OutProduct.GeneratedSourceBytes;
			const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
			if (!Root) { OutError = "Scene root source is unavailable."; return false; }
			OutProduct.SourceFilename = MakeDerivedImageSourcePath(
				Root->Filename, Descriptor.StableIdentity, Bytes);
		}
		else if (!Image.EmbeddedEncodedBytes.empty())
		{
			OutProduct.GeneratedSourceBytes.assign(Bytes.begin(), Bytes.end());
			const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
			if (!Root) { OutError = "Scene root source is unavailable."; return false; }
			OutProduct.SourceFilename = MakeEmbeddedImageSourcePath(
				Root->Filename, Image, Descriptor.StableIdentity, Bytes);
		}
		else OutProduct.SourceFilename = std::move(SourceFilename);
		OutProduct.SourceFileSize = Bytes.size();
		FTextureSourceData SourceData;
		const FXxHash128 SourceHash = FXxHash128::HashBuffer(Bytes);
		const Asset::FTexture2DBuildExecutionControl Control{
			.ShouldCancel = IsCancellationRequested};
		if (!TranslateTexture2DSource(Bytes, SourceData, OutError)
			|| !Asset::BuildTexture2D({
				.SourceData = std::move(SourceData),
				.SourceContentHashLow = SourceHash.HashLow,
				.SourceContentHashHigh = SourceHash.HashHigh,
				.Settings = {.Usage = Descriptor.TextureUsage,
					.bSRGB = Descriptor.TextureUsage == ETextureUsage::Color}},
				OutProduct.Product, OutError, &Control))
			return false;
		OutError.clear();
		return true;
	}

}
