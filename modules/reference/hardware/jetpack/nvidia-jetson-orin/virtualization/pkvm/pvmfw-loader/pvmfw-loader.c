// SPDX-FileCopyrightText: 2022-2026 TII (SSRC) and the Ghaf contributors
// SPDX-License-Identifier: Apache-2.0
//
// Put pvmfw where pKVM expects it.
//
// pKVM takes pvmfw from a reserved-memory region that the bootloader fills
// before Linux starts ("linux,pkvm-guest-firmware-memory"); on Android, ABL
// does this. systemd-boot runs this image from \EFI\systemd\drivers\ before it
// boots Linux. It loads \EFI\pvmfw\pvmfw.bin into the fixed region that the
// host DT overlay declares, as EfiReservedMemoryType so that nothing else
// takes it before pKVM does.
#include <efi.h>
#include <efilib.h>

#define PVMFW_BASE 0x500000000ULL /* must match pvmfw-region-overlay.dts */
#define PVMFW_SIZE 0x400000ULL    /* crosvm's guest firmware window */
#define PVMFW_PATH L"\\EFI\\pvmfw\\pvmfw.bin"

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
	EFI_PHYSICAL_ADDRESS base = PVMFW_BASE;
	EFI_FILE_IO_INTERFACE *vol;
	EFI_FILE_HANDLE root, file;
	EFI_LOADED_IMAGE *li;
	EFI_FILE_INFO *info;
	EFI_STATUS st;
	UINTN size;

	InitializeLib(image, systab);

	st = uefi_call_wrapper(BS->HandleProtocol, 3, image, &LoadedImageProtocol, (void **)&li);
	if (EFI_ERROR(st))
		goto out;
	st = uefi_call_wrapper(BS->HandleProtocol, 3, li->DeviceHandle, &FileSystemProtocol,
			       (void **)&vol);
	if (EFI_ERROR(st))
		goto out;
	st = uefi_call_wrapper(vol->OpenVolume, 2, vol, &root);
	if (EFI_ERROR(st))
		goto out;
	st = uefi_call_wrapper(root->Open, 5, root, &file, PVMFW_PATH, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st))
		goto out;

	info = LibFileInfo(file);
	if (!info) {
		st = EFI_LOAD_ERROR;
		goto out;
	}
	size = info->FileSize;
	FreePool(info);
	if (!size || size > PVMFW_SIZE) {
		st = EFI_BAD_BUFFER_SIZE;
		goto out;
	}

	st = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress, EfiReservedMemoryType,
			       EFI_SIZE_TO_PAGES(PVMFW_SIZE), &base);
	if (EFI_ERROR(st))
		goto out;

	ZeroMem((void *)base, PVMFW_SIZE);
	st = uefi_call_wrapper(file->Read, 3, file, &size, (void *)base);
	uefi_call_wrapper(file->Close, 1, file);
	if (EFI_ERROR(st))
		goto out;

	Print(L"pvmfw-loader: loaded %lu bytes at 0x%lx\n", size, base);
	return EFI_SUCCESS;
out:
	Print(L"pvmfw-loader: failed: %r\n", st);
	return st;
}
