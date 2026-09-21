#include <gtest/gtest.h>
#include "Asset/AssetPackageTaggedCodec.h"
#include "Asset/PackageSerialization.h"
#include "DObject/PackageCapture.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Serialization/BinaryEnvelope.h"

TEST(FPackageAssetTests, PackageCodecRetainsOwnedReaderAndWriterCauses)
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
	FTopLevelAssetPath Source;
	FObjectPath Destination;
	ASSERT_TRUE(FTopLevelAssetPath::TryCreate(Path, "Source", Source));
	ASSERT_TRUE(FObjectPath::TryCreate("/CodecCauseTests/Target.Target", Destination));
	const auto& Codec = TaggedPackage::GetCodec();
	const FAssetPackageEncodedClosure Sentinel{{std::byte{7}}, {std::byte{9}}};
	auto Closure = Sentinel;
	ObjectPackage::FPackageWriterResult WriterFailure;
	{
		const std::array Mappings{
			FAssetRedirectorWriteMapping{Source, Destination},
			FAssetRedirectorWriteMapping{Source, Destination}};
		WriterFailure = Codec.WriteRedirector(Path, Mappings, Closure);
	}
	EXPECT_EQ(WriterFailure.Failure, EPackageWriterFailure::DuplicateIdentity);

	EXPECT_EQ(Closure.PackageBytes, Sentinel.PackageBytes);
	EXPECT_EQ(Closure.BulkBytes, Sentinel.BulkBytes);

	const FAssetRedirectorWriteMapping Mapping{Source, Destination};
	const auto Written = Codec.WriteRedirector(Path, std::span(&Mapping, 1), Closure);
	ASSERT_TRUE(Written) << Written.Message;

	const auto SavedClosure = Closure;
	ObjectPackage::FPackageWriterResult ReaderFailure;
	{
		FByteBuffer Corrupt(BinaryEnvelopePreambleBytes, std::byte{0});
		ReaderFailure = Codec.Relocate({.PackageBytes = Corrupt, .PackagePath = Path}, Path, Closure);
	}
	EXPECT_EQ(ReaderFailure.Failure, EPackageWriterFailure::InvalidInput);

	EXPECT_EQ(Closure.PackageBytes, SavedClosure.PackageBytes);
	EXPECT_EQ(Closure.BulkBytes, SavedClosure.BulkBytes);
	const auto Read = Codec.Validate({.PackageBytes = Closure.PackageBytes,
		.BulkBytes = Closure.BulkBytes, .PackagePath = Path});
	ASSERT_TRUE(Read) << Read.Message;

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
		~FCleanup() { MarkObjectHierarchyAsGarbage(Package); CollectGarbage(); }
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
