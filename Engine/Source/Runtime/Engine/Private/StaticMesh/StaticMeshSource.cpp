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
				if (Count > MaximumCount || Count > MaximumStaticMeshImportedDataBytes / sizeof(TValue)
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

		auto SerializeStaticMeshImportedValue(
			FArchive& Ar, FStaticMeshDecodedGeometry& Value, FSourceReadControl* Control = nullptr) -> void
		{
			uint32 Schema = StaticMeshImportedDataSchemaVersion;
			Ar << Schema;
			if (Ar.IsLoading() && Schema != StaticMeshImportedDataSchemaVersion)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh imported-data schema is incompatible.");
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
			const FStaticMeshDecodedGeometry& Value, std::string& OutError,
			uint64* OutWireBytes = nullptr, FSourceReadControl* Control = nullptr) -> bool
		{
			if (Value.MaterialSlots.empty() || Value.MaterialSlots.size() > MaximumMeshMaterialSlots
				|| Value.Meshes.empty() || Value.Meshes.size() > 65536)
			{
				OutError = "StaticMesh canonical geometry has invalid slot or mesh counts.";
				return false;
			}
			uint64 WireBytes = 20;
			const auto AddBytes = [&](uint64 Count, uint64 Width, uint64 MaximumCount) {
				if (Count > MaximumCount || Count > (MaximumStaticMeshImportedDataBytes - WireBytes) / Width)
				{
					OutError = "StaticMesh canonical geometry exceeds its authored count or 1 GiB byte limit.";
					return false;
				}
				WireBytes += Count * Width;
				return true;
			};
			std::unordered_set<uint32> SourceMaterials;
			for (const FStaticMeshImportedMaterialSlot& Slot : Value.MaterialSlots)
			{
				if (!AddBytes(20, 1, 20) || !AddBytes(Slot.Name.size(), 1, 4096)
					|| !AddBytes(Slot.SourceName.size(), 1, 4096)) return false;
				if (!SourceMaterials.insert(Slot.SourceMaterialIndex).second)
				{
					OutError = "StaticMesh canonical material source indices must be unique.";
					return false;
				}
			}
			for (const FStaticMeshImportedMesh& Mesh : Value.Meshes)
			{
				if (!AddBytes(84, 1, 84) || !AddBytes(Mesh.Name.size(), 1, 4096)
					|| !AddBytes(Mesh.Positions.size(), 12, 50'000'000)
					|| !AddBytes(Mesh.Normals.size(), 12, 50'000'000)
					|| !AddBytes(Mesh.Tangents.size(), 16, 50'000'000)
					|| !AddBytes(Mesh.Colors.size(), 16, 50'000'000)
					|| !AddBytes(Mesh.Indices.size(), 4, 150'000'000)) return false;
				for (const auto& UVs : Mesh.UVChannels)
					if (!AddBytes(UVs.size(), 8, 50'000'000)) return false;
				if (!SourceMaterials.contains(Mesh.SourceMaterialIndex)
					|| Mesh.Positions.empty() || Mesh.Indices.empty()
					|| Mesh.Indices.size() % 3 != 0
					|| !std::ranges::all_of(Mesh.Positions,
						[Control](const FVector3f& Position) { if (Control) Control->Tick(); return Math::IsFinite(Position); }))
				{
					OutError = "StaticMesh canonical geometry is malformed.";
					return false;
				}
				const auto ValidChannel = [&](const auto& Channel) {
					return Channel.empty() || Channel.size() == Mesh.Positions.size();
				};
				if (!ValidChannel(Mesh.Normals) || !ValidChannel(Mesh.Tangents)
					|| !ValidChannel(Mesh.Colors)
					|| !std::ranges::all_of(Mesh.UVChannels, ValidChannel))
				{
					OutError = "StaticMesh canonical vertex channel lengths must match positions.";
					return false;
				}
				for (uint32 Index : Mesh.Indices)
				{
					if (Control) Control->Tick();
					if (Index >= Mesh.Positions.size())
					{
						OutError = "StaticMesh canonical geometry contains an out-of-range index.";
						return false;
					}
				}
			}
			if (OutWireBytes) *OutWireBytes = WireBytes;
			OutError.clear();
			return true;
		}

	}

	FStaticMeshImportedData::FStaticMeshImportedData(const FStaticMeshImportedData& Other)
	{
		*this = Other;
	}

	auto FStaticMeshImportedData::operator=(const FStaticMeshImportedData& Other)
		-> FStaticMeshImportedData&
	{
		if (this == &Other) return *this;
		std::scoped_lock Lock(ResidencyMutex, Other.ResidencyMutex);
		Geometry = Other.Geometry;
		MaterialSlotCount = Other.MaterialSlotCount;
		MeshCount = Other.MeshCount;
		SchemaVersion = Other.SchemaVersion;
		ResidentGeometry = Other.ResidentGeometry;
		ResidentIdentity = Other.ResidentIdentity;
		return *this;
	}

	auto FStaticMeshImportedData::Initialize(
		FStaticMeshDecodedGeometry Value, std::string& OutError) -> bool
	{
		OutError.clear();
		uint64 WireBytes = 0;
		if (!ValidateStaticMeshDecodedGeometry(Value, OutError, &WireBytes)) return false;
		FByteBuffer Bytes;
		Bytes.reserve(static_cast<size_t>(WireBytes));
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::BulkData);
		SerializeStaticMeshImportedValue(Ar, Value);
		if (Ar.HasError() || Bytes.size() > MaximumStaticMeshImportedDataBytes)
		{
			OutError = Ar.HasError() ? Ar.GetFailure()->Message
				: "StaticMesh canonical geometry exceeds the 1 GiB authored limit.";
			return false;
		}
		FStaticMeshImportedData Candidate;
		Candidate.Geometry = Geometry;
		if (!Candidate.Geometry.UpdatePayload(FSharedByteBuffer::Take(std::move(Bytes))))
		{
			OutError = "StaticMesh canonical geometry could not be retained as authored bulk.";
			return false;
		}
		Candidate.MaterialSlotCount = static_cast<uint32>(Value.MaterialSlots.size());
		Candidate.MeshCount = static_cast<uint32>(Value.Meshes.size());
		Candidate.ResidentGeometry = std::make_shared<const FStaticMeshDecodedGeometry>(std::move(Value));
		Candidate.ResidentIdentity = Candidate.GetIdentity();
		*this = Candidate;
		return true;
	}

	auto FStaticMeshImportedData::AcquireGeometry(std::string& OutError,
		const std::function<bool()>& ShouldCancel) const
		-> FStaticMeshGeometryReadHandle
	{
		std::lock_guard Lock(ResidencyMutex);
		OutError.clear();
		FSourceReadControl Control{ShouldCancel};
		try
		{
			Control.Check();
			const FXxHash128 Identity = GetIdentity();
			if (ResidentGeometry && ResidentIdentity == Identity) return ResidentGeometry;
			ResidentGeometry.reset();
			if (!IsValid())
			{
				OutError = "StaticMesh canonical imported-data header is missing or invalid.";
				return {};
			}
			const FPackageResourceReadResult Payload = Geometry.GetPayload().Wait();
			Control.Check();
			if (!Payload)
			{
				OutError = Payload.Message.empty() ? "StaticMesh canonical geometry read failed." : Payload.Message;
				return {};
			}
			const FByteView Bytes = Payload.Buffer.GetBytes();
			if (Bytes.size() != Geometry.GetPayloadSize() || Bytes.size() > MaximumStaticMeshImportedDataBytes)
			{
				OutError = "StaticMesh canonical geometry payload size does not match metadata.";
				return {};
			}
			auto Decoded = std::make_shared<FStaticMeshDecodedGeometry>();
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::BulkData);
			SerializeStaticMeshImportedValue(Ar, *Decoded, &Control);
			if (Ar.HasError() || !RequireArchiveEnd(Ar))
			{
				OutError = Ar.GetFailure()->Message;
				return {};
			}
			if (Decoded->MaterialSlots.size() != MaterialSlotCount || Decoded->Meshes.size() != MeshCount)
			{
				OutError = "StaticMesh canonical imported-data counts are invalid.";
				return {};
			}
			if (!ValidateStaticMeshDecodedGeometry(*Decoded, OutError, nullptr, &Control)) return {};
			Control.Check();
			ResidentIdentity = Identity;
			ResidentGeometry = std::move(Decoded);
			return ResidentGeometry;
		}
		catch (const FSourceReadCancelled&)
		{
			OutError = "StaticMesh canonical geometry read was cancelled.";
			return {};
		}
	}

	auto FStaticMeshImportedData::ReleaseGeometry() const -> void
	{
		std::lock_guard Lock(ResidencyMutex);
		ResidentGeometry.reset();
	}

	auto FStaticMeshImportedData::IsGeometryResident() const -> bool
	{
		std::lock_guard Lock(ResidencyMutex);
		return ResidentGeometry && ResidentIdentity == GetIdentity();
	}

	auto FStaticMeshImportedData::IsValid() const -> bool
	{
		return SchemaVersion == StaticMeshImportedDataSchemaVersion
			&& MaterialSlotCount > 0 && MaterialSlotCount <= MaximumMeshMaterialSlots
			&& MeshCount > 0 && MeshCount <= 65536
			&& Geometry.GetPayloadSize() > 0
			&& Geometry.GetPayloadSize() <= MaximumStaticMeshImportedDataBytes;
	}

	auto FStaticMeshImportedData::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(MaterialSlotCount);
		Builder.UpdateValue(MeshCount);
		Builder.UpdateValue(Geometry.GetPayloadId());
		return Builder.Finalize();
	}

}
