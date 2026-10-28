/** @file
  Find a boot payload and run it (M5).

  This is the step after M4: once eMMC produces BlockIo and FatPkg produces a
  SimpleFileSystem, something has to actually load and start the next stage.
  BdsDxe cannot do it here -- it was deliberately taken out of the APRIORI list
  because ConSplitter's console-mode negotiation dead-loops on this panel (see
  DandelionSetMode in DandelionConsoleDxe.c and CORRECTIONS.md C10's neighbours)
  -- so this driver does the only part of BDS this board needs: find a payload,
  LoadImage it, StartImage it, and paint the result where a photo can read it.

  Two sources, tried in order:

    1. A file on a mounted FAT volume.  Candidates are '\UniXNU.efi',
       '\EFI\BOOT\BOOTAA64.EFI' and '\xnu.efi', in that order.  The file is read
       into a pool buffer and LoadImage'd from memory, which keeps this driver
       free of DevicePath-manipulation libraries.
    2. A payload embedded in the firmware volume itself, found by file GUID
       (mPayloadFileGuid, which must match Applications/PayloadHello/
       PayloadHello.inf).  This exists so the load-and-start path can be proven
       on this board *without* eMMC working: if the FV payload runs, the chain
       is good and only the file lookup remains untested.

  Reporting: a full-width bar at framebuffer y 1570..1596, which nothing else in
  this firmware uses (the progress band is at 1560..1568, the readout at
  1244..1389, the stage cells at 1400..1540).

    blue    looking for a payload
    green   payload started and returned EFI_SUCCESS
    red     found but LoadImage/StartImage failed, or nothing was found

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>

#include <Pi/PiFirmwareVolume.h>
#include <Pi/PiFirmwareFile.h>

#include <Protocol/SimpleFileSystem.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>

#include <Guid/FileInfo.h>

#define PAYLOAD_FB_BASE    0x7EC50000ULL
#define PAYLOAD_FB_STRIDE  2944
#define PAYLOAD_FB_WIDTH   720
#define PAYLOAD_BAR_TOP    1570
#define PAYLOAD_BAR_HEIGHT 26

#define PAYLOAD_BAR_SEARCH 0x00FF8000
#define PAYLOAD_BAR_OK     0x0000FF00
#define PAYLOAD_BAR_FAIL   0x00FF0000

//
// Must equal the FILE_GUID in Applications/PayloadHello/PayloadHello.inf.
// If they ever drift apart the only symptom is "no payload found", so the value
// is repeated here with a searchable name rather than hidden behind a header.
//
STATIC EFI_GUID  mPayloadFileGuid = {
  0x7E1B3A5C, 0x2C4D, 0x4F6A, { 0x9B, 0x10, 0xDA, 0x9D, 0x0E, 0x11, 0x00, 0x01 }
};

//
// The device tree, embedded in the firmware volume as a FREEFORM file (see
// dandelion.fdf) and published as an EFI configuration table before the payload
// is started.  B1B621D5-F19C-41A5-830B-D9152C69AAE0 is gFdtTableGuid: the GUID
// DarwinBoot and UniXNU already look for, so nothing on the payload side has to
// learn a new name.  The FFS file uses the same GUID, which keeps this driver
// down to one constant.
//
STATIC EFI_GUID  mFdtTableGuid = {
  0xB1B621D5, 0xF19C, 0x41A5, { 0x83, 0x0B, 0xD9, 0x15, 0x2C, 0x69, 0xAA, 0xE0 }
};

STATIC
VOID
PaintBar (
  IN UINT32  Colour
  )
{
  volatile UINT32  *Row;
  UINTN            X;
  UINTN            Y;

  for (Y = PAYLOAD_BAR_TOP; Y < PAYLOAD_BAR_TOP + PAYLOAD_BAR_HEIGHT; Y++) {
    Row = (volatile UINT32 *)(UINTN)(PAYLOAD_FB_BASE + (UINT64)Y * PAYLOAD_FB_STRIDE);
    for (X = 0; X < PAYLOAD_FB_WIDTH; X++) {
      Row[X] = Colour;
    }
  }
}

STATIC
VOID
Say (
  IN CONST CHAR16  *Text
  )
{
  //
  // Console only.  DEBUG() does not take a CHAR16 string here, and a wrong
  // format specifier in a bring-up driver is a crash, not a typo.
  //
  if ((gST != NULL) && (gST->ConOut != NULL)) {
    gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Text);
  }
}

/**
  Read a whole file into a freshly allocated buffer.

  The buffer is intentionally not freed: the image it feeds has to stay resident
  for as long as the started image lives, which is forever on this board.
**/
STATIC
EFI_STATUS
ReadWholeFile (
  IN  EFI_FILE_PROTOCOL  *File,
  OUT VOID               **Buffer,
  OUT UINTN              *Size
  )
{
  EFI_STATUS  Status;
  EFI_FILE_INFO  *Info;
  UINTN       InfoSize;
  UINTN       ReadSize;
  VOID        *Data;

  InfoSize = 0;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return EFI_LOAD_ERROR;
  }

  Status = gBS->AllocatePool (EfiBootServicesData, InfoSize, (VOID **)&Info);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ReadSize = (UINTN)Info->FileSize;
  if ((ReadSize == 0) || (ReadSize > (16 * 1024 * 1024))) {
    return EFI_LOAD_ERROR;
  }

  Status = gBS->AllocatePool (EfiBootServicesData, ReadSize, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = File->Read (File, &ReadSize, Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Buffer = Data;
  *Size   = ReadSize;
  return EFI_SUCCESS;
}

/**
  Load and start an image from a memory buffer.

  LoadImage with a SourceBuffer and no DevicePath is the documented way to start
  something that was not read through a device path -- which is exactly our
  situation for both the FAT file and the FV-embedded payload.
**/
STATIC
EFI_STATUS
StartFromBuffer (
  IN VOID   *Image,
  IN UINTN  ImageSize
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  NewHandle;
  UINTN       ExitDataSize;
  CHAR16      *ExitData;

  NewHandle    = NULL;
  ExitData     = NULL;
  ExitDataSize = 0;

  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, Image, ImageSize, &NewHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PayloadBoot: LoadImage failed %r\n", Status));
    return Status;
  }

  Status = gBS->StartImage (NewHandle, &ExitDataSize, &ExitData);
  DEBUG ((DEBUG_ERROR, "PayloadBoot: StartImage -> %r\n", Status));
  return Status;
}

