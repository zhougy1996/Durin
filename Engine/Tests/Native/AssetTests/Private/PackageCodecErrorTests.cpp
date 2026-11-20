#include <gtest/gtest.h>
#include "Asset/AssetPackageTaggedCodec.h"
#include "Asset/PackageSerialization.h"
#include "DObject/PackageCapture.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Serialization/BinaryEnvelope.h"

TEST(FPackageAssetTests, PackageRelocationRejectsMalformedInputWithoutPublishing)
{
	using namespace Durin;
	using namespace Durin::AssetPrivate;
	using namespace Durin::ObjectPackage;
	Testing::InitializeDObjectSystemForTests();
	Testing::FScopedMountRegistryFixture Mounts;
	ASSERT_TRUE(Mounts.IsValid());
	Testing::RegisterMountPointForTests("/CodecCauseTests/",
		Testing::CreateTestFixtureDirectory("CodecCauses").generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CodecCauseTests/CodecCauses", Path));
	const auto& Codec = TaggedPackage::GetCodec();
	const FAssetPackageEncodedClosure Sentinel{{std::byte{7}}, {std::byte{9}}};
	auto Closure = Sentinel;
	const FByteBuffer Corrupt(BinaryEnvelopePreambleBytes, std::byte{0});
	const auto Failure = Codec.Relocate({.PackageBytes = Corrupt, .PackagePath = Path}, Path, Closure);
	EXPECT_EQ(Failure.Failure, EPackageWriterFailure::InvalidInput);
	EXPECT_EQ(Closure.PackageBytes, Sentinel.PackageBytes);
	EXPECT_EQ(Closure.BulkBytes, Sentinel.BulkBytes);
}

TEST(FPackageAssetTests, PackageSerializationFailurePreservesOutputAcrossRetry)
{
	using namespace Durin;
	Testing::InitializeDObjectSystemForTests();
	Testing::FScopedMountRegistryFixture Mounts;
	ASSERT_TRUE(Mounts.IsValid());
	Testing::RegisterMountPointForTests("/CodecCauseTests/",
		Testing::CreateTestFixtureDirectory("CaptureCause").generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CodecCauseTests/Capture", Path));
	DObject* Asset = nullptr;
	ASSERT_TRUE(CreatePackageLeafAssetForTesting(Path, Asset));
	struct FCleanup
	{
		DPackage* Package;
		~FCleanup() { MarkObjectHierarchyAsGarbage(Package); CollectGarbage(Durin::EObjectFlags::Standalone); }
	} Cleanup{Asset->GetPackage()};
	const std::string ObjectPath = Asset->GetObjectPath();
	FByteBuffer Bytes{std::byte{7}};
	ObjectPackage::FPackageWriterResult Failure;
	{
		auto Overrides = std::make_shared<FObjectSaveOverrides>();
		ASSERT_TRUE(Overrides->AddObjectOmission(*Asset));
		FAssetPackageSerializationOptions Options;
		Options.SaveOverrides = Overrides;
		Failure = SerializeAssetPackageBytes(Asset->GetPackage(), Bytes, Options);
	}
	EXPECT_EQ(Failure.Failure, ObjectPackage::EPackageWriterFailure::InvalidInput);

	EXPECT_EQ(Bytes, (FByteBuffer{std::byte{7}}));
	const auto Saved = SerializeAssetPackageBytes(Asset->GetPackage(), Bytes);
	ASSERT_TRUE(Saved) << Saved.Message;

}
