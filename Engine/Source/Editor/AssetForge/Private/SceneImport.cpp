#include "SceneImport.h"

#include "Animation/AnimationClip.h"
#include "ImportedScene.h"
#include "ImportService.h"
#include "AssetAuthoring.h"
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
#include "AssetForgeProviders.h"
#include "Texture2DSourceTranslation.h"
#include "Texture2DBuildAdapter.h"
#include "StaticMeshImportAdapter.h"
#include "StaticMesh/StaticMeshBuildOperations.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureBuildOperations.h"

namespace Durin::Asset::Forge
{
	namespace
	{

		enum class ESceneOutputKind : uint8
		{
			Skeleton,
			SkeletalMesh,
			AnimationClip,
			StaticMesh,
			MaterialInstance,
			Texture2D
		};

		enum class ESceneTextureDerivation : uint8
		{
			None,
			Red,
			Green,
			Blue,
			Alpha,
			ScaledNormal,
			ScaledColor
		};

		struct FSceneMaterialTextureBinding
		{
			uint32 MaterialRole = 0;
			std::string TextureIdentity;
			FImportedTextureBinding Binding;
		};

		struct FSceneOutputData
		{
			std::string StableIdentity;
			ESceneOutputKind Kind = ESceneOutputKind::StaticMesh;
			uint32 SourceIndex = 0;
			ETextureUsage TextureUsage = ETextureUsage::Color;
			ESceneTextureDerivation TextureDerivation = ESceneTextureDerivation::None;
			float TextureDerivationScale = 1.0f;
			FVector3f TextureDerivationColorScale{1.0f};
			std::vector<FSceneMaterialTextureBinding> TextureBindings;
			std::string SkeletonIdentity;
		};

		struct FSceneProviderPlanData
		{
			FImportedSceneData Scene;
			FStaticMeshImportSettings MeshSettings;
			std::vector<FSceneOutputData> Outputs;
			std::vector<std::string> Warnings;
		};

		struct FDecodedSceneSettings
		{
			FAssetPath DestinationDirectory;
			FStaticMeshImportSettings MeshSettings;
		};

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

		template<typename T>
		auto AppendValue(std::vector<uint8>& Bytes, const T& Value) -> void
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* Begin = reinterpret_cast<const uint8*>(&Value);
			Bytes.insert(Bytes.end(), Begin, Begin + sizeof(T));
		}

		template<typename T>
		auto ReadValue(std::span<const uint8> Bytes, size_t& Offset, T& OutValue) -> bool
		{
			static_assert(std::is_trivially_copyable_v<T>);
			if (Offset > Bytes.size() || sizeof(T) > Bytes.size() - Offset) return false;
			std::memcpy(&OutValue, Bytes.data() + Offset, sizeof(T));
			Offset += sizeof(T);
			return true;
		}

		auto AppendString(std::vector<uint8>& Bytes, std::string_view Value) -> bool
		{
			if (Value.size() > std::numeric_limits<uint32>::max()) return false;
			AppendValue(Bytes, static_cast<uint32>(Value.size()));
			Bytes.insert(Bytes.end(), Value.begin(), Value.end());
			return true;
		}

		auto UpdateHashString(FXxHash128Builder& Builder, std::string_view Value) -> void
		{
			Builder.UpdateValue(static_cast<uint64>(Value.size()));
			Builder.Update(Value);
		}

		auto ComputeSourceClosureHash(const FImportPlan& Plan) -> FXxHash128
		{
			FXxHash128Builder Builder;
			const std::span<const FSourceSnapshotEntry> Sources =
				Plan.GetSnapshot().GetSources();
			Builder.UpdateValue(static_cast<uint32>(Sources.size()));
			for (const FSourceSnapshotEntry& Source : Sources)
			{
				UpdateHashString(Builder, Source.StableIdentity);
				UpdateHashString(Builder, Source.Role);
				UpdateHashString(Builder, Source.SourcePath.Path);
				UpdateHashString(Builder, Source.DeclaringIdentity);
				Builder.UpdateValue(Source.ContentHash.HashLow);
				Builder.UpdateValue(Source.ContentHash.HashHigh);
				Builder.UpdateValue(Source.ByteCount);
				Builder.UpdateValue(Source.Depth);
				Builder.UpdateValue(Source.bEmbedded);
			}
			return Builder.Finalize();
		}

		auto MakeSceneProviderState(
			const FSceneProviderPlanData& Data,
			std::span<const FImportOutputPreview> PlannedOutputs,
			FImportRecordPayload& OutState,
			std::string& OutError) -> bool
		{
			constexpr uint32 Magic = 0x53504353;
			std::vector<uint8> Bytes;
			AppendValue(Bytes, Magic);
			AppendValue(Bytes, static_cast<uint32>(1));
			AppendValue(Bytes, static_cast<uint32>(Data.Scene.Skeletons.size()));
			AppendValue(Bytes, static_cast<uint32>(Data.Scene.SkeletalMeshes.size()));
			AppendValue(Bytes, static_cast<uint32>(Data.Scene.AnimationClips.size()));
			AppendValue(Bytes, static_cast<uint32>(Data.Outputs.size()));
			for (const FSceneOutputData& Output : Data.Outputs)
			{
				const auto Planned = std::ranges::find(
					PlannedOutputs, Output.StableIdentity,
					&FImportOutputPreview::StableIdentity);
				if (Planned == PlannedOutputs.end()
					|| !AppendString(Bytes, Output.StableIdentity)
					|| !AppendString(Bytes, Planned->Role)
					|| !AppendString(Bytes, Planned->AssetPath.ToString())
					|| !AppendString(Bytes, Planned->AssetClassName)
					|| !AppendString(Bytes, Output.SkeletonIdentity))
				{
					OutError = "Scene provider state contains an invalid output relationship.";
					return false;
				}
				AppendValue(Bytes, Output.Kind);
				AppendValue(Bytes, Output.SourceIndex);
			}
			return MakeImportRecordPayload(
				"Durin.Scene.ProviderState", 1, Bytes,
				MaximumImportRecordProviderStateBytes, OutState, OutError);
		}

		auto MakeSceneSettings(
			const FAssetPath& DestinationDirectory,
			const FStaticMeshImportSettings& Settings,
			FImportPayload& OutPayload,
			std::string& OutError) -> bool
		{
			const std::string Path = DestinationDirectory.ToString();
			if (Path.size() > std::numeric_limits<uint32>::max())
			{
				OutError = "Scene destination directory path is too large.";
				return false;
			}
			std::vector<uint8> Bytes;
			AppendValue(Bytes, static_cast<uint32>(Path.size()));
			Bytes.insert(Bytes.end(), Path.begin(), Path.end());
			AppendValue(Bytes, Settings.ForwardAxis);
			AppendValue(Bytes, Settings.RightAxis);
			AppendValue(Bytes, Settings.UpAxis);
			OutPayload = {
				.SchemaId = "Durin.Scene.ImportSettings",
				.SchemaVersion = 2,
				.Bytes = std::move(Bytes)};
			return OutPayload.Finalize(OutError);
		}

