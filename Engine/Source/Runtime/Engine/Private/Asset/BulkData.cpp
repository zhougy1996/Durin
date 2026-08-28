#include "Asset/BulkData.h"

namespace Durin::Asset
{
	auto ValidateBulkDataDescriptor(
		const FBulkDataDescriptor& Descriptor,
		std::string* OutError) -> bool
	{
		if (!Descriptor.PayloadId.IsValid())
			return Fail("Bulk data identity requires a valid payload id.", OutError);
		if (OutError) OutError->clear();
		return true;
	}

	auto VerifyBulkDataBuffer(
		const FBulkDataDescriptor& Descriptor,
		const FSharedByteBuffer& Buffer,
		std::string* OutError) -> bool
	{
		if (!ValidateBulkDataDescriptor(Descriptor, OutError)) return false;
		if (Buffer.GetSize() != Descriptor.LogicalByteCount)
			return Fail("Bulk data logical size verification failed.", OutError);
		if (FXxHash128::HashBuffer(Buffer.GetBytes()) != Descriptor.ContentHash)
			return Fail("Bulk data content hash verification failed.", OutError);
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::TryCreate(
		FBulkDataDescriptor InDescriptor,
		FSharedByteBuffer InBuffer,
		FBulkData& OutValue,
		std::string* OutError) -> bool
	{
		if (!VerifyBulkDataBuffer(InDescriptor, InBuffer, OutError)) return false;

		FBulkData Candidate;
		Candidate.Descriptor = std::move(InDescriptor);
		Candidate.Buffer = std::move(InBuffer);
		OutValue = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}
}
