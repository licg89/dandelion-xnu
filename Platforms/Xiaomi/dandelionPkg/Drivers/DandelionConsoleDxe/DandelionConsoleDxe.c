/** @file
  A framebuffer-backed EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL for dandelion.

  Why this exists: GraphicsConsoleDxe draws sheared text on this panel even
  though the GOP reports a 736-pixel pitch (0x02E0) and our own framebuffer
  writes at a 2944-byte stride are crisp -- both were measured in the same
  boot.  Rather than debug that path, this driver owns the console outright:
  8x16 glyphs written straight into the panel at 0x7EC50000 with the real
  stride, no GOP, no Blt, no FrameBufferBltLib.

  Because it installs gEfiConsoleOutDeviceGuid, ConPlatformDxe and ConSplitter
  adopt it automatically, so BDS, UiApp and the BootManagerMenu all render
  through it -- that is the point: readable UEFI output without a photograph.

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/DevicePath.h>
#include <Protocol/SimpleTextOut.h>

#include "DandelionFont.h"

//
// Measured on hardware: the panel scans out 736 pixels per line (2944 bytes)
// while 720 are visible.  Writing with 2944 is what makes text crisp; the
// 720-wide assumption is what shears every row by 16 pixels.
//
#define DANDELION_FB_BASE    0x7EC50000ULL
#define DANDELION_FB_STRIDE  2944
#define DANDELION_FB_WIDTH   720
#define DANDELION_FB_HEIGHT  1600

#define DANDELION_COLS       (DANDELION_FB_WIDTH / 8)    // 90
#define DANDELION_ROWS       (DANDELION_FB_HEIGHT / 16)  // 100

STATIC EFI_SIMPLE_TEXT_OUTPUT_MODE  mMode = {
  1,                // MaxMode
  0,                // Mode
  EFI_LIGHTGRAY,    // Attribute
  0,                // CursorColumn
  0,                // CursorRow
  TRUE              // CursorVisible
};

STATIC UINT32  mFg = 0x00FFFFFF;
STATIC UINT32  mBg = 0x00000000;

typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} DANDELION_CONSOLE_DEVICE_PATH;

STATIC DANDELION_CONSOLE_DEVICE_PATH  mDevicePath = {
  {
    {
      HARDWARE_DEVICE_PATH,
      HW_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH), 0 }
    },
    EFI_CALLER_ID_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

STATIC
UINT32
AttributeToColor (
  IN UINTN  Index
  )
{
  STATIC CONST UINT32  Palette[16] = {
    0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA,
    0x00AA0000, 0x00AA00AA, 0x00AA5500, 0x00AAAAAA,
    0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
    0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF
  };

  return Palette[Index & 0xF];
}

STATIC
VOID
FillRow (
  IN UINTN   Row,
  IN UINT32  Color
  )
{
  volatile UINT32  *Dst;
  UINTN            X;

  Dst = (volatile UINT32 *)(UINTN)(DANDELION_FB_BASE + (UINT64)Row * 16 * DANDELION_FB_STRIDE);
  for (X = 0; X < DANDELION_FB_WIDTH; X++) {
    Dst[X] = Color;
  }

  // Paint the hidden 16-pixel padding of the scanout line too, so nothing old
  // can bleed into the visible area when the panel re-reads the line.
  for (X = DANDELION_FB_WIDTH; X < DANDELION_FB_STRIDE / 4; X++) {
    Dst[X] = Color;
  }
}

STATIC
VOID
ScrollUp (VOID)
{
  CopyMem (
    (VOID *)(UINTN)DANDELION_FB_BASE,
    (VOID *)(UINTN)(DANDELION_FB_BASE + (UINT64)16 * DANDELION_FB_STRIDE),
    (UINTN)(DANDELION_FB_HEIGHT - 16) * DANDELION_FB_STRIDE
    );

  FillRow (DANDELION_ROWS - 1, mBg);
}

STATIC
VOID
DrawGlyph (
  IN UINTN   Column,
  IN UINTN   Row,
  IN CHAR16  Ch
  )
{
  volatile UINT32  *Line;
  UINTN            LineIndex;
  UINTN            Bit;
  UINT8            Bits;

  if ((Ch < 0x20) || (Ch > 0x7E)) {
    Ch = '?';
  }

  for (LineIndex = 0; LineIndex < 16; LineIndex++) {
    Bits = mDandelionFont8x16[Ch - 0x20][LineIndex];
    Line = (volatile UINT32 *)(UINTN)(
             DANDELION_FB_BASE + (UINT64)(Row * 16 + LineIndex) * DANDELION_FB_STRIDE
             ) + Column * 8;

    for (Bit = 0; Bit < 8; Bit++) {
      Line[Bit] = (Bits & (0x80 >> Bit)) ? mFg : mBg;
    }
  }
}

STATIC
VOID
ApplyAttribute (
  IN UINTN  Attribute
  )
{
  mFg = AttributeToColor (Attribute & 0xF);
  mBg = AttributeToColor ((Attribute >> 4) & 0xF);
}

STATIC
EFI_STATUS
EFIAPI
DandelionReset (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          ExtendedVerification
  )
{
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DandelionOutputString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  if (String == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  for ( ; *String != L'\0'; String++) {
    switch (*String) {
      case L'\r':
        mMode.CursorColumn = 0;
        break;

      case L'\n':
        mMode.CursorColumn = 0;
        if (mMode.CursorRow + 1 >= DANDELION_ROWS) {
          ScrollUp ();
        } else {
          mMode.CursorRow++;
        }

        break;

      case L'\b':
        if (mMode.CursorColumn > 0) {
          mMode.CursorColumn--;
        }

        break;

      default:
        DrawGlyph (mMode.CursorColumn, mMode.CursorRow, *String);
        mMode.CursorColumn++;
        if (mMode.CursorColumn >= DANDELION_COLS) {
          mMode.CursorColumn = 0;
          if (mMode.CursorRow + 1 >= DANDELION_ROWS) {
            ScrollUp ();
          } else {
            mMode.CursorRow++;
          }
        }

        break;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DandelionTestString (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN CHAR16                           *String
  )
{
  return (String == NULL) ? EFI_INVALID_PARAMETER : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DandelionQueryMode (
  IN  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN  UINTN                            ModeNumber,
  OUT UINTN                            *Columns,
  OUT UINTN                            *Rows
  )
{
  if ((Columns == NULL) || (Rows == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (ModeNumber >= mMode.MaxMode) {
    return EFI_UNSUPPORTED;
  }

  *Columns = DANDELION_COLS;
  *Rows    = DANDELION_ROWS;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DandelionSetMode (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            ModeNumber
  )
{
  if (ModeNumber >= mMode.MaxMode) {
    return EFI_UNSUPPORTED;
  }

  mMode.Mode         = (INT32)ModeNumber;
  mMode.CursorColumn = 0;
  mMode.CursorRow    = 0;
  ApplyAttribute (mMode.Attribute);

  for (UINTN Row = 0; Row < DANDELION_ROWS; Row++) {
    FillRow (Row, mBg);
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DandelionSetAttribute (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Attribute
  )
{
  mMode.Attribute = (INT32)Attribute;
  ApplyAttribute (Attribute);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DandelionClearScreen (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This
  )
{
  ApplyAttribute (mMode.Attribute);

  for (UINTN Row = 0; Row < DANDELION_ROWS; Row++) {
    FillRow (Row, mBg);
  }

  mMode.CursorColumn = 0;
  mMode.CursorRow    = 0;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DandelionSetCursorPosition (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN UINTN                            Column,
  IN UINTN                            Row
  )
{
  if ((Column >= DANDELION_COLS) || (Row >= DANDELION_ROWS)) {
    return EFI_UNSUPPORTED;
  }

  mMode.CursorColumn = (INT32)Column;
  mMode.CursorRow    = (INT32)Row;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
DandelionEnableCursor (
  IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *This,
  IN BOOLEAN                          Visible
  )
{
  mMode.CursorVisible = Visible;
  return EFI_SUCCESS;
}

STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  mTextOut = {
  DandelionReset,
  DandelionOutputString,
  DandelionTestString,
  DandelionQueryMode,
  DandelionSetMode,
  DandelionSetAttribute,
  DandelionClearScreen,
  DandelionSetCursorPosition,
  DandelionEnableCursor,
  &mMode
};

EFI_STATUS
EFIAPI
DandelionConsoleDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle;

  Handle = NULL;

  //
  // Start from a known state: light gray on black, cleared, cursor at home.
  // This also erases whatever the earlier stages left on the panel, which
  // makes the first BDS message unambiguous.
  //
  ApplyAttribute (mMode.Attribute);
  DandelionClearScreen (&mTextOut);

  //
  // Self test, 2026-10-28 19:1x: two lines through our own OutputString, at a
  // row nothing else touches (row 43/44 = y 688..720, between the 5x "UEFI"
  // at 560..640 and the progress driver's band at 1560).
  //
  // Purpose: separate "our console cannot render" from "BDS never reaches our
  // console".  These two lines do not depend on ConSplitter, on BDS, or on any
  // protocol: they are the same code path BDS would use, driven directly.
  //
  //   crisp  -> our renderer is fine, so anything smeared later comes from a
  //             different writer, or from BDS not being connected to us
  //   smeared-> the bug is in this driver
  //
  mMode.CursorColumn = 0;
  mMode.CursorRow    = 43;
  DandelionOutputString (&mTextOut, L"SELFTEST 0123456789");
  mMode.CursorColumn = 0;
  mMode.CursorRow    = 44;
  DandelionOutputString (&mTextOut, L"ABCDEFGHIJKLMNOPQRSTUVWXYZ");

  //
  // Control line for the A/B test in PlatformProgressDxe: same digits, same
  // row spacing, written by the same OutputString -- but called directly here
  // instead of through ConOut.  Row 48 is deliberately one row below where
  // PlatformProgressDxe writes its ConOut probe (row 46).
  //
  mMode.CursorColumn = 0;
  mMode.CursorRow    = 48;
  DandelionOutputString (&mTextOut, L"DIRECT CALL 0123456789");

  //
  // Unmissable proof that this driver actually executed: two solid bars at the
  // bottom of the panel.  If they are absent, the DXE dispatcher never ran us
  // and the noise on screen comes from somewhere else entirely.
  //
  {
    volatile UINT32  *Line;
    UINTN            Y;
    UINTN            X;

    for (Y = 1200; Y < 1280; Y++) {
      Line = (volatile UINT32 *)(UINTN)(DANDELION_FB_BASE + (UINT64)Y * DANDELION_FB_STRIDE);
      for (X = 0; X < DANDELION_FB_WIDTH; X++) {
        Line[X] = 0x00FF00FF;
      }
    }

    for (Y = 1300; Y < 1380; Y++) {
      Line = (volatile UINT32 *)(UINTN)(DANDELION_FB_BASE + (UINT64)Y * DANDELION_FB_STRIDE);
      for (X = 0; X < DANDELION_FB_WIDTH; X++) {
        Line[X] = 0x00FFFF00;
      }
    }
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  &mDevicePath,
                  &gEfiSimpleTextOutProtocolGuid,
                  &mTextOut,
                  &gEfiConsoleOutDeviceGuid,
                  NULL,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Before taking ConOut over, record what it looked like: this is the only
  // remaining question about the console stack, and one photo answers it.
  //
  {
    static CONST CHAR8 Hex[] = "0123456789ABCDEF";
    CHAR8              Report[16];
    EFI_STATUS         QStatus;
    UINTN              Cols;
    UINTN              Rows;
    INT32              Mode;
    UINTN              Digit;

    Cols   = 0;
    Rows   = 0;
    Mode   = -1;
    QStatus = EFI_UNSUPPORTED;

    if ((gST->ConOut != NULL) && (gST->ConOut->Mode != NULL)) {
      Mode    = gST->ConOut->Mode->Mode;
      QStatus = gST->ConOut->QueryMode (
                               gST->ConOut,
                               (Mode >= 0) ? (UINTN)Mode : 0,
                               &Cols,
                               &Rows
                               );
    }

    Report[0] = 'C';
    Report[1] = 'O';
    Report[2] = 'U';
    Report[3] = 'T';
    Report[4] = ' ';
    Report[5] = EFI_ERROR (QStatus) ? 'X' : (CHAR8)('0' + ((Mode >= 0) && (Mode <= 9) ? Mode : 0));
    Report[6] = ' ';
    Report[7] = EFI_ERROR (QStatus) ? '-' : Hex[(Cols >> 4) & 0xF];
    Report[8] = EFI_ERROR (QStatus) ? '-' : Hex[Cols & 0xF];
    Report[9] = 'x';
    Report[10] = EFI_ERROR (QStatus) ? '-' : Hex[(Rows >> 4) & 0xF];
    Report[11] = EFI_ERROR (QStatus) ? '-' : Hex[Rows & 0xF];
    Report[12] = '\0';

    mMode.CursorColumn = 0;
    mMode.CursorRow    = 52;
    {
      CHAR16  Wide[16];

      for (Digit = 0; (Digit < 15) && (Report[Digit] != '\0'); Digit++) {
        Wide[Digit] = (CHAR16)Report[Digit];
      }

      Wide[Digit] = L'\0';
      DandelionOutputString (&mTextOut, Wide);
    }

    if (gST->ConOut != NULL) {
      gST->ConOut->SetCursorPosition (gST->ConOut, 0, 54);
      gST->ConOut->OutputString (gST->ConOut, L"VIA CONOUT 0123456789");
    }
  }

  //
  // Publish ourselves as THE console.
  //
  // Why this and not ConSplitter: ConPlatformDxe/ConSplitterDxe bind console
  // devices through the driver model, so a device only joins ConOut when
  // something calls ConnectController for it -- and that call, from inside our
  // own entry point, is what wedged the dispatcher in the previous build (the
  // bars and SELFTEST were on the panel, nothing after the connect ever ran,
  // and PlatformProgressDxe never got dispatched).
  //
  // On this board ConSplitter has nothing else to aggregate anyway: the serial
  // console is gone (no UART) and GraphicsConsoleDxe is gone (720 pitch).  So
  // pointing the system table's ConOut straight at our protocol is both
  // sufficient and the shortest path to readable UEFI text -- BdsDxe and every
  // UiApp read gST->ConOut, not ConSplitter, to print.
  //
  // This is a bring-up stage decision.  Once storage and UniXNU.efi work, the
  // tidy version is to let ConPlatform bind us normally and drop these two
  // lines.
  //
  gST->ConOut = &mTextOut;
  gST->StdErr = &mTextOut;

  return Status;
}
