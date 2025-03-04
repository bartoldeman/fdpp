/****************************************************************/
/*                                                              */
/*                           task.c                             */
/*                                                              */
/*                 Task Manager for DOS Processes               */
/*                                                              */
/*                      Copyright (c) 1995                      */
/*                      Pasquale J. Villani                     */
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

#include "portab.h"
#include "globals.h"
#include "init-mod.h"

#ifdef VERSION_STRINGS
static BYTE *RcsId =
    "$Id: task.c 1563 2011-04-08 16:04:24Z bartoldeman $";
#endif

#ifndef USE_STDLIB
#define toupper(c)	((c) >= 'a' && (c) <= 'z' ? (char)((c) + ('A' - 'a')) : (char)(c))
#else
#include <ctype.h>
#endif

#define LOADNGO 0
#define LOAD    1
#define OVERLAY 3

#define LOAD_HIGH 0x80

/* static exe_header ExeHeader;
                           to save some bytes, both static and on stack,
                           we recycle SecPathBuffer                 TE */

#define ExeHeader_p ((exe_header FAR *)(SecPathName + 0))
#define ExeHeader (*ExeHeader_p)
#define TempExeBlock_p ((exec_blk FAR *)(SecPathName + sizeof(exe_header)))
#define TempExeBlock (*TempExeBlock_p)
#define Shell (SecPathName + sizeof(exe_header) + sizeof(exec_blk))

#ifdef __TURBOC__ /* this is a Borlandism and doesn't work elsewhere */
 #if sizeof(SecPathName) < sizeof(exe_header) + sizeof(exec_blk) + NAMEMAX
  #error No room in SecPathName to be recycled!
 #endif
#endif

STATIC DWORD res_read(UWORD fd, void FAR *buf, UCOUNT count);

#define CHUNK 32256
#define MAXENV 32768u
#define ENV_KEEPFREE 83         /* keep unallocated by environment variables */
        /* The '65' added to nEnvSize does not cover the additional stuff:
           + 2 bytes: number of strings
           + 80 bytes: maximum absolute filename
           + 1 byte: '\0'
           -- 1999/04/21 ska */

#ifdef __WATCOMC__
intvec getvec(unsigned char intno)
{
  intvec iv;
  disable();
  iv = *(intvec FAR *)MK_FP(0,4 * (intno));
  enable();
  return iv;
}

void setvec(unsigned char intno, intvec vector)
{
  disable();
  *(intvec FAR *)MK_FP(0,4 * intno) = vector;
  enable();
}
#endif

ULONG SftGetFsize(int sft_idx)
{
  sft FAR *s = idx_to_sft(sft_idx);

  /* Get the SFT block that contains the SFT      */
  if (FP_OFF(s) == (UWORD) -1)
    return DE_INVLDHNDL;

  return s->sft_size;
}

STATIC WORD SetverCompareFilename(const char FAR *m1, const char *m2, COUNT c)
{
  while (c--)
  {
    if (toupper(*m1) != toupper(*m2))
    {
      return *m1 - *m2;
    }

    m1 = m1 + 1; m2 = m2 + 1;
  }

  return 0;
}

STATIC UWORD SetverGetVersion(BYTE *table, const char FAR *name)
{
  BYTE *len;
  COUNT nlen;

  if ((table != NULL) && (name != NULL))
  {
    nlen = fstrlen(name);

    while (*(len = table) != 0)
    {
      if ((*len == nlen) && (SetverCompareFilename(name, table + 1, *len) == 0))
      {
        return *((UWORD *)(table + *len + 1));
      }

      table = table + *len + 3;
    }
  }

  return 0;
}

