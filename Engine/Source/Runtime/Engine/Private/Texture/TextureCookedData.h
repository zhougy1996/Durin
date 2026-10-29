#pragma once

#include "CoreMinimal.h"

#include "DObject/Archive.h"

#include "Asset/BulkData.h"
#include "Logging/LogMacros.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"
#include "Asset/Load.h"
#include "DObject/WeakObjectPtr.h"
#include "Threading/Task.h"
#include "Threading/TaskComposition.h"
#include "../Asset/CookedArchiveTarget.h"

namespace Durin::TexturePrivate
{
	// The caller supplies the historical wire owner; storage ownership must not
	// change the native field identity or the family-specific payload codec.
	template<class TPlatformData>
	auto SerializeCookedPlatformData(FArchive& Ar, FBulkData& CookedData,
		TPlatformData* PlatformData, FName WireOwner, std::string_view Family) -> void
	{
		if ((Ar.GetTarget().Platform != "Win64"
				&& Ar.GetTarget().Platform != "MacOS")
			|| Ar.GetTarget().Profile != "Game")
		{
			Ar.Fail(EArchiveFailureCode::InvalidData,
				std::format("{} cooked platform data requires a Win64 or MacOS Game target.", Family));
			return;
		}
		FBulkData Projection;
		FBulkData* Value = &CookedData;
		if (Ar.IsSaving())
		{
			if (!PlatformData || !PlatformData->IsValid())
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					std::format("{} cooked platform data is unavailable.", Family));
				return;
			}
			FByteBuffer Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload, {.Target = Ar.GetTarget()});
			PlatformData->Serialize(Writer);
			auto Bulk = Writer.IsError() ? std::expected<FBulkData, FBulkDataError>{}
				: FBulkData::TryCreateDetached(Bytes);
			if (Writer.IsError() || !Bulk)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData, Writer.IsError()
					? std::string(Writer.GetError()) : FormatBulkDataError(Bulk.error()));
				return;
			}
			Projection = std::move(*Bulk);
			Value = &Projection;
		}
		auto Field = EnterArchiveField(Ar, {WireOwner,
			FName("PlatformData"), FArchiveLogicalTypeDescriptor::BulkData()});
		Value->Serialize(Ar, {.Alignment = TexturePayloadAlignment,
			.StoragePolicy = EArchiveBulkDataStoragePolicy::AllowExternal});
	}

	// Decode into detached typed data and release the bulk lock before publishing
	// through the family's setter after successful payload validation. A failure never updates resources.
	template<class TPlatformData, class TTexture>
	auto LoadCookedPlatformData(TTexture& Texture, FBulkData& CookedData,
		std::string_view Family, bool bAsync, std::shared_ptr<FPackageResourceRequest>& Pending) -> bool
	{
		if (bAsync)
		{
			const auto Revision = CookedData.GetContentRevision();
			const auto Target = AssetPrivate::CookedArchiveTarget(GetAssetRuntimeConfiguration());
			const auto CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
			const auto Platform = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
			const TWeakObjectPtr<TTexture> Owner(&Texture);
			Pending = std::make_shared<FPackageResourceRequest>();
			const std::weak_ptr RequestIdentity(Pending);
			auto Candidate = std::make_shared<std::expected<std::unique_ptr<TPlatformData>, std::string>>(
				std::unexpected("Cooked texture decode did not complete."));
			auto Completion = Tasks::TCompletionSource<void>::Create({.DebugName = "CookedTextureDecode"});
			// Admit the owner continuation before IO, including during shutdown drain.
			auto Publication = Tasks::Then(Completion.TakeTask(), Tasks::ETaskExecutor::GameThreadDeferred,
				{.DebugName = "CookedTexturePublication"}, [Owner, Revision, Candidate, RequestIdentity, CookRoot, Platform] {
					auto* Current = Owner.Get();
					const auto Request = RequestIdentity.lock();
					if (!Current || !Request || Current->IsPendingKill() || Current->GetResourceUpdateState() == ETextureResourceUpdateState::Closed
						|| Current->CookedLoad != Request || Current->HasPlatformData()) return;
					const auto Resource = Current->GetCookedPlatformData().GetMetadata().Range.Resource;
					if (!GetAssetRuntimeConfiguration().RequiresCookedPayload()
						|| CookRoot != GetAssetRuntimeConfiguration().GetCookRoot()
						|| Platform != GetAssetRuntimeConfiguration().GetCookTargetPlatform()
						|| (Resource && Resource->IsRetired()))
					{
						Current->UpdateResource(); // No CPU product was admitted; keep the failure on its owner.
						Current->ResourceUpdateError = "Cooked texture request was invalidated before publication.";
						return;
					}
					if (Current->GetCookedPlatformData().GetContentRevision() != Revision)
					{
						Current->CookedLoad.reset();
						Current->BeginCachePlatformData();
						return;
					}
					if (*Candidate) Current->SetPlatformData(std::move(**Candidate));
					Current->UpdateResource();
					if (!*Candidate)
					{
						Current->ResourceUpdateError = Candidate->error();
						DURIN_WARN("Cooked texture '{}': {}", Current->GetObjectPath(), Current->ResourceUpdateError);
					}
				});
			*Pending = FPackageResourceRequest::Transform(CookedData.ReloadAsync(),
				[Candidate, Completion, Target](FPackageResourceReadResult Read) {
					struct FCompleteOnExit
					{
						decltype(Completion) Source;
						~FCompleteOnExit() { Source.TrySetValue(); }
					} Complete{Completion};
					if (Read)
					{
						auto Data = std::make_unique<TPlatformData>();
						FCanonicalMemoryReader Ar(Read->GetBytes(), EArchivePurpose::CookedPayload, {.Target = Target});
						Data->Serialize(Ar);
						if (!Ar.IsError() && RequireArchiveEnd(Ar)) *Candidate = std::move(Data);
						else *Candidate = std::unexpected(std::string(Ar.GetError()));
					}
					else *Candidate = std::unexpected(FormatPackageResourceReadError(Read));
					return Read;
				});
			return true;
		}

		auto FailCooked = [&](std::string Message) {
			DURIN_WARN("Cooked {} '{}': {}",
				Family, Texture.GetObjectPath(), Message);
			return false;
		};
		auto Read = CookedData.AcquireRead();
		if (!Read) return FailCooked(Read.error().Message);
		const FByteView Bytes = Read->GetBytes();
		auto Candidate = std::make_unique<TPlatformData>();
		FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::CookedPayload,
			{.Target = AssetPrivate::CookedArchiveTarget(GetAssetRuntimeConfiguration())});
		Candidate->Serialize(Ar);
		if (Ar.IsError() || !RequireArchiveEnd(Ar))
		{
			return FailCooked(std::string(Ar.GetError()));
		}
		Read->Reset();
		Texture.SetPlatformData(std::move(Candidate));
		Texture.UpdateResource();
		return true;
	}
}
