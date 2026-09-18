#include "StaticMesh/StaticMeshSource.h"

#include "Materials/MeshMaterialSlot.h"
#include "Math/Operations.h"

namespace Durin
{
	namespace
	{
		struct FSourceReadCancelled {};
		struct FSourceReadControl
		{
			const std::function<bool()>& ShouldCancel;
			uint32 Work = 0;
			auto Check() const -> void
			{
				if (ShouldCancel && ShouldCancel()) throw FSourceReadCancelled{};
			}
			auto Tick() -> void
			{
				if (++Work < 256) return;
				Work = 0;
				Check();
			}
		};
		auto SerializeImportedString(FArchive& Ar, std::string& Value) -> void
		{
			SerializeBoundedString(Ar, Value, 4096);
		}

		template<typename TValue, typename FSerializeValue>
		auto SerializeImportedArray(FArchive& Ar, std::vector<TValue>& Values,
			uint64 MaximumCount, FSerializeValue&& SerializeValue, FSourceReadControl* Control = nullptr) -> void
		{
			uint64 Count = Values.size();
			Ar << Count;
			if (Ar.IsLoading() && !Ar.HasError())
			{
				constexpr uint64 MinimumWireBytes = [] {
					if constexpr (std::is_same_v<TValue, FStaticMeshImportedMaterialSlot>) return uint64{20};
					else if constexpr (std::is_same_v<TValue, FStaticMeshImportedMesh>) return uint64{84};
					else if constexpr (std::is_same_v<TValue, FVector2f>) return uint64{8};
					else if constexpr (std::is_same_v<TValue, FVector3f>) return uint64{12};
					else if constexpr (std::is_same_v<TValue, FVector4f>) return uint64{16};
					else return uint64{4};
				}();
				if (Count > MaximumCount || Count > MaximumStaticMeshSourceBytes / sizeof(TValue)
					|| Count > Ar.GetRemainingPayloadBytes() / MinimumWireBytes)
				{
					Ar.Fail(EArchiveFailureCode::LimitExceeded,
						"StaticMesh imported array exceeds its element limit.");
					return;
				}
				if (Control) Control->Check();
				Values.resize(static_cast<size_t>(Count));
			}
			for (TValue& Value : Values)
			{
				if (Control) Control->Tick();
				SerializeValue(Ar, Value);
				if (Ar.HasError()) return;
			}
		}

		auto SerializeStaticMeshSourceGeometry(
			FArchive& Ar, FStaticMeshDecodedGeometry& Value, FSourceReadControl* Control = nullptr) -> void
		{
			uint32 Schema = StaticMeshSourceGeometryPayloadVersion;
			Ar << Schema;
			if (Ar.IsLoading() && Schema != StaticMeshSourceGeometryPayloadVersion)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh source schema is incompatible.");
				return;
			}
			SerializeImportedArray(Ar, Value.MaterialSlots, MaximumMeshMaterialSlots,
				[](FArchive& Inner, FStaticMeshImportedMaterialSlot& Slot) {
					SerializeImportedString(Inner, Slot.Name);
					Inner << Slot.SourceMaterialIndex;
					SerializeImportedString(Inner, Slot.SourceName);
				}, Control);
			SerializeImportedArray(Ar, Value.Meshes, 65536,
				[Control](FArchive& Inner, FStaticMeshImportedMesh& Mesh) {
					SerializeImportedString(Inner, Mesh.Name);
					Inner << Mesh.SourceMaterialIndex;
					auto Vector2 = [](FArchive& A, FVector2f& V) { A << V.x << V.y; };
					auto Vector3 = [](FArchive& A, FVector3f& V) { A << V.x << V.y << V.z; };
					auto Vector4 = [](FArchive& A, FVector4f& V) { A << V.x << V.y << V.z << V.w; };
					SerializeImportedArray(Inner, Mesh.Positions, 50'000'000, Vector3, Control);
					SerializeImportedArray(Inner, Mesh.Normals, 50'000'000, Vector3, Control);
					SerializeImportedArray(Inner, Mesh.Tangents, 50'000'000, Vector4, Control);
					for (auto& UVs : Mesh.UVChannels)
						SerializeImportedArray(Inner, UVs, 50'000'000, Vector2, Control);
					SerializeImportedArray(Inner, Mesh.Colors, 50'000'000, Vector4, Control);
					SerializeImportedArray(Inner, Mesh.Indices, 150'000'000,
						[](FArchive& A, uint32& Index) { A << Index; }, Control);
				}, Control);
		}