STATIC COUNT ChildEnv(exec_blk * exp, seg * pChildEnvSeg, const char FAR * pathname)
{
  BYTE FAR *pSrc;
  BYTE FAR *pDest;
  UWORD nEnvSize;
  COUNT RetCode;
/*  UWORD MaxEnvSize;                                                                                                                                           not used -- 1999/04/21 ska */
  psp FAR *ppsp = MK_FP(cu_psp, 0);

  /* create a new environment for the process             */
  /* copy parent's environment if exec.env_seg == 0       */

  pSrc = exp->exec.env_seg ?
      MK_FP(exp->exec.env_seg, 0) : MK_FP(ppsp->ps_environ, 0);

  /* This loop had not counted the very last '\0'
     -- 1999/04/21 ska */
  if (pSrc && pSrc[0])
  {
    for (nEnvSize = 0;; nEnvSize++)
    {
      /* Test env size and abort if greater than max          */
      if (nEnvSize >= MAXENV - ENV_KEEPFREE)
        return DE_INVLDENV;

      if (*(UWORD FAR *) (pSrc + nEnvSize) == 0)
        break;
    }
    nEnvSize += 2;              /* account for trailing \0\0 */
  }
  else if (!pSrc && master_env[0])
  {
    for (nEnvSize = 0;; nEnvSize++)
    {
      /* Test env size and abort if greater than max          */
      if (nEnvSize >= MAXENV - ENV_KEEPFREE)
        return DE_INVLDENV;

      if (*(UWORD *) (master_env + nEnvSize) == 0)
        break;
    }
    nEnvSize += 2;              /* account for trailing \0\0 */
    pSrc = MK_FAR_SZ(master_env, nEnvSize);
  }
  else
  {
    nEnvSize = 1;
  }

  /* allocate enough space for env + path                 */
  if ((RetCode = DosMemAlloc((nEnvSize + ENV_KEEPFREE + 15)/16,
                             mem_access_mode, pChildEnvSeg,
                             NULL /*(UWORD FAR *) MaxEnvSize ska */ )) < 0)
    return RetCode;
  fd_mark_mem_np(MK_FP(*pChildEnvSeg + 1, 0), ((nEnvSize + ENV_KEEPFREE + 15)/16)*16,
              FD_MEM_NORMAL);
  pDest = MK_FP(*pChildEnvSeg + 1, 0);

  /* fill the new env and inform the process of its       */
  /* location throught the psp                            */

  /* copy the environment */
  if (pSrc && pSrc[0])
  {
    fmemcpy(pDest, pSrc, nEnvSize);
    pDest += nEnvSize;
  }
  else
    *pDest++ = '\0';            /* create an empty environment */

  /* initialize 'extra strings' count */
  *((UWORD FAR *) pDest) = 1;
  pDest += sizeof(UWORD) / sizeof(BYTE);

  /* copy complete pathname */
  if ((RetCode = truename(pathname, PriPathName, CDS_MODE_SKIP_PHYSICAL)) < SUCCESS)
  {
    return RetCode;
  }
  fstrcpy(pDest, PriPathName);

  /* Theoretically one could either:
     + resize the already allocated block to best-fit behind the pathname, or
     + generate the filename into a temporary buffer to allocate only the
     minimum required environment -- 1999/04/21 ska */

  return SUCCESS;
}

static void copy_psp(psp FAR *p, psp FAR *q)
{
  fmemcpy(p, q, sizeof(psp));

  /* some progs (Alpha Waves game) create new psp from
   * the corrupted one. So restore also the sig. */
  p->ps_exit = PSP_SIG;
  /* terminate address                                    */
  p->ps_isv22 = getvec(0x22);
  /* break address                                        */
  p->ps_isv23 = getvec(0x23);
  /* critical error address                               */
  p->ps_isv24 = getvec(0x24);
}

void new_psp(seg para, seg cur_psp)
{
  psp FAR *p = MK_FP(para, 0);
  psp FAR *q = MK_FP(cur_psp, 0);
  int i;

  copy_psp(p, q);

  /* open file table pointer                              */
  p->ps_filetab = p->ps_files;
  /* maximum open files                                   */
  p->ps_maxfiles = 20;
  fmemset(p->ps_files, 0xff, 20);
  /* clone the file table -- without incrementing use cnt */
  for (i = 0; i < 20; i++)
    p->ps_files[i] = q->ps_filetab[i];
  /* DR-DOS says:
   *   we do not update file handle use unlike Int21/55
   * Unfortunately in this case Alpha Waves would hang on exit,
   * and it does so even on DR-DOS itself!
   * My best guess is that we need to at least increment the stdio fds. */
  for (i = 0; i < 3; i++)
    CloneHandle(i);
  /* RBIL is wrong on zeroing parent_psp, and in fact some
   * progs (Alpha Waves game, https://github.com/stsp/fdpp/issues/112)
   * won't work if its zeroed. */
#if 0
  /* parent psp segment set to 0 (see RBIL int21/ah=26)   */
  p->ps_parent = 0;
#endif
}