STATIC CHAR16  *mCandidates[] = {
  L"\\UniXNU.efi",
  L"\\EFI\\BOOT\\BOOTAA64.EFI",
  L"\\xnu.efi"
};

/**
  Try every mounted FAT volume for each candidate name.
**/
STATIC
EFI_STATUS
TryFileSystem (
  OUT CHAR16  **WhichName
  )
{
  EFI_STATUS               Status;
  EFI_HANDLE               *Handles;
  UINTN                    HandleCount;
  UINTN                    Index;
  UINTN                    Candidate;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL        *Root;
  EFI_FILE_PROTOCOL        *File;
  VOID                     *Image;
  UINTN                    ImageSize;

  Handles     = NULL;
  HandleCount = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (HandleCount == 0)) {
    DEBUG ((DEBUG_ERROR, "PayloadBoot: no SimpleFileSystem handle\n"));
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Root = NULL;
    if (EFI_ERROR (Fs->OpenVolume (Fs, &Root)) || (Root == NULL)) {
      continue;
    }

    for (Candidate = 0; Candidate < ARRAY_SIZE (mCandidates); Candidate++) {
      File = NULL;
      Status = Root->Open (Root, &File, mCandidates[Candidate], EFI_FILE_MODE_READ, 0);
      if (EFI_ERROR (Status) || (File == NULL)) {
        continue;
      }

      Image     = NULL;
      ImageSize = 0;
      Status    = ReadWholeFile (File, &Image, &ImageSize);
      File->Close (File);
      if (EFI_ERROR (Status)) {
        continue;
      }

      Say (L"PayloadBoot: found ");
      Say ((CHAR16 *)mCandidates[Candidate]);
      Say (L"\r\n");

      *WhichName = (CHAR16 *)mCandidates[Candidate];
      return StartFromBuffer (Image, ImageSize);
    }
  }

  return EFI_NOT_FOUND;
}

/**
  Fall back to a payload embedded in the firmware volume.

  FirmwareVolume2->ReadSection copies the whole section into the caller's
  buffer, and for an application FFS file the section we want is the PE32 one,
  i.e. a ready-to-load image with no file system in the way.
**/
STATIC
EFI_STATUS
TryFirmwareVolume (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    *Handles;
  UINTN                         HandleCount;
  UINTN                         Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
  VOID                          *Image;
  UINTN                         ImageSize;
  UINT32                        AuthenticationStatus;

  Handles     = NULL;
  HandleCount = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Image     = NULL;
    ImageSize = 0;
    Status    = Fv->ReadSection (
                      Fv,
                      &mPayloadFileGuid,
                      EFI_SECTION_PE32,
                      0,
                      &Image,
                      &ImageSize,
                      &AuthenticationStatus
                      );
    if (EFI_ERROR (Status) || (Image == NULL) || (ImageSize == 0)) {
      continue;
    }

    Say (L"PayloadBoot: using the payload embedded in the FV\r\n");
    return StartFromBuffer (Image, ImageSize);
  }

  return EFI_NOT_FOUND;
}

