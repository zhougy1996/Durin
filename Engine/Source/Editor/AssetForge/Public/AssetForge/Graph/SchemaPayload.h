#pragma once

#include "AssetForge/ImportTypes.h"

namespace Durin::AssetForge
{
inline constexpr uint32 AssetForgeContractVersion = 2;
	inline constexpr uint32 MaximumGraphNodes = 1'000'000;
	inline constexpr uint32 MaximumGraphDependencies = 4'000'000;
	inline constexpr uint32 MaximumImportDiagnostics = 4'096;
	inline constexpr uint64 MaximumSchemaPayloadBytes = 16ull * 1'024ull * 1'024ull * 1'024ull;

	// Bounds graph construction before provider-controlled values can allocate
	// unbounded framework storage.
	struct FGraphLimits
	{
		uint32 MaximumNodes = MaximumGraphNodes;
		uint32 MaximumDependencies = MaximumGraphDependencies;
		uint32 MaximumDiagnostics = MaximumImportDiagnostics;
		uint64 MaximumPayloadBytes = MaximumSchemaPayloadBytes;
	};

	// Owns one versioned payload. Persistent and cross-stage values use this
	// schema boundary instead of type-erased provider pointers.
	struct FSchemaPayload
	{
		std::string SchemaId;
		uint32 SchemaVersion = 0;
		std::vector<std::byte> Bytes;
		FXxHash128 ContentHash{};

		ASSETFORGE_API auto Finalize(std::string& OutError) -> bool;
		auto operator==(const FSchemaPayload&) const -> bool = default;
	};

	template<typename TValue>
	concept CSchemaPayloadValue = requires(
		std::span<const std::byte> Bytes, TValue& Value, std::string& Error)
	{
		{ TValue::SchemaId } -> std::convertible_to<std::string_view>;
		{ TValue::SchemaVersion } -> std::convertible_to<uint32>;
		{ TValue::DecodeSchemaPayload(Bytes, Value, Error) } -> std::same_as<bool>;
	};

	// Decodes only after exact schema identity and version validation.
	template<CSchemaPayloadValue TValue>
	auto DecodeSchemaPayload(
		const FSchemaPayload& Payload, TValue& OutValue, std::string& OutError) -> bool
	{
		if (Payload.SchemaId != std::string_view(TValue::SchemaId))
		{
			OutError = std::format("Durin.AssetForge.Diagnostic.SchemaMismatch: expected schema '{}' but received '{}'.",
				std::string_view(TValue::SchemaId), Payload.SchemaId);
			return false;
		}
		if (Payload.SchemaVersion != static_cast<uint32>(TValue::SchemaVersion))
		{
			OutError = std::format("SchemaVersionMismatch: schema '{}' expects version {} but received {}.",
				Payload.SchemaId, static_cast<uint32>(TValue::SchemaVersion), Payload.SchemaVersion);
			return false;
		}
		if (Payload.ContentHash != FXxHash128::HashBuffer(std::span<const std::byte>(Payload.Bytes)))
		{
			OutError = std::format("Durin.AssetForge.Diagnostic.PayloadHashMismatch: schema '{}' payload bytes do not match the recorded hash.",
				Payload.SchemaId);
			return false;
		}
		return TValue::DecodeSchemaPayload(Payload.Bytes, OutValue, OutError);
	}
}
