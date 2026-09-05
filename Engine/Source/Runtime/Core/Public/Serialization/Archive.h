#pragma once

#include "CoreAPI.h"
#include "Hash/XxHash.h"
#include "Misc/Guid.h"
#include "Misc/Name.h"
#include "Serialization/SharedByteBuffer.h"

#include <bit>
#include <concepts>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Durin
{
	// Selects the direction of a bidirectional serialization operation.
	enum class EArchiveDirection : uint8 { Load, Save };

	// Identifies the semantic byte boundary without coupling Core to an asset family.
	enum class EArchivePurpose : uint8
	{
		Discovery,
		ObjectGraph,
		Duplicate,
		PropertySnapshot,
		EditableCopy,
		AuthoredPackage,
		DerivedDataKey,
		DerivedDataPayload,
		CookedPackage,
		CookedPayload,
		BulkData
	};

	// Advertises optional behavior implemented by an archive specialization.
	enum class EArchiveCapability : uint32
	{
		None = 0,
		StructuredFields = 1 << 0,
		RawBytes = 1 << 1,
		CanonicalMapOrder = 1 << 2,
		ObjectReferences = 1 << 3,
		SoftObjectReferences = 1 << 4,
		UnknownFieldRetention = 1 << 5,
		RemainingPayload = 1 << 6,
		CustomVersions = 1 << 7,
		MultiPassDiscovery = 1 << 8,
		Position = 1 << 9
	};

	constexpr auto operator|(EArchiveCapability Left, EArchiveCapability Right) -> EArchiveCapability
	{
		return static_cast<EArchiveCapability>(static_cast<uint32>(Left) | static_cast<uint32>(Right));
	}
	constexpr auto operator&(EArchiveCapability Left, EArchiveCapability Right) -> EArchiveCapability
	{
		return static_cast<EArchiveCapability>(static_cast<uint32>(Left) & static_cast<uint32>(Right));
	}
	constexpr auto operator|=(EArchiveCapability& Left, EArchiveCapability Right) -> EArchiveCapability&
	{
		Left = Left | Right;
		return Left;
	}

	// Controls whether bulk bytes are serialized inline, omitted, or referenced externally.
	enum class EArchiveBulkDataPolicy : uint8 { Inline, Skip, External };

	enum class EArchiveBulkDataStorageKind : uint8 { Inline, External };

	// Selects whether a field accepts archive policy or requires one physical placement.
	enum class EArchiveBulkDataStoragePolicy : uint8
	{
		ArchiveDefault,
		ForceInline,
		AllowExternal,
	};

	// Supplies non-wire context required to serialize one BulkData field.
	struct FArchiveBulkDataParameters
	{
		const void* Owner = nullptr;
		uint64 ElementSize = 1;
		uint32 Alignment = 1;
		EArchiveBulkDataStoragePolicy StoragePolicy = EArchiveBulkDataStoragePolicy::ArchiveDefault;
		uint64 CookIndex = 0;
	};

	// Carries one archive-owned BulkData capture or load candidate.
	struct FArchiveBulkDataValue
	{
		FGuid PayloadId;
		uint64 LogicalSize = 0;
		uint64 StoredSize = 0;
		FXxHash128 ContentHash;
		FXxHash128 ContainerHash;
		EArchiveBulkDataStorageKind StorageKind = EArchiveBulkDataStorageKind::Inline;
		FSharedByteBuffer Buffer;
		std::shared_ptr<void> PackageResource;
		uint64 SegmentOffset = 0;
		uint32 Alignment = 1;
	};

	// Carries stable Cook target facts queried by serializers.
	struct FArchiveTarget
	{
		std::string Platform;
		std::string Profile;
	};

	// Carries orthogonal archive facts; purpose refines rather than replaces them.
	struct FArchiveState
	{
		EArchiveDirection Direction = EArchiveDirection::Save;
		EArchivePurpose Purpose = EArchivePurpose::ObjectGraph;
		EArchiveCapability Capabilities = EArchiveCapability::None;
		bool bPersistent = false;
		bool bCooking = false;
		bool bFilterEditorOnly = false;
		EArchiveBulkDataPolicy BulkDataPolicy = EArchiveBulkDataPolicy::Inline;
		FArchiveTarget Target;
	};

	struct FArchiveFormatVersion { FName Format; uint32 Version = 0; };
	struct FArchiveCustomVersion { FGuid Key; int32 Version = 0; };

	// Stores sorted or caller-defined format versions independently of wire bytes.
	struct FArchiveVersionContext
	{
		std::vector<FArchiveFormatVersion> Formats;
		std::vector<FArchiveCustomVersion> CustomVersions;

		CORE_API auto FindFormat(FName Format) const -> const FArchiveFormatVersion*;
		CORE_API auto FindCustom(const FGuid& Key) const -> const FArchiveCustomVersion*;
	};

	// Categorizes the first structured archive failure.
	enum class EArchiveFailureCode : uint8
	{
		UnsupportedCapability,
		UnsupportedType,
		UnsupportedOperation,
		InvalidData,
		TruncatedPayload,
		UnbalancedScope,
		MissingBaseReflectedFields,
		DuplicateBaseReflectedFields,
		DuplicateField,
		MalformedSerializer,
		InvalidObjectReference,
		InvalidPath,
		UnsupportedVersion,
		Overflow,
		LimitExceeded,
		InvalidAlignment,
		NonZeroPadding,
		TrailingData
	};

	struct FArchiveFailure
	{
		EArchiveFailureCode Code = EArchiveFailureCode::InvalidData;
		std::string Path;
		std::string Message;
	};

	enum class EArchiveLogicalPrimitiveKind : uint8
	{
		Bool,
		Int8, Int16, Int32, Int64,
		UInt8, UInt16, UInt32, UInt64,
		Float32, Float64,
		Guid,
	};
	enum class EArchiveLogicalTextKind : uint8 { String, Name };

	// Provides canonical little-endian primitives, context and sticky failure state.
	class FArchive
	{
	public:
		CORE_API explicit FArchive(FArchiveState State, FArchiveVersionContext Versions = {});
		virtual ~FArchive() = default;

		auto IsLoading() const -> bool { return State.Direction == EArchiveDirection::Load; }
		auto IsSaving() const -> bool { return State.Direction == EArchiveDirection::Save; }
		auto IsDiscovering() const -> bool { return State.Purpose == EArchivePurpose::Discovery; }
		auto IsPersistent() const -> bool { return State.bPersistent; }
		auto IsCooking() const -> bool { return State.bCooking; }
		auto IsFilterEditorOnly() const -> bool { return State.bFilterEditorOnly; }
		auto IsByteSwapping() const -> bool { return std::endian::native != std::endian::little; }
		auto GetPurpose() const -> EArchivePurpose { return State.Purpose; }
		auto GetBulkDataPolicy() const -> EArchiveBulkDataPolicy { return State.BulkDataPolicy; }
		auto GetTarget() const -> const FArchiveTarget& { return State.Target; }
		auto HasCapability(EArchiveCapability Capability) const -> bool
		{
			return (State.Capabilities & Capability) == Capability;
		}
		auto GetVersionContext() const -> const FArchiveVersionContext& { return Versions; }
		virtual auto GetLoadedDeprecatedProperties(FName) const -> std::span<const FName>
		{
			return {};
		}
		auto GetFailure() const -> const FArchiveFailure* { return Failure ? &*Failure : nullptr; }
		auto HasError() const -> bool { return Failure != nullptr; }
		CORE_API auto GetError() const -> std::string_view;
		CORE_API auto Fail(EArchiveFailureCode Code, std::string_view Message) -> void;
		auto SetError(std::string_view Message) -> void { Fail(EArchiveFailureCode::InvalidData, Message); }

		virtual CORE_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void;
		CORE_API auto Serialize(void* Data, uint64 Size) -> void;
		CORE_API auto WriteBytes(std::span<const std::byte> Bytes) -> void;
		CORE_API auto ReadBytes(std::span<std::byte> Bytes) -> void;
		// Transfers an owned byte Blob as a bounded count followed by exact bytes.
		// Loads commit only after the complete payload has been validated and read.
		CORE_API auto SerializeByteBlob(FByteArray& Bytes) -> void;
		// Transfers one atomic bulk value according to the selected physical policy.
		// Loading commits only a completely read and hash-verified resident candidate.
		virtual CORE_API auto SerializeBulkData(
			FArchiveBulkDataValue& Value,
			const FArchiveBulkDataParameters& Parameters) -> void;
		virtual auto Tell() const -> uint64 { return 0; }
		virtual auto GetRemainingPayloadBytes() const -> uint64 { return std::numeric_limits<uint64>::max(); }
		virtual auto IsCurrentFieldAvailable() const -> bool { return true; }

		CORE_API auto operator<<(bool& Value) -> FArchive&;
		CORE_API auto operator<<(int8& Value) -> FArchive&;
		CORE_API auto operator<<(int16& Value) -> FArchive&;
		CORE_API auto operator<<(int32& Value) -> FArchive&;
		CORE_API auto operator<<(int64& Value) -> FArchive&;
		CORE_API auto operator<<(uint8& Value) -> FArchive&;
		CORE_API auto operator<<(uint16& Value) -> FArchive&;
		CORE_API auto operator<<(uint32& Value) -> FArchive&;
		CORE_API auto operator<<(uint64& Value) -> FArchive&;
		CORE_API auto operator<<(float& Value) -> FArchive&;
		CORE_API auto operator<<(double& Value) -> FArchive&;
		CORE_API auto operator<<(FName& Value) -> FArchive&;
		CORE_API auto operator<<(FGuid& Value) -> FArchive&;
		CORE_API auto operator<<(std::string& Value) -> FArchive&;

		template<typename T> requires std::is_enum_v<T>
		auto operator<<(T& Value) -> FArchive&
		{
			if (!IsCurrentFieldAvailable()) return *this;
			using Underlying = std::underlying_type_t<T>;
			Underlying Encoded = static_cast<Underlying>(Value);
			*this << Encoded;
			if (IsLoading() && !HasError()) Value = static_cast<T>(Encoded);
			return *this;
		}

	protected:
		auto EnableCapabilities(EArchiveCapability Capabilities) -> void { State.Capabilities |= Capabilities; }
		CORE_API auto PushPath(std::string Segment) -> void;
		CORE_API auto PopPath() -> void;
		CORE_API auto GetPathString() const -> std::string;
		virtual CORE_API auto TryCaptureLogicalPrimitive(EArchiveLogicalPrimitiveKind Kind, const void* Value) -> bool;
		virtual CORE_API auto TryCaptureLogicalText(EArchiveLogicalTextKind Kind, std::string_view Value) -> bool;

	private:
		CORE_API auto FormatFailure() const -> std::string;
		friend CORE_API auto SerializeBoundedString(
			FArchive& Ar, std::string& Value, uint64 MaximumBytes) -> void;

		FArchiveState State;
		FArchiveVersionContext Versions;
		std::unique_ptr<FArchiveFailure> Failure;
		mutable std::string FormattedFailure;
		std::vector<std::string> PathSegments;
	};

	// Saves persistent canonical bytes into a caller-owned buffer.
	class FCanonicalMemoryWriter : public FArchive
	{
	public:
		CORE_API explicit FCanonicalMemoryWriter(
			FByteArray& Bytes,
			EArchivePurpose Purpose = EArchivePurpose::DerivedDataPayload,
			FArchiveState Context = {},
			FArchiveVersionContext Versions = {});
		CORE_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override;
		auto Tell() const -> uint64 override { return static_cast<uint64>(Bytes.size()); }

	private:
		FByteArray& Bytes;
	};

	// Loads persistent canonical bytes from a non-owning bounded span.
	class FCanonicalMemoryReader : public FArchive
	{
	public:
		CORE_API explicit FCanonicalMemoryReader(
			std::span<const std::byte> Bytes,
			EArchivePurpose Purpose = EArchivePurpose::DerivedDataPayload,
			FArchiveState Context = {},
			FArchiveVersionContext Versions = {});
		CORE_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override;
		CORE_API auto ReadRegion(uint64 Size, std::span<const std::byte>& OutRegion) -> bool;
		auto Tell() const -> uint64 override { return Offset; }
		auto GetRemainingPayloadBytes() const -> uint64 override
		{
			return static_cast<uint64>(Bytes.size()) - Offset;
		}

	private:
		std::span<const std::byte> Bytes;
		uint64 Offset = 0;
	};

	// Counts the canonical byte extent without allocating output storage.
	class FCountingArchive final : public FArchive
	{
	public:
		CORE_API explicit FCountingArchive(EArchivePurpose Purpose);
		CORE_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override;
		auto Tell() const -> uint64 override { return Count; }

	private:
		uint64 Count = 0;
	};

	// Hashes the exact canonical byte stream without retaining it.
	class FHashingArchive final : public FArchive
	{
	public:
		CORE_API explicit FHashingArchive(EArchivePurpose Purpose);
		CORE_API auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override;
		auto Tell() const -> uint64 override { return Count; }
		auto Finalize() const -> FXxHash128 { return Builder.Finalize(); }

	private:
		FXxHash128Builder Builder;
		uint64 Count = 0;
	};

	CORE_API auto SerializeByteBuffer(FArchive& Ar, FByteArray& Value, uint64 MaximumBytes) -> void;
	CORE_API auto SerializeBoundedString(FArchive& Ar, std::string& Value, uint64 MaximumBytes) -> void;
	CORE_API auto SerializeAlignment(FArchive& Ar, uint64 Alignment) -> void;
	CORE_API auto RequireArchiveEnd(FArchive& Ar) -> bool;

	template<typename T, typename F>
	auto SerializeBoundedSequence(
		FArchive& Ar, std::vector<T>& Values, uint64 MaximumCount, F&& SerializeElement) -> void
	{
		uint64 Count = Ar.IsSaving() ? static_cast<uint64>(Values.size()) : 0;
		Ar << Count;
		if (Ar.HasError()) return;
		if (Count > MaximumCount || Count > static_cast<uint64>(std::vector<T>().max_size()))
		{
			Ar.Fail(EArchiveFailureCode::LimitExceeded, "Sequence exceeds its serialization limit.");
			return;
		}
		if (Ar.IsLoading())
		{
			std::vector<T> Loaded(static_cast<size_t>(Count));
			for (T& Value : Loaded)
			{
				SerializeElement(Ar, Value);
				if (Ar.HasError()) return;
			}
			Values = std::move(Loaded);
			return;
		}
		for (T& Value : Values)
		{
			SerializeElement(Ar, Value);
			if (Ar.HasError()) return;
		}
	}

	template<typename T>
	concept CMemberArchiveSerializable = requires(T& Value, FArchive& Ar) { Value.Serialize(Ar); };
	template<typename T>
	concept CFreeArchiveSerializable = requires(T& Value, FArchive& Ar) { Serialize(Ar, Value); };

	template<CMemberArchiveSerializable T>
	auto operator<<(FArchive& Ar, T& Value) -> FArchive&
	{
		Value.Serialize(Ar);
		return Ar;
	}

	template<typename T> requires (!CMemberArchiveSerializable<T> && CFreeArchiveSerializable<T>)
	auto operator<<(FArchive& Ar, T& Value) -> FArchive&
	{
		Serialize(Ar, Value);
		return Ar;
	}
}