/**
  Draw a white block at the right end of the bar.

  One small mark, one meaning: the device tree made it into the configuration
  table.  The bar's colour keeps its own meaning (searching / started / failed),
  so the two facts stay readable in the same photo.
**/
STATIC
VOID
TickBar (
  VOID
  )
{
  volatile UINT32  *Row;
  UINTN            X;
  UINTN            Y;

  for (Y = PAYLOAD_BAR_TOP; Y < PAYLOAD_BAR_TOP + PAYLOAD_BAR_HEIGHT; Y++) {
    Row = (volatile UINT32 *)(UINTN)(PAYLOAD_FB_BASE + (UINT64)Y * PAYLOAD_FB_STRIDE);
    for (X = PAYLOAD_FB_WIDTH - 24; X < PAYLOAD_FB_WIDTH; X++) {
      Row[X] = 0x00FFFFFF;
    }
  }
}

/**
  Publish the device tree as an EFI configuration table.

  DarwinBoot and UniXNU find the device tree through gFdtTableGuid, so the
  firmware's whole job here is to make that table entry exist -- the payload does
  not have to know where the blob lives.  Embedding the tree in the firmware
  volume (dandelion.fdf, FREEFORM file with the same GUID) also gives it a
  version that travels with the firmware instead of with a FAT partition which
  does not exist on this board yet.

  Returns EFI_NOT_FOUND when the firmware volume simply has no such file, which
  is not an error worth stopping for.
**/
STATIC
EFI_STATUS
PublishDeviceTree (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    *Handles;
  UINTN                         HandleCount;
  UINTN                         Index;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
  VOID                          *Blob;
  UINTN                         BlobSize;
  UINT32                        AuthenticationStatus;

  Handles     = NULL;
  HandleCount = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiFirmwareVolume2ProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Blob     = NULL;
    BlobSize = 0;
    Status   = Fv->ReadSection (
                     Fv,
                     &mFdtTableGuid,
                     EFI_SECTION_RAW,
                     0,
                     &Blob,
                     &BlobSize,
                     &AuthenticationStatus
                     );
    if (EFI_ERROR (Status) || (Blob == NULL) || (BlobSize < 16)) {
      continue;
    }

    //
    // A device tree always starts with the magic 0xD00DFEED in big-endian, and
    // shipping one that does not is worse than shipping none: every consumer
    // would fail somewhere far away from here.
    //
    if (*(volatile UINT32 *)Blob != 0xD00DFEED) {
      DEBUG ((DEBUG_ERROR, "PayloadBoot: FV device tree has a bad magic %08x\n", *(volatile UINT32 *)Blob));
      continue;
    }

    Status = gBS->InstallConfigurationTable (&mFdtTableGuid, Blob);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "PayloadBoot: InstallConfigurationTable failed %r\n", Status));
      return Status;
    }

    DEBUG ((DEBUG_ERROR, "PayloadBoot: device tree %u bytes published\n", (UINT32)BlobSize));
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
PayloadBootEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  CHAR16      *WhichName;

  WhichName = NULL;
  PaintBar (PAYLOAD_BAR_SEARCH);

  //
  // Device tree first: a payload that finds a tree can describe the hardware
  // even before it can read anything from storage.
  //
  if (!EFI_ERROR (PublishDeviceTree ())) {
    Say (L"PayloadBoot: device tree published as an EFI configuration table\r\n");
    TickBar ();
  }

  Say (L"PayloadBoot: looking for a payload (FAT first, then FV)\r\n");

  Status = TryFileSystem (&WhichName);
  if (Status == EFI_NOT_FOUND) {
    Status = TryFirmwareVolume ();
  }

  if (EFI_ERROR (Status)) {
    //
    // Not fatal.  Without a payload the machine still reaches BDS, which prints
    // "No boot options" -- the red bar is how we tell "no payload" apart from
    // "payload ran and failed".
    //
    PaintBar (PAYLOAD_BAR_FAIL);
    Say (L"PayloadBoot: no payload found (this is expected before M4/M5)\r\n");
    return EFI_SUCCESS;
  }

  PaintBar (PAYLOAD_BAR_OK);
  return EFI_SUCCESS;
}
