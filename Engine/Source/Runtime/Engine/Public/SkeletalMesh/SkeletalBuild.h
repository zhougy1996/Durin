#pragma once

#include "SkeletalMesh/SkeletalBuildProvider.h"

namespace Durin
{
	enum class ESkeletalDerivedDataOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	template<typename T>
	struct TSkeletalDerivedDataResult
	{
		std::shared_ptr<const T> Payload;
		std::string Key;
		ESkeletalDerivedDataOrigin Origin = ESkeletalDerivedDataOrigin::Rebuilt;
		FSkeletalBuildProviderDescriptor Descriptor;
		uint64 CacheReadNanoseconds = 0;
		uint64 CacheWriteNanoseconds = 0;
		uint64 PayloadBytes = 0;
		std::string Diagnostic;
	};

	struct FSkeletalMeshDerivedDataRequest
	{
		FXxHash128 ImportedDataIdentity;
		std::string SkeletonCompatibilityIdentity;
		FSkeletalPayloadSerializationContext Context;
		std::shared_ptr<const FSkeletalMeshPayloadData> Payload;
		// Called only after a cache miss; captures detached canonical bulk, never an asset.
		std::function<std::shared_ptr<const FSkeletalMeshPayloadData>(std::string&)> LoadPayload;
		bool bPersistDerivedData = true;
		std::function<bool()> ShouldCancel;
	};

	struct FAnimationClipDerivedDataRequest
	{
		FXxHash128 ImportedDataIdentity;
		std::string SkeletonCompatibilityIdentity;
		FSkeletalPayloadSerializationContext Context;
		std::shared_ptr<const FAnimationClipPayloadData> Payload;
		// Called only after a cache miss; captures detached canonical bulk, never an asset.
		std::function<std::shared_ptr<const FAnimationClipPayloadData>(std::string&)> LoadPayload;
		bool bPersistDerivedData = true;
		std::function<bool()> ShouldCancel;
	};

	using FSkeletalMeshDerivedDataResult =
		TSkeletalDerivedDataResult<FSkeletalMeshPayloadData>;
	using FAnimationClipDerivedDataResult =
		TSkeletalDerivedDataResult<FAnimationClipPayloadData>;

	ENGINE_API auto BuildSkeletalMeshDerivedData(
		FSkeletalMeshDerivedDataRequest Request,
		FSkeletalMeshDerivedDataResult& OutResult,
		std::string& OutError) -> bool;
	ENGINE_API auto BuildAnimationClipDerivedData(
		FAnimationClipDerivedDataRequest Request,
		FAnimationClipDerivedDataResult& OutResult,
		std::string& OutError) -> bool;

}
