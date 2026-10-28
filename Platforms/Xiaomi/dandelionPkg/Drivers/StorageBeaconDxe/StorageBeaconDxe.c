/**
  M4 storage beacon: proves that the eMMC -> block I/O -> partition -> FAT
  stack actually produced a filesystem, without needing a working boot option.

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  The whole point is the [Depex] in the .inf: this driver is only dispatched
  once gEfiSimpleFileSystemProtocolGuid exists, i.e. once FAT has bound a
  volume.  So the line it prints is binary evidence:

      "STORAGE OK" on the panel  -> the chain works up to a readable FAT
      nothing on the panel        -> something below FAT is broken, and the
                                     next step is the BlockIo beacon instead
                                     of guessing

  It prints through ConOut on purpose: by the time a filesystem exists, the
  console stack is long since up, and going through ConOut also re-proves that
  the console works at that point in boot.
**/

#include <Uefi.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/SimpleFileSystem.h>

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

  Handles = NULL;
  Count   = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }

  if (SystemTable->ConOut != NULL) {
    SystemTable->ConOut->OutputString (SystemTable->ConOut, L"\r\nSTORAGE OK\r\n");
  }

  // One short line per volume, plus how many there are, so a partial result
  // (one volume out of two partitions) is still readable on a photo.
  for (Index = 0; Index < Count; Index++) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
    EFI_FILE_PROTOCOL                *Root;
    CHAR16                           Line[64];

    Fs   = NULL;
    Root = NULL;

    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
      continue;
    }

    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
      continue;
    }

    Line[0] = L'F';
    Line[1] = L'S';
    Line[2] = L' ';
    Line[3] = (CHAR16)(L'0' + ((Index < 9) ? Index : 9));
    Line[4] = L'/';
    Line[5] = (CHAR16)(L'0' + ((Count < 9) ? Count : 9));
    Line[6] = L'\r';
    Line[7] = L'\n';
    Line[8] = L'\0';

    if (SystemTable->ConOut != NULL) {
      SystemTable->ConOut->OutputString (SystemTable->ConOut, Line);
    }

    Root->Close (Root);
  }

  gBS->FreePool (Handles);

  return EFI_SUCCESS;
}