void child_psp(seg para, seg cur_psp, int psize)
{
  psp FAR *p = MK_FP(para, 0);
  psp FAR *q = MK_FP(cur_psp, 0);
  int i;

  copy_psp(p, q);

  /* Now for parent-child relationships                   */
  /* parent psp segment                                   */
  p->ps_parent = cu_psp;
  /* previous psp pointer                                 */
  p->ps_prevpsp = q;

  /* Environment and memory useage parameters             */
  /* memory size in paragraphs                            */
  p->ps_size = psize;

  /* File System parameters                               */
  /* maximum open files                                   */
  p->ps_maxfiles = 20;
  fmemset(p->ps_files, 0xff, 20);

  /* open file table pointer                              */
  p->ps_filetab = p->ps_files;

  /* clone the file table -- 0xff is unused               */
  for (i = 0; i < 20; i++)
    if (CloneHandle(i) >= 0)
      p->ps_files[i] = q->ps_filetab[i];

  /* first command line argument                          */
  p->ps_fcb1.fcb_drive = 0;
  fmemset(_DOS_FP(p->ps_fcb1).fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);
  /* second command line argument                         */
  p->ps_fcb2.fcb_drive = 0;
  fmemset(_DOS_FP(p->ps_fcb2).fcb_fname, ' ', FNAME_SIZE + FEXT_SIZE);

  /* local command line                                   */
  p->ps_cmd.ctCount = 0;
  p->ps_cmd.ctBuffer[0] = 0xd; /* command tail            */
}

STATIC UWORD patchPSP(UWORD pspseg, UWORD envseg, __XFAR(exec_blk)exb,
                      const char FAR * fnam)
{
  psp FAR *_psp;
  mcb FAR *pspmcb;
  int i;
  const char FAR *np;
  UWORD fakever;

  pspmcb = MK_FP(pspseg, 0);
  ++pspseg;
  _psp = MK_FP(pspseg, 0);

  /* complete the psp by adding the command line and FCBs     */
  fmemcpy(&_psp->ps_cmd, exb->exec.cmd_line, sizeof(CommandTail));
  if (exb->exec.fcb_1 != NULL && FP_OFF(exb->exec.fcb_1) != 0xffff)
    fmemcpy(&_psp->ps_fcb1, exb->exec.fcb_1, 16);
  if (exb->exec.fcb_2 != NULL && FP_OFF(exb->exec.fcb_2) != 0xffff)
    fmemcpy(&_psp->ps_fcb2, exb->exec.fcb_2, 16);

  /* identify the mcb as this functions'                  */
  fd_prot_mem(pspmcb, sizeof(*pspmcb), FD_MEM_NORMAL);
  pspmcb->m_psp = pspseg;
  fd_prot_mem(pspmcb, sizeof(*pspmcb), FD_MEM_READONLY);
  /* Patch in environment segment, if present, also adjust its MCB */
  if (envseg)
  {
    mcb FAR *pe = (mcb FAR *) MK_FP(envseg, 0);
    fd_prot_mem(pe, sizeof(*pe), FD_MEM_NORMAL);
    pe->m_psp = pspseg;
    fd_prot_mem(pe, sizeof(*pe), FD_MEM_READONLY);
    envseg++;
  }
  _psp->ps_environ = envseg;

  /* use the file name less extension - left adjusted and */
  np = fnam;
  for (;;)
  {
    switch (*fnam++)
    {
      case '\0':
        goto set_name;
      case ':':
      case '/':
      case '\\':
        np = fnam;
    }
  }
set_name:
  for (i = 0; i < 8 && np[i] != '.' && np[i] != '\0'; i++)
  {
    fd_prot_mem(pspmcb, sizeof(*pspmcb), FD_MEM_NORMAL);
    pspmcb->m_name[i] = toupper(np[i]);
    fd_prot_mem(pspmcb, sizeof(*pspmcb), FD_MEM_READONLY);
  }
  if (i < 8) {
    fd_prot_mem(pspmcb, sizeof(*pspmcb), FD_MEM_NORMAL);
    pspmcb->m_name[i] = '\0';
    fd_prot_mem(pspmcb, sizeof(*pspmcb), FD_MEM_READONLY);
  }

  if ((fakever = SetverGetVersion(setverPtr, np)) != 0)
  {
    _psp->ps_retdosver = fakever;
  }

  /* return value: AX value to be passed based on FCB values */
  return (get_cds1(_psp->ps_fcb1.fcb_drive) ? 0 : 0xff) |
    (get_cds1(_psp->ps_fcb2.fcb_drive) ? 0 : 0xff00);
}