		auto DecodeSceneSettings(
			const FImportPayload& Payload,
			FDecodedSceneSettings& OutSettings,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != "Durin.Scene.ImportSettings"
				|| Payload.SchemaVersion != 2)
			{
				OutError = "Scene settings schema is unsupported.";
				return false;
			}
			size_t Offset = 0;
			uint32 PathSize = 0;
			if (!ReadValue(std::span<const uint8>(Payload.Bytes), Offset, PathSize)
				|| Offset > Payload.Bytes.size()
				|| PathSize > Payload.Bytes.size() - Offset)
			{
				OutError = "Scene settings payload is truncated.";
				return false;
			}
			const std::string Path(
				reinterpret_cast<const char*>(Payload.Bytes.data() + Offset), PathSize);
			Offset += PathSize;
			if (!FAssetPath::TryCreate(Path, OutSettings.DestinationDirectory, &OutError)
				|| !ReadValue(std::span<const uint8>(Payload.Bytes), Offset,
					OutSettings.MeshSettings.ForwardAxis)
				|| !ReadValue(std::span<const uint8>(Payload.Bytes), Offset,
					OutSettings.MeshSettings.RightAxis)
				|| !ReadValue(std::span<const uint8>(Payload.Bytes), Offset,
					OutSettings.MeshSettings.UpAxis)
				|| Offset != Payload.Bytes.size()
				|| !OutSettings.MeshSettings.IsValid(&OutError))
			{
				if (OutError.empty()) OutError = "Scene settings payload is invalid.";
				return false;
			}
			return true;
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
					AddDiagnostic(Diagnostics, EImportDiagnosticCategory::ProviderFailure,
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
			const FSourcePath& RootSource) -> FMeshImportOptions
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
			Options.RootSource = RootSource;
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
			std::span<const uint8> Bytes,
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
			std::span<const uint8> Bytes,
			FDependencyRequestSink& Sink) -> bool
		{
			return VisitGltfUris(Bytes,
				[&](std::string_view Uri, uint32 Index) {
					return Sink.AddRelative("root",
						std::format("scene-dependency:{}", Index),
						"SceneDependency", Uri);
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
				if (!RootSource || RootSource->SourcePath.IsEmpty())
				{
					OutError = "Scene snapshot has no logical root source.";
					return false;
				}
				static std::atomic<uint64> Serial = 0;
				Root = std::filesystem::temp_directory_path()
					/ std::format("DurinSceneImport_{}_{}_{}",
						RootSource->ContentHash.ToString(),
						FPlatformProcess::CurrentProcessId(), ++Serial);
				const std::filesystem::path VirtualRoot(RootSource->SourcePath.Path);
				const std::filesystem::path VirtualParent = VirtualRoot.parent_path();
				for (const FSourceSnapshotEntry& Source : Snapshot.GetSources())
				{
					if (Source.SourcePath.IsEmpty()) continue;
					const std::filesystem::path Virtual(Source.SourcePath.Path);
					std::filesystem::path Relative = Virtual.lexically_relative(VirtualParent);
					if (Relative.empty() || Relative.is_absolute()
						|| std::ranges::find(Relative, std::filesystem::path("..")) != Relative.end())
					{
						OutError = std::format(
							"Scene source {} escapes its logical root.", Source.SourcePath.Path);
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
				MakeMeshImportOptions(Settings, Root->SourcePath)))
			{
				OutError = "Captured Scene sources could not be decoded.";
				return false;
			}
			return true;
		}

		class FSceneProvider final : public IImportProvider
		{
		public:
			auto GetProviderId() const -> std::string_view override
			{
				return SceneImportProviderId;
			}
			auto GetContractVersion() const -> uint32 override
			{
				return SceneImportProviderContractVersion;
			}
			auto CanImport(const FImportSourceRecognition& Source) const -> bool override
			{
				const std::string Extension = FoldAscii(Source.Extension);
				return Extension == ".gltf" || Extension == ".glb" || Extension == ".fbx";
			}
			auto CaptureSettings(FImportPayload& OutSettings,
				std::vector<FImportDiagnostic>&) const -> bool override
			{
				std::string Error;
				FAssetPath DefaultDirectory;
				if (!FAssetPath::TryCreate(
					"/Imported/Scene", DefaultDirectory, &Error)) return false;
				return MakeSceneSettings(
					DefaultDirectory,
					FStaticMeshImportSettings::MakeDurin(), OutSettings, Error);
			}
			auto DiscoverDependencies(
				std::span<const FSourceSnapshotEntry> Sources,
				FDependencyRequestSink& Sink,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				if (IsImportCancellationRequested()) return false;
				const auto Root = std::ranges::find(
					Sources, std::string_view("root"), &FSourceSnapshotEntry::StableIdentity);
				if (Root == Sources.end()) return false;
				const std::string Extension = FoldAscii(
					std::filesystem::path(Root->SourcePath.Path).extension().generic_string());
				if (Extension != ".gltf") return true;
				if (!DiscoverGltfUris(Root->GetBytes(), Sink))
				{
					AddDiagnostic(OutDiagnostics, EImportDiagnosticCategory::UnsafeDependency,
						"dependency-discovery", "glTF contains an unsupported or unsafe URI.", "root");
					return false;
				}
				return !IsImportCancellationRequested();
			}
			auto Plan(
				const FSourceSnapshot& Snapshot,
				const FImportPayload& Settings,
				FImportPlanBuilder& Builder,
				std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				auto CheckCanceled = [&]() -> bool {
					if (!IsImportCancellationRequested()) return false;
					AddDiagnostic(OutDiagnostics, EImportDiagnosticCategory::Canceled,
						"scene-parse", "Scene import preparation was canceled.", "root");
					return true;
				};
				if (CheckCanceled()) return false;
				FDecodedSceneSettings Decoded;
				std::string Error;
				if (!DecodeSceneSettings(Settings, Decoded, Error))
				{
					AddDiagnostic(OutDiagnostics, EImportDiagnosticCategory::InvalidPlan,
						"scene-plan", Error);
					return false;
				}
				auto Data = std::make_shared<FSceneProviderPlanData>();
				Data->MeshSettings = Decoded.MeshSettings;
				if (!DecodeSceneSnapshot(Snapshot, Decoded.MeshSettings, Data->Scene, Error))
				{
					AddDiagnostic(OutDiagnostics, EImportDiagnosticCategory::ProviderFailure,
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
					std::filesystem::path(RootSource->SourcePath.Path)
						.stem().generic_string(), "Scene");
				std::unordered_set<std::string> SkeletonNames;
				for (uint32 SkeletonIndex = 0;
					SkeletonIndex < Data->Scene.Skeletons.size(); ++SkeletonIndex)
				{
					if (CheckCanceled()) return false;
					const FImportedSkeletonData& Skeleton =
						Data->Scene.Skeletons[SkeletonIndex];
					FAssetPath SkeletonPath;
					if (!MakeSceneOutputPath(Decoded.DestinationDirectory, "Skeletons",
						MakeUniqueName(Skeleton.SuggestedName,
							std::format("Skeleton_{}", SkeletonIndex), SkeletonNames),
						SkeletonPath, Error)) return false;
					const uint64 AuthoredBytes = Skeleton.Bones.size() * sizeof(FSkeletonBone)
						+ Skeleton.CompatibilityIdentity.size();
					Builder.AddOutput({
						.StableIdentity = Skeleton.StableIdentity,
						.Role = "Skeleton",
						.AssetPath = SkeletonPath,
						.AssetClassName = "Durin::DSkeleton",
						.Policy = EImportOutputPolicy::Create,
						.Collision = EImportCollisionAction::Create,
						.EstimatedCpuBytes = AuthoredBytes,
						.EstimatedDiskBytes = AuthoredBytes});
					Data->Outputs.push_back({
						.StableIdentity = Skeleton.StableIdentity,
						.Kind = ESceneOutputKind::Skeleton,
						.SourceIndex = SkeletonIndex});
				}
				uint64 MeshBytes = 0;
				for (const FImportedMeshData& Mesh : Data->Scene.Meshes)
				{
					if (CheckCanceled()) return false;
					MeshBytes += Mesh.Positions.size() * sizeof(Mesh.Positions.front());
					MeshBytes += Mesh.Normals.size() * sizeof(Mesh.Normals.front());
					MeshBytes += Mesh.Tangents.size() * sizeof(Mesh.Tangents.front());
					MeshBytes += Mesh.Colors.size() * sizeof(Mesh.Colors.front());
					MeshBytes += Mesh.Indices.size() * sizeof(Mesh.Indices.front());
					for (const auto& UVs : Mesh.UVChannels)
						MeshBytes += UVs.size() * sizeof(UVs.front());
				}

				FAssetPath MeshPath;
				if (!MakeSceneOutputPath(Decoded.DestinationDirectory, "Meshes",
					SceneName, MeshPath, Error)) return false;
				Builder.AddOutput({
					.StableIdentity = "scene:mesh:combined",
					.Role = "StaticMesh",
					.AssetPath = MeshPath,
					.AssetClassName = "Durin::DStaticMesh",
					.Policy = EImportOutputPolicy::Create,
					.Collision = EImportCollisionAction::Create,
					.EstimatedCpuBytes = MeshBytes,
					.EstimatedGpuBytes = MeshBytes,
					.EstimatedDiskBytes = MeshBytes});
				Data->Outputs.push_back({
					.StableIdentity = "scene:mesh:combined",
					.Kind = ESceneOutputKind::StaticMesh});

				std::unordered_set<uint32> UsedMaterialIndices;
				std::vector<uint32> MaterialIndices;
				for (const FImportedMaterialSlot& Slot : Data->Scene.MaterialSlots)
					if (UsedMaterialIndices.insert(Slot.SourceMaterialIndex).second)
						MaterialIndices.push_back(Slot.SourceMaterialIndex);
				for (const FImportedSkeletalMeshData& Mesh : Data->Scene.SkeletalMeshes)
					for (const FSkeletalMeshMaterialSlotDefinition& Slot : Mesh.MaterialSlots)
						if (UsedMaterialIndices.insert(Slot.SourceMaterialIndex).second)
							MaterialIndices.push_back(Slot.SourceMaterialIndex);
				std::unordered_map<std::string, uint32> MaterialNameCounts;
				for (const FImportedMaterial& Material : Data->Scene.Materials)
					++MaterialNameCounts[FoldAscii(Material.SourceName)];
				std::unordered_set<std::string> MaterialNames;
				std::unordered_set<std::string> TextureNames;
				std::unordered_map<std::string, std::string> TextureByKey;
				for (const uint32 MaterialIndex : MaterialIndices)
				{
					if (CheckCanceled()) return false;
					const auto Material = std::ranges::find(
						Data->Scene.Materials, MaterialIndex,
						&FImportedMaterial::SourceMaterialIndex);
					if (Material == Data->Scene.Materials.end()) return false;
					const std::string MaterialKey = !Material->SourceName.empty()
						&& MaterialNameCounts[FoldAscii(Material->SourceName)] == 1
						? std::string("name:") + FoldAscii(Material->SourceName)
						: std::format("index:{}", MaterialIndex);
					const std::string MaterialIdentity =
						std::string("scene:material:") + StableSuffix(MaterialKey);
					FAssetPath MaterialPath;
					if (!MakeSceneOutputPath(Decoded.DestinationDirectory, "Materials",
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
						if (Binding.ImageIndex >= Data->Scene.Images.size()) return false;
						const FImportedImage& Image = Data->Scene.Images[Binding.ImageIndex];
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
							if (!MakeSceneOutputPath(Decoded.DestinationDirectory, "Textures",
								MakeUniqueName(Image.SuggestedName + "_" + std::string(Role),
									"Image_" + std::string(Role), TextureNames), TexturePath, Error)) return false;
							Builder.AddOutput({
								.StableIdentity = TextureIdentity,
								.Role = "Texture2D." + std::string(Role),
								.AssetPath = TexturePath,
								.AssetClassName = "Durin::DTexture2D",
								.Policy = EImportOutputPolicy::Create,
								.Collision = EImportCollisionAction::Create,
								.EstimatedCpuBytes = Image.EncodedByteCount * 4,
								.EstimatedGpuBytes = Image.EncodedByteCount * 4,
								.EstimatedDiskBytes = Image.EncodedByteCount});
							Data->Outputs.push_back({
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
					Builder.AddOutput({
						.StableIdentity = MaterialIdentity,
						.Role = "MaterialInstance",
						.AssetPath = MaterialPath,
						.AssetClassName = "Durin::DMaterialInstance",
						.Policy = EImportOutputPolicy::Create,
						.Collision = EImportCollisionAction::Create,
						.EstimatedCpuBytes = 4096,
						.EstimatedGpuBytes = 256,
						.EstimatedDiskBytes = 4096});
					Data->Outputs.push_back(std::move(MaterialOutput));
				}

				std::unordered_set<std::string> SkeletalMeshNames;
				for (uint32 MeshIndex = 0;
					MeshIndex < Data->Scene.SkeletalMeshes.size(); ++MeshIndex)
				{
					if (CheckCanceled()) return false;
					const FImportedSkeletalMeshData& Mesh =
						Data->Scene.SkeletalMeshes[MeshIndex];
					if (!Mesh.Payload || Mesh.SkeletonIndex >= Data->Scene.Skeletons.size())
						return false;
					const FImportedSkeletonData& Skeleton =
						Data->Scene.Skeletons[Mesh.SkeletonIndex];
					FAssetPath MeshPath;
					if (!MakeSceneOutputPath(Decoded.DestinationDirectory, "SkeletalMeshes",
						MakeUniqueName(Mesh.SuggestedName,
							std::format("SkeletalMesh_{}", MeshIndex), SkeletalMeshNames),
						MeshPath, Error)) return false;
					uint64 PayloadBytes = Mesh.Payload->Positions.size() * sizeof(FVector3f)
						+ Mesh.Payload->Normals.size() * sizeof(FVector3f)
						+ Mesh.Payload->Tangents.size() * sizeof(FVector4f)
						+ Mesh.Payload->Colors.size() * sizeof(FVector4f)
						+ Mesh.Payload->Indices.size() * sizeof(uint32)
						+ Mesh.Payload->Influences.size() * sizeof(FSkeletalMeshVertexInfluences)
						+ Mesh.Payload->Sections.size() * sizeof(FSkeletalMeshSection)
						+ Mesh.Payload->PaletteBoneIndices.size() * sizeof(uint16)
						+ Mesh.Payload->InverseBindMatrices.size() * sizeof(FMatrix4f);
					for (const auto& UVs : Mesh.Payload->UVChannels)
						PayloadBytes += UVs.size() * sizeof(FVector2f);
					Builder.AddOutput({
						.StableIdentity = Mesh.StableIdentity,
						.Role = "SkeletalMesh",
						.AssetPath = MeshPath,
						.AssetClassName = "Durin::DSkeletalMesh",
						.Policy = EImportOutputPolicy::Create,
						.Collision = EImportCollisionAction::Create,
						.EstimatedCpuBytes = PayloadBytes,
						.EstimatedGpuBytes = PayloadBytes,
						.EstimatedDiskBytes = PayloadBytes});
					Data->Outputs.push_back({
						.StableIdentity = Mesh.StableIdentity,
						.Kind = ESceneOutputKind::SkeletalMesh,
						.SourceIndex = MeshIndex,
						.SkeletonIdentity = Skeleton.StableIdentity});
				}

				std::unordered_set<std::string> AnimationNames;
				for (uint32 ClipIndex = 0;
					ClipIndex < Data->Scene.AnimationClips.size(); ++ClipIndex)
				{
					if (CheckCanceled()) return false;
					const FImportedAnimationClipData& Clip =
						Data->Scene.AnimationClips[ClipIndex];
					if (!Clip.Payload || Clip.SkeletonIndex >= Data->Scene.Skeletons.size())
						return false;
					const FImportedSkeletonData& Skeleton =
						Data->Scene.Skeletons[Clip.SkeletonIndex];
					FAssetPath ClipPath;
					if (!MakeSceneOutputPath(Decoded.DestinationDirectory, "Animations",
						MakeUniqueName(Clip.SuggestedName,
							std::format("Animation_{}", ClipIndex), AnimationNames),
						ClipPath, Error)) return false;
					uint64 PayloadBytes = sizeof(float)
						+ Clip.Payload->Tracks.size() * sizeof(FAnimationTrackData);
					for (const FAnimationTrackData& Track : Clip.Payload->Tracks)
						PayloadBytes += Track.Times.size() * sizeof(float)
							+ Track.VectorValues.size() * sizeof(FVector3f)
							+ Track.RotationValues.size() * sizeof(FVector4f);
					Builder.AddOutput({
						.StableIdentity = Clip.StableIdentity,
						.Role = std::format("AnimationClip.{:.6g}s", Clip.Payload->DurationSeconds),
						.AssetPath = ClipPath,
						.AssetClassName = "Durin::DAnimationClip",
						.Policy = EImportOutputPolicy::Create,
						.Collision = EImportCollisionAction::Create,
						.EstimatedCpuBytes = PayloadBytes,
						.EstimatedDiskBytes = PayloadBytes});
					Data->Outputs.push_back({
						.StableIdentity = Clip.StableIdentity,
						.Kind = ESceneOutputKind::AnimationClip,
						.SourceIndex = ClipIndex,
						.SkeletonIdentity = Skeleton.StableIdentity});
				}
				for (const FSceneImportDiagnostic& Diagnostic : Data->Scene.Diagnostics)
				{
					if (Diagnostic.Severity != EImportDiagnosticSeverity::Warning) continue;
					Data->Warnings.push_back(Diagnostic.Message);
					EImportDiagnosticCategory Category = EImportDiagnosticCategory::ProviderFailure;
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
				Builder.SetProviderData<FSceneProviderPlanData>(std::move(Data));
				return true;
			}
		};

		auto MakeCandidatePath(const FAssetPath& TargetPath, FAssetPath& OutPath) -> bool
		{
			for (uint32 Suffix = 1; Suffix != 0; ++Suffix)
			{
				if (!FAssetPath::TryCreate(std::format(
					"{}_SceneCandidate_{}", TargetPath.ToString(), Suffix), OutPath)) return false;
				if (!Asset::FindResidentPackage(OutPath)
					&& !Asset::FindAssetExact(OutPath)) return true;
			}
			return false;
		}

		class FSceneCandidate final : public ISingleAssetCandidate
		{
		public:
			FSceneCandidate(DObject* InAsset, bool bInNewAsset)
				: AssetObject(InAsset), Package(InAsset ? InAsset->GetPackage() : nullptr),
				  bNewAsset(bInNewAsset) {}
			auto SetProspectiveSkeleton(DSkeleton* InSkeleton) -> void
			{
				ProspectiveSkeleton = InSkeleton;
			}
			auto GetAsset() const -> DObject* override { return AssetObject; }
			auto GetPackage() const -> DPackage* override { return Package; }
			auto IsNewAsset() const -> bool override { return bNewAsset; }
			auto GetAuthoredFingerprint() const -> std::string override
			{
				if (const auto* Mesh = Cast<DStaticMesh>(AssetObject))
					return Mesh->GetSourceImportData().SourceContentHash;
				if (const auto* Texture = Cast<DTexture2D>(AssetObject))
					return Texture->GetDerivedDataKey();
				std::string Fingerprint;
				std::string Error;
				return ComputeImportPackageFingerprint(Package, Fingerprint, Error)
					? Fingerprint : std::string{};
			}
			auto Validate(std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				bool bValid = AssetObject && Package
					&& (!AssetObject->IsA<DStaticMesh>()
						|| Cast<DStaticMesh>(AssetObject)->GetRenderData())
					&& (!AssetObject->IsA<DTexture2D>()
						|| (Cast<DTexture2D>(AssetObject)->GetPlatformData()
							&& Cast<DTexture2D>(AssetObject)->GetBuildStatus()
								== ETextureBuildStatus::Ready))
					&& (!AssetObject->IsA<DMaterialInstance>()
						|| Cast<DMaterialInstance>(AssetObject)->GetParent());
				std::string Error;
				if (bValid && AssetObject->IsA<DSkeleton>())
					bValid = Cast<DSkeleton>(AssetObject)->Validate(Error);
				else if (bValid && AssetObject->IsA<DSkeletalMesh>())
					bValid = ProspectiveSkeleton
						&& Cast<DSkeletalMesh>(AssetObject)->ValidateAgainstSkeleton(
							*ProspectiveSkeleton, Error);
				else if (bValid && AssetObject->IsA<DAnimationClip>())
					bValid = ProspectiveSkeleton
						&& Cast<DAnimationClip>(AssetObject)->ValidateAgainstSkeleton(
							*ProspectiveSkeleton, Error);
				if (!bValid) AddDiagnostic(OutDiagnostics,
					EImportDiagnosticCategory::ValidationFailure,
					"candidate-validation", Error.empty()
						? "Scene output candidate is incomplete." : Error);
				return bValid;
			}
			auto Abandon() noexcept -> void override
			{
				if (Package) (void)Asset::UnloadPackage(Package, Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
				Package = nullptr;
				AssetObject = nullptr;
			}

		private:
			DObject* AssetObject = nullptr;
			DPackage* Package = nullptr;
			DSkeleton* ProspectiveSkeleton = nullptr;
			bool bNewAsset = false;
		};

		template<typename T>
		class TNoFailExchange final : public IPreparedImportedStateExchange
		{
		public:
			TNoFailExchange(T& InTarget, T& InCandidate)
				: Target(&InTarget), Candidate(&InCandidate) {}
			auto Commit() noexcept -> void override
			{
				if (!bCommitted) { Target->ExchangeImportedState(*Candidate); bCommitted = true; }
			}
			auto Reverse() noexcept -> void override
			{
				if (bCommitted) { Target->ExchangeImportedState(*Candidate); bCommitted = false; }
			}
			auto Finalize() noexcept -> void override { Target = nullptr; Candidate = nullptr; }

		private:
			T* Target = nullptr;
			T* Candidate = nullptr;
			bool bCommitted = false;
		};

		template<typename TExchange>
		class TPreparedSceneExchange final : public IPreparedImportedStateExchange
		{
		public:
			explicit TPreparedSceneExchange(
				std::unique_ptr<TExchange> InExchange)
				: Exchange(std::move(InExchange)) {}
			auto Commit() noexcept -> void override { Exchange->Commit(); }
			auto Reverse() noexcept -> void override { Exchange->Reverse(); }
			auto Finalize() noexcept -> void override { Exchange->Finalize(); }

		private:
			std::unique_ptr<TExchange> Exchange;
		};

		using FStaticMeshExchange = TPreparedSceneExchange<FStaticMeshImportedStateExchange>;
		using FSkeletonExchange = TPreparedSceneExchange<FSkeletonImportedStateExchange>;
		using FSkeletalMeshExchange = TPreparedSceneExchange<FSkeletalMeshImportedStateExchange>;
		using FAnimationClipExchange = TPreparedSceneExchange<FAnimationClipImportedStateExchange>;

		auto FindSnapshotImageBytes(
			const FSourceSnapshot& Snapshot,
			const FImportedSceneData& Scene,
			const FImportedImage& Image,
			std::span<const uint8>& OutBytes,
			FSourcePath& OutSource) -> bool
		{
			if (!Image.EmbeddedEncodedBytes.empty())
			{
				OutBytes = Image.EmbeddedEncodedBytes;
				const FSourceSnapshotEntry* Root = Snapshot.FindSource("root");
				if (!Root) return false;
				OutSource = Root->SourcePath;
				return true;
			}
			if (!Image.ExternalDependencyIndex
				|| *Image.ExternalDependencyIndex >= Scene.Dependencies.size()) return false;
			const FSourcePath& Dependency =
				Scene.Dependencies[*Image.ExternalDependencyIndex].Source;
			const auto Source = std::ranges::find_if(
				Snapshot.GetSources(), [&](const FSourceSnapshotEntry& Entry) {
					return Entry.SourcePath == Dependency;
				});
			if (Source == Snapshot.GetSources().end()) return false;
			OutBytes = Source->GetBytes();
			OutSource = Source->SourcePath;
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
			const FSourcePath& RootSource,
			const FImportedImage& Image,
			std::string_view TextureIdentity,
			std::span<const uint8> Bytes) -> std::string
		{
			const std::filesystem::path RootPath(RootSource.Path);
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
			std::span<const uint8> EncodedBytes,
			ESceneTextureDerivation Derivation,
			float Scale,
			const FVector3f& ColorScale,
			std::vector<uint8>& OutBytes,
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
			std::vector<uint8> Pixels(Image.Pixels.size());
			for (size_t Offset = 0; Offset < Image.Pixels.size(); Offset += 4)
			{
				uint8 Red = Image.Pixels[Offset + 0];
				uint8 Green = Image.Pixels[Offset + 1];
				uint8 Blue = Image.Pixels[Offset + 2];
				if (Derivation == ESceneTextureDerivation::Red
					|| Derivation == ESceneTextureDerivation::Green
					|| Derivation == ESceneTextureDerivation::Blue
					|| Derivation == ESceneTextureDerivation::Alpha)
				{
					const size_t Channel = Derivation == ESceneTextureDerivation::Red ? 0
						: Derivation == ESceneTextureDerivation::Green ? 1
						: Derivation == ESceneTextureDerivation::Blue ? 2 : 3;
					Red = Green = Blue = Image.Pixels[Offset + Channel];
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
				Pixels[Offset + 0] = Blue;
				Pixels[Offset + 1] = Green;
				Pixels[Offset + 2] = Red;
				Pixels[Offset + 3] = 255;
			}
			OutBytes.assign(18, 0);
			OutBytes[2] = 2;
			OutBytes[12] = static_cast<uint8>(Image.Width & 0xff);
			OutBytes[13] = static_cast<uint8>((Image.Width >> 8) & 0xff);
			OutBytes[14] = static_cast<uint8>(Image.Height & 0xff);
			OutBytes[15] = static_cast<uint8>((Image.Height >> 8) & 0xff);
			OutBytes[16] = 32;
			OutBytes[17] = 0x28;
			OutBytes.insert(OutBytes.end(), Pixels.begin(), Pixels.end());
			OutError.clear();
			return true;
		}

		auto MakeDerivedImageSourcePath(
			const FSourcePath& RootSource,
			std::string_view TextureIdentity,
			std::span<const uint8> Bytes) -> std::string
		{
			const std::filesystem::path RootPath(RootSource.Path);
			return (RootPath.parent_path()
				/ (RootPath.stem().generic_string() + "_Derived")
				/ std::format("{}_{}.tga", StableSuffix(TextureIdentity),
					FXxHash128::HashBuffer(Bytes).ToString())).generic_string();
		}

		auto CreateOrLoadTarget(
			const FMultiOutputReconciliation& Entry,
			DObject*& OutTarget,
			DObject*& OutCandidate,
			bool& bOutNew,
			std::string& OutError) -> bool
		{
			bOutNew = Entry.ProposedAction == EMultiOutputProposedAction::Create;
			FAssetPath CandidatePath = Entry.ResolvedAssetPath;
			if (!bOutNew && !MakeCandidatePath(Entry.ResolvedAssetPath, CandidatePath))
			{
				OutError = "Could not allocate a Scene candidate path.";
				return false;
			}
			if (!bOutNew)
			{
				const FScopedSkeletalDerivedDataRepairLoad RepairLoad;
				const Asset::FAssetResult Load = Asset::LoadAsset(Entry.ResolvedAssetPath, OutTarget);
				if (!Load) { OutError = Load.Message; return false; }
			}
			DClass* Class = nullptr;
			if (Entry.AssetClassName == DStaticMesh::StaticClass()->GetQualifiedName().ToString())
				Class = DStaticMesh::StaticClass();
			else if (Entry.AssetClassName == DSkeleton::StaticClass()->GetQualifiedName().ToString())
				Class = DSkeleton::StaticClass();
			else if (Entry.AssetClassName == DSkeletalMesh::StaticClass()->GetQualifiedName().ToString())
				Class = DSkeletalMesh::StaticClass();
			else if (Entry.AssetClassName == DAnimationClip::StaticClass()->GetQualifiedName().ToString())
				Class = DAnimationClip::StaticClass();
			else if (Entry.AssetClassName == DMaterialInstance::StaticClass()->GetQualifiedName().ToString())
				Class = DMaterialInstance::StaticClass();
			else if (Entry.AssetClassName == DTexture2D::StaticClass()->GetQualifiedName().ToString())
				Class = DTexture2D::StaticClass();
			if (!Class)
			{
				OutError = "Scene plan contains an unsupported output class.";
				return false;
			}
			Asset::FAssetResult Create;
			if (Class == DStaticMesh::StaticClass())
			{
				DStaticMesh* AssetObject = nullptr;
				Create = Asset::CreateAsset(CandidatePath, AssetObject);
				OutCandidate = AssetObject;
			}
			else if (Class == DSkeleton::StaticClass())
			{
				DSkeleton* AssetObject = nullptr;
				Create = Asset::CreateAsset(CandidatePath, AssetObject);
				OutCandidate = AssetObject;
			}
			else if (Class == DSkeletalMesh::StaticClass())
			{
				DSkeletalMesh* AssetObject = nullptr;
				Create = Asset::CreateAsset(CandidatePath, AssetObject);
				OutCandidate = AssetObject;
			}
			else if (Class == DAnimationClip::StaticClass())
			{
				DAnimationClip* AssetObject = nullptr;
				Create = Asset::CreateAsset(CandidatePath, AssetObject);
				OutCandidate = AssetObject;
			}
			else if (Class == DMaterialInstance::StaticClass())
			{
				DMaterialInstance* AssetObject = nullptr;
				Create = Asset::CreateAsset(CandidatePath, AssetObject);
				OutCandidate = AssetObject;
			}
			else
			{
				DTexture2D* AssetObject = nullptr;
				Create = Asset::CreateAsset(CandidatePath, AssetObject);
				OutCandidate = AssetObject;
			}
			if (!Create) { OutError = Create.Message; return false; }
			return true;
		}
	}

	auto CreateSceneImportProvider() -> std::shared_ptr<IImportProvider>
	{
		return std::make_shared<FSceneProvider>();
	}

	auto RollbackSceneSourceBundle(FPreparedSceneSourceBundle& Bundle) -> void
	{
		for (auto It = Bundle.Sources.rbegin(); It != Bundle.Sources.rend(); ++It)
			RollbackMountedSourceFile(*It);
		Bundle = {};
	}

	auto CommitSceneSourceBundle(FPreparedSceneSourceBundle& Bundle) -> void
	{
		for (FMountedSourceFile& Source : Bundle.Sources)
			CommitMountedSourceFile(Source);
	}

	auto PrepareSceneSourceBundle(
		const std::filesystem::path& InputRoot,
		std::string_view ReferencingContentPath,
		std::string_view ExternalIngestDestination,
		FPreparedSceneSourceBundle& OutBundle,
		std::string& OutError,
		bool bEngineAuthoringContext) -> bool
	{
		OutBundle = {};
		FScopedMountedSourceFile Root;
		if (!PrepareMountedSourceFile(InputRoot, ReferencingContentPath,
			ExternalIngestDestination, Root, OutError,
			bEngineAuthoringContext
				? EMountedSourceMutationContext::EngineAuthoring
				: EMountedSourceMutationContext::DependencySafe)) return false;
		OutBundle.RootSource = Root.SourcePath;
		OutBundle.Sources.push_back(std::move(Root));
		const std::string Extension = FoldAscii(
			InputRoot.extension().generic_string());
		if (Extension != ".gltf") return true;

		std::vector<uint8> RootBytes;
		if (!FFileHelper::LoadFileToArray(RootBytes, InputRoot)
			|| RootBytes.size() > MaxImportedSceneSourceBytes)
		{
			RollbackSceneSourceBundle(OutBundle);
			OutError = "The glTF root source cannot be read or exceeds the source limit.";
			return false;
		}
		const std::filesystem::path InputParent =
			std::filesystem::absolute(InputRoot).lexically_normal().parent_path();
		const std::filesystem::path TargetParent =
			std::filesystem::path(OutBundle.RootSource.Path).parent_path();
		const bool bVisited = VisitGltfUris(RootBytes,
			[&](std::string_view Uri, uint32) {
				const std::filesystem::path Relative =
					std::filesystem::path(Uri).lexically_normal();
				if (Relative.empty() || Relative.is_absolute()
					|| std::ranges::find(Relative, std::filesystem::path(".."))
						!= Relative.end())
				{
					OutError = std::format(
						"glTF dependency '{}' escapes the source document directory.", Uri);
					return false;
				}
				FScopedMountedSourceFile Dependency;
				if (!PrepareMountedSourceFile(
					InputParent / Relative, ReferencingContentPath,
					(TargetParent / Relative).generic_string(), Dependency, OutError,
					bEngineAuthoringContext
						? EMountedSourceMutationContext::EngineAuthoring
						: EMountedSourceMutationContext::DependencySafe))
					return false;
				OutBundle.Sources.push_back(std::move(Dependency));
				return true;
			});
		if (!bVisited)
		{
			RollbackSceneSourceBundle(OutBundle);
			if (OutError.empty())
				OutError = "glTF dependency discovery found an unsafe or unsupported URI.";
			return false;
		}
		return true;
	}

	auto FinalizeSceneImportPlan(
		const FSceneImportRequest& Request,
		FImportPlanResult Generic) -> FSceneImportPlanResult
	{
		FSceneImportPlanResult Result;
		FSceneDiagnosticScope DiagnosticScope(
			Result.bSucceeded, Result.Message, Result.Diagnostics, "scene-plan");
		if (!Generic)
		{
			Result.Message = std::move(Generic.Message);
			Result.Diagnostics = std::move(Generic.Diagnostics);
			return Result;
		}
		if (Request.ExistingRecord)
		{
			for (const FImportRecordOutput& Output : Request.ExistingRecord->GetOutputs())
			{
				if (Output.Policy != EImportRecordOutputPolicy::Managed) continue;
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Load = Asset::LoadAsset(Output.AssetPath, Loaded);
				if (!Load && Load.Error != Asset::EAssetError::NotFound)
				{
					Result.Message = Load.Message;
					return Result;
				}
			}
		}
		FAssetPath RecordPath;
		if (Request.ExistingRecord)
		{
			if (!Request.ExistingRecord->GetPackage()
				|| !FAssetPath::TryCreate(Request.ExistingRecord->GetPackage()->GetPackagePath(),
					RecordPath, &Result.Message)) return Result;
		}
		else
		{
			const std::string RecordName = SanitizeAssetName(
				std::filesystem::path(Request.RootSource.Path).stem().generic_string(),
				"Scene") + "_Import";
			if (!MakeSceneOutputPath(Request.DestinationDirectory, {}, RecordName,
				RecordPath, Result.Message)) return Result;
		}
		const auto Data = std::static_pointer_cast<const FSceneProviderPlanData>(
			Generic.Plan.GetProviderData());
		FImportRecordPayload ProviderState;
		if (!Data || !MakeSceneProviderState(
			*Data, Generic.Plan.GetOutputs(), ProviderState, Result.Message)) return Result;
		FMultiOutputPlanResult Multi = GetImportService().CreateMultiOutputImportPlan({
			.GenericPlan = std::move(Generic.Plan),
			.RecordPath = RecordPath,
			.ExistingRecord = Request.ExistingRecord,
			.ProviderState = std::move(ProviderState),
			.PrimaryOutput = {},
			.bRecreateMissingManagedOutputs = Request.bRecreateMissingManagedOutputs,
			.Progress = Request.Progress},
			GetImportRecordIndex());
		if (!Multi)
		{
			Result.Message = std::move(Multi.Message);
			Result.Diagnostics = std::move(Multi.Diagnostics);
			return Result;
		}
		const auto FinalData = std::static_pointer_cast<const FSceneProviderPlanData>(
			Multi.Plan.GetGenericPlan().GetProviderData());
		Result.Plan.MultiOutputPlan = std::move(Multi.Plan);
		if (FinalData) Result.Plan.Warnings = FinalData->Warnings;
		Result.Diagnostics.assign(
			Result.Plan.MultiOutputPlan.GetGenericPlan().GetDiagnostics().begin(),
			Result.Plan.MultiOutputPlan.GetGenericPlan().GetDiagnostics().end());
		Result.bSucceeded = true;
		return Result;
	}

	auto PlanSceneImport(const FSceneImportRequest& Request) -> FSceneImportPlanResult
	{
		FSceneImportPlanResult Invalid;
		if (Request.RootSource.IsEmpty() || !Request.DestinationDirectory.IsValid()
			|| !Request.MeshSettings.IsValid(&Invalid.Message))
		{
			if (Invalid.Message.empty()) Invalid.Message =
				"Scene import requires a mounted root source and destination directory.";
			return Invalid;
		}
		if (!GetImportService().IsImporterRegistered(SceneImportProviderId))
		{
			Invalid.Message = "The AssetForge Scene provider is unavailable.";
			return Invalid;
		}
		FImportPayload Settings;
		if (!MakeSceneSettings(
			Request.DestinationDirectory, Request.MeshSettings, Settings, Invalid.Message))
			return Invalid;
		FImportPlanResult Generic = GetImportService().CreateImportPlan({
			.RootSource = Request.RootSource,
			.ProviderId = std::string(SceneImportProviderId),
			.Settings = std::move(Settings),
			.Progress = Request.Progress});
		return FinalizeSceneImportPlan(Request, std::move(Generic));
	}

	auto FSceneImportAsyncPlanHandle::GetStatus() const
		-> EAsyncImportPlanStatus
	{
		if (bConsumed) return EAsyncImportPlanStatus::Invalid;
		if (ImmediateResult) return ImmediateResult->bSucceeded
			? EAsyncImportPlanStatus::Succeeded : EAsyncImportPlanStatus::Failed;
		return GenericHandle.GetStatus();
	}

	auto BeginSceneImportPlan(
		const FSceneImportRequest& Request,
		std::string_view OwnerId) -> FSceneImportAsyncPlanHandle
	{
		FSceneImportAsyncPlanHandle Handle;
		Handle.Request = Request;
		Handle.Request.Progress = nullptr;
		FSceneImportPlanResult Invalid;
		if (Request.RootSource.IsEmpty() || !Request.DestinationDirectory.IsValid()
			|| !Request.MeshSettings.IsValid(&Invalid.Message))
		{
			if (Invalid.Message.empty()) Invalid.Message =
				"Scene import requires a mounted root source and destination directory.";
			Handle.ImmediateResult = std::move(Invalid);
			return Handle;
		}
		if (!GetImportService().IsImporterRegistered(SceneImportProviderId))
		{
			Invalid.Message = "The AssetForge Scene provider is unavailable.";
			Handle.ImmediateResult = std::move(Invalid);
			return Handle;
		}
		FImportPayload Settings;
		if (!MakeSceneSettings(
			Request.DestinationDirectory, Request.MeshSettings, Settings, Invalid.Message))
		{
			Handle.ImmediateResult = std::move(Invalid);
			return Handle;
		}
		Handle.GenericHandle = GetImportService().LaunchAsyncImportPlan({
			.RootSource = Request.RootSource,
			.ProviderId = std::string(SceneImportProviderId),
			.Settings = std::move(Settings)}, OwnerId);
		return Handle;
	}

	auto PollSceneImportPlan(
		FSceneImportAsyncPlanHandle& Handle,
		FSceneImportPlanResult& OutResult) -> EAsyncImportPlanStatus
	{
		if (Handle.bConsumed) return EAsyncImportPlanStatus::Invalid;
		if (Handle.ImmediateResult)
		{
			OutResult = std::move(*Handle.ImmediateResult);
			Handle.ImmediateResult.reset();
			Handle.bConsumed = true;
			return OutResult ? EAsyncImportPlanStatus::Succeeded
				: EAsyncImportPlanStatus::Failed;
		}
		(void)DrainAsyncImportCompletionMailbox();
		FImportPlanResult Generic;
		const EAsyncImportPlanStatus Status =
			TryTakeAsyncImportPlanResult(Handle.GenericHandle, Generic);
		if (Status == EAsyncImportPlanStatus::Pending) return Status;
		if (Status == EAsyncImportPlanStatus::Succeeded)
			OutResult = FinalizeSceneImportPlan(Handle.Request, std::move(Generic));
		else
		{
			OutResult.Message = std::move(Generic.Message);
			OutResult.Diagnostics = std::move(Generic.Diagnostics);
			if (OutResult.Message.empty()) OutResult.Message =
				"Asynchronous Scene import preparation did not complete.";
		}
		Handle.bConsumed = true;
		return Status == EAsyncImportPlanStatus::Succeeded && !OutResult
			? EAsyncImportPlanStatus::Failed : Status;
	}

	auto CancelAndDrainSceneImportPlan(
		FSceneImportAsyncPlanHandle& Handle) -> void
	{
		if (Handle.bConsumed) return;
		if (Handle.GenericHandle)
			(void)GetImportService().CancelAndDrainAsyncImport(Handle.GenericHandle);
		Handle.ImmediateResult.reset();
		Handle.bConsumed = true;
	}

	auto PlanSceneReimport(
		DImportRecord& Record,
		bool bRecreateMissingManagedOutputs,
		IImportProgressReporter* Progress) -> FSceneImportPlanResult
	{
		FSceneImportPlanResult Result;
		if (Record.GetProviderId() != SceneImportProviderId
			|| Record.GetProviderContractVersion() != SceneImportProviderContractVersion)
		{
			Result.Message = "Import record is not owned by the supported Scene provider.";
			return Result;
		}
		const auto Root = std::ranges::find(
			Record.GetSources(), std::string_view("root"),
			&FImportRecordSource::StableIdentity);
		FDecodedSceneSettings Settings;
		FImportPayload Payload{
			.SchemaId = Record.GetSettings().SchemaId,
			.SchemaVersion = Record.GetSettings().SchemaVersion,
			.Bytes = Record.GetSettings().Bytes,
			.ContentHash = {
				Record.GetSettings().ContentHashLow,
				Record.GetSettings().ContentHashHigh}};
		if (Root == Record.GetSources().end()
			|| !DecodeSceneSettings(Payload, Settings, Result.Message)) return Result;
		const FScopedSkeletalDerivedDataRepairLoad RepairLoad;
		return PlanSceneImport({
			.RootSource = Root->SourcePath,
			.DestinationDirectory = Settings.DestinationDirectory,
			.MeshSettings = Settings.MeshSettings,
			.ExistingRecord = &Record,
			.bRecreateMissingManagedOutputs = bRecreateMissingManagedOutputs,
			.Progress = Progress});
	}

	auto ExecuteSceneImport(
		const FSceneImportPlan& Plan,
		const FMultiOutputExecutionOptions& Options) -> FSceneImportExecutionResult
	{
		FSceneImportExecutionResult Result;
		FSceneDiagnosticScope DiagnosticScope(
			Result.bSucceeded, Result.Message, Result.Diagnostics, "scene-execution");
		const auto Data = std::static_pointer_cast<const FSceneProviderPlanData>(
			Plan.MultiOutputPlan.GetGenericPlan().GetProviderData());
		if (!Data)
		{
			Result.Message = "Scene provider plan data is unavailable.";
			return Result;
		}
		const FImportPlan& GenericPlan = Plan.MultiOutputPlan.GetGenericPlan();
		const FXxHash128 SourceClosureHash = ComputeSourceClosureHash(GenericPlan);
		FXxHash128 ProviderStateHash;
		ProviderStateHash.HashLow = Plan.MultiOutputPlan.GetProviderState().ContentHashLow;
		ProviderStateHash.HashHigh = Plan.MultiOutputPlan.GetProviderState().ContentHashHigh;
		auto MakeDerivedDataKeyInput = [&](
			std::string_view StableIdentity,
			std::string_view CompatibilityIdentity,
			const FXxHash128& PayloadFingerprint) {
			return Asset::Build::FSkeletalBuildKeyFields{
				.ProviderIdentity = std::string(SceneImportProviderId),
				.ProviderVersion = SceneImportProviderContractVersion,
				.SourceClosureHash = SourceClosureHash,
				.SettingsHash = GenericPlan.GetSettings().ContentHash,
				.ProviderStateHash = ProviderStateHash,
				.PayloadInputFingerprint = PayloadFingerprint,
				.StableOutputIdentity = std::string(StableIdentity),
				.SkeletonCompatibilityIdentity = std::string(CompatibilityIdentity),
				.TargetPlatform = ESkeletalPayloadTargetPlatform::Win64,
				.TargetProfile = ESkeletalPayloadTargetProfile::Game};
		};
		std::string Error;
		FAssetPath StandardMaterialPath;
		DMaterial* StandardMaterial = nullptr;
		if (!FAssetPath::TryCreate(
			ImportedSurfaceMaterialPath, StandardMaterialPath, &Error))
		{
			Result.Message = std::move(Error);
			return Result;
		}
		const Asset::FAssetResult LoadStandard =
			Asset::LoadAsset(StandardMaterialPath, StandardMaterial);
		if (!LoadStandard || !StandardMaterial)
		{
			Result.Message = LoadStandard ? "Standard imported material is unavailable."
				: LoadStandard.Message;
			return Result;
		}

		std::unordered_map<std::string, const FSceneOutputData*> OutputData;
		for (const FSceneOutputData& Output : Data->Outputs)
			OutputData.emplace(Output.StableIdentity, &Output);
		FPreparedMultiOutputImport Prepared(
			Plan.MultiOutputPlan.GetGenericPlan().GetProvider());
		ReportImportProgress(Options.Progress, EImportPhase::CandidateBuild,
			EImportProgressState::Started);
		std::vector<FScopedMountedSourceFile> EmbeddedSources;
		std::unordered_map<std::string, DObject*> PublishedObjects;
		std::unordered_map<std::string, DObject*> ProspectiveObjects;
		auto FailPrepared = [&](std::string Message) -> FSceneImportExecutionResult {
			for (auto It = EmbeddedSources.rbegin(); It != EmbeddedSources.rend(); ++It)
				RollbackMountedSourceFile(*It);
			Result.Message = std::move(Message);
			ReportImportProgress(Options.Progress, EImportPhase::CandidateBuild,
				EImportProgressState::Failed, "root", "request", 0, 0, Result.Message);
			return std::move(Result);
		};
		auto IsCanceled = [&]() -> bool {
			return Options.IsCancellationRequested
				&& Options.IsCancellationRequested();
		};
		auto FailCanceled = [&]() -> FSceneImportExecutionResult {
			Result.Diagnostics.push_back({
				.Severity = EImportDiagnosticSeverity::Error,
				.Category = EImportDiagnosticCategory::Canceled,
				.Phase = "candidate-build",
				.SourceIdentity = "root",
				.OutputIdentity = "request",
				.Message = "Scene candidate preparation was canceled."});
			return FailPrepared("Scene candidate preparation was canceled.");
		};
		for (const FMultiOutputReconciliation& Entry
			: Plan.MultiOutputPlan.GetReconciliation())
		{
			if (IsCanceled()) return FailCanceled();
			if (Entry.ProposedAction != EMultiOutputProposedAction::Reference) continue;
			DObject* Referenced = nullptr;
			const Asset::FAssetResult Load = Asset::LoadAsset(Entry.ResolvedAssetPath, Referenced);
			if (!Load || !Referenced)
			{
				return FailPrepared(
					Load ? "Referenced Scene output is unavailable." : Load.Message);
			}
			PublishedObjects.emplace(Entry.StableIdentity, Referenced);
			ProspectiveObjects.emplace(Entry.StableIdentity, Referenced);
		}
		for (const FMultiOutputReconciliation& Entry
			: Plan.MultiOutputPlan.GetReconciliation())
		{
			if (IsCanceled()) return FailCanceled();
			if (Entry.ProposedAction != EMultiOutputProposedAction::Create
				&& Entry.ProposedAction != EMultiOutputProposedAction::ReplaceManaged) continue;
			DObject* Target = nullptr;
			DObject* Candidate = nullptr;
			bool bNew = false;
			if (!CreateOrLoadTarget(Entry, Target, Candidate, bNew, Error))
			{
				return FailPrepared(std::move(Error));
			}
			FPreparedMultiOutput Output{
				.StableIdentity = Entry.StableIdentity,
				.ExistingTarget = Target,
				.Candidate = std::make_unique<FSceneCandidate>(Candidate, bNew)};
			if (!bNew)
			{
				if (auto* MeshTarget = Cast<DStaticMesh>(Target))
				{
					auto* MeshCandidate = Cast<DStaticMesh>(Candidate);
					MeshCandidate->SeedMaterialReconciliationFrom(*MeshTarget);
				}
			}
			PublishedObjects.emplace(Entry.StableIdentity, bNew ? Candidate : Target);
			ProspectiveObjects.emplace(Entry.StableIdentity, Candidate);
			Prepared.Outputs.push_back(std::move(Output));
		}

		auto FindPrepared = [&](std::string_view Identity) -> FPreparedMultiOutput* {
			const auto It = std::ranges::find(
				Prepared.Outputs, Identity, &FPreparedMultiOutput::StableIdentity);
			return It == Prepared.Outputs.end() ? nullptr : &*It;
		};
		for (const auto& [Identity, Descriptor] : OutputData)
		{
			if (IsCanceled()) return FailCanceled();
			if (Descriptor->Kind != ESceneOutputKind::Skeleton) continue;
			FPreparedMultiOutput* Output = FindPrepared(Identity);
			if (!Output) continue;
			if (Descriptor->SourceIndex >= Data->Scene.Skeletons.size())
				return FailPrepared("Scene Skeleton candidate mapping is invalid.");
			auto* Skeleton = Cast<DSkeleton>(Output->Candidate->GetAsset());
			const FImportedSkeletonData& Imported =
				Data->Scene.Skeletons[Descriptor->SourceIndex];
			if (!Skeleton || !Skeleton->InitializeCanonicalBones(Imported.Bones, Error)
				|| Skeleton->GetCompatibilityIdentity() != Imported.CompatibilityIdentity)
			{
				return FailPrepared(Error.empty()
					? "Scene Skeleton candidate failed." : std::move(Error));
			}
			if (Output->ExistingTarget)
			{
				auto Exchange = Cast<DSkeleton>(Output->ExistingTarget)
					->PrepareImportedStateExchange(*Skeleton, Error);
				if (!Exchange) return FailPrepared(std::move(Error));
				Output->Exchange = std::make_unique<FSkeletonExchange>(std::move(Exchange));
			}
		}
		for (const auto& [Identity, Descriptor] : OutputData)
		{
			if (IsCanceled()) return FailCanceled();
			if (Descriptor->Kind != ESceneOutputKind::Texture2D) continue;
			FPreparedMultiOutput* Output = FindPrepared(Identity);
			if (!Output) continue;
			auto* Texture = Cast<DTexture2D>(Output->Candidate->GetAsset());
			if (!Texture || Descriptor->SourceIndex >= Data->Scene.Images.size())
			{
				return FailPrepared("Scene texture candidate mapping is invalid.");
			}
			const FImportedImage& Image = Data->Scene.Images[Descriptor->SourceIndex];
			if (!IsSceneSurfaceImageEncodingSupported(Image.Encoding))
				return FailPrepared("Scene surface image encoding is unsupported.");
			std::span<const uint8> Bytes;
			FSourcePath Source;
			if (!FindSnapshotImageBytes(Plan.MultiOutputPlan.GetGenericPlan().GetSnapshot(),
				Data->Scene, Image, Bytes, Source))
			{
				return FailPrepared("Scene image snapshot mapping is invalid.");
			}
			std::vector<uint8> DerivedBytes;
			if (Descriptor->TextureDerivation != ESceneTextureDerivation::None)
			{
				if (!BuildDerivedTextureBytes(Bytes, Descriptor->TextureDerivation,
					Descriptor->TextureDerivationScale,
					Descriptor->TextureDerivationColorScale, DerivedBytes, Error))
				{
					return FailPrepared(Error.empty()
						? "Scene derived texture generation failed." : std::move(Error));
				}
				FScopedMountedSourceFile DerivedSource;
				const FSourceSnapshotEntry* Root =
					Plan.MultiOutputPlan.GetGenericPlan().GetSnapshot().FindSource("root");
				if (!Root || !PrepareMountedSourceBytes(
					DerivedBytes,
					Plan.MultiOutputPlan.GetRecordPath().ToString(),
					MakeDerivedImageSourcePath(Root->SourcePath, Identity, DerivedBytes),
					DerivedSource,
					Error))
				{
					return FailPrepared(Error.empty()
						? "Derived Scene image source publication failed." : std::move(Error));
				}
				Bytes = DerivedBytes;
				Source = DerivedSource.SourcePath;
				EmbeddedSources.push_back(std::move(DerivedSource));
			}
			else if (!Image.EmbeddedEncodedBytes.empty())
			{
				FScopedMountedSourceFile EmbeddedSource;
				const FSourceSnapshotEntry* Root =
					Plan.MultiOutputPlan.GetGenericPlan().GetSnapshot().FindSource("root");
				if (!Root || !PrepareMountedSourceBytes(
					Bytes,
					Plan.MultiOutputPlan.GetRecordPath().ToString(),
					MakeEmbeddedImageSourcePath(Root->SourcePath, Image, Identity, Bytes),
					EmbeddedSource,
					Error))
				{
					return FailPrepared(
						Error.empty() ? "Embedded Scene image source publication failed."
							: std::move(Error));
				}
				Source = EmbeddedSource.SourcePath;
				EmbeddedSources.push_back(std::move(EmbeddedSource));
			}
			if (!BuildTexture2DCandidateFromSource(*Texture, Bytes, Source,
					{.Usage = Descriptor->TextureUsage,
						.bSRGB = Descriptor->TextureUsage == ETextureUsage::Color}, Error))
			{
				return FailPrepared(
					Error.empty() ? "Scene image candidate failed." : std::move(Error));
			}
			if (Output->ExistingTarget)
				Output->Exchange = std::make_unique<TNoFailExchange<DTexture2D>>(
					*Cast<DTexture2D>(Output->ExistingTarget), *Texture);
		}

		for (const auto& [Identity, Descriptor] : OutputData)
		{
			if (IsCanceled()) return FailCanceled();
			if (Descriptor->Kind != ESceneOutputKind::MaterialInstance) continue;
			FPreparedMultiOutput* Output = FindPrepared(Identity);
			if (!Output) continue;
			auto* Material = Cast<DMaterialInstance>(Output->Candidate->GetAsset());
			const auto Imported = std::ranges::find(
				Data->Scene.Materials, Descriptor->SourceIndex,
				&FImportedMaterial::SourceMaterialIndex);
			FMaterialStaticProperties StaticProperties = StandardMaterial->GetStaticProperties();
			if (Imported != Data->Scene.Materials.end())
			{
				StaticProperties.BlendMode = Imported->AlphaMode == EImportedAlphaMode::Mask
					? EMaterialBlendMode::Masked
					: Imported->AlphaMode == EImportedAlphaMode::Blend
						? EMaterialBlendMode::Translucent : EMaterialBlendMode::Opaque;
				StaticProperties.bTwoSided = Imported->bDoubleSided;
				StaticProperties.OpacityMaskThreshold = Imported->AlphaCutoff;
			}
			if (!Material || Imported == Data->Scene.Materials.end()
				|| !Material->SetParent(StandardMaterial)
				|| !Material->SetStaticPropertiesOverride(StaticProperties)
				|| !Material->SetVectorParameterValue(
					MaterialParameters::BaseColorName(), FVector3(Imported->BaseColorFactor))
				|| !Material->SetScalarParameterValue(
					MaterialParameters::OpacityName(), Imported->BaseColorFactor.a)
				|| !Material->SetScalarParameterValue(
					MaterialParameters::MetallicName(), Imported->MetallicFactor)
				|| !Material->SetScalarParameterValue(
					MaterialParameters::RoughnessName(), Imported->RoughnessFactor)
				|| !Material->SetScalarParameterValue(
					MaterialParameters::OpacityMaskName(), Imported->BaseColorFactor.a))
			{
				return FailPrepared("Scene material candidate mapping failed.");
			}
			const auto Occlusion = std::ranges::find(
				Imported->TextureBindings, EImportedTextureSemantic::Occlusion,
				&FImportedTextureBinding::Semantic);
			const bool bHasEmissiveTexture = std::ranges::any_of(
				Descriptor->TextureBindings, [](const FSceneMaterialTextureBinding& Binding) {
					return Binding.MaterialRole == 5;
				});
			if (!Material->SetScalarParameterValue(
					MaterialParameters::AmbientOcclusionName(),
					Occlusion == Imported->TextureBindings.end() ? 1.0f : Occlusion->Strength)
				|| !Material->SetVectorParameterValue(
					MaterialParameters::EmissiveName(), bHasEmissiveTexture
						? FVector3(0.0f) : FVector3(Imported->EmissiveFactor)))
			{
				return FailPrepared("Scene material factor mapping failed.");
			}
			const std::array<const FName*, 8> TextureNames{
				&MaterialParameters::BaseColorTextureName(),
				&MaterialParameters::NormalTextureName(),
				&MaterialParameters::MetallicTextureName(),
				&MaterialParameters::RoughnessTextureName(),
				&MaterialParameters::AmbientOcclusionTextureName(),
				&MaterialParameters::EmissiveTextureName(),
				&MaterialParameters::OpacityTextureName(),
				&MaterialParameters::OpacityMaskTextureName()};
			for (const FSceneMaterialTextureBinding& Binding : Descriptor->TextureBindings)
			{
				auto Texture = PublishedObjects.find(Binding.TextureIdentity);
				if (Texture == PublishedObjects.end()
					|| !Material->SetTextureParameterValue(
						*TextureNames[Binding.MaterialRole], Cast<DTexture2D>(Texture->second))
					|| !Material->SetParameterOverride(
						MaterialParameters::UVChannelIds[Binding.MaterialRole],
						EMaterialParameterType::Scalar,
						FMaterialParameterValue::MakeScalar(
							static_cast<float>(Binding.Binding.UVChannel)))
					|| !Material->SetParameterOverride(
						MaterialParameters::UVScaleIds[Binding.MaterialRole],
						EMaterialParameterType::Vector2,
						FMaterialParameterValue::MakeVector2(FVector2(Binding.Binding.Scale)))
					|| !Material->SetParameterOverride(
						MaterialParameters::UVOffsetIds[Binding.MaterialRole],
						EMaterialParameterType::Vector2,
						FMaterialParameterValue::MakeVector2(FVector2(Binding.Binding.Offset)))
					|| !Material->SetParameterOverride(
						MaterialParameters::UVRotationIds[Binding.MaterialRole],
						EMaterialParameterType::Scalar,
						FMaterialParameterValue::MakeScalar(Binding.Binding.RotationRadians))
					|| !Material->SetParameterOverride(
						MaterialParameters::SamplerStateIds[Binding.MaterialRole],
						EMaterialParameterType::Scalar,
						FMaterialParameterValue::MakeScalar(EncodeMaterialSamplerState(
							MakeMaterialSamplerState(Binding.Binding.Sampler)))))
				{
					return FailPrepared("Scene material texture mapping failed.");
				}
			}
			if (Output->ExistingTarget)
				Output->Exchange = std::make_unique<TNoFailExchange<DMaterialInstance>>(
					*Cast<DMaterialInstance>(Output->ExistingTarget), *Material);
		}

		auto FindMaterial = [&](uint32 SourceMaterialIndex) -> DMaterialInterface* {
			for (const FSceneOutputData& Descriptor : Data->Outputs)
			{
				if (Descriptor.Kind != ESceneOutputKind::MaterialInstance
					|| Descriptor.SourceIndex != SourceMaterialIndex) continue;
				const auto Object = PublishedObjects.find(Descriptor.StableIdentity);
				return Object == PublishedObjects.end()
					? nullptr : Cast<DMaterialInterface>(Object->second);
			}
			return nullptr;
		};
		for (const auto& [Identity, Descriptor] : OutputData)
		{
			if (IsCanceled()) return FailCanceled();
			if (Descriptor->Kind != ESceneOutputKind::SkeletalMesh) continue;
			FPreparedMultiOutput* Output = FindPrepared(Identity);
			if (!Output) continue;
			if (Descriptor->SourceIndex >= Data->Scene.SkeletalMeshes.size())
				return FailPrepared("Scene SkeletalMesh candidate mapping is invalid.");
			const auto FinalSkeletonObject = PublishedObjects.find(Descriptor->SkeletonIdentity);
			const auto ProspectiveSkeletonObject = ProspectiveObjects.find(Descriptor->SkeletonIdentity);
			auto* FinalSkeleton = FinalSkeletonObject == PublishedObjects.end()
				? nullptr : Cast<DSkeleton>(FinalSkeletonObject->second);
			auto* ProspectiveSkeleton = ProspectiveSkeletonObject == ProspectiveObjects.end()
				? nullptr : Cast<DSkeleton>(ProspectiveSkeletonObject->second);
			auto* Mesh = Cast<DSkeletalMesh>(Output->Candidate->GetAsset());
			const FImportedSkeletalMeshData& Imported =
				Data->Scene.SkeletalMeshes[Descriptor->SourceIndex];
			if (!FinalSkeleton || !ProspectiveSkeleton || !Mesh
				|| Imported.SkeletonIndex >= Data->Scene.Skeletons.size())
				return FailPrepared("Scene SkeletalMesh Skeleton relationship is invalid.");
			std::vector<FSkeletalMeshMaterialSlotDefinition> MaterialSlots = Imported.MaterialSlots;
			for (FSkeletalMeshMaterialSlotDefinition& Slot : MaterialSlots)
			{
				Slot.DefaultMaterial = FindMaterial(Slot.SourceMaterialIndex);
				if (!Slot.DefaultMaterial)
					return FailPrepared("Scene SkeletalMesh material relationship is invalid.");
			}
			const FImportedSkeletonData& ImportedSkeleton =
				Data->Scene.Skeletons[Imported.SkeletonIndex];
			Asset::Build::FSkeletalMeshBuildKeyInput KeyInput;
			static_cast<Asset::Build::FSkeletalBuildKeyFields&>(KeyInput) =
				MakeDerivedDataKeyInput(
					Identity, ImportedSkeleton.CompatibilityIdentity, {});
			Asset::Build::FSkeletalMeshBuildProduct Product;
			if (!Asset::Build::BuildSkeletalMeshProduct({
					.SkeletonBoneCount = ProspectiveSkeleton->GetBoneCount(),
					.SkeletonCompatibilityIdentity = ImportedSkeleton.CompatibilityIdentity,
					.MeshNodeBindTransform = Imported.MeshNodeBindTransform,
					.MaterialSlotCount = static_cast<uint32>(MaterialSlots.size()),
					.Payload = Imported.Payload,
					.KeyInput = std::move(KeyInput)}, Product, Error))
				return FailPrepared(std::move(Error));
			if (!Mesh->PublishBuiltProduct({
					.Skeleton = FinalSkeleton,
					.ValidationSkeleton = ProspectiveSkeleton,
					.SkeletonCompatibilityIdentity = ImportedSkeleton.CompatibilityIdentity,
					.MeshNodeBindTransform = Product.MeshNodeBindTransform,
					.MaterialSlots = std::move(MaterialSlots),
					.Payload = std::move(Product.Payload),
					.DerivedDataKey = std::move(Product.DerivedDataKey),
					.DiagnosticMessage = std::move(Product.Diagnostic)}, Error))
			{
				return FailPrepared(Error.empty()
					? "Scene SkeletalMesh candidate failed." : std::move(Error));
			}
			static_cast<FSceneCandidate*>(Output->Candidate.get())
				->SetProspectiveSkeleton(ProspectiveSkeleton);
			if (Output->ExistingTarget)
			{
				auto Exchange = Cast<DSkeletalMesh>(Output->ExistingTarget)
					->PrepareImportedStateExchange(*Mesh, *ProspectiveSkeleton, Error);
				if (!Exchange) return FailPrepared(std::move(Error));
				Output->Exchange = std::make_unique<FSkeletalMeshExchange>(std::move(Exchange));
			}
		}

		for (const auto& [Identity, Descriptor] : OutputData)
		{
			if (IsCanceled()) return FailCanceled();
			if (Descriptor->Kind != ESceneOutputKind::AnimationClip) continue;
			FPreparedMultiOutput* Output = FindPrepared(Identity);
			if (!Output) continue;
			if (Descriptor->SourceIndex >= Data->Scene.AnimationClips.size())
				return FailPrepared("Scene AnimationClip candidate mapping is invalid.");
			const auto FinalSkeletonObject = PublishedObjects.find(Descriptor->SkeletonIdentity);
			const auto ProspectiveSkeletonObject = ProspectiveObjects.find(Descriptor->SkeletonIdentity);
			auto* FinalSkeleton = FinalSkeletonObject == PublishedObjects.end()
				? nullptr : Cast<DSkeleton>(FinalSkeletonObject->second);
			auto* ProspectiveSkeleton = ProspectiveSkeletonObject == ProspectiveObjects.end()
				? nullptr : Cast<DSkeleton>(ProspectiveSkeletonObject->second);
			auto* Clip = Cast<DAnimationClip>(Output->Candidate->GetAsset());
			const FImportedAnimationClipData& Imported =
				Data->Scene.AnimationClips[Descriptor->SourceIndex];
			if (!FinalSkeleton || !ProspectiveSkeleton || !Clip
				|| Imported.SkeletonIndex >= Data->Scene.Skeletons.size())
				return FailPrepared("Scene AnimationClip Skeleton relationship is invalid.");
			const FImportedSkeletonData& ImportedSkeleton =
				Data->Scene.Skeletons[Imported.SkeletonIndex];
			Asset::Build::FAnimationClipBuildKeyInput KeyInput;
			static_cast<Asset::Build::FSkeletalBuildKeyFields&>(KeyInput) =
				MakeDerivedDataKeyInput(
					Identity, ImportedSkeleton.CompatibilityIdentity, {});
			Asset::Build::FAnimationClipBuildProduct Product;
			if (!Asset::Build::BuildAnimationClipProduct({
					.SkeletonBoneCount = ProspectiveSkeleton->GetBoneCount(),
					.SkeletonCompatibilityIdentity = ImportedSkeleton.CompatibilityIdentity,
					.ClipName = FName(Imported.SuggestedName),
					.Payload = Imported.Payload,
					.KeyInput = std::move(KeyInput)}, Product, Error))
				return FailPrepared(std::move(Error));
			if (!Clip->PublishBuiltProduct({
					.Skeleton = FinalSkeleton,
					.ValidationSkeleton = ProspectiveSkeleton,
					.SkeletonCompatibilityIdentity = ImportedSkeleton.CompatibilityIdentity,
					.ClipName = Product.ClipName,
					.Payload = std::move(Product.Payload),
					.DerivedDataKey = std::move(Product.DerivedDataKey),
					.DiagnosticMessage = std::move(Product.Diagnostic)}, Error))
			{
				return FailPrepared(Error.empty()
					? "Scene AnimationClip candidate failed." : std::move(Error));
			}
			static_cast<FSceneCandidate*>(Output->Candidate.get())
				->SetProspectiveSkeleton(ProspectiveSkeleton);
			if (Output->ExistingTarget)
			{
				auto Exchange = Cast<DAnimationClip>(Output->ExistingTarget)
					->PrepareImportedStateExchange(*Clip, *ProspectiveSkeleton, Error);
				if (!Exchange) return FailPrepared(std::move(Error));
				Output->Exchange = std::make_unique<FAnimationClipExchange>(std::move(Exchange));
			}
		}

		const FSceneOutputData* MeshDescriptor = nullptr;
		for (const FSceneOutputData& Descriptor : Data->Outputs)
			if (Descriptor.Kind == ESceneOutputKind::StaticMesh) MeshDescriptor = &Descriptor;
		FPreparedMultiOutput* MeshOutput = MeshDescriptor
			? FindPrepared(MeshDescriptor->StableIdentity) : nullptr;
		if (MeshOutput)
		{
			if (IsCanceled()) return FailCanceled();
			auto* Mesh = Cast<DStaticMesh>(MeshOutput->Candidate->GetAsset());
			const FSourceSnapshotEntry* Root =
				Plan.MultiOutputPlan.GetGenericPlan().GetSnapshot().FindSource("root");
			Asset::Build::FStaticMeshBuildProduct Product;
			if (!Mesh || !Root
				|| !Asset::Build::FStaticMeshBuildOperations::BuildImportedProduct(
					Asset::Build::FStaticMeshBuildOperations::CaptureReconciliationSnapshot(*Mesh),
					MakeStaticMeshImportedData(Data->Scene),
					{
					.SourcePath = Root->SourcePath,
					.SourceContentHash = Root->ContentHash.ToString(),
					.ImporterId = std::string(SceneImportProviderId),
					.ImporterVersion = SceneImportProviderContractVersion,
					.ImportSettings = Data->MeshSettings},
					Root->SourcePath.Path, Product, Error)
				|| !Asset::Build::FStaticMeshBuildOperations::PublishImportedProduct(
					*Mesh, std::move(Product), Error))
			{
				return FailPrepared(
					Error.empty() ? "Scene mesh candidate failed." : std::move(Error));
			}
			for (const FSceneOutputData& Descriptor : Data->Outputs)
			{
				if (Descriptor.Kind != ESceneOutputKind::MaterialInstance) continue;
				auto Material = PublishedObjects.find(Descriptor.StableIdentity);
				if (Material == PublishedObjects.end()
					|| !Mesh->SetImportedDefaultMaterial(
						Descriptor.SourceIndex, Cast<DMaterialInstance>(Material->second), Error))
				{
					return FailPrepared(Error.empty()
						? "Scene mesh material mapping failed." : std::move(Error));
				}
			}
			if (MeshOutput->ExistingTarget)
			{
				auto Exchange = Cast<DStaticMesh>(MeshOutput->ExistingTarget)
					->PrepareImportedStateExchange(*Mesh, Error);
				if (!Exchange) return FailPrepared(std::move(Error));
				MeshOutput->Exchange =
					std::make_unique<FStaticMeshExchange>(std::move(Exchange));
			}
		}

		ReportImportProgress(Options.Progress, EImportPhase::CandidateBuild,
			EImportProgressState::Succeeded, "root", "request",
			Prepared.Outputs.size(), Prepared.Outputs.size());
		if (IsCanceled()) return FailCanceled();
		FMultiOutputExecutionResult Executed = GetImportService().ExecuteMultiOutputImport(
			Plan.MultiOutputPlan, std::move(Prepared), GetImportRecordIndex(), Options);
		if (Executed)
		{
			for (FMountedSourceFile& Source : EmbeddedSources)
				CommitMountedSourceFile(Source);
		}
		else
		{
			for (auto It = EmbeddedSources.rbegin(); It != EmbeddedSources.rend(); ++It)
				RollbackMountedSourceFile(*It);
		}
		Result.bSucceeded = Executed.bSucceeded;
		Result.Message = std::move(Executed.Message);
		Result.Record = Executed.Record;
		Result.OrphanedAssets = std::move(Executed.Orphans);
		Result.Diagnostics = std::move(Executed.Diagnostics);
		Result.Provider = std::move(Executed.Provider);
		for (DObject* Output : Executed.Outputs)
		{
			if (auto* Mesh = Cast<DStaticMesh>(Output)) Result.Meshes.push_back(Mesh);
			else if (auto* Skeleton = Cast<DSkeleton>(Output)) Result.Skeletons.push_back(Skeleton);
			else if (auto* SkeletalMesh = Cast<DSkeletalMesh>(Output))
				Result.SkeletalMeshes.push_back(SkeletalMesh);
			else if (auto* Clip = Cast<DAnimationClip>(Output))
				Result.AnimationClips.push_back(Clip);
			else if (auto* Material = Cast<DMaterialInstance>(Output))
				Result.Materials.push_back(Material);
			else if (auto* Texture = Cast<DTexture2D>(Output))
				Result.Textures.push_back(Texture);
		}
		return Result;
	}

	auto FindSceneImportRecordForOutput(
		const DObject& Output,
		std::string& OutError) -> DImportRecord*
	{
		if (!Output.GetPackage())
		{
			OutError = "Scene output is not packaged.";
			return nullptr;
		}
		FAssetPath OutputPath;
		if (!FAssetPath::TryCreate(
			Output.GetPackage()->GetPackagePath(), OutputPath, &OutError)) return nullptr;
		FImportRecordIndex& Index = GetImportRecordIndex();
		if (!Index.EnsureCurrent(OutError)) return nullptr;
		const std::vector<FImportRecordManagement> Managers = Index.FindManagers(OutputPath);
		if (Managers.size() != 1)
		{
			OutError = Managers.empty()
				? "Output is not managed by an import record."
				: "Output has conflicting import-record managers.";
			return nullptr;
		}
		DImportRecord* Record = nullptr;
		const Asset::FAssetResult Load = Asset::LoadAsset(Managers.front().RecordPath, Record);
		if (!Load || !Record)
		{
			OutError = Load.Message;
			return nullptr;
		}
		if (Record->GetProviderId() != SceneImportProviderId)
		{
			OutError = "Managing import record belongs to another provider.";
			return nullptr;
		}
		OutError.clear();
		return Record;
	}
}
