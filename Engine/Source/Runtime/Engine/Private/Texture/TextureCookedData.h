#pragma once

#include "CoreMinimal.h"

#include "DObject/Archive.h"

#include "Asset/BulkData.h"
#include "Logging/LogMacros.h"
#include "Serialization/Archive.h"
#include "Texture/TextureDerivedData.h"
#include "Asset/Load.h"
#include "DynamicRHI.h"
#include "DObject/WeakObjectPtr.h"
#include "Threading/Task.h"
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

	// Both execution modes decode the same detached product and use the same
	// owner publication. The queued callback is only an automatic wakeup.
	template<class TPlatformData, class TTexture>
	auto LoadCookedPlatformData(TTexture& Texture, bool bAsync) -> void
	{
		auto& CookedData = Texture.GetMutableCookedPlatformData();
		const auto Revision = CookedData.GetContentRevision();
		const auto Target = AssetPrivate::CookedArchiveTarget(GetAssetRuntimeConfiguration());
		const auto CookRoot = GetAssetRuntimeConfiguration().GetCookRoot();
		const auto Platform = GetAssetRuntimeConfiguration().GetCookTargetPlatform();
		const TWeakObjectPtr<TTexture> Owner(&Texture);
		const auto Pending = bAsync ? std::make_shared<FPackageResourceRequest>() : nullptr;
		const std::weak_ptr RequestIdentity(Pending);
		auto Candidate = std::make_shared<std::expected<std::unique_ptr<TPlatformData>, std::string>>(
			std::unexpected("Cooked texture decode did not complete."));
		const auto Decode = [Target](FByteView Bytes) -> std::expected<std::unique_ptr<TPlatformData>, std::string> {
			auto Data = std::make_unique<TPlatformData>();
			FCanonicalMemoryReader Ar(Bytes, EArchivePurpose::CookedPayload, {.Target = Target});
			Data->Serialize(Ar);
			if (Ar.IsError() || !RequireArchiveEnd(Ar)) return std::unexpected(std::string(Ar.GetError()));
			if (!Data->IsValid()) return std::unexpected("Cooked texture platform data is invalid.");
			return Data;
		};
		const auto Publish = [Owner, Revision, Candidate, RequestIdentity, CookRoot, Platform, bAsync] {
			auto* Current = Owner.Get();
			const auto Request = RequestIdentity.lock();
			if (!Current || Current->IsPendingKill()
				|| (bAsync && (!Request || Current->CookedLoad != Request)) || Current->HasPlatformData()) return;
			const auto Fail = [&](std::string Diagnostic) {
				Current->CookedLoad.reset();
				Current->CookedPublication = {};
				DURIN_WARN("Cooked texture '{}': {}", Current->GetObjectPath(), Diagnostic);
			};
			const auto Resource = Current->GetCookedPlatformData().GetMetadata().Range.Resource;
			if (!GetAssetRuntimeConfiguration().RequiresCookedPayload()
				|| CookRoot != GetAssetRuntimeConfiguration().GetCookRoot()
				|| Platform != GetAssetRuntimeConfiguration().GetCookTargetPlatform()
				|| (Resource && Resource->IsRetired()))
				return Fail("Cooked texture request was invalidated before publication.");
			if (Current->GetCookedPlatformData().GetContentRevision() != Revision)
				return Fail("Cooked texture content changed before publication.");
			if (!*Candidate) return Fail(Candidate->error());
			Current->SetPlatformData(std::move(**Candidate));
			if (GDynamicRHI) Current->UpdateResource();
		};
		if (!bAsync)
		{
			auto Read = CookedData.AcquireRead();
			if (Read)
			{
				*Candidate = Decode(Read->GetBytes());
				Read->Reset();
			}
			else *Candidate = std::unexpected(Read.error().Message);
			Publish();
			return;
		}
		Texture.CookedLoad = Pending;
		Texture.CookedPublication = Publish;
		std::optional<Tasks::FTaskEvent> Completion;
		if (GetGameThreadDeferredWorkQueueDiagnostics().bAccepting)
		{
			Completion.emplace("CookedTextureDecode");
			// Register automatic publication before IO, so shutdown needs no new admission.
			auto Publication = Tasks::LaunchTask(Tasks::ETaskExecutor::GameThreadDeferred,
				{.DebugName = "CookedTexturePublication"}, Publish, *Completion);
		}
		try
		{
			*Pending = FPackageResourceRequest::Transform(CookedData.ReloadAsync(),
				[Candidate, Completion, Decode](FPackageResourceReadResult Read) {
					struct FCompleteOnExit
					{
						decltype(Completion) Signal;
						~FCompleteOnExit() { if (Signal) Signal->Trigger(); }
					} Complete{Completion};
					// The accepted Transform task owns this callable through capture
					// retirement. This signal only releases Candidate publication.
					*Candidate = Read ? Decode(Read->GetBytes())
						: std::expected<std::unique_ptr<TPlatformData>, std::string>(std::unexpected(FormatPackageResourceReadError(Read)));
					return Read;
				});
		}
		catch (...)
		{
			// Failed producer setup must also release the pre-admitted publication.
			// Candidate already carries a failure if no decode was performed.
			if (Completion) Completion->Trigger();
			throw;
		}
	}
}