STATIC int load_transfer(UWORD ds, exec_blk *exp, UWORD fcbcode, COUNT mode)
{
  psp FAR *p = MK_FP(ds, 0);
  psp FAR *q = MK_FP(cu_psp, 0);

  /* Transfer control to the executable                   */
  p->ps_parent = cu_psp;
  p->ps_prevpsp = q;
  q->ps_stack = (BYTE FAR *)user_r;
  user_r->FLAGS &= ~FLG_CARRY;

  cu_psp = ds;
  /* process dta                                          */
  dta = &p->ps_cmd;

  if (mode == LOADNGO)
  {
    exec_regs FAR *irp;

    /* build the user area on the stack                     */
    irp = (exec_regs FAR *)(exp->exec.stack - sizeof(exec_regs));

    irp->DS = ds;
    irp->CS = FP_SEG(exp->exec.start_addr);
    irp->IP = FP_OFF(exp->exec.start_addr);
    irp->DI = FP_OFF(exp->exec.stack);
    irp->AX = fcbcode;
    irp->FLAGS = 0x200;

    if (InDOS)
      --InDOS;
    exec_user(irp);

    /* We should never be here
       fatal("KERNEL RETURNED!!!");                    */
  }
  /* mode == LOAD */
  exp->exec.stack -= 2;
  *((UWORD FAR *)(exp->exec.stack)) = fcbcode;
  return SUCCESS;
}

/* Now find out how many paragraphs are available
   considering a threshold, trying HIGH then LOW */
STATIC int ExecMemLargest(UWORD *asize, UWORD threshold)
{
  int rc;
  if (mem_access_mode & 0x80)
  {
    mem_access_mode &= ~0x80;
    mem_access_mode |= 0x40;
    rc = DosMemLargest(asize);
    if (rc != SUCCESS || *asize < threshold)
    {
    /* less memory than the .COM/.EXE file has:
       try low memory first */
      mem_access_mode &= ~0x40;
      rc = DosMemLargest(asize);
      mem_access_mode |= 0x80;
    }
  }
  else
    rc = DosMemLargest(asize);
  return (*asize < threshold ? DE_NOMEM : rc);
}

STATIC int ExecMemAlloc(UWORD size, seg *para, UWORD *asize)
{
  /* We can still get an error on first fit if the above  */
  /* returned size was a best fit case                    */
  /* ModeLoadHigh = 80 = try high, then low               */
  int rc = DosMemAlloc(size, mem_access_mode, para, asize);

  if (rc != SUCCESS)
  {
    if (rc == DE_NOMEM)
    {
      rc = DosMemAlloc(0, LARGEST, para, asize);
      if ((mem_access_mode & 0x80) && (rc != SUCCESS))
      {
        mem_access_mode &= ~0x80;
        rc = DosMemAlloc(0, LARGEST, para, asize);
        mem_access_mode |= 0x80;
      }
    }
  }
  else
  {
    fd_mark_mem_np(MK_FP(*para + 1, 0), size * 16, FD_MEM_READEXEC);
    /* with no error, we got exactly what we asked for      */
    *asize = size;
  }

  /* This should never happen, but ... */
  if (rc == SUCCESS && *asize < size)
  {
    DosMemFree(*para);
    return DE_NOMEM;
  }
  return rc;
}

