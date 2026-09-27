#pragma once

#include "Asset/EditorBulkData.h"
#include "DObject/Class.h"
#include "DObject/Property.h"

namespace Durin::Testing
{
	// Retains canonical stored source bytes behind the same counted range-read
	// boundary as a package. This measures source requests, not physical disk I/O.
	class FBuildSourceReadProbe final : public FPackageResource
	{
	public:
		explicit FBuildSourceReadProbe(FSharedByteBuffer InBytes)
			: FPackageResource(InBytes.GetSize()), Bytes(std::move(InBytes)) {}
	private:
		auto ReadRangeImpl(uint64 Offset, uint64 Size, const std::atomic_bool&)
			-> FPackageResourceReadResult override { return Bytes.MakeView(Offset, Size); }
		FSharedByteBuffer Bytes;
	};

	// Test-only access to an authored persistent field; no mutable bulk escape
	// hatch is added to the production source interface.
	template<typename TSource>
	auto AttachBuildSourceReadProbe(TSource& Source, const FEditorBulkData& Bulk, const char* Field)
		-> std::shared_ptr<FBuildSourceReadProbe>
	{
		auto Bytes = Bulk.GetPayload().Wait();
		if (!Bytes) return {};
		auto Probe = std::make_shared<FBuildSourceReadProbe>(*Bytes);
		auto Attached = FEditorBulkData::TryCreatePackageBacked(Bulk.GetInstanceId(), Bulk.GetPayloadId(),
			Bulk.GetPayloadSize(), {.Resource = Probe, .StoredSize = Bulk.GetPayloadSize()});
		auto* Property = TSource::StaticStruct()->FindPropertyByName(Field);
		if (!Attached || !Property) return {};
		*Property->template ContainerPtrToValuePtr<FEditorBulkData>(&Source) = std::move(*Attached);
		return Probe;
	}
}