		auto ValidateStaticMeshDecodedGeometry(
			const FStaticMeshDecodedGeometry& Value, FStaticMeshSourceError& OutError,
			uint64* OutWireBytes = nullptr, FSourceReadControl* Control = nullptr) -> bool
		{
			if (Value.MaterialSlots.empty() || Value.MaterialSlots.size() > MaximumMeshMaterialSlots
				|| Value.Meshes.empty() || Value.Meshes.size() > 65536)
			{
				OutError = {.Code = EStaticMeshSourceError::Counts, .SlotCount = Value.MaterialSlots.size(), .MeshCount = Value.Meshes.size(), .ExpectedSlotCount = MaximumMeshMaterialSlots, .ExpectedMeshCount = 65536};
				return false;
			}
			uint64 WireBytes = 20;
			const auto AddBytes = [&](std::string_view Field, uint64 Count, uint64 Width, uint64 MaximumCount) {
				if (Count > MaximumCount || Count > (MaximumStaticMeshSourceBytes - WireBytes) / Width)
				{
					OutError.Code = EStaticMeshSourceError::Limit;
					OutError.Field = Field;
					OutError.Actual = Count;
					OutError.Expected = MaximumCount;
					OutError.Width = Width;
					OutError.WireBytes = WireBytes;
					return false;
				}
				WireBytes += Count * Width;
				return true;
			};
			std::unordered_set<uint32> SourceMaterials;
			for (const FStaticMeshImportedMaterialSlot& Slot : Value.MaterialSlots)
			{
				if (!AddBytes("SlotMetadata", 20, 1, 20) || !AddBytes("SlotName", Slot.Name.size(), 1, 4096)
					|| !AddBytes("SlotSourceName", Slot.SourceName.size(), 1, 4096)) return false;
				if (!SourceMaterials.insert(Slot.SourceMaterialIndex).second)
				{
					OutError.Code = EStaticMeshSourceError::DuplicateMaterial;
					OutError.Actual = Slot.SourceMaterialIndex;
					return false;
				}
			}
			for (const FStaticMeshImportedMesh& Mesh : Value.Meshes)
			{
				OutError.MeshName = Mesh.Name;
				if (!AddBytes("MeshMetadata", 84, 1, 84) || !AddBytes("MeshName", Mesh.Name.size(), 1, 4096)
					|| !AddBytes("Positions", Mesh.Positions.size(), 12, 50'000'000)
					|| !AddBytes("Normals", Mesh.Normals.size(), 12, 50'000'000)
					|| !AddBytes("Tangents", Mesh.Tangents.size(), 16, 50'000'000)
					|| !AddBytes("Colors", Mesh.Colors.size(), 16, 50'000'000)
					|| !AddBytes("Indices", Mesh.Indices.size(), 4, 150'000'000)) return false;
				for (size_t Channel = 0; Channel < Mesh.UVChannels.size(); ++Channel)
				{
					if (AddBytes("UVs", Mesh.UVChannels[Channel].size(), 8, 50'000'000)) continue;
					OutError.Index = Channel;
					return false;
				}
				if (!SourceMaterials.contains(Mesh.SourceMaterialIndex))
				{
					OutError.Code = EStaticMeshSourceError::MissingMaterial;
					OutError.Actual = Mesh.SourceMaterialIndex;
					return false;
				}
				if (Mesh.Positions.empty() || Mesh.Indices.empty() || Mesh.Indices.size() % 3 != 0)
				{
					OutError.Code = Mesh.Positions.empty() || Mesh.Indices.empty() ? EStaticMeshSourceError::EmptyMesh : EStaticMeshSourceError::TriangleList;
					OutError.Field = Mesh.Positions.empty() ? "Positions" : "Indices";
					OutError.Actual = Mesh.Positions.empty() ? 0 : Mesh.Indices.size();
					OutError.Expected = OutError.Code == EStaticMeshSourceError::EmptyMesh ? 1 : 3;
					return false;
				}
				for (size_t Index = 0; Index < Mesh.Positions.size(); ++Index)
				{
					if (Control) Control->Tick();
					if (Math::IsFinite(Mesh.Positions[Index])) continue;
					OutError.Code = EStaticMeshSourceError::NonFinitePosition;
					OutError.Index = Index;
					OutError.Position = Mesh.Positions[Index];
					return false;
				}
				const auto ValidChannel = [&](std::string_view Field, const auto& Channel) {
					if (Channel.empty() || Channel.size() == Mesh.Positions.size()) return true;
					OutError.Code = EStaticMeshSourceError::ChannelLength;
					OutError.Field = Field;
					OutError.Actual = Channel.size();
					OutError.Expected = Mesh.Positions.size();
					return false;
				};
				if (!ValidChannel("Normals", Mesh.Normals) || !ValidChannel("Tangents", Mesh.Tangents)
					|| !ValidChannel("Colors", Mesh.Colors)) return false;
				for (size_t Channel = 0; Channel < Mesh.UVChannels.size(); ++Channel)
				{
					if (ValidChannel("UVs", Mesh.UVChannels[Channel])) continue;
					OutError.Index = Channel;
					return false;
				}
				for (size_t Offset = 0; Offset < Mesh.Indices.size(); ++Offset)
				{
					if (Control) Control->Tick();
					if (Mesh.Indices[Offset] < Mesh.Positions.size()) continue;
					OutError.Code = EStaticMeshSourceError::IndexRange;
					OutError.Index = Offset;
					OutError.Actual = Mesh.Indices[Offset];
					OutError.Expected = Mesh.Positions.size();
					return false;
				}
			}
			if (OutWireBytes) *OutWireBytes = WireBytes;
			OutError = {};
			return true;
		}

	}

	FStaticMeshSource::FStaticMeshSource(const FStaticMeshSource& Other)
	{
		*this = Other;
	}

	auto FStaticMeshSource::operator=(const FStaticMeshSource& Other)
		-> FStaticMeshSource&
	{
		if (this == &Other) return *this;
		std::scoped_lock Lock(ResidencyMutex, Other.ResidencyMutex);
		Geometry = Other.Geometry;
		MaterialSlotCount = Other.MaterialSlotCount;
		MeshCount = Other.MeshCount;
		ResidentGeometry = Other.ResidentGeometry;
		ResidentIdentity = Other.ResidentIdentity;
		return *this;
	}

	auto FormatStaticMeshSourceError(const FStaticMeshSourceError& Error) -> std::string
	{
		std::string_view Reason;
		switch (Error.Code)
		{
		case EStaticMeshSourceError::None: return {};
		case EStaticMeshSourceError::Read: return Error.ReadCause ? FormatPackageResourceReadError(*Error.ReadCause) : "StaticMesh source read failed.";
		case EStaticMeshSourceError::Counts: Reason = "has invalid slot or mesh counts."; break;
		case EStaticMeshSourceError::Limit: Reason = "exceeds its authored count or 1 GiB byte limit."; break;
		case EStaticMeshSourceError::DuplicateMaterial: Reason = "material source indices must be unique."; break;
		case EStaticMeshSourceError::MissingMaterial: Reason = "references a missing source material."; break;
		case EStaticMeshSourceError::EmptyMesh: Reason = "contains an empty mesh."; break;
		case EStaticMeshSourceError::TriangleList: Reason = "index count is not a triangle list."; break;
		case EStaticMeshSourceError::NonFinitePosition: Reason = "contains a non-finite position."; break;
		case EStaticMeshSourceError::ChannelLength: Reason = "vertex channel lengths must match positions."; break;
		case EStaticMeshSourceError::IndexRange: Reason = "contains an out-of-range index."; break;
		case EStaticMeshSourceError::InvalidHeader: Reason = "source header is missing or invalid."; break;
		case EStaticMeshSourceError::PayloadSize: Reason = "payload size does not match metadata."; break;
		case EStaticMeshSourceError::Archive: Reason = "Archive decoding failed."; break;
		case EStaticMeshSourceError::EncodeArchive: Reason = "Archive encoding failed."; break;
		case EStaticMeshSourceError::EncodedSize: Reason = "exceeds the 1 GiB authored limit."; break;
		case EStaticMeshSourceError::BulkUpdate:
			return Error.BulkCause ? FormatEditorBulkDataError(*Error.BulkCause) : "StaticMesh source Bulk update failed.";
		case EStaticMeshSourceError::MetadataCounts: Reason = "source counts do not match metadata."; break;
		case EStaticMeshSourceError::Cancelled: Reason = "read was cancelled."; break;
		}
		return std::format("StaticMesh canonical geometry {}", Reason);
	}

	auto FStaticMeshSource::Initialize(
		FStaticMeshDecodedGeometry Value) -> FStaticMeshSourceResult
	{
		uint64 WireBytes = 0;
		FStaticMeshSourceError Validation;
		if (!ValidateStaticMeshDecodedGeometry(Value, Validation, &WireBytes))
		{
			return {std::move(Validation)};
		}
		FByteBuffer Bytes;
		Bytes.reserve(static_cast<size_t>(WireBytes));
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::BulkData);
		SerializeStaticMeshSourceGeometry(Ar, Value);
		if (Ar.HasError())
			return {{.Code = EStaticMeshSourceError::EncodeArchive,
				.ArchiveCode = Ar.GetFailure()->Code, .ArchivePath = Ar.GetFailure()->Path}};
		if (Bytes.size() > MaximumStaticMeshSourceBytes)
			return {{.Code = EStaticMeshSourceError::EncodedSize, .Actual = Bytes.size(), .Expected = MaximumStaticMeshSourceBytes}};
		FStaticMeshSource Candidate;
		Candidate.Geometry = Geometry;
		if (const auto Updated = Candidate.Geometry.UpdatePayload(FSharedByteBuffer::Take(std::move(Bytes))); !Updated)
		{
			return {{.Code = EStaticMeshSourceError::BulkUpdate, .BulkCause = Updated.Error}};
		}
		Candidate.MaterialSlotCount = static_cast<uint32>(Value.MaterialSlots.size());
		Candidate.MeshCount = static_cast<uint32>(Value.Meshes.size());
		Candidate.ResidentGeometry = std::make_shared<const FStaticMeshDecodedGeometry>(std::move(Value));
		Candidate.ResidentIdentity = Candidate.GetIdentity();
		*this = Candidate;
		return {};
	}

	auto FStaticMeshSource::AcquireGeometry(const std::function<bool()>& ShouldCancel) const
		-> FStaticMeshSourceReadResult
	{
		std::lock_guard Lock(ResidencyMutex);
		FSourceReadControl Control{ShouldCancel};
		try
		{
			Control.Check();
			const FXxHash128 Identity = GetIdentity();
			if (ResidentGeometry && ResidentIdentity == Identity) return {.Geometry = ResidentGeometry};
			ResidentGeometry.reset();
			if (!IsValid())
			{
				return {.Error = {.Code = EStaticMeshSourceError::InvalidHeader, .SlotCount = MaterialSlotCount, .MeshCount = MeshCount}};
			}
			const FPackageResourceReadResult Payload = Geometry.GetPayload().Wait();
			Control.Check();
			if (!Payload)
			{
				return {.Error = {.Code = EStaticMeshSourceError::Read, .ReadCause = Payload}};
			}
			const FByteView Bytes = Payload.Buffer.GetBytes();
			if (Bytes.size() != Geometry.GetPayloadSize() || Bytes.size() > MaximumStaticMeshSourceBytes)
			{
				return {.Error = {.Code = EStaticMeshSourceError::PayloadSize, .Actual = Bytes.size(), .Expected = Geometry.GetPayloadSize()}};
			}
			auto Decoded = std::make_shared<FStaticMeshDecodedGeometry>();
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::BulkData);
			SerializeStaticMeshSourceGeometry(Ar, *Decoded, &Control);
			if (Ar.HasError() || !RequireArchiveEnd(Ar))
			{
				return {.Error = {.Code = EStaticMeshSourceError::Archive, .Actual = Ar.Tell(), .Expected = Bytes.size(),
					.ArchiveCode = Ar.GetFailure()->Code, .ArchivePath = Ar.GetFailure()->Path}};
			}
			if (Decoded->MaterialSlots.size() != MaterialSlotCount || Decoded->Meshes.size() != MeshCount)
			{
				return {.Error = {.Code = EStaticMeshSourceError::MetadataCounts, .SlotCount = Decoded->MaterialSlots.size(), .MeshCount = Decoded->Meshes.size(),
					.ExpectedSlotCount = MaterialSlotCount, .ExpectedMeshCount = MeshCount}};
			}
			FStaticMeshSourceError Validation;
			if (!ValidateStaticMeshDecodedGeometry(*Decoded, Validation, nullptr, &Control)) return {.Error = std::move(Validation)};
			Control.Check();
			ResidentIdentity = Identity;
			ResidentGeometry = std::move(Decoded);
			return {.Geometry = ResidentGeometry};
		}
		catch (const FSourceReadCancelled&)
		{
			return {.Error = {.Code = EStaticMeshSourceError::Cancelled}};
		}
	}

	auto FStaticMeshSource::ReleaseGeometry() const -> void
	{
		std::lock_guard Lock(ResidencyMutex);
		ResidentGeometry.reset();
	}

	auto FStaticMeshSource::IsGeometryResident() const -> bool
	{
		std::lock_guard Lock(ResidencyMutex);
		return ResidentGeometry && ResidentIdentity == GetIdentity();
	}

	auto FStaticMeshSource::IsValid() const -> bool
	{
		return MaterialSlotCount > 0 && MaterialSlotCount <= MaximumMeshMaterialSlots
			&& MeshCount > 0 && MeshCount <= 65536
			&& Geometry.GetPayloadSize() > 0
			&& Geometry.GetPayloadSize() <= MaximumStaticMeshSourceBytes;
	}

	auto FStaticMeshSource::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(StaticMeshSourceGeometryPayloadVersion);
		Builder.UpdateValue(MaterialSlotCount);
		Builder.UpdateValue(MeshCount);
		Builder.UpdateValue(Geometry.GetPayloadId());
		return Builder.Finalize();
	}

}
