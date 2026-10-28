/**
  M4 storage beacon: report how far the eMMC stack got, on the panel.

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  Dispatch is gated on gEfiBlockIoProtocolGuid, so the first line only prints
  once MsdcDxe (SdMmcPassThru) -> EmmcDxe produced a block device:

      "BLOCK OK"    eMMC is readable
      "FAT n/m"     FAT bound n of m SimpleFileSystem handles

  Reading the panel therefore says exactly where the storage chain stopped,
  which is what M4 needs -- dandelion has no UART and BDS's own output is not
  reliable yet.
**/

#include <Uefi.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

/**
  Repaint the status band (rows 26..33, full width).

  Same strip that DandelionConsoleDxe (magenta) and the MSDC stages
  (yellow/orange/cyan) use, so the colour on screen at reset is the last stage
  reached.  Purple = block device, green = filesystem.
**/
STATIC
VOID
PaintBand (
  IN UINT32  Colour
  )
{
  volatile UINT32  *Band;
  UINTN            Y;
  UINTN            X;

  for (Y = 26; Y < 34; Y++) {
    Band = (volatile UINT32 *)(UINTN)(0x7EC50000ULL + (UINT64)Y * 2944);
    for (X = 0; X < 736; X++) {
      Band[X] = Colour;
    }
  }
}

STATIC
VOID
Say (
  IN EFI_SYSTEM_TABLE  *SystemTable,
  IN CHAR16            *Text
  )
{
  if (SystemTable->ConOut != NULL) {
    SystemTable->ConOut->OutputString (SystemTable->ConOut, Text);
  }
}

STATIC
VOID
SayCount (
  IN EFI_SYSTEM_TABLE  *SystemTable,
  IN CHAR16            *Prefix,
  IN UINTN             Value,
  IN UINTN             Total
  )
{
  CHAR16  Line[32];

  Line[0] = Prefix[0];
  Line[1] = Prefix[1];
  Line[2] = Prefix[2];
  Line[3] = Prefix[3];
  Line[4] = L' ';
  Line[5] = (CHAR16)(L'0' + ((Value < 9) ? Value : 9));
  Line[6] = L'/';
  Line[7] = (CHAR16)(L'0' + ((Total < 9) ? Total : 9));
  Line[8] = L'\r';
  Line[9] = L'\n';
  Line[10] = L'\0';
  Say (SystemTable, Line);
}

EFI_STATUS
EFIAPI
StorageBeaconDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;
  UINTN       Volumes;
  UINTN       BlockDevices;

  PaintBand (0x008000FF);
  Say (SystemTable, L"\r\nBLOCK OK\r\n");

  //
  // Count block devices first: that alone localises a failure to "no block
  // device" versus "block device but no filesystem".
  //
  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &Count, &Handles);
  BlockDevices = EFI_ERROR (Status) ? 0 : Count;
  if (!EFI_ERROR (Status)) {
    gBS->FreePool (Handles);
  }

  SayCount (SystemTable, L"BLK ", BlockDevices, BlockDevices);

  //
  // Give PartitionDxe / FatPkg a moment: they bind asynchronously once the
  // block device appears, and this driver can be dispatched first.
  //
  gBS->Stall (2000 * 1000);

  Handles = NULL;
  Count   = 0;
  Volumes = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < Count; Index++) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
      EFI_FILE_PROTOCOL                *Root;

      Fs   = NULL;
      Root = NULL;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
        continue;
      }

      if (!EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
        Volumes++;
        Root->Close (Root);
      }
    }

    gBS->FreePool (Handles);
  }

  if (Volumes > 0) {
    PaintBand (0x0000FF00);
    Say (SystemTable, L"FAT OK\r\n");
  } else {
    Say (SystemTable, L"FAT NONE\r\n");
  }

  SayCount (SystemTable, L"FS  ", Volumes, Count);

  return EFI_SUCCESS;
}
