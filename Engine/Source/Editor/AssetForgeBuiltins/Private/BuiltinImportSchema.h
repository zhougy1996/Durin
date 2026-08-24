#pragma once

#include "AssetForge/Graph/SchemaPayload.h"
#include "AssetForge/ImportTypes.h"

namespace Durin::AssetForge::Builtins
{
	namespace
	{
		template<typename T>
		auto AppendValue(std::vector<std::byte>& Bytes, const T& Value) -> void
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const std::span<const std::byte> ValueBytes =
				std::as_bytes(std::span{&Value, 1});
			Bytes.insert(Bytes.end(), ValueBytes.begin(), ValueBytes.end());
		}

		auto AppendString(std::vector<std::byte>& Bytes, std::string_view Value) -> void
		{
			AppendValue(Bytes, static_cast<uint64>(Value.size()));
			const std::span<const std::byte> ValueBytes = std::as_bytes(std::span(Value));
			Bytes.insert(Bytes.end(), ValueBytes.begin(), ValueBytes.end());
		}

		template<typename T>
		auto ReadValue(std::span<const std::byte>& Bytes, T& OutValue) -> bool
		{
			static_assert(std::is_trivially_copyable_v<T>);
			if (Bytes.size() < sizeof(T)) return false;
			std::memcpy(&OutValue, Bytes.data(), sizeof(T));
			Bytes = Bytes.subspan(sizeof(T));
			return true;
		}

		auto ReadString(std::span<const std::byte>& Bytes, std::string& OutValue) -> bool
		{
			uint64 Size = 0;
			if (!ReadValue(Bytes, Size) || Size > Bytes.size() || Size > 1'024 * 1'024)
				return false;
			OutValue.assign(reinterpret_cast<const char*>(Bytes.data()),
				static_cast<size_t>(Size));
			Bytes = Bytes.subspan(static_cast<size_t>(Size));
			return true;
		}

		template<typename T>
		auto AppendTrivialVector(
			std::vector<std::byte>& Bytes, std::span<const T> Values) -> void
		{
			static_assert(std::is_trivially_copyable_v<T>);
			AppendValue(Bytes, static_cast<uint64>(Values.size()));
			const std::span<const std::byte> ValueBytes = std::as_bytes(Values);
			Bytes.insert(Bytes.end(), ValueBytes.begin(), ValueBytes.end());
		}

		template<typename T>
		auto ReadTrivialVector(std::span<const std::byte>& Bytes,
			std::vector<T>& OutValues, uint64 MaximumCount) -> bool
		{
			static_assert(std::is_trivially_copyable_v<T>);
			uint64 Count = 0;
			if (!ReadValue(Bytes, Count) || Count > MaximumCount
				|| Count > Bytes.size() / sizeof(T)) return false;
			OutValues.resize(static_cast<size_t>(Count));
			const size_t ByteCount = static_cast<size_t>(Count) * sizeof(T);
			std::memcpy(OutValues.data(), Bytes.data(), ByteCount);
			Bytes = Bytes.subspan(ByteCount);
			return true;
		}

		auto MakeImportPayload(
			std::string SchemaId, uint32 Version, std::vector<std::byte> Bytes)
			-> FImportPayload
		{
			FImportPayload Payload{
				.SchemaId = std::move(SchemaId),
				.SchemaVersion = Version,
				.Bytes = std::move(Bytes)};
			std::string Error;
			requiref(Payload.Finalize(Error), "{}", Error);
			return Payload;
		}

		auto MakeSchemaPayload(
			std::string SchemaId, uint32 Version, std::vector<std::byte> Bytes)
			-> FSchemaPayload
		{
			FSchemaPayload Payload{
				.SchemaId = std::move(SchemaId),
				.SchemaVersion = Version,
				.Bytes = std::move(Bytes)};
			std::string Error;
			requiref(Payload.Finalize(Error), "{}", Error);
			return Payload;
		}

		auto ValidateSchemaPayload(const FSchemaPayload& Payload,
			std::string_view SchemaId, uint32 Version, std::string& OutError) -> bool
		{
			if (Payload.SchemaId != SchemaId || Payload.SchemaVersion != Version
				|| Payload.ContentHash != FXxHash128::HashBuffer(Payload.Bytes))
			{
				OutError = std::format("AssetForge payload '{}' is invalid.", SchemaId);
				return false;
			}
			return true;
		}
	}
}
