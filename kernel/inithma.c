/****************************************************************/
/*                                                              */
/*                          initHMA.c                           */
/*                            DOS-C                             */
/*                                                              */
/*          move kernel to HMA area                             */
/*                                                              */
/*                      Copyright (c) 2001                      */
/*                      tom ehlert                              */
/*                      All Rights Reserved                     */
/*                                                              */
/* This file is part of DOS-C.                                  */
/*                                                              */
/* DOS-C is free software; you can redistribute it and/or       */
/* modify it under the terms of the GNU General Public License  */
/* as published by the Free Software Foundation; either version */
/* 2, or (at your option) any later version.                    */
/*                                                              */
/* DOS-C is distributed in the hope that it will be useful, but */
/* WITHOUT ANY WARRANTY; without even the implied warranty of   */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See    */
/* the GNU General Public License for more details.             */
/*                                                              */
/* You should have received a copy of the GNU General Public    */
/* License along with DOS-C; see the file COPYING.  If not,     */
/* write to the Free Software Foundation, 675 Mass Ave,         */
/* Cambridge, MA 02139, USA.                                    */
/****************************************************************/

/*
    current status:

    load FreeDOS high, if DOS=HIGH detected

    suppress High Loading, if any SHIFT status detected (for debugging)

    if no XMS driver (HIMEM,FDXMS,...) loaded, should work

    cooperation with XMS drivers as follows:

    copy HMA_TEXT segment up.

    after each loaded DEVICE=SOMETHING.SYS, try to request the HMA
    (XMS function 0x01).
    if no XMS driver detected, during ONFIG.SYS processing,
    create a dummy VDISK entry in high memory

    this works with

     FD FDXMS - no problems detected


     MS HIMEM.SYS (from DOS 6.2, 9-30-93)

        works if and only if

            /TESTMEM:OFF

        is given

        otherwise HIMEM will TEST AND ZERO THE HIGH MEMORY+HMA.
        so, in CONFIG.C, if "HIMEM.SYS" is detected, a "/TESTMEM:OFF"
        parameter is forced.
*/

#include "portab.h"
#include "globals.h"
#include "init-mod.h"

#ifdef VERSION_STRINGS
static BYTE *RcsId =
    "$Id: inithma.c 956 2004-05-24 17:07:04Z bartoldeman $";
#endif

BSS(BYTE, DosLoadedInHMA, FALSE);  /* set to TRUE if loaded HIGH          */
BSS(BYTE, HMAclaimed, 0);          /* set to TRUE if claimed from HIMEM   */
BSS(UWORD, HMAFree, 0);            /* first byte in HMA not yet used      */

STATIC void InstallVDISK(void);

#ifdef DEBUG
#ifdef __TURBOC__
#define int3() __int__(3);
#elif defined(__WATCOMC__)
void int3()
{
  __asm int 3;
}
#endif
#else
#define int3()
#endif

#ifdef DEBUG
#define HMAInitPrintf(x) DebugPrintf(x)
#else
#define HMAInitPrintf(x)
#endif

#if 0
#ifdef DEBUG
STATIC VOID hdump(BYTE FAR * p)
{
  int loop;
  HMAInitPrintf(("%P", GET_FP32(p)));

  for (loop = 0; loop < 16; loop++)
    HMAInitPrintf(("%02x ", (const char)p[loop]));

  DebugPrintf(("\n"));
}
#else
#define hdump(ptr)
#endif
#endif

#define KeyboardShiftState() (*(BYTE FAR *)(MK_FP(0x40,0x17)))

/*
    this tests, if the HMA area can be enabled.
    if so, it simply leaves it on
*/

STATIC int EnabledA20(void)
{
  return fmemcmp(MK_FP_N(0, 0), MK_FP(0xffff, 0x0010), 128);
}

STATIC int EnableHMA(VOID)
{

  _EnableA20();

  if (!EnabledA20())
  {
    _printf("HMA can't be enabled\n");
    return FALSE;
  }

  _DisableA20();

#ifdef DEBUG
  if (EnabledA20())
  {
    DebugPrintf(("HMA can't be disabled - no problem for us\n"));
  }
#endif

  _EnableA20();
  if (!EnabledA20())
  {
    DebugPrintf(("HMA can't be enabled second time\n"));
    return FALSE;
  }

  HMAInitPrintf(("HMA success - leaving enabled\n"));

  return TRUE;

}

/*
    move the kernel up to high memory
    this is very unportable

    if we thin we succeeded, we return TRUE, else FALSE
*/

#define HMAOFFSET  0x20
#define HMASEGMENT 0xffff

