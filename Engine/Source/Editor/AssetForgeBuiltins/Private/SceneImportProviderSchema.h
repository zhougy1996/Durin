#pragma once

#include "StaticMeshImportProviderSchema.h"
#include "Texture2DImportProviderSchema.h"

#include "AssetForge/Persistence/ImportRecord.h"
#include "SceneImportInternal.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
	inline constexpr std::string_view SceneTranslatorId = "Durin.SceneGraph";
		inline constexpr std::string_view ScenePlanningPassId = "Durin.Scene.Default";
		inline constexpr std::string_view ScenePlanSchema = "Durin.Scene.Plan";
		inline constexpr std::string_view SceneNodeSchema = "Durin.Scene.Node";
		inline constexpr std::array<std::string_view, 7> SceneBuilderIds{
			"Durin.Scene.StaticMesh.Builder", "Durin.Scene.Material.Builder",
			"Durin.Scene.Skeleton.Builder", "Durin.Scene.SkeletalMesh.Builder",
			"Durin.Scene.AnimationClip.Builder", "Durin.Scene.Texture2D.Builder",
			"Durin.Scene.ImportRecord.Builder"};

		struct FSceneImportPlan
		{
			FAssetPath DestinationDirectory;
			FStaticMeshImportSettings MeshSettings;
			EImportOutputPolicy DefaultPolicy = EImportOutputPolicy::Create;
			std::vector<FOutputMapping> ExistingMappings;
		};

		struct FSceneCachedImportPlan
		{
			std::shared_ptr<const FSceneProviderPlanData> Data;
			std::shared_ptr<const FSourceSnapshot> Snapshot;
			std::vector<FImportOutputPreview> Outputs;
		};

		std::mutex GSceneImportCacheMutex;
		std::unordered_map<std::string, std::shared_ptr<const FSceneCachedImportPlan>>
			GSceneImportCache;
		std::unordered_map<std::string, std::vector<FImportOutputPreview>>
			GSceneImportOutputCache;

		auto MakeSceneOutputCacheKey(
			std::string_view SourceKey, const FAssetPath& Destination) -> std::string
		{
			return std::format("{}|{}", SourceKey, Destination.ToString());
		}

		auto EncodeSceneImportPlan(const FSceneImportPlan& Plan)
			-> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.DestinationDirectory.ToString());
			AppendValue(Bytes, Plan.DefaultPolicy);
			AppendValue(Bytes, Plan.MeshSettings.ForwardAxis);
			AppendValue(Bytes, Plan.MeshSettings.RightAxis);
			AppendValue(Bytes, Plan.MeshSettings.UpAxis);
			AppendValue(Bytes, static_cast<uint64>(Plan.ExistingMappings.size()));
			for (const FOutputMapping& Mapping : Plan.ExistingMappings)
			{
				AppendString(Bytes, Mapping.SourceNodeIdentity);
				AppendString(Bytes, Mapping.OutputIdentity);
				AppendString(Bytes, Mapping.AssetPath.ToString());
			}
			return MakeSchemaPayload(std::string(ScenePlanSchema), 1, std::move(Bytes));
		}

		auto EncodeSceneAuthoredSettings(const FSceneImportPlan& Plan)
			-> FSchemaPayload
		{
			return EncodeSceneImportPlan({
				.DestinationDirectory = Plan.DestinationDirectory,
				.MeshSettings = Plan.MeshSettings,
				.DefaultPolicy = EImportOutputPolicy::Create});
		}

		auto DecodeSceneImportPlan(
			const FSchemaPayload& Payload,
			FSceneImportPlan& OutPlan,
			std::string& OutError) -> bool
		{
			std::string Destination;
			std::span<const std::byte> Bytes(Payload.Bytes);
			if (Payload.SchemaId != ScenePlanSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes)
				|| !ReadString(Bytes, Destination)
				|| !ReadValue(Bytes, OutPlan.DefaultPolicy)
				|| !ReadValue(Bytes, OutPlan.MeshSettings.ForwardAxis)
				|| !ReadValue(Bytes, OutPlan.MeshSettings.RightAxis)
				|| !ReadValue(Bytes, OutPlan.MeshSettings.UpAxis)
				|| !FAssetPath::TryCreate(Destination, OutPlan.DestinationDirectory, &OutError)
				|| !OutPlan.MeshSettings.IsValid(&OutError))
			{
				if (OutError.empty()) OutError = "Scene AssetForge plan payload is malformed.";
				return false;
			}
			uint64 MappingCount = 0;
			if (!ReadValue(Bytes, MappingCount) || MappingCount > MaximumImportRecordOutputs)
			{
				OutError = "Scene AssetForge output mapping count is invalid.";
				return false;
			}
			OutPlan.ExistingMappings.clear();
			OutPlan.ExistingMappings.reserve(static_cast<size_t>(MappingCount));
			for (uint64 Index = 0; Index < MappingCount; ++Index)
			{
				std::string SourceIdentity;
				std::string OutputIdentity;
				std::string AssetPath;
				FAssetPath ParsedPath;
				if (!ReadString(Bytes, SourceIdentity) || !ReadString(Bytes, OutputIdentity)
					|| !ReadString(Bytes, AssetPath)
					|| !FAssetPath::TryCreate(AssetPath, ParsedPath, &OutError)) return false;
				OutPlan.ExistingMappings.push_back({.SourceNodeIdentity = std::move(SourceIdentity),
					.OutputIdentity = std::move(OutputIdentity), .AssetPath = std::move(ParsedPath)});
			}
			if (!Bytes.empty()) { OutError = "Scene AssetForge plan has trailing bytes."; return false; }
			OutError.clear();
			return true;
		}

		auto EncodeSceneNodeReference(std::string_view CacheKey, uint32 OutputIndex)
			-> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, CacheKey);
			AppendValue(Bytes, OutputIndex);
			return MakeSchemaPayload(std::string(SceneNodeSchema), 1, std::move(Bytes));
		}

		auto DecodeSceneNodeReference(
			const FSchemaPayload& Payload,
			std::shared_ptr<const FSceneCachedImportPlan>& OutPlan,
			const FSceneOutputData*& OutOutput,
			std::string& OutError) -> bool
		{
			std::span<const std::byte> Bytes(Payload.Bytes);
			std::string Key;
			uint32 Index = 0;
			if (Payload.SchemaId != SceneNodeSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes)
				|| !ReadString(Bytes, Key) || !ReadValue(Bytes, Index) || !Bytes.empty())
			{
				OutError = "Scene source-node reference is malformed.";
				return false;
			}
			{
				std::lock_guard Lock(GSceneImportCacheMutex);
				const auto It = GSceneImportCache.find(Key);
				if (It != GSceneImportCache.end()) OutPlan = It->second;
			}
			if (!OutPlan || !OutPlan->Data || Index >= OutPlan->Data->Outputs.size())
			{
				OutError = "Scene source-node immutable value is no longer available.";
				return false;
			}
			OutOutput = &OutPlan->Data->Outputs[Index];
			OutError.clear();
			return true;
		}

		auto SceneNodeKind(ESceneOutputKind Kind) -> std::string_view
		{
			switch (Kind)
			{
			case ESceneOutputKind::StaticMesh: return "Durin.Scene.StaticMesh";
			case ESceneOutputKind::MaterialInstance: return "Durin.Scene.Material";
			case ESceneOutputKind::Skeleton: return "Durin.Scene.Skeleton";
			case ESceneOutputKind::SkeletalMesh: return "Durin.Scene.SkeletalMesh";
			case ESceneOutputKind::AnimationClip: return "Durin.Scene.AnimationClip";
			case ESceneOutputKind::Texture2D: return "Durin.Scene.Texture2D";
			case ESceneOutputKind::ImportRecord: return "Durin.Scene.ImportRecord";
			}
			return {};
		}

		auto SceneAssetBuilderIndex(ESceneOutputKind Kind) -> size_t
		{
			switch (Kind)
			{
			case ESceneOutputKind::StaticMesh: return 0;
			case ESceneOutputKind::MaterialInstance: return 1;
			case ESceneOutputKind::Skeleton: return 2;
			case ESceneOutputKind::SkeletalMesh: return 3;
			case ESceneOutputKind::AnimationClip: return 4;
			case ESceneOutputKind::Texture2D: return 5;
			case ESceneOutputKind::ImportRecord: return 6;
			}
			return 0;
		}

		auto SceneOutputClassName(ESceneOutputKind Kind) -> std::string_view
		{
			switch (Kind)
			{
			case ESceneOutputKind::StaticMesh: return "Durin::DStaticMesh";
			case ESceneOutputKind::MaterialInstance: return "Durin::DMaterialInstance";
			case ESceneOutputKind::Skeleton: return "Durin::DSkeleton";
			case ESceneOutputKind::SkeletalMesh: return "Durin::DSkeletalMesh";
			case ESceneOutputKind::AnimationClip: return "Durin::DAnimationClip";
			case ESceneOutputKind::Texture2D: return "Durin::DTexture2D";
			case ESceneOutputKind::ImportRecord: return "Durin::AssetForge::DImportRecord";
			}
			return {};
		}

		}
}