STATIC COUNT DosComLoader(const char FAR * namep, exec_blk FAR * exp, COUNT mode, COUNT fd)
{
  seg mem, env;
  UWORD asize = 0;

  {
    UWORD com_size;
    {
      ULONG com_size_long = SftGetFsize(fd);
      /* maximally 64k - 256 bytes stack -
         256 bytes psp */
      com_size = ((UWORD)min(com_size_long, 0xfe00u) >> 4) + 0x10;
    }

    if ((mode & 0x7f) != OVERLAY)
    {
      COUNT rc;
      UBYTE orig_mem_access = mem_access_mode;

      if (mode & 0x80)
      {
        mem_access_mode |= 0x80;
        DosUmbLink(1);            /* link in UMBs */
      }

      rc = ChildEnv(exp, &env, namep);

      /* COMFILES will always be loaded in largest area. is that true TE */
      /* yes, see RBIL, int21/ah=48 -- Bart */

      if (rc == SUCCESS)
        rc = ExecMemLargest(&asize, com_size);

      if (rc == SUCCESS)
        /* Allocate our memory and pass back any errors         */
        rc = ExecMemAlloc(asize, &mem, &asize);

      if (rc != SUCCESS)
        DosMemFree(env);

      /* comcom32 doesn't set mode, so reset also if mem_access_mode has UMA */
      if ((mode | orig_mem_access) & 0x80)
      {
        DosUmbLink(0);       /* restore link state */
        mem_access_mode = orig_mem_access & 0x7f;
        mode &= 0x7f;
      }
      if (rc != SUCCESS)
        return rc;

      ++mem;
    }
    else
      mem = exp->load.load_seg;
  }

#ifdef DEBUG
  DebugPrintf(("DosComLoader. Loading '%s' at %04x\n", GET_PTR(namep), mem));
#endif
  /* Now load the executable                              */
  {
    BYTE FAR *sp;

    if (mode == OVERLAY)  /* memory already allocated */
      sp = MK_FP(mem, 0);
    else                  /* test the filesize against the allocated memory */
      sp = MK_FP(mem, sizeof(psp));

    /* MS DOS always only loads the very first 64KB - sizeof(psp) bytes.
       -- 1999/04/21 ska */

    /* rewind to start */
    SftSeek(fd, 0, 0);
    /* read everything, but at most 64K - sizeof(PSP)             */
    /* lpproj: some device drivers (not exe) are larger than 0xff00bytes... */
    DosReadSft(fd, (mode == OVERLAY) ? 0xfffeU : 0xff00U, sp);
    DosCloseSft(fd, FALSE);
  }

  if (mode == OVERLAY)
    return SUCCESS;

  {
    UWORD fcbcode;
    psp FAR *p;

    /* point to the PSP so we can build it                  */
    setvec(0x22, (intvec)MK_FP(user_r->CS, user_r->IP));
    child_psp(mem, cu_psp, mem + asize);

    fcbcode = patchPSP(mem - 1, env, exp, namep);
    /* set asize to end of segment */
    if (asize > 0x1000)
      asize = 0x1000;
    if (asize < 0x11)
      return DE_NOMEM;
    asize -= 0x11;
    /* CP/M compatibility--size of first segment for .COM files
       while preserving the far call to 0:00c0 +
       copy in HMA at ffff:00d0 */
    p = MK_FP(mem, 0);
    p->ps_reentry = MK_FP(0x1000c - asize, asize << 4);
    asize <<= 4;
    asize += 0x10e;
    exp->exec.stack = MK_FP(mem, asize);
    exp->exec.start_addr = MK_FP(mem, 0x100);
    *((UWORD FAR *) MK_FP(mem, asize)) = (UWORD) 0;
    load_transfer(mem, exp, fcbcode, mode);
  }
  return SUCCESS;
}

static VOID do_ret_user(iregs FAR *irp, intvec vec22)
{
  irp->CS = FP_SEG(vec22);
  irp->IP = FP_OFF(vec22);
  irp->FLAGS = 0x200; /* clear trace and carry flags, set interrupt flag */

  if (InDOS)
    --InDOS;
  ret_user(irp);
}

VOID return_user(void)
{
  psp FAR *p;
  psp FAR *q;
  REG COUNT i;
  seg parent;
  intvec vec22;

  p = MK_FP(cu_psp, 0);

  if (p->ps_exit != PSP_SIG)
  {
#ifdef DEBUG
    DebugPrintf(("return_user: terminating bad PSP, sig %x\n", p->ps_exit));
#endif
  }

  parent = p->ps_parent;
  vec22 = p->ps_isv22;
  /* When process returns - restore the isv               */
  setvec(0x22, p->ps_isv22);
  setvec(0x23, p->ps_isv23);
  setvec(0x24, p->ps_isv24);

  /* And free all process memory if not a TSR return      */
  network_redirector(REM_PROCESS_END);
  /* might be a good idea to do that after closing
     but doesn't help NET either TE */

  if (!tsr && parent != cu_psp)
  {
    network_redirector(REM_CLOSEALL);
    for (i = 0; i < p->ps_maxfiles; i++)
    {
      DosClose(i);
    }
    FcbCloseAll();
    FreeProcessMem(cu_psp);
  }

  cu_psp = parent;
  q = MK_FP(cu_psp, 0);
  do_ret_user(q->ps_stack, vec22);
}