int MoveKernelToHMA()
{
  void FAR *xms_addr;

  if (DosLoadedInHMA)
  {
    return TRUE;
  }

  if ((xms_addr = DetectXMSDriver()) == NULL)
    return FALSE;

  XMSDriverAddress = xms_addr;

#ifdef DEBUG
  /* A) for debugging purpose, suppress this,
     if any shift key is pressed
   */
  if (KeyboardShiftState() & 0x0f)
  {
    DebugPrintf(("Keyboard state is %0x, NOT moving to HMA\n",
           KeyboardShiftState()));
    return FALSE;
  }
#endif

  /* B) check out, if we can have HMA */

  if (!EnableHMA())
  {
    _printf("Can't enable HMA area (the famous A20), NOT moving to HMA\n");

    return FALSE;
  }

  /*  allocate HMA through XMS driver */

  if (HMAclaimed == 0 &&
      (HMAclaimed =
       init_call_XMScall(xms_addr, 0x0100, 0xffff)) == 0)
  {
    _printf("Can't reserve HMA area ??\n");

    return FALSE;
  }

  MoveKernel(0xffff);

  {
    /* E) up to now, nothing really bad was done.
       but now, we reuse the HMA area. bad things will happen

       to find bugs early,
       cause INT 3 on all accesses to this area
     */

    DosLoadedInHMA = TRUE;
  }

  /*
    on finalize, will install a VDISK
  */

  InstallVDISK();

  /* report the fact we are running high through int 21, ax=3306 */
  LoL->_version_flags |= 0x10;

  return TRUE;

}

/*
    now protect against HIMEM/FDXMS/... by simulating a VDISK
    FDXMS should detect us and not give HMA access to ohers
    unfortunately this also disables HIMEM completely

    so: we install this after all drivers have been loaded
*/
  static struct _S {               /* Boot-Sektor of a RAM-Disk */
    UBYTE dummy1[3];            /* HIMEM.SYS uses 3, but FDXMS uses 2 */
    char Name[5];
    BYTE dummy2[3];
    WORD BpS;
    BYTE dummy3[6];
    WORD Sektoren;
    BYTE dummy4;
  } VDISK_BOOT_SEKTOR = {
    {
    0xcf, ' ', ' '},
    {
    'V', 'D', 'I', 'S', 'K'},
    {
    ' ', ' ', ' '}, 512,
    {
    'F', 'D', 'O', 'S', ' ', ' '}, 128, /* 128*512 = 64K */
  ' '};

STATIC void InstallVDISK(void)
{
  if (!DosLoadedInHMA)
    return;

  n_fmemcpy(MK_FP(0xffff, 0x0010), &VDISK_BOOT_SEKTOR,
          sizeof(VDISK_BOOT_SEKTOR));

  *(WORD FAR *) MK_FP(0xffff, 0x002e) = 1024 + 64;
}

/*
    this allocates some bytes from the HMA area
    only available if DOS=HIGH was successful
*/

VOID FAR * HMAalloc(UCOUNT bytesToAllocate)
{
  VOID FAR *HMAptr;

  if (!DosLoadedInHMA)
    return NULL;

  if (HMAFree > 0xfff0 - bytesToAllocate)
    return NULL;

  HMAptr = MK_FP(0xffff, HMAFree);

  /* align on 16 byte boundary */
  HMAFree = (HMAFree + bytesToAllocate + 0xf) & 0xfff0;

  /*_printf("HMA allocated %d byte at %x\n", bytesToAllocate, HMAptr); */

  fmemset(HMAptr, 0, bytesToAllocate);

  return HMAptr;
}

DATA(UWORD, CurrentKernelSegment, 0);

void MoveKernel(UWORD NewKernelSegment)
{
  UBYTE FAR *HMADest;
  UBYTE FAR *HMASource;
  UWORD len;
  UWORD offs = 0;
  struct RelocationTable FAR *rp;
  BOOL initial = 0;

  if (CurrentKernelSegment == 0) {
    CurrentKernelSegment = FP_SEG(_HMATextEnd);
    initial = 1;
  }

  if (CurrentKernelSegment == 0xffff)
    return;

  ___assert(!(FP_OFF(_HMATextStart) & 0xf));
  HMASource =
      MK_FP(CurrentKernelSegment, FP_OFF(_HMATextStart));
  HMADest = MK_FP(NewKernelSegment, 0x0000);

  len = FP_OFF(_HMATextEnd) - FP_OFF(_HMATextStart);

  if (NewKernelSegment == 0xffff)
  {
    HMASource += HMAOFFSET;
    HMADest += HMAOFFSET;
    len -= HMAOFFSET;
    offs = HMAOFFSET;
  }

  if (!initial)
    fmemcpy(HMADest, HMASource, len);
  /* else it's the very first relocation: handled by kernel.asm */

  HMAFree = (FP_OFF(HMADest) + len + 0xf) & 0xfff0;
  /* first free byte after HMA_TEXT on 16 byte boundary */

  if (NewKernelSegment == 0xffff)
  {
    /* jmp FAR cpm_entry (copy from 0:c0) */
    pokeb(0xffff, 0x30 * 4 + 0x10, 0xea);
    pokel(0xffff, 0x30 * 4 + 0x11, (ULONG)cpm_entry);
  }

  HMAInitPrintf(("HMA moving %P up to %P for %04x bytes\n",
                 GET_FP32(HMASource), GET_FP32(HMADest), len));

  NewKernelSegment -= FP_OFF(_HMATextStart) >> 4;
  offs += FP_OFF(_HMATextStart);
  for (rp = _HMARelocationTableStart; rp < _HMARelocationTableEnd; rp++)
  {
    *rp->jmpSegment = NewKernelSegment;
  }
  RelocHook(CurrentKernelSegment, NewKernelSegment, offs, len);
  CurrentKernelSegment = NewKernelSegment;
}
