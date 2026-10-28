//
// dandelion: MTK GPD/BD descriptor definitions for the DMA read path.
//
// Copied from Platforms/Benesse/a04br3Pkg/Drivers/SdCardBlockIoDxe, which is the
// only implementation in this tree that moves data on hardware of this family
// (MT6765-class MSDC).  The layout is fixed by the controller:
//
//   GPD (general purpose descriptor) 28 bytes: where the buffer descriptors are
//   BD  (buffer descriptor)          16 bytes: where the data goes
//
// Both carry a one-byte checksum in bits 15:8 that the controller verifies; get
// it wrong and the transfer fails with BDCSERR/GPDCSERR, which is at least a
// clear failure rather than a silent one.
//
typedef struct {
  UINT32  Info;
  UINT32  Next;
  UINT32  Pointer;
  UINT32  BufferLength;
  UINT32  Argument;
  UINT32  BlockCount;
  UINT32  Command;
} DDN_MSDC_GPD;

typedef struct {
  UINT32  Info;
  UINT32  Next;
  UINT32  Pointer;
  UINT32  BufferLength;
} DDN_MSDC_BD;

#define DDN_GPD_HWO             BIT0
#define DDN_GPD_BDP             BIT1
#define DDN_DESC_CHECKSUM_SHIFT 8
#define DDN_GPD_NEXT_HIGH_SHIFT 24
#define DDN_GPD_BD_HIGH_SHIFT   28
#define DDN_BD_EOL              BIT0
#define DDN_BD_DATA_HIGH_SHIFT  28
#define DDN_DESC_HIGH_MASK      0xFU
#define DDN_DESC_LENGTH_MASK    0x00FFFFFFU
#define DDN_DESC_CHECKSUM_BYTES 16U

#define DDN_DMA_SA_HIGH         (0x08c)
#define DDN_DMA_SA              (0x090)
#define DDN_DMA_CA              (0x094)
#define DDN_DMA_CTRL            (0x098)
#define DDN_DMA_CFG             (0x09c)
#define DDN_DMA_LEN             (0x0a8)

#define DDN_DMA_CTRL_START      BIT0
#define DDN_DMA_CTRL_STOP       BIT1
#define DDN_DMA_CTRL_MODE       BIT8
#define DDN_DMA_CTRL_LASTBUF    BIT10
#define DDN_DMA_CTRL_BURST_MASK (0x7U << 12)
#define DDN_DMA_CTRL_BURST_64B  (0x6U << 12)
#define DDN_DMA_CFG_STS         BIT0
#define DDN_DMA_CFG_DECSEN      BIT1

STATIC DDN_MSDC_GPD  mDdnGpd[2] __attribute__ ((aligned (64)));
STATIC DDN_MSDC_BD   mDdnBd __attribute__ ((aligned (64)));

//
// CacheMaintenanceLib, declared rather than included.
//
// This file has no include block a patch can extend, and the two prototypes are
// stable EDK2 API.  The library itself has to be listed in MsdcDxe.inf -- the
// generator adds it there, in the same patch -- or the link fails with
// "undefined symbol", which is a clear enough failure.
//
// Both calls are load-bearing for DMA on ARM:
//
//   * the descriptors we just wrote are cacheable data; without cleaning them the
//     controller reads stale bytes out of DRAM and fails the descriptor checksum;
//   * the destination buffer's cache lines must be invalidated afterwards or the
//     CPU keeps reading whatever it had before the transfer.
//
VOID
WriteBackInvalidateDataCacheRange (
  IN VOID   *Address,
  IN UINTN  Length
  );

VOID
InvalidateDataCacheRange (
  IN VOID   *Address,
  IN UINTN  Length
  );

//
// Which data path the next read uses.
//
// TRUE means DMA first, because PIO has never moved a byte on this board: every
// attempt ends in DATTMO with an empty FIFO (see
// docs/ANALYSIS-EMMC-DATA-PATH.md).  If the DMA attempt fails the flag is cleared
// so the rest of the boot behaves exactly as before -- one experiment, no
// regression.
//
STATIC BOOLEAN  mDdnUseDma = TRUE;
