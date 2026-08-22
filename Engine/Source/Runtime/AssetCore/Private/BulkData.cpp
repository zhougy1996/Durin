#include "Asset/BulkData.h"

#include "Misc/Failure.h"

namespace Durin::Asset
{
	namespace
	{
		auto IsConcreteDomain(EBulkDataStorageDomain Domain) -> bool
		{
			return Domain == EBulkDataStorageDomain::Authored
				|| Domain == EBulkDataStorageDomain::Derived
				|| Domain == EBulkDataStorageDomain::Cooked;
		}
	}

	auto ValidateBulkDataDescriptor(
		const FBulkDataDescriptor& Descriptor,
		std::string* OutError) -> bool
	{
		if (!Descriptor.PayloadId.IsValid() || !Descriptor.FormatId.IsValid()
			|| Descriptor.FormatVersion == 0)
			return Fail(
				"Bulk data identity requires valid payload and format ids plus a nonzero format version.",
				OutError);
		if (Descriptor.LogicalByteCount != 0 && Descriptor.StoredByteCount == 0)
			return Fail(
				"Non-empty bulk data cannot declare zero stored bytes.", OutError);
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

	auto FBulkData::TryCreateResident(
		FBulkDataDescriptor InDescriptor,
		FSharedByteBuffer InBuffer,
		EBulkDataStorageDomain InStorageDomain,
		FBulkData& OutValue,
		std::string* OutError) -> bool
	{
		if (!IsConcreteDomain(InStorageDomain))
			return Fail("Resident bulk data requires a concrete storage domain.", OutError);
		if (!VerifyBulkDataBuffer(InDescriptor, InBuffer, OutError)) return false;

		FBulkData Candidate;
		Candidate.Descriptor = std::move(InDescriptor);
		Candidate.StorageDomain = InStorageDomain;
		Candidate.Residency = EBulkDataResidency::Resident;
		Candidate.Buffer = std::move(InBuffer);
		OutValue = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::TryCreateUnloaded(
		FBulkDataDescriptor InDescriptor,
		std::shared_ptr<const IBulkDataProvider> InProvider,
		FBulkData& OutValue,
		std::string* OutError) -> bool
	{
		if (!ValidateBulkDataDescriptor(InDescriptor, OutError)) return false;
		if (!InProvider || !IsConcreteDomain(InProvider->GetStorageDomain()))
			return Fail(
				"Unloaded bulk data requires a provider with a concrete storage domain.",
				OutError);

		FBulkData Candidate;
		Candidate.Descriptor = std::move(InDescriptor);
		Candidate.StorageDomain = InProvider->GetStorageDomain();
		Candidate.Residency = EBulkDataResidency::Unloaded;
		Candidate.Provider = std::move(InProvider);
		OutValue = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}

	auto FBulkData::LoadSynchronous(std::string& OutError) -> bool
	{
		if (Residency == EBulkDataResidency::Resident)
		{
			OutError.clear();
			return true;
		}
		if (Residency == EBulkDataResidency::Failed)
		{
			OutError = Failure;
			return false;
		}
		if (!Provider)
		{
			Failure = "Bulk data has no synchronous provider.";
			Residency = EBulkDataResidency::Failed;
			OutError = Failure;
			return false;
		}

		FSharedByteBuffer Candidate;
		std::string CandidateError;
		if (!Provider->LoadSynchronous(Descriptor, Candidate, CandidateError))
		{
			Failure = CandidateError.empty()
				? "Bulk data provider failed without a diagnostic."
				: std::move(CandidateError);
			Residency = EBulkDataResidency::Failed;
			OutError = Failure;
			return false;
		}
		if (!VerifyBulkDataBuffer(Descriptor, Candidate, &CandidateError))
		{
			Failure = std::move(CandidateError);
			Residency = EBulkDataResidency::Failed;
			OutError = Failure;
			return false;
		}

		Buffer = std::move(Candidate);
		Residency = EBulkDataResidency::Resident;
		Failure.clear();
		OutError.clear();
		return true;
	}
}
