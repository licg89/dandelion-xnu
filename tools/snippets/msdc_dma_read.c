/**
  One-byte checksum over the first 16 bytes of a descriptor.

  The controller verifies this on every GPD and BD; a wrong value fails the
  transfer with GPDCSERR/BDCSERR.  Algorithm and width from
  Platforms/Benesse/a04br3Pkg (MSDC_DESC_CHECKSUM_BYTES = 16, 0xff - sum).
**/
STATIC
UINT8
DdnDescriptorChecksum (
  IN CONST VOID  *Descriptor
  )
{
  CONST UINT8  *Bytes;
  UINT32       Index;
  UINT32       Sum;

  Bytes = (CONST UINT8 *)Descriptor;
  Sum   = 0;
  for (Index = 0; Index < DDN_DESC_CHECKSUM_BYTES; ++Index) {
    Sum += Bytes[Index];
  }

  return (UINT8)(0xFFU - (UINT8)Sum);
}

/**
  Read one or more blocks through the controller's DMA engine.

  Called *after* MsdcSendCmd has issued the data command (the command word carries
  the block length and the data type; the controller starts the data phase as soon
  as the card answers).  MSDC_CFG.PIO is therefore cleared before the command is
  written -- see MsdcSendCmd -- and this function only arms the descriptors and
  waits.

  No cache maintenance is done here on purpose.  The question this path answers is
  "does the card send data at all", and completion is visible in MSDC_INT without
  touching the buffer contents.  If DMA turns out to be the working path, the
  buffer must be cache-invalidated before the caller looks at it, and that needs
  CacheMaintenanceLib in the .inf -- a separate, deliberate change.
**/
EFI_STATUS
MsdcDmaRead (
  MSDC_PRIVATE_DATA* Private,
  VOID  *Buffer,
  UINT32 BufferLength)
{
  UINT32      Blocks;
  UINTN       Address;
  UINTN       GpdAddress;
  UINTN       BdAddress;
  UINTN       NullGpdAddress;
  UINT32      DmaControl;
  UINT32      IntStatus;
  UINT32      Spins;
  UINT8       Checksum;
  BOOLEAN     Done;
  BOOLEAN     Failed;

  if ((Buffer == NULL) || (BufferLength == 0) || ((BufferLength & 511U) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Failed     = FALSE;
  Blocks     = BufferLength >> 9;
  Address    = (UINTN)Buffer;
  GpdAddress = (UINTN)&mDdnGpd[0];
  BdAddress  = (UINTN)&mDdnBd;
  NullGpdAddress = (UINTN)&mDdnGpd[1];

  if (((Address >> 36) != 0) || (((Address + BufferLength - 1) >> 36) != 0) ||
      ((GpdAddress >> 36) != 0) || ((BdAddress >> 36) != 0) ||
      ((NullGpdAddress >> 36) != 0))
  {
    return EFI_DEVICE_ERROR;
  }

  //
  // Clear both descriptor areas with a plain byte loop: this file does not link
  // BaseMemoryLib, and adding a library for two memsets is not worth an .inf
  // change in the middle of a bring-up.
  //
  {
    UINT8  *Raw;
    UINTN  Index;

    Raw = (UINT8 *)&mDdnBd;
    for (Index = 0; Index < sizeof (mDdnBd); Index++) {
      Raw[Index] = 0;
    }

    Raw = (UINT8 *)mDdnGpd;
    for (Index = 0; Index < sizeof (mDdnGpd); Index++) {
      Raw[Index] = 0;
    }
  }

  //
  // Buffer descriptor: the destination, its length, and the end-of-list bit.
  //
  mDdnBd.Info = DDN_BD_EOL |
                (((UINT32)(Address >> 32) & DDN_DESC_HIGH_MASK) << DDN_BD_DATA_HIGH_SHIFT);
  mDdnBd.Pointer      = (UINT32)Address;
  mDdnBd.BufferLength = (UINT32)BufferLength & DDN_DESC_LENGTH_MASK;
  Checksum            = DdnDescriptorChecksum (&mDdnBd);
  mDdnBd.Info        |= (UINT32)Checksum << DDN_DESC_CHECKSUM_SHIFT;

  //
  // GPD 0: hardware-owned, points at the BD; GPD 1 is the terminating entry.
  //
  mDdnGpd[0].Info    = DDN_GPD_HWO | DDN_GPD_BDP |
                       (((UINT32)(NullGpdAddress >> 32) & DDN_DESC_HIGH_MASK) << DDN_GPD_NEXT_HIGH_SHIFT) |
                       (((UINT32)(BdAddress >> 32) & DDN_DESC_HIGH_MASK) << DDN_GPD_BD_HIGH_SHIFT);
  mDdnGpd[0].Next    = (UINT32)NullGpdAddress;
  mDdnGpd[0].Pointer = (UINT32)BdAddress;
  Checksum           = DdnDescriptorChecksum (&mDdnGpd[0]);
  mDdnGpd[0].Info   |= (UINT32)Checksum << DDN_DESC_CHECKSUM_SHIFT;

  Checksum          = DdnDescriptorChecksum (&mDdnGpd[1]);
  mDdnGpd[1].Info  |= (UINT32)Checksum << DDN_DESC_CHECKSUM_SHIFT;

  //
  // Clean the descriptors out of the cache before the engine walks them.  On this
  // SoC the controller reads DRAM directly, so a descriptor still dirty in the CPU
  // cache is a descriptor the engine has never seen.
  //
  WriteBackInvalidateDataCacheRange ((VOID *)&mDdnBd, sizeof (mDdnBd));
  WriteBackInvalidateDataCacheRange ((VOID *)mDdnGpd, sizeof (mDdnGpd));

  //
  // Arm and start.  Order matters: the start address is latched before the
  // descriptor enable, and the start bit is set last.
  //
  MsdcWrite (Private, DDN_DMA_SA_HIGH, (UINT32)(GpdAddress >> 32) & DDN_DESC_HIGH_MASK);
  MsdcWrite (Private, DDN_DMA_SA, (UINT32)GpdAddress);
  MsdcWrite (Private, DDN_DMA_LEN, 0);
  MsdcSetBits (Private, DDN_DMA_CFG, DDN_DMA_CFG_DECSEN);

  MsdcRead (Private, DDN_DMA_CTRL, &DmaControl);
  DmaControl &= ~(DDN_DMA_CTRL_START | DDN_DMA_CTRL_STOP |
                  DDN_DMA_CTRL_LASTBUF | DDN_DMA_CTRL_BURST_MASK);
  DmaControl |= DDN_DMA_CTRL_MODE | DDN_DMA_CTRL_BURST_64B;
  MsdcWrite (Private, DDN_DMA_CTRL, DmaControl);
  MsdcSetBits (Private, DDN_DMA_CTRL, DDN_DMA_CTRL_START);

  Done  = FALSE;
  Spins = 0;

  //
  // Wait on a *clock*, not on a loop count.
  //
  // The first version spun four million times and gave up after roughly a tenth
  // of a second -- shorter than the controller's own data timeout -- so it
  // reported "no interrupt at all" before the hardware had a chance to say
  // DATTMO.  A measurement that ends before the thing being measured can happen
  // is worse than no measurement: it looks like evidence.  Two seconds of
  // one-millisecond sleeps is longer than any of these timeouts.
  //
  while (!Done) {
    MsdcRead (Private, MSDC_INT, &IntStatus);

    if (IntStatus & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR | MSDC_INT_ACMDTMO |
                     MSDC_INT_ACMDCRCERR | MSDC_INT_BDCSERR | MSDC_INT_GPDCSERR))
    {
      UINT32  DbgDmaCfg, DbgDmaCtrl, DbgDmaCa, DbgFifo;

      MsdcRead (Private, DDN_DMA_CFG, &DbgDmaCfg);
      MsdcRead (Private, DDN_DMA_CTRL, &DbgDmaCtrl);
      MsdcRead (Private, DDN_DMA_CA, &DbgDmaCa);
      MsdcRead (Private, MSDC_FIFOCS, &DbgFifo);
      DEBUG ((
        DEBUG_ERROR,
        "ddn dma FAIL int=%08x cfg=%08x ctrl=%08x ca=%08x fifo=%08x",
        IntStatus,
        DbgDmaCfg,
        DbgDmaCtrl,
        DbgDmaCa,
        DbgFifo
        ));
      MsdcPanelValue (0, IntStatus);
      MsdcPanelValue (2, DbgFifo);
      Failed    = TRUE;
      IntStatus = 0;
      Done      = TRUE;
      break;
    }

    if (IntStatus & MSDC_INT_XFER_COMPL) {
      MsdcWrite (Private, MSDC_INT, IntStatus);
      Done = TRUE;
      break;
    }

    if (IntStatus != 0) {
      MsdcWrite (Private, MSDC_INT, IntStatus);
    }

    MicroSecondDelay (1000);
    if (++Spins > 2000) {
      DEBUG ((DEBUG_ERROR, "ddn dma stall int=%08x", IntStatus));
      MsdcPanelValue (0, IntStatus);
      Failed = TRUE;
      break;
    }
  }

  //
  // Stop the engine and leave the controller in PIO mode again, whatever
  // happened: the next command must not inherit a half-armed DMA channel.
  //
  MsdcSetBits (Private, DDN_DMA_CTRL, DDN_DMA_CTRL_STOP);
  for (Spins = 0; Spins < 1000000; Spins++) {
    MsdcRead (Private, DDN_DMA_CFG, &DmaControl);
    if ((DmaControl & DDN_DMA_CFG_STS) == 0) {
      break;
    }
  }
  MsdcClrBits (Private, DDN_DMA_CFG, DDN_DMA_CFG_DECSEN);
  MsdcSetBits (Private, MSDC_CFG, MSDC_CFG_PIO);

  //
  // Report what actually happened.  `Failed` is the authority: the error branch
  // clears IntStatus so the latched bits cannot be mistaken for a clean run, and
  // a stall leaves Done FALSE.  Getting this wrong the first time made every
  // failure look like a success, which is the one outcome a bring-up cannot
  // afford.
  //
  if (Failed) {
    return EFI_DEVICE_ERROR;
  }

  if (!Done) {
    return EFI_TIMEOUT;
  }

  //
  // And drop the destination's cache lines so the caller sees what the engine
  // wrote rather than what was there before.
  //
  InvalidateDataCacheRange (Buffer, BufferLength);

  MsdcPanelValue (5, BufferLength);
  DEBUG ((DEBUG_ERROR, "ddn dma ok len=%u blocks=%u", BufferLength, Blocks));
  return EFI_SUCCESS;
}
