#pragma once

enum class RHI_API EGpuVendorId
{
	Unknown = -1,
	NotQueried = 0, // Not queried yet

	Amd = 0x1002,
	ImgTec = 0x1010,
	Nvidia = 0x10DE,
	Arm = 0x13B5,
	Broadcom = 0x14E4,
	Qualcomm = 0x5143,
	Intel = 0x8086,
	Apple = 0x106B,
};

inline RHI_API EGpuVendorId RHIConvertToGpuVenderId(uint32 VendorId)
{
	switch (static_cast<EGpuVendorId>(VendorId))
	{
	case EGpuVendorId::NotQueried:
		return EGpuVendorId::NotQueried;

	case EGpuVendorId::Amd:
	case EGpuVendorId::ImgTec:
	case EGpuVendorId::Nvidia:
	case EGpuVendorId::Arm:
	case EGpuVendorId::Broadcom:
	case EGpuVendorId::Qualcomm:
	case EGpuVendorId::Intel:
	case EGpuVendorId::Apple:
		return static_cast<EGpuVendorId>(VendorId);

	default:
		return EGpuVendorId::Unknown;
	}
}