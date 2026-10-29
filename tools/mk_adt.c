/* Host-side generator for the Apple Device Tree DarwinBoot insists on.
 *
 * Why: DarwinBoot does not use the UEFI device tree at all.  In Boot() it reads
 * the kernel, then builds "<kernel path>.adt" and reads THAT, then walks it for
 * the ramdisk / trustcache / addon offsets:
 *
 *     StrCatS (TreePath, ..., L".adt");
 *     Status = ReadFile (Root, TreePath, SIZE_16MB, &Tree, &TreeSize);
 *     if (EFI_ERROR (Status)) { goto Fail; }
 *     ...
 *     DARWINBOOT: boot refused: Not Found; firmware retained
 *
 * That is exactly the failure the phone reported once the kernel itself read
 * fine (the DandelionEmmcDxe progress bar went fully green at 15 MiB).  So the
 * payload volume needs \EFI\UniXNU\kernel.macho.adt.
 *
 * The serializer already exists in this repository: bootshim/adt.c, line A's
 * "dandelion Apple Device Tree serializer", which builds the same structure the
 * line-A shim hands to XNU.  It compiles on the host, so this program is nothing
 * but a main() around dandelion_make_adt().
 *
 *     cc -O2 -I. -Ipexpert -Ipexpert/pexpert/arm64 -Ibootshim \
 *        tools/mk_adt.c bootshim/adt.c -o out/mk_adt
 *     out/mk_adt -o Resources/UniXNU/kernel.macho.adt
 *
 * The four numbers are guesses on purpose and they are named so: phys base and
 * memory size only have to be plausible for a first attempt, and the timebase is
 * MT6765's 13 MHz.  Whoever tunes the kernel will be back here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

size_t dandelion_make_adt (void *buffer, size_t capacity,
                           uint64_t phys_base, uint64_t mem_size,
                           uint64_t kernel_end_pa,
                           uint32_t timebase,
                           const unsigned char *seed, size_t seed_bytes);

int
main (int argc, char **argv)
{
  const char           *out = "Resources/UniXNU/kernel.macho.adt";
  uint64_t              phys_base = 0x40000000ULL;         /* MT6765 DRAM base  */
  uint64_t              mem_size  = 0xC0000000ULL;         /* 3 GiB, the large variant */
  uint64_t              kernel_end = 0x50000000ULL;        /* 256 MiB into DRAM */
  uint64_t              timebase  = 13000000ULL;           /* 13 MHz generic timer */
  unsigned char        *buffer;
  size_t                capacity = 1u << 20;
  size_t                used;
  FILE                 *fp;
  size_t                index;

  for (index = 1; index < (size_t)argc; index++) {
    if ((strcmp (argv[index], "-o") == 0) && (index + 1 < (size_t)argc)) {
      out = argv[++index];
    } else if ((strcmp (argv[index], "--base") == 0) && (index + 1 < (size_t)argc)) {
      phys_base = strtoull (argv[++index], NULL, 0);
    } else if ((strcmp (argv[index], "--mem") == 0) && (index + 1 < (size_t)argc)) {
      mem_size = strtoull (argv[++index], NULL, 0);
    } else if ((strcmp (argv[index], "--kernel-end") == 0) && (index + 1 < (size_t)argc)) {
      kernel_end = strtoull (argv[++index], NULL, 0);
    } else if ((strcmp (argv[index], "--timebase") == 0) && (index + 1 < (size_t)argc)) {
      timebase = strtoull (argv[++index], NULL, 0);
    } else {
      fprintf (stderr, "usage: %s [-o out.adt] [--base 0x..] [--mem 0x..]"
                       " [--kernel-end 0x..] [--timebase N]\n", argv[0]);
      return 2;
    }
  }

  buffer = malloc (capacity);
  if (buffer == NULL) {
    fprintf (stderr, "out of memory\n");
    return 1;
  }

  used = dandelion_make_adt (buffer, capacity, phys_base, mem_size, kernel_end,
                             (uint32_t)timebase, NULL, 0);
  if (used == 0) {
    fprintf (stderr, "dandelion_make_adt failed (capacity %zu)\n", capacity);
    return 1;
  }

  fp = fopen (out, "wb");
  if (fp == NULL) {
    fprintf (stderr, "cannot write %s\n", out);
    return 1;
  }

  if (fwrite (buffer, 1, used, fp) != used) {
    fprintf (stderr, "short write\n");
    fclose (fp);
    return 1;
  }

  fclose (fp);
  printf ("wrote %s, %zu bytes\n", out, used);
  printf ("  phys_base  0x%llX\n", (unsigned long long)phys_base);
  printf ("  mem_size   0x%llX\n", (unsigned long long)mem_size);
  printf ("  kernel_end 0x%llX\n", (unsigned long long)kernel_end);
  printf ("  timebase   %llu\n", (unsigned long long)timebase);
  free (buffer);
  return 0;
}