STATIC COUNT DosExeLoader(const char FAR * namep, exec_blk FAR * exp, COUNT mode, COUNT fd)
{
  seg mem, env, start_seg;
  UWORD asize = 0;
  UWORD exe_size;
  {
    UWORD image_size;

    if (ExeHeader.exInitSP > 0 && ExeHeader.exInitSP < sizeof(exec_regs))
    {
      _printf("Too small stack, %i bytes\n", ExeHeader.exInitSP);
      return DE_INVLDDATA;
    }

    /* compute image size by removing the offset from the   */
    /* number pages scaled to bytes plus the remainder and  */
    /* the psp                                              */
#if 0
    if (ExeHeader.exPages >= 2048)
      return DE_INVLDDATA; /* we're not able to get >=1MB in dos memory */
#else
    /* TurboC++ 3 BOSS NE stub: image > 1 MB but load only "X mod 1 MB" */
    /* ExeHeader.exPages &= 0x7ff; */ /* just let << 5 do the clipping! */
#endif
    /*  First scale the size and remove the offset          */
    image_size = (ExeHeader.exPages << 5) - ExeHeader.exHeaderSize;

    /* We should not attempt to allocate
       memory if we are overlaying the current process, because the new
       process will simply re-use the block we already have allocated.
       Jun 11, 2000 - rbc */

    if ((mode & 0x7f) != OVERLAY)
    {
      UBYTE orig_mem_access = mem_access_mode;
      COUNT rc;

      /* and finally add in the psp size                      */
      image_size += sizeof(psp) / 16;        /* TE 03/20/01 */
      exe_size = image_size + ExeHeader.exMinAlloc;

      /* Clone the environement and create a memory arena     */
      if (mode & 0x80)
      {
        DosUmbLink(1);          /* link in UMBs */
        mem_access_mode |= 0x80;
      }

      rc = ChildEnv(exp, &env, namep);

      if (rc == SUCCESS)
        /* Now find out how many paragraphs are available       */
        rc = ExecMemLargest(&asize, exe_size);

      exe_size = image_size + ExeHeader.exMaxAlloc;
      /* second test is for overflow (avoiding longs) --
         exMaxAlloc can be high */
      if (exe_size > asize || exe_size < image_size)
        exe_size = asize;

      /* TE if ExeHeader.exMinAlloc == ExeHeader.exMaxAlloc == 0,
         DOS will allocate the largest possible memory area
         and load the image as high as possible into it.
         discovered (and after that found in RBIL), when testing NET */

      if ((ExeHeader.exMinAlloc | ExeHeader.exMaxAlloc) == 0)
        exe_size = asize;

      /* Allocate our memory and pass back any errors         */
      if (rc == SUCCESS)
        rc = ExecMemAlloc(exe_size, &mem, &asize);

      if (rc != SUCCESS)
        DosMemFree(env);

      /* comcom32 doesn't set mode, so reset also if mem_access_mode has UMA */
      if ((mode | orig_mem_access) & 0x80)
      {
        mem_access_mode = orig_mem_access & 0x7f; /* restore old situation */
        DosUmbLink(0);     /* restore link state */
        mode &= 0x7f; /* forget about high loading from now on */
      }
      if (rc != SUCCESS)
        return rc;

#ifdef DEBUG
      DebugPrintf(("DosExeLoader. Loading '%s' at %04x\n", GET_PTR(namep), mem));
#endif

      /* memory found large enough - continue processing      */
      ++mem;

/* /// Added open curly brace and "else" clause.  We should not attempt
   to allocate memory if we are overlaying the current process, because
   the new process will simply re-use the block we already have allocated.
   This was causing execl() to fail in applications which use it to
   overlay (replace) the current exe file with a new one.
   Jun 11, 2000 - rbc */
    }
    else /* !!OVERLAY */
    {
      mem = exp->load.load_seg;
    }

    /* Now load the executable                              */
    /* offset to start of image                             */
    if (SftSeek(fd, ExeHeader.exHeaderSize * 16UL, 0) != SUCCESS)
    {
      if (mode != OVERLAY)
      {
        DosMemFree(--mem);
        DosMemFree(env);
      }
      return DE_INVLDDATA;
    }

    /* create the start seg for later computations          */
    start_seg = mem;
    exe_size = image_size;
    if (mode != OVERLAY)
    {
      exe_size -= sizeof(psp) / 16;
      start_seg += sizeof(psp) / 16;
      if (exe_size > 0 && (ExeHeader.exMinAlloc | ExeHeader.exMaxAlloc) == 0)
      {
        mcb FAR *mp = MK_FP(mem - 1, 0);

        /* then the image should be placed as high as possible */
        start_seg += mp->m_size - image_size;
      }
    }
  }

  /* read in the image in 32256 chunks                      */
  {
    int nBytesRead, toRead = CHUNK;
    seg sp = start_seg;

    while (1)
    {
      if (exe_size < CHUNK/16)
        toRead = exe_size*16;
      nBytesRead = (WORD)DosReadSft(fd, toRead, MK_FP(sp, 0));
      if (nBytesRead < toRead || exe_size <= CHUNK/16)
        break;
      sp += CHUNK/16;
      exe_size -= CHUNK/16;
    }
  }

  {                             /* relocate the image for new segment                   */
    COUNT i;
    UWORD reloc[2];
    UWORD FAR *spot;

    SftSeek(fd, ExeHeader.exRelocTable, 0);
    for (i = 0; i < ExeHeader.exRelocItems; i++)
    {
      if (DosReadSft(fd, sizeof(reloc), MK_FAR_SCP(reloc)) != sizeof(reloc))
      {
        if (mode != OVERLAY)
        {
          DosMemFree(--mem);
          DosMemFree(env);
        }
        return DE_INVLDDATA;
      }
      if (mode == OVERLAY)
      {
        spot = MK_FP((reloc[1] + mem) & 0xffff, reloc[0]);
        *spot += exp->load.reloc;
      }
      else
      {
        /*      spot = MK_FP(reloc[1] + mem + 0x10, reloc[0]); */
        spot = MK_FP((reloc[1] + start_seg) & 0xffff, reloc[0]);
        *spot += start_seg;
      }
    }
  }

  /* and finally close the file                           */
  DosCloseSft(fd, FALSE);

  /* exit here for overlay                                */
  if (mode == OVERLAY)
    return SUCCESS;

  {
    UWORD fcbcode;

    /* point to the PSP so we can build it                  */
    setvec(0x22, (intvec)MK_FP(user_r->CS, user_r->IP));
    child_psp(mem, cu_psp, mem + asize);

    fcbcode = patchPSP(mem - 1, env, exp, namep);
    exp->exec.stack =
      MK_FP((ExeHeader.exInitSS + start_seg) & 0xffff, ExeHeader.exInitSP);
    exp->exec.start_addr =
      MK_FP((ExeHeader.exInitCS + start_seg) & 0xffff, ExeHeader.exInitIP);

    /* Transfer control to the executable                   */
    load_transfer(mem, exp, fcbcode, mode);
  }
  return SUCCESS;
}

