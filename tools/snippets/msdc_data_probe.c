//
// Defined further down the file; the probe below needs it.
//
EFI_STATUS
MsdcSendCmd (
  MSDC_PRIVATE_DATA* Private,
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET *Packet
  );

/**
  Tell the card to stop transmitting, and get it back into Transfer state.

  A read that ends halfway (no data, or fewer bytes than the block) leaves the
  card clocking out a data block that nobody is reading.  Until it is told to
  stop, it ignores every command -- which is exactly what the panel showed on
  2026-10-29 (IMG_5724): the data read failed, and the *next* command, a CMD7,
  came back EFI_TIMEOUT (status=0x12) for no reason of its own.

  LK does this after every failed transfer (msdc_cmd_stop); this driver never did.
  CMD12 is R1b with the stop bit, and it is best effort: if the card is already
  idle it answers with an error, which is fine.
**/
STATIC
VOID
DdnStopTransmission (
  MSDC_PRIVATE_DATA  *Private
  )
{
  UINT32  IntStatus;
  UINT32  Spins;

  MsdcWrite (Private, MSDC_INT, 0xFFFFFFFF);
  MsdcWrite (Private, SDC_ARG, 0);
  //
  // opcode 12 | RSPTYP 7 (R1b) << 7 | stop (bit 14)
  //
  MsdcWrite (Private, SDC_CMD, 12 | (7 << 7) | BIT14);

  for (Spins = 0; Spins < 100; Spins++) {
    MsdcRead (Private, MSDC_INT, &IntStatus);
    if (IntStatus & (MSDC_INT_CMDRDY | MSDC_INT_CMDTMO | MSDC_INT_CMDCRCERR)) {
      break;
    }

    MicroSecondDelay (1000);
  }

  MsdcWrite (Private, MSDC_INT, 0xFFFFFFFF);
  MsdcSetBits (Private, MSDC_CFG, MSDC_CFG_PIO);
}

/**
  Ask the card for its 512-byte ext-CSD twice, once per data path.

  Why do this at all when EmmcDxe is about to read the same thing: EmmcDxe gives
  up after the first failed transfer, so a single boot could only ever test one
  data path, and the answer "does the card send data at all" kept being decided by
  whichever path happened to be first.  This probe runs immediately after
  identification, does the transfer with PIO, stops the card, does it again with
  DMA, stops the card again, and prints both MSDC_INT values.

  Nothing here is fatal: the probe always leaves the card in Transfer state and the
  controller in PIO mode, so identification carries on regardless.
**/
STATIC
VOID
DdnDataProbe (
  MSDC_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS                          Status;
  VOID                                *Buffer;
  EFI_SD_MMC_COMMAND_BLOCK            CmdBlk;
  EFI_SD_MMC_STATUS_BLOCK             StatusBlk;
  EFI_SD_MMC_PASS_THRU_COMMAND_PACKET Packet;
  UINT32                              IntAfter;
  BOOLEAN                             SavedDma;

  Buffer = AllocatePool (512);
  if (Buffer == NULL) {
    return;
  }

  //
  // Clear the three structures and the buffer with plain byte loops: this file
  // does not link BaseMemoryLib (see the DMA snippet for the same reasoning).
  //
  {
    UINT8  *Raw;
    UINTN  Index;

    Raw = (UINT8 *)&CmdBlk;
    for (Index = 0; Index < sizeof (CmdBlk); Index++) {
      Raw[Index] = 0;
    }

    Raw = (UINT8 *)&StatusBlk;
    for (Index = 0; Index < sizeof (StatusBlk); Index++) {
      Raw[Index] = 0;
    }

    Raw = (UINT8 *)&Packet;
    for (Index = 0; Index < sizeof (Packet); Index++) {
      Raw[Index] = 0;
    }

    Raw = (UINT8 *)Buffer;
    for (Index = 0; Index < 512; Index++) {
      Raw[Index] = 0;
    }
  }

  CmdBlk.CommandIndex    = 8;                        // SEND_EXT_CSD
  CmdBlk.CommandType     = SdMmcCommandTypeAdtc;
  CmdBlk.ResponseType    = SdMmcResponseTypeR1;
  CmdBlk.CommandArgument = 0;

  Packet.SdMmcCmdBlk        = &CmdBlk;
  Packet.SdMmcStatusBlk     = &StatusBlk;
  Packet.InDataBuffer       = Buffer;
  Packet.InTransferLength   = 512;

  SavedDma = mDdnUseDma;

  //
  // 1. PIO.
  //
  mDdnUseDma = FALSE;
  Status     = MsdcSendCmd (Private, &Packet);
  MsdcRead (Private, MSDC_INT, &IntAfter);
  DEBUG ((
    DEBUG_ERROR,
    "ddn probe pio res=%08x int=%08x",
    (UINT32)Status,
    IntAfter
    ));
  MsdcPanelValue (3, IntAfter);
  DdnStopTransmission (Private);

  //
  // 2. DMA.
  //
  mDdnUseDma = TRUE;
  Status     = MsdcSendCmd (Private, &Packet);
  MsdcRead (Private, MSDC_INT, &IntAfter);
  DEBUG ((
    DEBUG_ERROR,
    "ddn probe dma res=%08x int=%08x",
    (UINT32)Status,
    IntAfter
    ));
  MsdcPanelValue (4, IntAfter);
  DdnStopTransmission (Private);

  mDdnUseDma = SavedDma;
}
