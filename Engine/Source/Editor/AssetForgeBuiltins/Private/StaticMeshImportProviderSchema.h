#pragma once

#include "BuiltinImportSchema.h"

#include "AssetForge/Builtins/ImportedScene.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "StaticMesh/StaticMeshBuildOperations.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
	inline constexpr std::string_view GeometryTranslatorId = "Durin.Geometry";
		inline constexpr std::string_view StaticMeshPlanningPassId = "Durin.StaticMesh.Default";
	inline constexpr std::string_view StaticMeshBuilderId = "Durin.StaticMesh.Builder";
		inline constexpr std::string_view StaticMeshNodeSchema =
			"Durin.Geometry.StaticMesh";
		inline constexpr std::string_view StaticMeshPlanSchema = "Durin.StaticMesh.Plan";
		inline constexpr std::string_view GeometryTranslatorSettingsSchema =
			"Durin.Geometry.TranslatorSettings";
		struct FDecodedStaticMeshImportValue
		{
			Asset::Build::FStaticMeshImportedData ImportedData;
			FSourcePath SourcePath;
			FXxHash128 SourceHash{};
		};

		struct FStaticMeshImportPlan
		{
			FAssetPath Destination;
			FStaticMeshImportSettings Settings;
			EImportOutputPolicy Policy = EImportOutputPolicy::Create;
		};

		auto EncodeGeometryTranslatorSettings(const FStaticMeshImportSettings& Settings)
			-> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, Settings.ForwardAxis);
			AppendValue(Bytes, Settings.RightAxis);
			AppendValue(Bytes, Settings.UpAxis);
			return MakeSchemaPayload(
				std::string(GeometryTranslatorSettingsSchema), 1, std::move(Bytes));
		}

		auto DecodeGeometryTranslatorSettings(
			const FSchemaPayload& Payload,
			FStaticMeshImportSettings& OutSettings,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != GeometryTranslatorSettingsSchema
				|| Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "Geometry translator settings schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			if (!ReadValue(Bytes, OutSettings.ForwardAxis)
				|| !ReadValue(Bytes, OutSettings.RightAxis)
				|| !ReadValue(Bytes, OutSettings.UpAxis)
				|| !Bytes.empty() || !OutSettings.IsValid(&OutError))
			{
				if (OutError.empty()) OutError = "Geometry translator settings are malformed.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto EncodeStaticMeshImportValue(
			const FDecodedStaticMeshImportValue& Value) -> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Value.SourcePath.Path);
			AppendValue(Bytes, Value.SourceHash.HashLow);
			AppendValue(Bytes, Value.SourceHash.HashHigh);
			AppendValue(Bytes, static_cast<uint64>(Value.ImportedData.MaterialSlots.size()));
			for (const Asset::Build::FStaticMeshImportedMaterialSlot& Slot
				: Value.ImportedData.MaterialSlots)
			{
				AppendString(Bytes, Slot.Name);
				AppendValue(Bytes, Slot.SourceMaterialIndex);
				AppendString(Bytes, Slot.SourceName);
			}
			AppendValue(Bytes, static_cast<uint64>(Value.ImportedData.Meshes.size()));
			for (const Asset::Build::FStaticMeshImportedMesh& Mesh
				: Value.ImportedData.Meshes)
			{
				AppendString(Bytes, Mesh.Name);
				AppendValue(Bytes, Mesh.SourceMaterialIndex);
				AppendTrivialVector(Bytes, std::span(Mesh.Positions));
				AppendTrivialVector(Bytes, std::span(Mesh.Normals));
				AppendTrivialVector(Bytes, std::span(Mesh.Tangents));
				for (const auto& UVs : Mesh.UVChannels)
					AppendTrivialVector(Bytes, std::span(UVs));
				AppendTrivialVector(Bytes, std::span(Mesh.Colors));
				AppendTrivialVector(Bytes, std::span(Mesh.Indices));
			}
			return MakeSchemaPayload(
				std::string(StaticMeshNodeSchema), 1, std::move(Bytes));
		}

		auto DecodeStaticMeshImportValue(
			const FSchemaPayload& Payload,
			FDecodedStaticMeshImportValue& OutValue,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != StaticMeshNodeSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "StaticMesh source-node payload schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			uint64 SlotCount = 0;
			uint64 MeshCount = 0;
			if (!ReadString(Bytes, OutValue.SourcePath.Path)
				|| !ReadValue(Bytes, OutValue.SourceHash.HashLow)
				|| !ReadValue(Bytes, OutValue.SourceHash.HashHigh)
				|| !ReadValue(Bytes, SlotCount) || SlotCount > MaxImportedSourceMaterials)
			{
				OutError = "StaticMesh source-node payload header is malformed.";
				return false;
			}
			OutValue.ImportedData.MaterialSlots.resize(static_cast<size_t>(SlotCount));
			for (auto& Slot : OutValue.ImportedData.MaterialSlots)
				if (!ReadString(Bytes, Slot.Name)
					|| !ReadValue(Bytes, Slot.SourceMaterialIndex)
					|| !ReadString(Bytes, Slot.SourceName))
				{
					OutError = "StaticMesh material-slot payload is malformed.";
					return false;
				}
			if (!ReadValue(Bytes, MeshCount) || MeshCount > MaxImportedSourceMeshes)
			{
				OutError = "StaticMesh mesh count exceeds its contract.";
				return false;
			}
			OutValue.ImportedData.Meshes.resize(static_cast<size_t>(MeshCount));
			constexpr uint64 MaximumElements = 268'435'456;
			for (auto& Mesh : OutValue.ImportedData.Meshes)
			{
				if (!ReadString(Bytes, Mesh.Name)
					|| !ReadValue(Bytes, Mesh.SourceMaterialIndex)
					|| !ReadTrivialVector(Bytes, Mesh.Positions, MaximumElements)
					|| !ReadTrivialVector(Bytes, Mesh.Normals, MaximumElements)
					|| !ReadTrivialVector(Bytes, Mesh.Tangents, MaximumElements))
				{
					OutError = "StaticMesh vertex payload is malformed.";
					return false;
				}
				for (auto& UVs : Mesh.UVChannels)
					if (!ReadTrivialVector(Bytes, UVs, MaximumElements))
					{
						OutError = "StaticMesh UV payload is malformed.";
						return false;
					}
				if (!ReadTrivialVector(Bytes, Mesh.Colors, MaximumElements)
					|| !ReadTrivialVector(Bytes, Mesh.Indices, MaximumElements))
				{
					OutError = "StaticMesh color or index payload is malformed.";
					return false;
				}
			}
			if (!Bytes.empty() || OutValue.SourcePath.IsEmpty()
				|| OutValue.ImportedData.Meshes.empty())
			{
				OutError = "StaticMesh source-node payload is incomplete.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto EncodeStaticMeshImportPlan(const FStaticMeshImportPlan& Plan)
			-> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.Destination.ToString());
			AppendValue(Bytes, Plan.Policy);
			AppendValue(Bytes, Plan.Settings.ForwardAxis);
			AppendValue(Bytes, Plan.Settings.RightAxis);
			AppendValue(Bytes, Plan.Settings.UpAxis);
			return MakeSchemaPayload(
				std::string(StaticMeshPlanSchema), 1, std::move(Bytes));
		}

		auto DecodeStaticMeshImportPlan(
			const FSchemaPayload& Payload,
			FStaticMeshImportPlan& OutPlan,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != StaticMeshPlanSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "StaticMesh plan payload schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			std::string Destination;
			if (!ReadString(Bytes, Destination)
				|| !ReadValue(Bytes, OutPlan.Policy)
				|| !ReadValue(Bytes, OutPlan.Settings.ForwardAxis)
				|| !ReadValue(Bytes, OutPlan.Settings.RightAxis)
				|| !ReadValue(Bytes, OutPlan.Settings.UpAxis)
				|| !Bytes.empty()
				|| !FAssetPath::TryCreate(Destination, OutPlan.Destination, &OutError)
				|| !OutPlan.Settings.IsValid(&OutError))
			{
				if (OutError.empty()) OutError = "StaticMesh plan payload is malformed.";
				return false;
			}
			OutError.clear();
			return true;
		}

		}
}