/* mode = LOAD or EXECUTE
   ep = EXE block
   lp = filename to load (string)

   leb = local copy of exe block
 */
COUNT DosExec(COUNT mode, exec_blk FAR * ep, const char FAR * lp)
{
  COUNT rc;
  COUNT fd;

  if ((mode & 0x7f) > 3 || (mode & 0x7f) == 2)
    return DE_INVLDFMT;

  if (TempExeBlock_p != ep)
    fmemcpy_n(TempExeBlock_p, ep, sizeof(exec_blk) - 8);
  /* If file not found - free ram and return error        */

  if (IsDevice(lp) ||        /* we don't want to execute C:>NUL */
      (fd = (short)DosOpenSft(lp, O_LEGACY | O_OPEN | O_RDONLY, 0)) < 0)
  {
    return DE_FILENOTFND;
  }

  exe_header FAR *eh = ExeHeader_p;
  rc = (WORD)DosReadSft(fd, sizeof(exe_header), eh);

  if (rc == sizeof(exe_header) &&
      (ExeHeader.exSignature == MAGIC || ExeHeader.exSignature == OLD_MAGIC))
  {
    rc = DosExeLoader(lp, TempExeBlock_p, mode, fd);
  }
  else if (rc != 0)
  {
    rc = DosComLoader(lp, TempExeBlock_p, mode, fd);
  }

  DosCloseSft(fd, FALSE);


  if (mode == LOAD && rc == SUCCESS && ep != TempExeBlock_p)
    fmemcpy(ep, TempExeBlock_p, sizeof(exec_blk));

  return rc;
}

