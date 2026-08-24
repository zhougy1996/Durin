#pragma once

#include "BuiltinImportSchema.h"

#include "AssetForge/Builtins/Texture2DImport.h"
#include "Texture/Texture2D.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
	inline constexpr std::string_view ImageTranslatorId = "Durin.Image";
		inline constexpr std::string_view Texture2DPlanningPassId = "Durin.Texture2D.Default";
	inline constexpr std::string_view Texture2DBuilderId = "Durin.Texture2D.Builder";
		inline constexpr std::string_view ImageNodeSchema = "Durin.Image.RGBA8";
		inline constexpr std::string_view Texture2DPlanSchema = "Durin.Texture2D.Plan";
		inline constexpr std::string_view EmptyTranslatorSettingsSchema =
			"Durin.Image.TranslatorSettings";
		struct FDecodedImageImportValue
		{
			FTextureSourceData SourceData;
			FSourcePath SourcePath;
			FXxHash128 SourceHash{};
			uint64 SourceFileSize = 0;
			int64 SourceLastWriteTime = 0;
		};

		struct FTexture2DImportPlan
		{
			FAssetPath Destination;
			FTexture2DImportSettings Settings;
			EImportOutputPolicy Policy = EImportOutputPolicy::Create;
		};

		auto EncodeImageImportValue(const FDecodedImageImportValue& Value)
			-> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendValue(Bytes, Value.SourceData.Width);
			AppendValue(Bytes, Value.SourceData.Height);
			AppendValue(Bytes, Value.SourceData.SourceChannelCount);
			AppendValue(Bytes, Value.SourceData.Format);
			AppendValue(Bytes, Value.SourceData.bHasTransparency);
			AppendValue(Bytes, Value.SourceHash.HashLow);
			AppendValue(Bytes, Value.SourceHash.HashHigh);
			AppendValue(Bytes, Value.SourceFileSize);
			AppendValue(Bytes, Value.SourceLastWriteTime);
			AppendString(Bytes, Value.SourcePath.Path);
			const uint64 PixelBytes = Value.SourceData.Pixels.size();
			AppendValue(Bytes, PixelBytes);
			Bytes.insert(Bytes.end(), Value.SourceData.Pixels.begin(),
				Value.SourceData.Pixels.end());
			return MakeSchemaPayload(std::string(ImageNodeSchema), 1, std::move(Bytes));
		}

		auto DecodeImageImportValue(
			const FSchemaPayload& Payload,
			FDecodedImageImportValue& OutValue,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != ImageNodeSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "Normalized image payload schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			uint64 PixelBytes = 0;
			if (!ReadValue(Bytes, OutValue.SourceData.Width)
				|| !ReadValue(Bytes, OutValue.SourceData.Height)
				|| !ReadValue(Bytes, OutValue.SourceData.SourceChannelCount)
				|| !ReadValue(Bytes, OutValue.SourceData.Format)
				|| !ReadValue(Bytes, OutValue.SourceData.bHasTransparency)
				|| !ReadValue(Bytes, OutValue.SourceHash.HashLow)
				|| !ReadValue(Bytes, OutValue.SourceHash.HashHigh)
				|| !ReadValue(Bytes, OutValue.SourceFileSize)
				|| !ReadValue(Bytes, OutValue.SourceLastWriteTime)
				|| !ReadString(Bytes, OutValue.SourcePath.Path)
				|| !ReadValue(Bytes, PixelBytes) || PixelBytes != Bytes.size())
			{
				OutError = "Normalized image payload is malformed.";
				return false;
			}
			OutValue.SourceData.Pixels.assign(Bytes.begin(), Bytes.end());
			if (!OutValue.SourceData.IsValid() || OutValue.SourcePath.IsEmpty())
			{
				OutError = "Normalized image payload contains invalid source data.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto EncodeTexture2DImportPlan(const FTexture2DImportPlan& Plan)
			-> FSchemaPayload
		{
			std::vector<std::byte> Bytes;
			AppendString(Bytes, Plan.Destination.ToString());
			AppendValue(Bytes, Plan.Policy);
			AppendValue(Bytes, Plan.Settings.Usage);
			AppendValue(Bytes, Plan.Settings.CompressionQuality);
			AppendValue(Bytes, Plan.Settings.AlphaMipMode);
			AppendValue(Bytes, Plan.Settings.AlphaCoverageThreshold);
			AppendValue(Bytes, Plan.Settings.MaxResolution);
			const int8 SRGB = !Plan.Settings.bSRGB ? -1
				: *Plan.Settings.bSRGB ? 1 : 0;
			AppendValue(Bytes, SRGB);
			return MakeSchemaPayload(std::string(Texture2DPlanSchema), 1,
				std::move(Bytes));
		}

		auto DecodeTexture2DImportPlan(
			const FSchemaPayload& Payload,
			FTexture2DImportPlan& OutPlan,
			std::string& OutError) -> bool
		{
			if (Payload.SchemaId != Texture2DPlanSchema || Payload.SchemaVersion != 1
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = "Texture2D plan payload schema, version, or hash is invalid.";
				return false;
			}
			std::span<const std::byte> Bytes(Payload.Bytes);
			std::string Destination;
			int8 SRGB = -1;
			if (!ReadString(Bytes, Destination)
				|| !ReadValue(Bytes, OutPlan.Policy)
				|| !ReadValue(Bytes, OutPlan.Settings.Usage)
				|| !ReadValue(Bytes, OutPlan.Settings.CompressionQuality)
				|| !ReadValue(Bytes, OutPlan.Settings.AlphaMipMode)
				|| !ReadValue(Bytes, OutPlan.Settings.AlphaCoverageThreshold)
				|| !ReadValue(Bytes, OutPlan.Settings.MaxResolution)
				|| !ReadValue(Bytes, SRGB) || !Bytes.empty()
				|| !FAssetPath::TryCreate(Destination, OutPlan.Destination, &OutError)
				|| (SRGB < -1 || SRGB > 1))
			{
				if (OutError.empty()) OutError = "Texture2D plan payload is malformed.";
				return false;
			}
			if (SRGB >= 0) OutPlan.Settings.bSRGB = SRGB != 0;
			OutError.clear();
			return true;
		}

		}
}
