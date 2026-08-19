#pragma once

#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "Texture/Texture2D.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace
{
	auto ExpectPlatformDataEqual(const Durin::FTexturePlatformData& Actual,
		const Durin::FTexturePlatformData& Expected) -> void
	{
		EXPECT_EQ(Actual.PixelFormat, Expected.PixelFormat);
		ASSERT_EQ(Actual.Mips.size(), Expected.Mips.size());
		for (size_t MipIndex = 0; MipIndex < Actual.Mips.size(); ++MipIndex)
		{
			EXPECT_EQ(Actual.Mips[MipIndex].Width, Expected.Mips[MipIndex].Width);
			EXPECT_EQ(Actual.Mips[MipIndex].Height, Expected.Mips[MipIndex].Height);
			EXPECT_EQ(Actual.Mips[MipIndex].RowPitch, Expected.Mips[MipIndex].RowPitch);
			EXPECT_EQ(Actual.Mips[MipIndex].Pixels, Expected.Mips[MipIndex].Pixels);
		}
	}

	struct FScopedDerivedDataCacheRoot
	{
		explicit FScopedDerivedDataCacheRoot(const std::filesystem::path& Root)
			: PreviousRoot(Durin::FPaths::DerivedDataCacheDir())
		{
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}

		~FScopedDerivedDataCacheRoot()
		{
			Durin::FPaths::SetDerivedDataCacheDirForTests(PreviousRoot);
		}

		std::string PreviousRoot;
	};
}