STATIC CommandTail FAR *ParseCmdLine(BYTE FAR *endp)
{
  CommandTail FAR *tail;
  BYTE FAR *tailp;
  BYTE *p;
  /* if there are no parameters, point to end without "\r\n" */
  if((tailp = fstrchr(Shell,'\t')) == NULL &&
       (tailp = fstrchr(Shell, ' ')) == NULL)
    tailp = endp - 2;
  /* shift tail to right by 2 to make room for '\0', ctCount */
  for (p = endp - 1; p >= tailp; p--)
    *(p + 2) = *p;
  /* terminate name and tail */
  *tailp =  *(endp + 2) = '\0';
  /* ctCount: just past '\0' do not count the "\r\n" */
  tail = tailp + 1;
  tail->ctCount = endp - tailp - 2;
  return tail;
}

/* start process 0 (the shell) */
VOID ASMCFUNC P_0(const struct config FAR *Config)
{
  BYTE FAR *endp;
  exec_blk FAR *exb = TempExeBlock_p;
  BYTE off = 0;

  /* build exec block and save all parameters here as init part will vanish! */
  exb->exec.fcb_1 = exb->exec.fcb_2 = (fcb FAR *)-1L;
  exb->exec.env_seg = 0;
  if ((bprm.ShellDrive & 0x80) && Config->cfgInit[1] != ':')
  {
    memcpy(Shell, "C:\\", 3);
    Shell[0] += bprm.ShellDrive & ~0x80;
    off = 3;
  }
  fstrcpy(Shell + off, _MK_DOS_FP(char, FP_SEG(Config), (uint16_t)Config->cfgInit));
  /* join name and tail */
  fstrcpy(Shell + strlen(Shell), _MK_DOS_FP(char, FP_SEG(Config), (uint16_t)Config->cfgInitTail));
  endp =  Shell + strlen(Shell);
  exb->exec.cmd_line = ParseCmdLine(endp);
  _printf("Process 0 starting: %s%s", GET_PTR(Shell),
      exb->exec.cmd_line->ctBuffer);

  p0_exec_mode = Config->cfgP_0_startmode;
  p0_cmdline_p = Shell;
  p0_execblk_p = exb;
}

VOID ASMCFUNC P_0_exit(void)
{
  _printf("\nShell %s exited, press any key...\n", GET_PTR(Shell));
  con_flush_stdin();
  read_char_stdin(1);
  fdexit(0);
}

VOID ASMCFUNC P_0_bad(void)
{
  BYTE FAR *endp;
  COUNT rd;
  exec_blk FAR *exb = TempExeBlock_p;

  if (termNoComcom) {
    fdloudprintf("Bad or missing Command Interpreter: %s\n", GET_PTR(Shell));
    fdexit(1);
  }
  put_string("Bad or missing Command Interpreter: "); /* failure _or_ exit */
  put_string(Shell);
  put_string(exb->exec.cmd_line->ctBuffer);
  put_string(" Enter the full shell command line: ");
  rd = res_read(STDIN, Shell, NAMEMAX);
  if (rd <= 0 || Shell[0] == '\xd')
    fdexit(1);
  /* handle EOT, EOF */
  if (Shell[0] == '\x4' || Shell[0] == '\x1a')
    panic("Unable to start shell");
  endp = Shell + rd;
  *endp = '\0';                             /* terminate string for strchr */
  exb->exec.cmd_line = ParseCmdLine(endp);
}

COUNT res_DosExec(COUNT mode, exec_blk FAR * ep, __XFAR(const char) filename)
{
    iregs regs = {};
    regs.ds = FP_SEG(filename);
    regs.d.x = FP_OFF(filename);
    regs.es = FP_SEG(ep);
    regs.b.x = FP_OFF(ep);
    regs.a.b.l = mode;
    regs.a.b.h = 0x4b;
    call_intr(0x21, MK_FAR_SCP(regs));
    if (regs.flags & FLG_CARRY)
        return regs.a.x;
    return 0;
}

STATIC DWORD res_read(UWORD fd, void FAR *buf, UCOUNT count)
{
    iregs regs = {};
    regs.c.x = count;
    regs.ds = FP_SEG(buf);
    regs.d.x = FP_OFF(buf);
    regs.b.x = fd;
    regs.a.b.h = 0x3f;
    call_intr(0x21, MK_FAR_SCP(regs));
    if (regs.flags & FLG_CARRY)
        return -1;
    return regs.a.x;
}
