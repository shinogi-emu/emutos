/*
 *  bios.c - C portion of BIOS initialization and front end
 *
 * Copyright (C) 2001 Lineo, Inc.
 * Copyright (C) 2001-2026 The EmuTOS development team
 *
 * Authors:
 *  SCC     Steve C. Cavender
 *  KTB     Karl T. Braun (kral)
 *  JSL     Jason S. Loveman
 *  EWF     Eric W. Fleischman
 *  LTG     Louis T. Garavaglia
 *  MAD     Martin Doering
 *  LVL     Laurent Vogel
 *
 * This file is distributed under the GPL, version 2 or at your
 * option any later version.  See doc/license.txt for details.
 */

/* #define ENABLE_KDEBUG */

#include "emutos.h"
#include "biosext.h"
#include "bios.h"
#include "../bdos/bdosstub.h"
#include "../bdos/hostfs.h"
#include "virtio_9p.h"
#include "hostfs_link.h"
#include "../vdi/vdistub.h"
#include "bdosbind.h"
#include "gemerror.h"
#include "lineavars.h"
#include "vt52.h"
#include "processor.h"
#include "initinfo.h"
#include "machine.h"
#ifdef MACHINE_QEMU_VIRT
#include "qemuvirt.h"
#endif
#include "has.h"
#include "cookie.h"
#include "country.h"
#include "nls.h"
#include "biosmem.h"
#include "ikbd.h"
#include "virtio_input.h"
#include "mouse.h"
#include "midi.h"
#include "mfp.h"
#include "floppy.h"
#include "sound.h"
#include "dmasound.h"
#include "dsp.h"
#include "screen.h"
#include "clock.h"
#include "vectors.h"
#include "asm.h"
#include "chardev.h"
#include "disk.h"
#include "blkdev.h"
#include "parport.h"
#include "serport.h"
#include "string.h"
#include "natfeat.h"
#include "delay.h"
#include "biosbind.h"
#include "memory.h"
#include "nova.h"
#include "tosvars.h"
#include "amiga.h"
#include "lisa.h"
#include "coldfire.h"
#if WITH_CLI
#include "../cli/clistub.h"
#endif



/*==== Defines ============================================================*/

#define DBGBIOS 0               /* If you want to enable debug wrappers */
#define ENABLE_RESET_RESIDENT 0 /* enable to run "reset-resident" code (see below) */

#define ENV_SIZE    12          /* sufficient for standard PATH=^X:\^^ (^=nul byte) */
#define DEF_PATH    "A:\\"      /* default value for path */

static char default_env[ENV_SIZE];  /* default environment area */

/*==== External declarations ==============================================*/

#if STONX_NATIVE_PRINT
void stonx_kprintf_init(void);      /* defined in kprintasm.S */
#endif

#if CONF_WITH_CARTRIDGE
void run_cartridge_applications(WORD typebit);  /* defined in startup.S */
#endif

#if CONF_WITH_68040_PMMU
long setup_68040_pmmu(void);        /* defined in 68040_pmmu.S */
#endif

extern UBYTE osxhbootdelay;         /* defined in OSXH header in startup.S */

/*==== Declarations =======================================================*/

/* used by kprintf() */
WORD boot_status;               /* see kprint.h for bit flags */

/* Boot flags */
UBYTE bootflags;

/* Non-Atari hardware vectors */
#if !CONF_WITH_MFP
void (*vector_5ms)(void);       /* 200 Hz system timer */
#endif

/*==== BOOT ===============================================================*/


/*
 * setup all vectors
 */

static void vecs_init(void)
{
#if !CONF_ATARI_HARDWARE
    /* On Atari hardware, the first 2 longs of the address space are physically
     * routed to the start of the ROM (instead of the start of the ST-RAM).
     * On other machines, there is actual RAM at that place.
     * To mimic Atari hardware behaviour, we copy the start of our OS there.
     * This may improve compatibility with some Atari software,
     * which may peek the OS version or reset address from there.
     * As C forbids from dereferencing the NULL pointer, we use assembly. */
    __asm__ volatile
    (
        "move.l  %0,a0\n\t"         /* a0 = ROM */
        "suba.l  a1,a1\n\t"         /* a1 = 0 */
        "move.l  (a0)+,(a1)+\n\t"   /* Reset: Initial SSP */
        "move.l  (a0),(a1)"         /* Reset: Initial PC */
    : /* outputs */
    : "g"(&os_header) /* inputs */
    : "a0", "a1", "cc" AND_MEMORY
    );
#endif

    /* Initialize the exception vectors.
     * By default, any unexpected exception calls dopanic().
     */
    init_exc_vec();
    init_user_vec(FIRST_BOOT);

    /* Some user drivers may install interrupt handlers and call the previous
     * ones. For example, ARAnyM's network driver for MiNT (nfeth.xif) and fVDI
     * driver (aranym.sys) install custom handlers on INT3, and call the
     * previous one. This panics with "Exception number 27" if VEC_LEVEL3 is
     * not initialized with a valid default handler.
     */
    VEC_LEVEL1 = just_rte;
    VEC_LEVEL2 = just_rte;
    VEC_LEVEL3 = just_rte;
    VEC_LEVEL4 = just_rte;
    VEC_LEVEL5 = just_rte;
    VEC_LEVEL6 = just_rte;
    VEC_LEVEL7 = just_rte;

#ifdef __mcoldfire__
    /* On ColdFire, when a zero divide exception occurs, the PC value in the
     * exception frame points to the offending instruction, not the next one.
     * If we put a simple rte in the exception handler, this will result in
     * an endless loop.
     * New ColdFire programs are supposed to be clean and avoid zero
     * divides. So we keep the default panic() behaviour in such case. */
#else
    /* Original TOS cowardly ignores integer divide by zero. */
    VEC_DIVNULL = just_rte;
#endif

    /* initialise some vectors we really need */
    VEC_GEM = vditrap;
    VEC_BIOS = biostrap;
    VEC_XBIOS = xbiostrap;
    VEC_LINEA = int_linea;

    /* Emulate some instructions unsupported by the processor. */
#ifdef __mcoldfire__
    /* On ColdFire, all the unsupported assembler instructions
     * will be emulated by a specific emulation layer loaded later. */
#else
    if (longframe) {
        /* On 68010+, "move from sr" called from user mode causes a
         * privilege violation. This instruction must be emulated for
         * compatibility with 68000 processors. */
        VEC_PRIVLGE = int_priv;
    } else {
        /* On 68000, "move from ccr" is unsupported and causes an illegal
         * instruction exception. This instruction must be emulated for
         * compatibility with higher processors. */
        VEC_ILLEGAL = int_illegal;
    }
#endif
#if CONF_WITH_ADVANCED_CPU
    /* On the 68060, instructions that were implemented in earlier
     * processors but not in the 68060 cause this trap to be taken,
     * for the purposes of emulation. The only instruction currently
     * emulated is movep; fortunately this is both the simplest and
     * commonest.
     */
    VEC_UNIMPINT = int_unimpint;
#endif
}

extern PFVOID vbl_list[8]; /* Default array for vblqueue */

#ifdef MACHINE_QEMU_VIRT
/*
 * Temporary scaffolding: exercise the host-folder drive from inside the
 * guest and report on the serial line, so the golden tests can verify it
 * headlessly with no display and no interaction. Removed once GEM
 * itself drives these paths.
 *
 * Buffers are static. selftest_big in particular is 16KB, which no
 * stack here would survive, and it has to be larger than one 9P
 * message for the read to be worth anything: p9_read() chunks by the
 * negotiated msize, so a request smaller than a single chunk never
 * takes the loop and a large file would still come back mangled.
 */
#define SELFTEST_BIG 16384

static char selftest_dta[44];
static UBYTE selftest_buf[64];
static UBYTE selftest_big[SELFTEST_BIG];

static void hostfs_pathtest(void);
static void hostfs_subdirtest(void);
static void hostfs_manytest(void);
static void hostfs_longnametest(void);
static void hostfs_dtatest(void);
static void hostfs_exectest(void);
static void hostfs_writetest(void);

static void hostfs_selftest(void)
{
    LONG fh, n, total;
    ULONG hash;
    WORD i;

    /*
     * Everything below names drive C, which is the host folder only
     * while the host link is up. With no helper and a vvfat block
     * drive attached, C: is that drive instead -- and parts of this
     * self-test WRITE, so it would scribble on the block drive. Ask
     * for the link by name rather than for a drive C.
     */
    if (!hostfs_link_present())
        return;

    /*
     * List the drive while opening a file inside the loop.
     *
     * The open/close pair in the middle of the scan is the whole point:
     * Fsfirst/Fsnext keep their state in single globals, so an Fopen
     * that resolved its path by re-snapshotting the directory would
     * reset the scan under itself and this loop would either repeat
     * entries or stop early. That it still reports exactly the entries,
     * in order, is what the listing goldens are checking here -- the
     * desktop's own idiom is precisely this interleaving.
     */
    Fsetdta(selftest_dta);
    for (n = Fsfirst("C:\\*.*", 0); n == 0; n = Fsnext())
    {
        fh = Fopen("C:\\HELLO.TXT", 0);
        if (fh > 0)
            Fclose((WORD)fh);
    }

    /* Open, read, seek, close. */
    fh = Fopen("C:\\HELLO.TXT", 0);
    if (fh > 0)
    {
        KINFO(("hostfs: open HELLO.TXT handle %ld\n", fh));

        n = Fread((WORD)fh, 64L, selftest_buf);
        if (n >= 0)
        {
            selftest_buf[n] = '\0';
            KINFO(("hostfs: read %ld bytes: %s\n", n, selftest_buf));
        }

        if (Fseek(2L, (WORD)fh, 0) == 2)
        {
            n = Fread((WORD)fh, 3L, selftest_buf);
            if (n >= 0)
            {
                selftest_buf[n] = '\0';
                KINFO(("hostfs: seek 2 read %ld: %s\n", n, selftest_buf));
            }
        }

        KINFO(("hostfs: close handle %ld\n", fh));
        Fclose((WORD)fh);
    }

    /*
     * Wrap the slot table. Slots rotate, so 23 open/close pairs walk
     * the eight of them nearly three times over; 23 is deliberately not
     * a multiple of the table size, so the loop finishes mid-table
     * rather than landing back on its starting slot by construction,
     * and the reported handle differs from the one the open above
     * returned. If any error path had failed to clunk, the slot whose
     * fid stayed bound would fail its next walk, the loop would break
     * early and no line would be reported at all.
     */
    fh = -1;
    for (i = 0; i < 23; i++)
    {
        fh = Fopen("C:\\HELLO.TXT", 0);
        if (fh <= 0)
            break;
        Fclose((WORD)fh);
    }
    if (fh > 0)
        KINFO(("hostfs: wrap %d opens, last handle %ld ok\n", i, fh));

    /*
     * A file larger than the negotiated msize, read in requests larger
     * than the msize, hashed rather than printed: the host computes the
     * same FNV-1a over the same file, so any byte that chunking dropped,
     * duplicated or reordered changes the result.
     */
    fh = Fopen("C:\\BIG.DAT", 0);
    if (fh > 0)
    {
        total = 0;
        hash = 2166136261UL;

        while ((n = Fread((WORD)fh, (LONG)SELFTEST_BIG, selftest_big)) > 0)
        {
            LONG j;

            /*
             * The size of the FIRST read is the part that proves the
             * chunking loop, not the hash: a p9_read() that issued one
             * Tread and stopped would satisfy this outer loop just as
             * well, one short buffer at a time, and produce the same
             * hash. A single Fread returning more than msize - 11 bytes
             * cannot have been one message.
             */
            if (total == 0)
                KINFO(("hostfs: big first read %ld bytes\n", n));

            for (j = 0; j < n; j++)
            {
                hash ^= selftest_big[j];
                hash *= 16777619UL;
            }
            total += n;
        }

        if (n == 0)
            KINFO(("hostfs: big %ld bytes fnv1a 0x%08lx\n", total, hash));

        Fclose((WORD)fh);
    }

    hostfs_pathtest();
}

/*
 * Everything below runs AFTER the tests above, deliberately: opening a
 * file rotates the slot table, so an extra open placed earlier would
 * renumber the handles the read golden pins down.
 */
static const char * const selftest_escapes[] = {
    "..",                       /* relative, from the root */
    "\\..",                     /* absolute */
    "\\SUB\\..\\..",            /* one level too many */
    "C:/../..",                 /* host separators, with a drive letter */
    "\\SUB/../..",              /* mixed separators */
    "\\..\\..\\ETC",            /* climbing, then naming something outside */
    "\\SUB\\..\\..\\SUB",       /* out and back: must not be reduced to \SUB */
    "\\NOSUCH",                 /* not an escape, just absent */
    "\\HELLO.TXT",              /* exists, but is not a directory */
};

static char selftest_path[128];
static LONG selftest_dfree[4];

static void hostfs_pathtest(void)
{
    LONG fh, n, saved_drive;
    WORD i;

    /*
     * Nothing to say when there is no host folder: every call below
     * would fall through to stock EmuTOS and report a missing drive C,
     * which is correct but is two dozen alarming lines on the serial
     * console of a machine that simply has no 9P device.
     */
    if (!(Drvmap() & (1L << HOSTFS_DRIVE)))
        return;

    /*
     * Open for read/write and only read. The desktop itself opens with
     * mode 0 everywhere (desk/deskdir.c:402, desk/desksupp.c:623,777,
     * aes/gemrslib.c:839), but mode 2 is a common habit elsewhere and
     * must not be the difference between a file opening and not.
     */
    fh = Fopen("C:\\HELLO.TXT", 2);
    if (fh > 0)
    {
        KINFO(("hostfs: rwopen handle %ld\n", fh));

        n = Fread((WORD)fh, 64L, selftest_buf);
        if (n >= 0)
        {
            selftest_buf[n] = '\0';
            KINFO(("hostfs: rwread %ld bytes: %s\n", n, selftest_buf));
        }

        /*
         * Write through the read/write handle -- and write back
         * exactly the bytes just read, to the offset they came from.
         *
         * This runs against the goldens' own fixture folder, so the
         * only write that belongs here is the one that cannot change a
         * byte of it: the file is identical afterwards and every
         * listing golden over it still holds. What is asserted is that
         * a mode-2 handle now takes a write and reports the count,
         * where it used to answer EACCDN.
         *
         * Whether the bytes REACHED the host is not a question the
         * guest can answer -- everything it could read back comes
         * through the same path that took the write. That claim is
         * made by tools/check-hostfs-write.py, which asks the folder.
         */
        if (n > 0 && Fseek(0L, (WORD)fh, 0) == 0)
            KINFO(("hostfs: rwwrite rc %ld\n",
                   Fwrite((WORD)fh, n, selftest_buf)));
        KINFO(("hostfs: rwclose rc %ld\n", Fclose((WORD)fh)));
    }
    else
    {
        KINFO(("hostfs: rwopen failed %ld\n", fh));
    }

    /* Two statements, not one KINFO: argument order is unspecified. */
    saved_drive = Dgetdrv();
    n = Dsetdrv(HOSTFS_DRIVE);
    KINFO(("hostfs: setdrv rc 0x%08lx curdrv %ld\n", n, Dgetdrv()));

    /*
     * The character devices, opened while C: is the CURRENT drive.
     *
     * "CON:" carries no drive letter, so a hook that fell back to the
     * current drive would claim it for the host folder -- and the hook
     * sits above the special-name table, so the console would never be
     * consulted and every one of these would answer EPTHNF. The handles
     * are the fixed ones from that table (0xffff, 0xfffe, 0xfffd), not
     * anything a filesystem allocates, which is why they are reported
     * rather than read: getting a handle at all is the whole assertion.
     */
    KINFO(("hostfs: specname CON: rc 0x%08lx\n", Fopen("CON:", 0)));
    KINFO(("hostfs: specname AUX: rc 0x%08lx\n", Fopen("AUX:", 0)));
    KINFO(("hostfs: specname PRN: rc 0x%08lx\n", Fopen("PRN:", 0)));

    Dgetpath(selftest_path, 0);
    KINFO(("hostfs: getpath \"%s\"\n", selftest_path));

    /* Down, deeper, back up with a trailing separator, then all the way. */
    KINFO(("hostfs: setpath \"SUB\" rc %ld\n", Dsetpath("SUB")));
    Dgetpath(selftest_path, 0);
    KINFO(("hostfs: getpath \"%s\"\n", selftest_path));

    KINFO(("hostfs: setpath \"\\SUB\\DEEP\" rc %ld\n", Dsetpath("C:\\SUB\\DEEP")));
    Dgetpath(selftest_path, 0);
    KINFO(("hostfs: getpath \"%s\"\n", selftest_path));

    KINFO(("hostfs: setpath \"..\\\" rc %ld\n", Dsetpath("..\\")));
    Dgetpath(selftest_path, 0);
    KINFO(("hostfs: getpath \"%s\"\n", selftest_path));

    KINFO(("hostfs: setpath \"\\SUB\\..\" rc %ld\n", Dsetpath("\\SUB\\..")));
    Dgetpath(selftest_path, 0);
    KINFO(("hostfs: getpath \"%s\"\n", selftest_path));

    /*
     * Every one of these must be refused. The last line re-reads the
     * path afterwards: a refusal that had already moved the current
     * directory would be a containment failure even though it reported
     * an error.
     */
    for (i = 0; i < (WORD)(sizeof(selftest_escapes)/sizeof(selftest_escapes[0])); i++)
        KINFO(("hostfs: escape \"%s\" rc %ld\n", selftest_escapes[i],
               Dsetpath(selftest_escapes[i])));

    Dgetpath(selftest_path, 0);
    KINFO(("hostfs: getpath \"%s\"\n", selftest_path));

    KINFO(("hostfs: fattrib HELLO.TXT rc 0x%08lx\n",
           Fattrib("C:\\HELLO.TXT", 0, 0)));
    KINFO(("hostfs: fattrib SUB rc %ld\n", Fattrib("C:\\SUB", 0, 0)));
    KINFO(("hostfs: fattrib set rc %ld\n", Fattrib("C:\\HELLO.TXT", 1, 0)));

    if (Dfree(selftest_dfree, HOSTFS_DRIVE + 1) == 0)
        KINFO(("hostfs: dfree free %ld total %ld secsiz %ld clsiz %ld\n",
               selftest_dfree[0], selftest_dfree[1],
               selftest_dfree[2], selftest_dfree[3]));

    hostfs_subdirtest();
    hostfs_manytest();
    hostfs_longnametest();
    hostfs_dtatest();
    hostfs_exectest();
    hostfs_writetest();

    /*
     * Put the boot drive back. The switch to C: above is a test, not a
     * policy: leaving it selected would silently change which drive the
     * AES and the desktop start on.
     */
    Dsetdrv((WORD)saved_drive);
}

/*
 * Subdirectories: the same calls again, one level down.
 *
 * This runs LAST, after everything above, for the same reason the path
 * test does: opening a file rotates the slot table, so extra opens
 * placed earlier would renumber the handles the read goldens pin down.
 * It also runs while C: is still the current drive, so the relative
 * cases below mean what they say.
 */
static void hostfs_subdirtest(void)
{
    DTA *dta = (DTA *)selftest_dta;
    LONG fh, n, rc;

    /*
     * Nothing to say on a host folder without the fixture subdirectory:
     * every line below would report a failure that is about the fixture
     * rather than about the code, on the console of a run that is
     * testing something else entirely.
     */
    if (Dsetpath("C:\\SUB") != 0)
        return;
    Dsetpath("C:\\");

    /*
     * List the subdirectory, opening a file inside it in the middle of
     * the scan. The interleaving is the point: the snapshot, the cursor
     * and the pattern are single globals, so an Fopen that reached them
     * would truncate the very listing that produced the name it is
     * being asked to open -- and Fsfirst; { Fopen; Fread; Fclose }
     * Fsnext is the desktop's own idiom, not a contrived case.
     *
     * "." and ".." must appear here and must NOT appear in the drive
     * root's listing: the root has no parent inside the drive to offer.
     */
    Fsetdta(selftest_dta);
    for (rc = Fsfirst("C:\\SUB\\*.*", 0); rc == 0; rc = Fsnext())
    {
        fh = Fopen("C:\\SUB\\INNER.TXT", 0);
        if (fh > 0)
            Fclose((WORD)fh);
        KINFO(("hostfs: sublist %s\n", dta->d_fname));
    }

    /* Open and read a file inside the subdirectory, by absolute path. */
    fh = Fopen("C:\\SUB\\INNER.TXT", 0);
    KINFO(("hostfs: subopen rc %ld\n", fh));
    if (fh > 0)
    {
        n = Fread((WORD)fh, 32L, selftest_buf);
        if (n > 0)
        {
            selftest_buf[n - 1] = '\0';     /* drop the trailing newline */
            KINFO(("hostfs: subread %ld bytes: %s\n", n, selftest_buf));
        }
        Fclose((WORD)fh);
    }

    KINFO(("hostfs: subattrib rc 0x%08lx\n",
           Fattrib("C:\\SUB\\INNER.TXT", 0, 0)));

    /* The other file, by a RELATIVE name, from inside the directory. */
    KINFO(("hostfs: subsetpath rc %ld\n", Dsetpath("C:\\SUB")));
    fh = Fopen("DEEP.TXT", 0);
    KINFO(("hostfs: subrelopen rc %ld\n", fh));
    if (fh > 0)
    {
        n = Fread((WORD)fh, 32L, selftest_buf);
        if (n > 0)
        {
            selftest_buf[n - 1] = '\0';
            KINFO(("hostfs: subrelread %ld bytes: %s\n", n, selftest_buf));
        }
        Fclose((WORD)fh);
    }

    /* And a relative listing, which must scan the subdirectory. */
    for (rc = Fsfirst("*.*", 0); rc == 0; rc = Fsnext())
        KINFO(("hostfs: subrellist %s\n", dta->d_fname));

    /*
     * Containment on the file calls, not just on Dsetpath. Each of
     * these names a file that really exists in the drive root, so a
     * ".." that was clamped away instead of refused would find it and
     * report success -- and the third would then list the whole root
     * under the guise of listing a subdirectory.
     */
    KINFO(("hostfs: subescape open rc %ld\n",
           Fopen("C:\\SUB\\..\\..\\HELLO.TXT", 0)));
    KINFO(("hostfs: subescape attrib rc %ld\n",
           Fattrib("..\\..\\HELLO.TXT", 0, 0)));
    KINFO(("hostfs: subescape list rc %ld\n",
           Fsfirst("C:\\SUB\\..\\..\\*.*", 0)));

    Dsetpath("C:\\");
}

/*
 * A directory far larger than one 9P Rreaddir message.
 *
 * A single Treaddir returns whatever fits in the reply the buffer was
 * sized for -- a couple of dozen entries here -- so a snapshot that
 * issued one request and stopped would list the first page and silently
 * lose the rest. Nothing about that is visible from a small fixture,
 * which is why this one is deliberately larger than any page.
 *
 * The count alone would not prove much either: it is asserted together
 * with the FIRST and LAST names in sort order, so an implementation
 * that read the right number of entries from the wrong pages, or
 * repeated a page, cannot match. The dot entries are skipped when
 * picking those two -- they sort ahead of everything and would make
 * "first" say nothing -- but they are still counted.
 */
static char selftest_first[16];
static char selftest_last[16];

static void hostfs_manytest(void)
{
    DTA *dta = (DTA *)selftest_dta;
    LONG rc;
    WORD count = 0;

    /* Nothing to say on a host folder without the fixture directory. */
    if (Dsetpath("C:\\MANY") != 0)
        return;
    Dsetpath("C:\\");

    selftest_first[0] = '\0';
    selftest_last[0] = '\0';

    Fsetdta(selftest_dta);
    for (rc = Fsfirst("C:\\MANY\\*.*", 0); rc == 0; rc = Fsnext())
    {
        count++;

        if (dta->d_fname[0] == '.')
            continue;

        if (selftest_first[0] == '\0')
            strcpy(selftest_first, dta->d_fname);
        strcpy(selftest_last, dta->d_fname);
    }

    KINFO(("hostfs: many count %d first %s last %s\n",
           count, selftest_first, selftest_last));
}

/*
 * A guest name longer than 8.3, which must open the file that the
 * listing shows clipped.
 *
 * TOS truncates a name to 8.3 on the way into GEMDOS, so an application
 * asking for "VERYLONGNAME.TXT" is asking for "VERYLONG.TXT" and the
 * reference implementation opens it. Clipping only the HOST side and
 * comparing it against the raw guest string answers EFILNF instead --
 * which looks like a missing file, on a name the drive has just listed.
 *
 * Both spellings are tried, because the fix must not cost the ordinary
 * case: the already-clipped name has to keep working too.
 *
 * No handle number is reported. This runs last precisely so it cannot
 * renumber the handles the other goldens pin down, and printing one
 * here would re-create that coupling in the other direction.
 */
static void hostfs_longnametest(void)
{
    LONG fh, n;
    WORD i;
    static const char * const spellings[] = {
        "C:\\VERYLONGNAME.TXT",     /* as the application would type it */
        "C:\\VERYLONG.TXT",         /* as the listing displays it */
    };

    for (i = 0; i < (WORD)(sizeof(spellings)/sizeof(spellings[0])); i++)
    {
        fh = Fopen(spellings[i], 0);
        if (fh <= 0)
        {
            KINFO(("hostfs: longname %s rc %ld\n", spellings[i], fh));
            continue;
        }

        n = Fread((WORD)fh, 32L, selftest_buf);
        if (n > 0)
        {
            selftest_buf[n - 1] = '\0';     /* drop the trailing newline */
            KINFO(("hostfs: longname %s ok %ld bytes: %s\n",
                   spellings[i], n, selftest_buf));
        }
        else
        {
            KINFO(("hostfs: longname %s read rc %ld\n", spellings[i], n));
        }

        Fclose((WORD)fh);
    }
}

/*
 * Where an Fsnext goes is decided by the DTA, not by a global.
 *
 * Both cases below produced wrong answers when the routing was "was the
 * last Fsfirst anywhere in the system ours", and the second produced
 * something worse than a wrong answer. A declined Fsnext lands in stock
 * xsnext(), which reads dt_offset_drive out of the DTA; a DTA this
 * module never stamped holds whatever the application left there, and
 * ixsnext() masks it to five bits, checks only the top of the range and
 * indexes drvtbl[] with no NULL test. There is no floppy on this
 * machine, so a zeroed DTA means drvtbl[0], which is NULL -- a read of
 * low memory followed by a real sector request, on a machine with no
 * MMU to object.
 *
 * A second DTA is what makes the interleaving case reachable at all:
 * with one DTA there is only one search and nothing to confuse.
 */
static char selftest_dta2[44];

static void hostfs_dtatest(void)
{
    DTA *dta = (DTA *)selftest_dta;
    LONG rc;

    /*
     * A failed Fsfirst must end the search, not leave the previous
     * one running. Without this the sequence below answers with
     * leftovers from the root listing -- entries of a directory the
     * caller did not ask about, reported as if the missing one had
     * them.
     */
    Fsetdta(selftest_dta);
    if (Fsfirst("C:\\*.*", 0) != 0)
        return;                         /* no drive: nothing to prove */

    rc = Fsfirst("C:\\NOSUCH\\*.*", 0);
    KINFO(("hostfs: dtaguard missing rc %ld\n", rc));
    KINFO(("hostfs: dtaguard stale rc %ld\n", Fsnext()));

    /*
     * An unrelated search opened on ANOTHER DTA, in the middle of ours.
     * The Fsnext that follows belongs to the first DTA and has to
     * continue the C: listing -- here, with the drive's second entry,
     * FIXED.TXT.
     */
    Fsetdta(selftest_dta);
    if (Fsfirst("C:\\*.*", 0) != 0)
        return;

    Fsetdta(selftest_dta2);
    KINFO(("hostfs: dtaguard other rc %ld\n", Fsfirst("A:\\*.*", 0)));

    Fsetdta(selftest_dta);
    rc = Fsnext();
    KINFO(("hostfs: dtaguard resume rc %ld name %s\n",
           rc, rc == 0 ? dta->d_fname : (char *)""));
}

/*
 * Running a program off the drive.
 *
 * GATED ON C:\EXEC.PRG BEING THERE, for the same reason the write test
 * is gated: the folder the other goldens run against has no such
 * program, and it must not grow one. tools/make-fixtures.py builds a
 * separate folder that has it -- and an AUTO directory too, which is
 * the second half of what this proves and which no shared folder could
 * carry, since EmuTOS runs whatever it finds there at boot.
 *
 * Pexec is the call that could not work at all until the loader learnt
 * about this drive: xexec() looks for the program with ixsfirst() and
 * opens it with xopen(), neither of which passes through the GEMDOS
 * trap where the host folder is hooked. What is checked here is not the
 * return code -- a Pexec that loaded nothing could still return zero --
 * but the line the PROGRAM ITSELF writes to the serial port before it
 * exits. The golden holds that line, so it can only appear if the
 * bytes on the host really were fetched, relocated and jumped to.
 *
 * The gate is Fattrib rather than Fsfirst because Fsfirst would consume
 * the DTA the tests above set up, and this runs among them.
 */
static void hostfs_exectest(void)
{
    if (Fattrib("C:\\EXEC.PRG", 0, 0) < 0)
        return;

    KINFO(("hostfs: exec rc %ld\n",
           Pexec(PE_LOADGO, "C:\\EXEC.PRG", "", NULL)));
}

/*
 * The WRITE half of the drive.
 *
 * GATED ON C:\WTEST BEING THERE, and that is the whole point of the
 * gate rather than a convenience. The listing goldens run against a
 * fixture folder whose every file and every listing they assert; a
 * self-test that created, renamed and deleted things in it would break
 * half of them and would leave the folder different after each run.
 * tools/check-hostfs-write.py builds a THROWAWAY folder that has a
 * WTEST directory in it, and only there does any of this happen. On the
 * goldens' folder, and on a person's own drive C, the first call below
 * fails and the test says nothing at all.
 *
 * Every line it prints is what the GUEST was told. None of it is
 * evidence that a byte reached the host: everything the guest could
 * read back arrives through the same transport that took the write, so
 * a transport that dropped, duplicated or truncated data could satisfy
 * every check here. That claim belongs to the host side and is made by
 * tools/check-hostfs-write.py, which hashes the folder itself. These
 * lines are the other half of the story -- they say which calls the
 * guest even made, so a missing file on the host can be told from a
 * call the guest never issued.
 *
 * The names and the bodies are declared once on the host, in
 * tools/make-fixtures.py, and repeated here in C. That the two agree is
 * exactly what the host-side script measures.
 */
#define WTEST_BIG_SIZE  40960L

static const char wtest_new_body[]   = "hostfs write test\n";
static const char wtest_over_body[]  = "overwritten\n";
static const char wtest_after_body[] = "written after the delete\n";
static const char wtest_ren_body[]   = "rename me\n";
static const char wtest_inner_body[] = "inside a folder the guest made\n";

/* Create, write one string, close. Reports each step separately so a
 * failure says which of the three it was. */
static void wtest_put(const char *path, const char *what, const char *body)
{
    LONG fh = Fcreate(path, 0);

    if (fh <= 0)
    {
        KINFO(("hostfs: wtest %s create rc %ld\n", what, fh));
        return;
    }

    KINFO(("hostfs: wtest %s write rc %ld\n", what,
           Fwrite((WORD)fh, (LONG)strlen(body), (char *)body)));
    KINFO(("hostfs: wtest %s close rc %ld\n", what, Fclose((WORD)fh)));
}

static void hostfs_writetest(void)
{
    UWORD wtest_td[2];      /* Fdatime's two words: time, then date */
    LONG fh, n, total, j;
    ULONG hash;

    /*
     * Is this a folder that has asked to be written to? Dsetpath is the
     * cheapest question that distinguishes a directory from a file and
     * from nothing at all, and the current directory is put straight
     * back so nothing below depends on where it left it.
     */
    if (Dsetpath("C:\\WTEST") != 0)
        return;
    Dsetpath("C:\\");

    KINFO(("hostfs: wtest begin\n"));

    /* Create, write, close -- the ordinary case. */
    wtest_put("C:\\WTEST\\NEW.TXT", "new", wtest_new_body);

    /*
     * Overwrite something that was already on the host, and make it
     * SHORTER. Fcreate truncates, so a host that opened the file
     * without O_TRUNC would leave the tail of the old contents behind
     * and the guest would never see it: the length it reads back is its
     * own idea of the file, not the folder's.
     */
    wtest_put("C:\\WTEST\\OVER.TXT", "over", wtest_over_body);

    /*
     * A file far larger than one transport frame (4080 bytes of
     * payload), written in buffers larger than one frame as well. Both
     * loops have to be right: the one in this function, which moves the
     * file position between calls, and the one in hostfs_link_write(),
     * which cuts a single Fwrite into frames. A write that issued one
     * frame per Fwrite and reported the whole count would produce a
     * file 4080 bytes long and tell the guest it wrote 40960.
     */
    fh = Fcreate("C:\\WTEST\\BIGW.DAT", 0);
    if (fh <= 0)
    {
        KINFO(("hostfs: wtest big create rc %ld\n", fh));
    }
    else
    {
        total = 0;
        while (total < WTEST_BIG_SIZE)
        {
            LONG chunk = WTEST_BIG_SIZE - total;

            if (chunk > (LONG)SELFTEST_BIG)
                chunk = (LONG)SELFTEST_BIG;

            for (j = 0; j < chunk; j++)
                selftest_big[j] = (UBYTE)(((total + j) * 13 + 5) & 0xff);

            n = Fwrite((WORD)fh, chunk, selftest_big);
            if (n != chunk)
            {
                KINFO(("hostfs: wtest big short %ld of %ld\n", n, chunk));
                break;
            }
            total += n;
        }
        KINFO(("hostfs: wtest big write %ld bytes\n", total));
        KINFO(("hostfs: wtest big close rc %ld\n", Fclose((WORD)fh)));
    }

    /*
     * Read it back and hash it. This proves the guest's own view is
     * self-consistent and nothing more -- see the note at the top --
     * but a hash that differs from the host's says something is wrong
     * without needing the folder at all.
     */
    fh = Fopen("C:\\WTEST\\BIGW.DAT", 0);
    if (fh > 0)
    {
        total = 0;
        hash = 2166136261UL;

        while ((n = Fread((WORD)fh, (LONG)SELFTEST_BIG, selftest_big)) > 0)
        {
            for (j = 0; j < n; j++)
            {
                hash ^= selftest_big[j];
                hash *= 16777619UL;
            }
            total += n;
        }

        if (n == 0)
            KINFO(("hostfs: wtest big reread %ld bytes fnv1a 0x%08lx\n",
                   total, hash));

        Fclose((WORD)fh);
    }

    /*
     * Delete, and then write a DIFFERENT file.
     *
     * This exact sequence is what corrupted the folder under QEMU's
     * vvfat driver: the delete was accepted, the following file's data
     * arrived, and its LENGTH came out as the deleted file's, with the
     * guest told the write had succeeded. It is repeated here as the
     * one ordering most likely to expose the same class of bug, and
     * AFTER.TXT is deliberately a different length from DEL.TXT.
     */
    KINFO(("hostfs: wtest delete rc %ld\n", Fdelete("C:\\WTEST\\DEL.TXT")));
    wtest_put("C:\\WTEST\\AFTER.TXT", "afterdel", wtest_after_body);

    /* Rename. The source is created here so the test is repeatable. */
    wtest_put("C:\\WTEST\\REN1.TXT", "ren", wtest_ren_body);
    KINFO(("hostfs: wtest rename rc %ld\n",
           Frename("C:\\WTEST\\REN1.TXT", "C:\\WTEST\\REN2.TXT")));

    /* A folder the guest makes, and a file written INSIDE it. */
    KINFO(("hostfs: wtest mkdir rc %ld\n", Dcreate("C:\\WTEST\\NEWDIR")));
    wtest_put("C:\\WTEST\\NEWDIR\\INSIDE.TXT", "inner", wtest_inner_body);

    /* And a folder the guest makes and removes again. */
    KINFO(("hostfs: wtest mktmp rc %ld\n", Dcreate("C:\\WTEST\\TMPDIR")));
    KINFO(("hostfs: wtest rmdir rc %ld\n", Ddelete("C:\\WTEST\\TMPDIR")));

    /*
     * A handle opened for READING refuses a write, and KEEP.TXT is
     * still exactly what it was afterwards -- which the host-side
     * script checks, since this is the assertion the guest cannot make
     * for itself.
     */
    fh = Fopen("C:\\WTEST\\KEEP.TXT", 0);
    if (fh > 0)
    {
        KINFO(("hostfs: wtest ro write rc %ld\n",
               Fwrite((WORD)fh, 4L, (char *)"junk")));

        /*
         * Fdatime, both halves, on a handle that is definitely ours.
         * The query answers from the host's own timestamp; the set half
         * is refused, because no request on this wire carries a time --
         * see hostfs_datime() in bdos/hostfs.c. Refusing it is the
         * honest answer and is asserted here so it cannot quietly
         * become a silent success.
         */
        KINFO(("hostfs: wtest datime rc %ld\n",
               Fdatime(wtest_td, (WORD)fh, 0)));
        KINFO(("hostfs: wtest datime set rc %ld\n",
               Fdatime(wtest_td, (WORD)fh, 1)));

        Fclose((WORD)fh);
    }

    /* Fattrib's set half: mark it read-only, read that back, clear it. */
    KINFO(("hostfs: wtest attr set rc %ld\n",
           Fattrib("C:\\WTEST\\NEW.TXT", 1, 0x01)));
    KINFO(("hostfs: wtest attr get rc %ld\n",
           Fattrib("C:\\WTEST\\NEW.TXT", 0, 0)));
    KINFO(("hostfs: wtest attr clear rc %ld\n",
           Fattrib("C:\\WTEST\\NEW.TXT", 1, 0x00)));

    /*
     * Containment, asked of the calls that CREATE.
     *
     * The read side has its own escape list in hostfs_pathtest(); this
     * is the same question put to the three calls that would leave
     * something behind if the answer were ever wrong. Each names a
     * place outside the shared folder and each must be refused before
     * any request is built. A refusal reported HERE would prove little
     * on its own, so tools/check-hostfs-write.py looks at the folder's
     * parent directory afterwards as well.
     */
    KINFO(("hostfs: wtest escape create rc %ld\n",
           Fcreate("C:\\WTEST\\..\\..\\ESCAPE.TXT", 0)));
    KINFO(("hostfs: wtest escape mkdir rc %ld\n",
           Dcreate("C:\\..\\ESCAPEDIR")));
    KINFO(("hostfs: wtest escape delete rc %ld\n",
           Fdelete("C:\\..\\..\\ESCAPE.TXT")));

    KINFO(("hostfs: wtest end\n"));
}

#if CONF_WITH_VIRTIO_BLK
/*
 * Exercise the virtio-blk drive, on the same terms as the host folder
 * above: everything here goes through plain GEMDOS calls, so what it
 * proves is the whole stack -- the block driver, the MBR scan, the BPB
 * and FAT16 -- and not just that a sector came back.
 *
 * The drive LETTER is looked up rather than assumed. Which letter the
 * partition lands on depends on what else registered first (the host
 * folder takes C while both exist), and hard-coding it would make this
 * report nothing the day that changes.
 */
#if CONF_WITH_VIRTIO_BLK_WRITE
static void vblk_writetest(WORD drive);
static void vblk_dirtest(WORD drive);
#endif

static WORD vblk_drive(void)
{
    WORD i;

    for (i = 0; i < BLKDEVNUM; i++)
        if ((blkdev[i].flags & DEVICE_VALID)
            && IS_VIRTIO_DEVICE(blkdev[i].unit - NUMFLOPPIES))
            return i;

    return -1;
}

static void vblk_selftest(void)
{
    char path[16];
    WORD drive = vblk_drive();
    LONG fh, n, total;
    ULONG hash;

    if (drive < 0)
        return;                 /* no virtio-blk drive: nothing to prove */

    KINFO(("vblk: drive %c unit %d\n", 'A' + drive, blkdev[drive].unit));

    strcpy(path, "@:\\HELLO.TXT");
    path[0] = 'A' + drive;

    fh = Fopen(path, 0);
    if (fh > 0)
    {
        n = Fread((WORD)fh, 63L, selftest_buf);
        if (n >= 0)
        {
            selftest_buf[n] = '\0';
            KINFO(("vblk: read %ld bytes: %s\n", n, selftest_buf));
        }
        Fclose((WORD)fh);
    }
    else
        KINFO(("vblk: open %s rc %ld\n", path, fh));

    /*
     * A file spanning many clusters, hashed rather than printed. One
     * sector arriving correctly proves very little: the interesting
     * failures are a request whose byte count wrapped, a chain whose
     * status byte landed in the payload, and a FAT walk that repeated
     * or skipped a cluster. All three survive a single-sector read and
     * none of them survives this.
     */
    strcpy(path, "@:\\BIG.DAT");
    path[0] = 'A' + drive;

    fh = Fopen(path, 0);
    if (fh > 0)
    {
        total = 0;
        hash = 2166136261UL;

        while ((n = Fread((WORD)fh, (LONG)SELFTEST_BIG, selftest_big)) > 0)
        {
            LONG j;

            for (j = 0; j < n; j++)
            {
                hash ^= selftest_big[j];
                hash *= 16777619UL;
            }
            total += n;
        }

        if (n == 0)
            KINFO(("vblk: big %ld bytes fnv1a 0x%08lx\n", total, hash));

        Fclose((WORD)fh);
    }
    else
        KINFO(("vblk: open %s rc %ld\n", path, fh));

#if CONF_WITH_VIRTIO_BLK_WRITE
    vblk_writetest(drive);
#else
    /*
     * Writes are refused unless the build asks for them, so this
     * asserts that they fail as write-protect rather than being
     * attempted and half-succeeding. See include/config.h for why the
     * default is off -- it is about QEMU's vvfat, not about the driver.
     */
    strcpy(path, "@:\\NEW.TXT");
    path[0] = 'A' + drive;
    KINFO(("vblk: create rc %ld\n", Fcreate(path, 0)));
#endif
}

#if CONF_WITH_VIRTIO_BLK_WRITE

/* Contents of NEW.TXT. No newline: it is printed on a golden line. */
#define VBLK_WRITE_TEXT "vvfat write test"

/*
 * BIGWRITE.DAT is deliberately larger than one cluster. vvfat lays the
 * drive out with 16 sectors to a cluster, so 8192 bytes is exactly one,
 * and a file that fits in one proves nothing about allocating a chain:
 * 12288 forces a second cluster, a FAT entry linking the two, and a
 * directory entry whose size is not a single cluster's worth.
 */
#define VBLK_WRITE_BIG  12288

static UBYTE vblk_bigbyte(WORD i)
{
    /*
     * i is a 16-bit int here, so the arithmetic is done in ULONG. The
     * pattern differs from BIG.DAT's on purpose -- a read-back that
     * silently returned the other file would otherwise hash correctly.
     */
    return (UBYTE)(((ULONG)(UWORD)i * 11UL + 7UL) & 0xffUL);
}

/*
 * Write to the drive, and read back what was written.
 *
 * This can only ever report what the GUEST saw. That the file appears
 * on the host with the right contents, and that nothing already in the
 * host folder was damaged, is checked from the host side by
 * tools/check-vvfat-write.py in the shinogi repository: vvfat's
 * documented failure mode is mangling the directory it maps, which no
 * amount of guest-side bookkeeping would notice.
 */
static void vblk_writetest(WORD drive)
{
    char path[16];
    LONG fh, n, total;
    ULONG hash;
    WORD i;

    strcpy(path, "@:\\NEW.TXT");
    path[0] = 'A' + drive;

    fh = Fcreate(path, 0);
    if (fh < 0)
    {
        KINFO(("vblk: create rc %ld\n", fh));
        return;
    }

    n = Fwrite((WORD)fh, (LONG)strlen(VBLK_WRITE_TEXT), VBLK_WRITE_TEXT);
    KINFO(("vblk: write %ld bytes\n", n));
    KINFO(("vblk: wclose rc %ld\n", Fclose((WORD)fh)));

    /* Round trip: what the guest wrote, read back by the guest. */
    fh = Fopen(path, 0);
    if (fh > 0)
    {
        n = Fread((WORD)fh, 63L, selftest_buf);
        if (n >= 0)
        {
            selftest_buf[n] = '\0';
            KINFO(("vblk: reread %ld bytes: %s\n", n, selftest_buf));
        }
        Fclose((WORD)fh);
    }
    else
        KINFO(("vblk: reopen rc %ld\n", fh));

    /*
     * The directory operations run HERE, between the small write and
     * the large one, rather than after both. They delete a file, and
     * the large write that follows then reallocates the clusters that
     * delete freed -- which is the case a mapping driver gets wrong:
     * writing the new file's data through to the host file the freed
     * clusters used to belong to. Doing the delete last would never
     * reach for those clusters again and the whole question would go
     * untested.
     */
    vblk_dirtest(drive);

    strcpy(path, "@:\\BIGWRITE.DAT");
    path[0] = 'A' + drive;

    for (i = 0; i < VBLK_WRITE_BIG; i++)
        selftest_big[i] = vblk_bigbyte(i);

    fh = Fcreate(path, 0);
    if (fh < 0)
    {
        KINFO(("vblk: bigcreate rc %ld\n", fh));
        return;
    }

    KINFO(("vblk: bigwrite %ld bytes\n",
           Fwrite((WORD)fh, (LONG)VBLK_WRITE_BIG, selftest_big)));
    KINFO(("vblk: bigwclose rc %ld\n", Fclose((WORD)fh)));

    /*
     * Read it back into the same buffer, which still holds the pattern
     * that was just written -- so it is cleared first. Otherwise a read
     * that returned nothing at all would still hash as if it had
     * worked.
     */
    bzero(selftest_big, VBLK_WRITE_BIG);

    fh = Fopen(path, 0);
    if (fh > 0)
    {
        total = 0;
        hash = 2166136261UL;

        while ((n = Fread((WORD)fh, (LONG)SELFTEST_BIG, selftest_big)) > 0)
        {
            LONG j;

            for (j = 0; j < n; j++)
            {
                hash ^= selftest_big[j];
                hash *= 16777619UL;
            }
            total += n;
        }

        if (n == 0)
            KINFO(("vblk: bigreread %ld bytes fnv1a 0x%08lx\n", total, hash));

        Fclose((WORD)fh);
    }
    else
        KINFO(("vblk: bigreopen rc %ld\n", fh));
}

/*
 * Delete, rename and subdirectory creation.
 *
 * Creating a file is the easy half. What actually reaches into the
 * directory the host driver is mapping is removing an entry, renaming
 * one, and adding a directory -- and the desktop does all three the
 * moment anyone drags an icon. Testing only Fcreate would leave the
 * riskiest operations unexercised while reporting that "writing works".
 */
static void vblk_dirtest(WORD drive)
{
    char path[24];
    char other[24];
    LONG fh;
    WORD i;

    /* Create a file, then delete it, then confirm it is really gone. */
    strcpy(path, "@:\\DEL.TXT");
    path[0] = 'A' + drive;

    fh = Fcreate(path, 0);
    if (fh < 0)
    {
        KINFO(("vblk: delcreate rc %ld\n", fh));
        return;
    }
    Fwrite((WORD)fh, 8L, "deleteme");
    Fclose((WORD)fh);

    KINFO(("vblk: delete rc %ld\n", Fdelete(path)));

    /*
     * Asked for by name through a fresh search, not by reopening the
     * handle: a directory entry that was unlinked from the FAT but left
     * in place still answers an Fopen.
     */
    Fsetdta(selftest_dta);
    KINFO(("vblk: gone rc %ld\n", Fsfirst(path, 0)));

    /* Rename the file the caller above wrote. */
    strcpy(path, "@:\\NEW.TXT");
    path[0] = 'A' + drive;
    strcpy(other, "@:\\RENAMED.TXT");
    other[0] = 'A' + drive;

    KINFO(("vblk: rename rc %ld\n", Frename(path, other)));

    Fsetdta(selftest_dta);
    KINFO(("vblk: renamed rc %ld\n", Fsfirst(other, 0)));

    /* A new subdirectory, with a file in it. */
    strcpy(path, "@:\\NEWDIR");
    path[0] = 'A' + drive;
    KINFO(("vblk: mkdir rc %ld\n", Dcreate(path)));

    strcpy(path, "@:\\NEWDIR\\INSIDE.TXT");
    path[0] = 'A' + drive;

    fh = Fcreate(path, 0);
    if (fh < 0)
    {
        KINFO(("vblk: innercreate rc %ld\n", fh));
        return;
    }

    for (i = 0; i < 64; i++)
        selftest_buf[i] = (UBYTE)('a' + (i % 26));

    KINFO(("vblk: inner %ld bytes\n",
           Fwrite((WORD)fh, 64L, selftest_buf)));
    KINFO(("vblk: innerclose rc %ld\n", Fclose((WORD)fh)));
}

#endif /* CONF_WITH_VIRTIO_BLK_WRITE */
#endif /* CONF_WITH_VIRTIO_BLK */

#endif /* MACHINE_QEMU_VIRT */

/*
 * Initialize the BIOS
 */

static void bios_init(void)
{
    KDEBUG(("bios_init()\n"));

    /* initialize Native Features, if available
     * do it as soon as possible so that kprintf can make use of them
     */
#if DETECT_NATIVE_FEATURES
    KDEBUG(("natfeat_init()\n"));
    natfeat_init();
#endif
#if STONX_NATIVE_PRINT
    KDEBUG(("stonx_kprintf_init()\n"));
    stonx_kprintf_init();
#endif
#if CONF_WITH_UAE
    KDEBUG(("amiga_uae_init()\n"));
    amiga_uae_init();
#endif

    /* Initialize the processor */
    KDEBUG(("processor_init()\n"));
    processor_init();   /* Set CPU type, longframe and FPU type */

#ifdef MACHINE_QEMU_VIRT
    /*
     * Take the CPU type from the bootinfo block rather than the probe.
     *
     * processor_init() cannot tell a 68040 from a 68060 here: the PCR
     * probe that would do it is compiled out (see processor.S), because
     * reading an unimplemented control register aborts the machine on
     * unpatched QEMU instead of faulting, and the probe would kill the
     * emulator before EmuTOS finished booting.  QEMU already knows what
     * it is running -- it derives BI_CPUTYPE from -cpu -- so ask it
     * instead of guessing.
     *
     * For the default -cpu m68040 this agrees with the probe and changes
     * nothing.  It is -cpu m68060 that was reported as a 68040 to every
     * program reading the _CPU cookie.
     */
    mcpu = qemuvirt_cputype();
#endif

#if CONF_WITH_ADVANCED_CPU
    is_bus32 = (UBYTE)detect_32bit_address_bus();
#endif
    KDEBUG(("Address Bus width is %d-bit\n", IS_BUS32 ? 32 : 24));

    KDEBUG(("vecs_init()\n"));
    vecs_init();        /* setup all exception vectors (above) */
    KDEBUG(("init_delay()\n"));
    init_delay();       /* set 'reasonable' default values for delay */

    /* Detect optional hardware (video, sound, etc.) */
    KDEBUG(("machine_detect()\n"));
    machine_detect();   /* detect hardware */
    KDEBUG(("machine_init()\n"));
    machine_init();     /* initialise machine-specific stuff */

    /* Initialize the BIOS memory management */
    KDEBUG(("bmem_init()\n"));
    bmem_init();

#if defined(MACHINE_AMIGA)
    /* Detect and initialize Zorro II/III expansion boards.
     * This must be done after is_bus32 and bmem_init().
     * Alt-RAM found on those boards will be added later in altram_init().
     */
    amiga_autoconfig();
#endif

#if CONF_WITH_68040_PMMU
    /*
     * Initialize the 68040 MMU if required
     * Must be done after TT-RAM memory detection (which takes place
     * in machine_detect() above) and memory management initialization.
     */
    if ((mcpu == 40) && mmu_is_emulated())
    {
        if (setup_68040_pmmu() != 0)
            panic("setup_68040_pmmu() failed\n");
    }
#endif /* CONF_WITH_68040_PMMU */

    KDEBUG(("cookie_init()\n"));
    cookie_init();      /* sets a cookie jar */
    KDEBUG(("fill_cookie_jar()\n"));
    fill_cookie_jar();  /* detect hardware features and fill the cookie jar */
    KDEBUG(("font_init()\n"));
    font_init();        /* initialize font ring (requires cookie_akp) */

#if CONF_WITH_BLITTER
    /*
     * If a PAK 68/3 is installed, the blitter cannot access the PAK ROMs.
     * So we must mark the blitter as not installed (this is what the
     * patched TOS3.06 ROM on the PAK does).  Since we can't detect a
     * PAK 68/3 directly, we look for a 68030 on an ST(e)/Mega ST(e).
     */
    if ((mcpu == 30)
     && ((cookie_mch == MCH_ST) || (cookie_mch == MCH_STE) || (cookie_mch == MCH_MSTE)))
        has_blitter = 0;
#endif

    /*
     * Initialize MFP and SCC: among other things this ensures that the
     * respective interrupts are disabled.
     */

#if CONF_WITH_MFP
    KDEBUG(("mfp_init()\n"));
    mfp_init();
#endif

#if CONF_WITH_TT_MFP
    if (has_tt_mfp)
    {
        KDEBUG(("tt_mfp_init()\n"));
        tt_mfp_init();
    }
#endif

#if CONF_WITH_SCC
    if (has_scc)
    {
        KDEBUG(("scc_init()\n"));
        scc_init();
    }
#endif

    /*
     * Initialize the screen mode
     * Must be done before calling linea_init().
     */
    KDEBUG(("screen_init_mode()\n"));
    screen_init_mode(); /* detect monitor type, ... */

    /* Set up the BIOS console output */
    KDEBUG(("linea_init()\n"));
    linea_init();       /* initialize screen related line-a variables */

    /*
     * Initialize the screen address
     * Must be done after calling linea_init().
     */
    KDEBUG(("screen_init_address()\n"));
    screen_init_address();

    KDEBUG(("vt52_init()\n"));
    vt52_init();        /* initialize the vt52 console */

    /* Now kcprintf() will also send debug info to the screen */
    KDEBUG(("after vt52_init()\n"));

    /* now we have output, let the user know we're alive */
    display_startup_msg();

#if DETECT_NATIVE_FEATURES
    /*
     * Tell ARAnyM where the LineA variables are
     */
    nf_setlinea();
#endif

    /* misc. variables */
    dumpflg = -1;
    sysbase = &os_header;
    savptr = (LONG) trap_save_area;
    etv_timer = (ETV_TIMER_T) just_rts;
    etv_critic = default_etv_critic;
    etv_term = just_rts;
    swv_vec = just_rts;

    /* setup default VBL queue with vbl_list[] */
    nvbls = ARRAY_SIZE(vbl_list);
    vblqueue = vbl_list;
    {
        int i;
        for(i = 0 ; i < nvbls ; i++) {
            vbl_list[i] = NULL;
        }
    }

    /*
     * Initialize the system 200 Hz timer (timer C on Atari hardware).
     * Note that init_system_timer() no longer enables the 50 Hz processing
     * that is called from the system timer interrupt.  This allows us to
     * enable interrupts earlier (so that the interrupt-driven serial port
     * routines work), even though we haven't yet initialised the sound &
     * keyboard repeat stuff.
     */
    KDEBUG(("init_system_timer()\n"));
    init_system_timer();

    /*
     * Now we can enable interrupts.  Although VBL & timer interrupts will
     * occur:
     *  . VBL processing will not be enabled until later, when the vblsem
     *    semaphore is set
     *  . 50 Hz processing (sound playback & keyboard repeat) will not be
     *    enabled until later, when timer_c_sieve is set
     */
#if CONF_WITH_ATARI_VIDEO
    /* Keep the HBL disabled */
    set_sr(0x2300);
#else
    set_sr(0x2000);
#endif

#if defined(MACHINE_ARANYM) || defined(TARGET_1024)
    /* ARAnyM 1.1.0 loads only the first half of 1024k ROMs.
     * Detect this situation and warn the user.
     * This method is ugly, but safe for releases as they are thoroughly tested.
     */
    if (IS_ARANYM && ((ULONG)_edata > 0x00e80000ul) && ULONG_AT(0x00e80000) == 0)
    {
        kcprintf(
            "\r\n"
            "ERROR: This 1024k ROM isn't supported by your ARAnyM version.\r\n"
            "Please use etos512*.img instead, until next ARAnyM release.\r\n"
        );

        /* Don't use halt() on ARAnyM, as it causes an infinite loop
           with the message: "STOPed with interrupts disabled, exiting;".
           FIXME: Fix halt() instead.
        */
        for(;;)
        {
#if USE_STOP_INSN_TO_FREE_HOST_CPU
            stop_until_interrupt();
#endif
        }
    }
#endif

    /* Initialize the RS-232 port(s) */
    KDEBUG(("chardev_init()\n"));
    chardev_init();     /* Initialize low-memory bios vectors */
    boot_status |= CHARDEV_AVAILABLE;   /* track progress */
    KDEBUG(("init_serport()\n"));
    init_serport();
    boot_status |= RS232_AVAILABLE;     /* track progress */
#if CONF_WITH_SCC
    if (has_scc)
        boot_status |= SCC_AVAILABLE;   /* track progress */
#endif

    /*
     * Initialise sound processing
     */
#if CONF_WITH_DMASOUND
    KDEBUG(("dmasound_init()\n"));
    dmasound_init();
#endif
    KDEBUG(("snd_init()\n"));
    snd_init();         /* Reset Soundchip, deselect floppies */

    /*
     * Initialise the two ACIA devices (MIDI and KBD), then initialise
     * the associated IORECs & vectors
     */
    KDEBUG(("kbd_init()\n"));
    kbd_init();         /* init keyboard, disable mouse and joystick */
    KDEBUG(("midi_init()\n"));
    midi_init();        /* init MIDI acia so that kbd acia irq works */
    KDEBUG(("init_acia_vecs()\n"));
    init_acia_vecs();   /* Init the ACIA interrupt vector and related stuff */
    KDEBUG(("after init_acia_vecs()\n"));

#ifdef MACHINE_QEMU_VIRT
    /*
     * Only now may the virtio input devices be enabled.
     *
     * Enabling them writes DRIVER_OK, and the host may deliver an event
     * immediately afterwards; the 200 Hz tick drains it and hands it to
     * call_ikbdraw(), which needs the kbdvec and IOREC that the call
     * above has just installed, and the key table bioskeys() selected
     * inside kbd_init().  Doing this from kbd_init() -- where it used to
     * live -- put it before both, so an event arriving in that window
     * jumped through an uninitialised vector and the boot died there,
     * between the keyboard's queue coming up and the tablet's.
     *
     * Intermittent by nature: it needs an event to be waiting, which
     * depends on whether the host had one queued.
     */
    KDEBUG(("virtio_input_init()\n"));
    virtio_input_init();
#endif
    boot_status |= MIDI_AVAILABLE;  /* track progress */

    /* Enable 50 Hz processing */
    timer_c_sieve = 0x1111;

    KDEBUG(("calibrate_delay()\n"));
    calibrate_delay();  /* determine values for delay() function */
                        /*  - requires interrupts to be enabled  */

    /* Initialize the DSP.  Since we currently use the system timer
     * in dsp_execboot(), which is called from dsp_init(), the latter
     * must be called *after* system timer interrupts are enabled.
     */
#if CONF_WITH_DSP
    KDEBUG(("dsp_init()\n"));
    dsp_init();
#endif

#if CONF_WITH_MEMORY_TEST
    /*
     * we only do this on cold boot
     */
    if (FIRST_BOOT)
    {
        BOOL ok;
        cprintf("\n%s:\n",_("Memory test"));
        ok = memory_test();         /* simple memory test, like Atari TOS */
        cprintf("%s %s\n",_("Memory test"),ok?_("complete"):_("aborted"));
    }
#endif

    /* User configurable boot delay to allow harddisks etc. to get ready */
    if (FIRST_BOOT && osxhbootdelay)
    {
        long end = hz_200 + (long)osxhbootdelay * CLOCKS_PER_SEC;
        while (hz_200 < end)
        {
#if USE_STOP_INSN_TO_FREE_HOST_CPU
            stop_until_interrupt();
#endif
        }
    }

    KDEBUG(("blkdev_init()\n"));
    blkdev_init();      /* floppy and harddisk initialisation */
    KDEBUG(("after blkdev_init()\n"));

#ifdef MACHINE_QEMU_VIRT
    hostfs_init();      /* register the host folder as drive C, if present */
#endif

    /* initialize BIOS components */

    KDEBUG(("parport_init()\n"));
    parport_init();     /* parallel port */
    KDEBUG(("clock_init()\n"));
    clock_init();       /* init clock */
    KDEBUG(("after clock_init()\n"));

#if CONF_WITH_NOVA
    /* Detect and initialize a Nova card, skip if Ctrl is pressed */
    if (HAS_NOVA && !(kbshift(-1) & MODE_CTRL)) {
        WORD cache_state = 0;
        KDEBUG(("init_nova()\n"));

        /*
         * On the Falcon, the Nova HW registers are in cached memory.
         * Thus, initialization only works when cache is disabled.
         */
        if (HAS_VIDEL) {
            cache_state = get_cache();
            set_cache(0);
        }

        if (init_nova()) {

#if CONF_WITH_BLITTER
            /* On the Falcon, the Blitter cannot access Nova video memory.
             * Therefore, disable the Blitter entirely, to prevent it from
             * being used.
             */
            if (HAS_VIDEL) {
                has_blitter = 0;
            }
#endif

            set_rez_hacked();   /* also reinitializes the vt52 console */
        }

        /* Restore old cache state on the Falcon */
        if (HAS_VIDEL) {
            set_cache(cache_state);
        }
    }
#endif /* CONF_WITH_NOVA */

#if CONF_WITH_NLS
    KDEBUG(("nls_init()\n"));
    nls_init();         /* init native language support */
    nls_set_lang(get_lang_name());
#endif

    /* Set start of user interface.
     * No need to check if os_header.os_magic->gm_magic == GEM_MUPB_MAGIC,
     * as this is always true. */
    exec_os = os_header.os_magic->gm_init;

    KDEBUG(("osinit_before_xmaddalt()\n"));
    osinit_before_xmaddalt();   /* initialize BDOS (part 1) */
    KDEBUG(("after osinit_before_xmaddalt()\n"));

#if CONF_WITH_ALT_RAM
    /* Add Alt-RAM to BDOS pool */
    KDEBUG(("altram_init()\n"));
    altram_init();
#endif

    KDEBUG(("osinit_after_xmaddalt()\n"));
    osinit_after_xmaddalt();    /* initialize BDOS (part 2) */
    KDEBUG(("after osinit_after_xmaddalt()\n"));
    boot_status |= DOS_AVAILABLE;   /* track progress */

#ifdef MACHINE_QEMU_VIRT
    hostfs_selftest();
#if CONF_WITH_VIRTIO_BLK
    vblk_selftest();
#endif
#endif

    /* Enable VBL processing */
    swv_vec = os_header.reseth; /* reset system on monitor change & jump to _main */
    vblsem = 1;

#if CONF_WITH_CARTRIDGE
    {
        WORD save_hz = V_REZ_HZ, save_vt = V_REZ_VT, save_pl = v_planes;

        /* Run all boot applications from the application cartridge.
         * Beware: Hatari features a special cartridge which is used
         * for GEMDOS drive emulation. It will hack drvbits and hook Pexec().
         * It will also hack Line A variables to enable extended VDI video modes.
         */
        KDEBUG(("run_cartridge_applications(3)\n"));
        run_cartridge_applications(3); /* Type "Execute prior to bootdisk" */
        KDEBUG(("after run_cartridge_applications()\n"));

        if ((V_REZ_HZ != save_hz) || (V_REZ_VT != save_vt) || (v_planes != save_pl))
        {
            set_rez_hacked();   /* also reinitializes the vt52 console */
        }
    }
#endif

    /* Boot hard disks */
    KDEBUG(("disk_try_dmaboot()\n"));
    disk_try_dmaboot();

    KDEBUG(("bios_init() end\n"));
}

#if DETECT_NATIVE_FEATURES

static void bootstrap(void)
{
    /* start the kernel provided by the emulator */
    PD *pd;
    LONG length;
    LONG r;
    char args[128];

    args[0] = '\0';
    nf_getbootstrap_args(args, sizeof(args));

    /* allocate space */
    pd = (PD *) Pexec(PE_BASEPAGEFLAGS, (char*)PF_STANDARD, args, default_env);

    /* get the TOS executable from the emulator */
    length = nf_bootstrap(pd->p_lowtpa + sizeof(PD), pd->p_hitpa - pd->p_lowtpa);

    /* free the allocated space if something is wrong */
    if (length <= 0)
        goto err;

    /* relocate the loaded executable */
    r = Pexec(PE_RELOCATE, (char *)length, (char *)pd, default_env);
    if (r != (LONG)pd)
        goto err;

    /* set the boot drive for the new OS to use */
    bootdev = nf_getbootdrive();

    /* execute the relocated process */
    Pexec(PE_GO, "", (char *)pd, default_env);

err:
    Mfree(pd->p_env); /* Mfree() the environment */
    Mfree(pd); /* Mfree() the process area */
}

#endif /* DETECT_NATIVE_FEATURES */

/*
 * Build the default environment string: "PATH=^X:\^^" [where ^=nul]
 */
static void init_default_environment(void)
{
    char *p;

    strcpy(default_env,PATH_ENV);
    p = default_env + sizeof(PATH_ENV); /* point to first byte of path string */
    strcpy(p,DEF_PATH);
    *p += bootdev;                      /* fix up drive letter */
    p += sizeof(DEF_PATH);
    *p = '\0';                          /* terminate with double nul */
}

#if ENABLE_RESET_RESIDENT
/*
 * run_reset_resident - run "reset-resident" code
 *
 * "Reset-resident" code is code that has been loaded into RAM prior
 * to a warm boot.  It has a special header with a magic number, it
 * is 512 bytes long (aligned on a 512-byte boundary), and it has a
 * specific checksum (calculated on a word basis).
 *
 * Note: this is an undocumented feature of TOS that exists in all
 * versions of Atari TOS.
 */
struct rrcode {
    long magic;
    struct rrcode *pointer;
    char program[502];
    short chksumfix;
};
#define RR_MAGIC    0x12123456L
#define RR_CHKSUM   0x5678

static void run_reset_resident(void)
{
    const struct rrcode *p = (const struct rrcode *)phystop;

    for (--p; p > (struct rrcode *)&etv_timer; p--)
    {
        if (p->magic != RR_MAGIC)
            continue;
        if (p->pointer != p)
            continue;
        if (compute_cksum((const UWORD *)p) != RR_CHKSUM)
            continue;
        regsafe_call(p->program);
    }
}
#endif

/*
 * autoexec - run programs in auto folder
 *
 * Skip this if user holds the Control key down.
 *
 * Note that GEMDOS already created a default basepage so it is safe
 * to use GEMDOS calls here!
 */

static void run_auto_program(const char* filename)
{
    char path[30];

    strcpy(path, "\\AUTO\\");
    strcat(path, filename);

    KDEBUG(("Loading %s ...\n", path));
    Pexec(PE_LOADGO, path, "", NULL);
    KDEBUG(("[OK]\n"));
}

static void autoexec(void)
{
    DTA dta;
    WORD err;

    /* check if the user does not want to run AUTO programs */
    if (bootflags & BOOTFLAG_SKIP_AUTO_ACC)
        return;

#if CONF_WITH_SHINOGI_SPLASH
    /*
     * Here, and not earlier.  Anywhere before this the startup screen
     * paints the whole display and wipes it off again -- which is what
     * happened on the first attempt, drawn after vt52_init() and gone
     * before it could be seen.
     *
     * This is the right moment on its own merits: the AUTO scan that
     * follows is where the boot spends its time, reading the kernel and
     * fVDI over hostfs a few kbyte at a time, and until now that was a
     * screen of startup text sitting still.  The logo replaces it, and
     * the AUTO programs' own output scrolls over the top from the left.
     */
    shinogi_splash();
#endif

#if DETECT_NATIVE_FEATURES
    bootstrap();                        /* try to boot the new OS kernel directly */
#endif

    if(!blkdev_avail(bootdev))          /* check, if bootdev available */
        return;

    Fsetdta(&dta);
    err = Fsfirst("\\AUTO\\*.PRG", 7);
    while(err == 0) {
#ifdef TARGET_PRG
        if (!strncmp(dta.d_fname, "EMUTOS", 6))
        {
            KDEBUG(("Skipping %s from AUTO folder\n", dta.d_fname));
        }
        else
#endif
        {
            run_auto_program(dta.d_fname);

            /* Setdta. BetaDOS corrupted the AUTO load if the Setdta
             * not repeated here */
            Fsetdta(&dta);
        }

        err = Fsnext();
    }
}

#if CONF_WITH_SHUTDOWN

/* Try to shutdown the machine. This may fail. */
static void shutdown(void)
{
#if DETECT_NATIVE_FEATURES
    nf_shutdown();
#endif

#ifdef MACHINE_FIREBEE
    firebee_shutdown();
#elif defined(MACHINE_AMIGA)
    amiga_shutdown();
#elif defined(MACHINE_LISA)
    lisa_shutdown();
#elif defined(MACHINE_QEMU_VIRT)
    qemuvirt_shutdown();
#endif
}

/* Will shutdown() succeed ? */
BOOL can_shutdown(void)
{
#if DETECT_NATIVE_FEATURES
    if (has_nf_shutdown())
        return TRUE;
#endif

#ifdef MACHINE_FIREBEE
    return TRUE;
#elif defined(MACHINE_AMIGA)
    return amiga_can_shutdown();
#elif defined(MACHINE_LISA)
    return TRUE;
#elif defined(MACHINE_QEMU_VIRT)
    return qemuvirt_can_shutdown();
#else
    return FALSE;
#endif
}

#endif /* CONF_WITH_SHUTDOWN */

/*
 * biosmain - c part of the bios init code
 *
 * Print some status messages
 * exec the user interface (shell or AES)
 */

void biosmain(void)
{
    BOOL show_initinfo;         /* TRUE if welcome screen must be displayed */
    ULONG shiftbits;

    bios_init();                /* Initialize the BIOS */

    /* Steem needs this to initialize its GEMDOS hard disk emulation.
     * This may change drvbits. See Steem sources:
     * File steem/code/emulator.cpp, function intercept_bios(). */
    Drvmap();

    /*
     * if it's not the first boot, we use the existing bootdev.
     * this allows a boot device that was selected via the welcome
     * screen to persist across warm boots.
     */
    if (FIRST_BOOT)
        bootdev = blkdev_avail(DEFAULT_BOOTDEV) ? DEFAULT_BOOTDEV : FLOPPY_BOOTDEV;

#if INITINFO_DURATION == 0
    show_initinfo = FALSE;
#elif ALWAYS_SHOW_INITINFO
    show_initinfo = TRUE;
#else
    show_initinfo = FIRST_BOOT;
#endif

    if (show_initinfo)
        bootdev = initinfo(&shiftbits); /* show the welcome screen */
    else
        shiftbits = kbshift(-1);

    KDEBUG(("bootdev = %d\n", bootdev));

    if (shiftbits & MODE_ALT)
        bootflags |= BOOTFLAG_SKIP_HDD_BOOT;

    if (shiftbits & MODE_CTRL)
        bootflags |= BOOTFLAG_SKIP_AUTO_ACC;

    KDEBUG(("bootflags = 0x%02x\n", bootflags));

    /* boot eventually from a block device (floppy or harddisk) */
    blkdev_boot();

    Dsetdrv(bootdev);           /* Set boot drive */
    init_default_environment(); /* Build default environment string */

#if ENABLE_RESET_RESIDENT
    run_reset_resident();       /* see comments above */
#endif

#if WITH_CLI
    if (bootflags & BOOTFLAG_EARLY_CLI) {
        /*
         * run an early console, passing the default environment
         */
        PD *pd = (PD *) Pexec(PE_BASEPAGEFLAGS, (char *)PF_STANDARD, "", default_env);
        pd->p_tbase = (UBYTE *) coma_start;
        pd->p_tlen = pd->p_dlen = pd->p_blen = 0;
        Pexec(PE_GOTHENFREE, "", (char *)pd, default_env);
    }
#endif

    autoexec();                 /* autoexec PRGs from AUTO folder */

    /* clear commandline */

    if(cmdload != 0) {
        /*
         * Pexec a program called COMMAND.PRG
         * like Atari TOS, it inherits an empty environment
         */
        Pexec(PE_LOADGO, "COMMAND.PRG", "", NULL);
    } else if (exec_os) {
        /*
         * start the default (ROM) shell
         * like Atari TOS, we pass the default environment
         */
        PD *pd;
        pd = (PD *) Pexec(PE_BASEPAGEFLAGS, (char *)PF_STANDARD, "", default_env);
        pd->p_tbase = (UBYTE *) exec_os;
        pd->p_tlen = pd->p_dlen = pd->p_blen = 0;
        Pexec(PE_GO, "", (char *)pd, default_env);
    }

#if CONF_WITH_SHUTDOWN
    /* try to shutdown the machine / close the emulator */
    shutdown();
#endif

    /* hide cursor */
    cprintf("\033f");

    kcprintf(_("System halted!\n"));
    halt();
}



/**
 * bios_0 - (getmpb) Load Memory parameter block
 *
 * Returns values of the initial memory parameter block, which contains the
 * start address and the length of the TPA.
 * Just executed one time, before GEMDOS is loaded.
 *
 * Arguments:
 *   mpb - first memory descriptor, filled from BIOS
 *
 */

#if DBGBIOS
static void bios_0(MPB *mpb)
{
    getmpb(mpb);
}
#endif


/**
 * bios_1 - (bconstat) Status of input device
 *
 * Arguments:
 *   handle - device handle (0:PRT 1:AUX 2:CON)
 *
 *
 * Returns status in D0.L:
 *  -1  device is ready
 *   0  device is not ready
 */

LONG bconstat(WORD handle)        /* GEMBIOS character_input_status */
{
#if BCONMAP_AVAILABLE
    WORD map_index;
#endif

    if (!(boot_status & CHARDEV_AVAILABLE))
        return 0;

#if BCONMAP_AVAILABLE
    map_index = handle - BCONMAP_START_HANDLE;
    if (map_index >= bconmap_root.maptabsize)
        return 0L;
    if (map_index >= 0)
        return protect_v(bconmap_root.maptab[map_index].Bconstat);
#endif

    if ((handle >= 0) && (handle <= 7))
        return protect_v(bconstat_vec[handle]);
    return 0L;
}

#if DBGBIOS
static LONG bios_1(WORD handle)
{
    return bconstat(handle);
}
#endif

/**
 * bconin  - Get character from device
 *
 * Arguments:
 *   handle - device handle (0:PRT 1:AUX 2:CON)
 *
 * This function does not return until a character has been
 * input.  It returns the character value in D0.L, with the
 * high word set to zero.  For CON:, it returns the GSX 2.0
 * compatible scan code in the low byte of the high word, &
 * the ASCII character in the lower byte, or zero in the
 * lower byte if the character is non-ASCII.  For AUX:, it
 * returns the character in the low byte.
 */

LONG bconin(WORD handle)
{
#if BCONMAP_AVAILABLE
    WORD map_index;
#endif

    if (!(boot_status & CHARDEV_AVAILABLE))
        return 0;

#if BCONMAP_AVAILABLE
    map_index = handle - BCONMAP_START_HANDLE;
    if (map_index >= bconmap_root.maptabsize)
        return 0L;
    if (map_index >= 0)
        return protect_v(bconmap_root.maptab[map_index].Bconin);
#endif

    if ((handle >= 0) && (handle <= 7))
        return protect_v(bconin_vec[handle]);
    return 0L;
}

#if DBGBIOS
static LONG bios_2(WORD handle)
{
    return bconin(handle);
}
#endif

/**
 * bconout  - Print character to output device
 */

LONG bconout(WORD handle, WORD what)
{
#if BCONMAP_AVAILABLE
    WORD map_index;
#endif

    if (!(boot_status & CHARDEV_AVAILABLE))
        return 0;

#if BCONMAP_AVAILABLE
    map_index = handle - BCONMAP_START_HANDLE;
    if (map_index >= bconmap_root.maptabsize)
        return 0L;
    if (map_index >= 0)
        return protect_ww((PFLONG)bconmap_root.maptab[map_index].Bconout, handle, what);
#endif

    if ((handle >= 0) && (handle <= 7))
        return protect_ww((PFLONG)bconout_vec[handle], handle, what);
    return 0L;
}

#if DBGBIOS
static LONG bios_3(WORD handle, WORD what)
{
    return bconout(handle, what);
}
#endif

#if CONF_SERIAL_CONSOLE_ANSI
/* Output a string via bconout() */
void bconout_str(WORD handle, const char* str)
{
    while (*str)
        bconout(handle, (UBYTE)*str++);
}
#endif

/**
 * rwabs  - Read or write sectors
 *
 * Returns a 2's complement error number in D0.L.  It
 * is the responsibility of the driver to check for
 * media change before any write to FAT sectors.  If
 * media has changed, no write should take place, just
 * return with error code.
 *
 * r_w   = 0:Read 1:Write
 * *adr  = where to get/put the data
 * numb  = # of sectors to get/put
 * first = 1st sector # to get/put = 1st record # tran
 * drive = drive #: 0 = A:, 1 = B:, etc
 */

LONG lrwabs(WORD r_w, UBYTE *adr, WORD numb, WORD first, WORD drive, LONG lfirst)
{
    return protect_wlwwwl((PFLONG)hdv_rw, r_w, (LONG)adr, numb, first, drive, lfirst);
}

#if DBGBIOS
static LONG bios_4(WORD r_w, UBYTE *adr, WORD numb, WORD first, WORD drive, LONG lfirst)
{
    LONG ret;
    KDEBUG(("BIOS rwabs(rw = %d, addr = %p, count = 0x%04x, "
            "sect = 0x%04x, dev = 0x%04x, lsect = 0x%08lx)",
            r_w, adr, numb, first, drive, lfirst));
    ret = lrwabs(r_w, adr, numb, first, drive, lfirst);
    KDEBUG((" = 0x%08lx\n", ret));
    return ret;
}
#endif



/**
 * Setexc - set exception vector
 *
 */

LONG setexc(WORD num, LONG vector)
{
    LONG oldvector;
    LONG *addr = (LONG *) (4L * num);
    oldvector = *addr;

    if(vector != -1) {
        *addr = vector;
    }
    return oldvector;
}

#if DBGBIOS
static LONG bios_5(WORD num, LONG vector)
{
    LONG ret = setexc(num, vector);
    KDEBUG(("Bios 5: Setexc(num = 0x%x, vector = %p)\n", num, (void*)vector));
    return ret;
}
#endif


/**
 * tickcal - Time between two systemtimer calls
 */

LONG tickcal(void)
{
    return(20L);        /* system timer is 50 Hz so 20 ms is the period */
}

#if DBGBIOS
static LONG bios_6(void)
{
    return tickcal();
}
#endif


/**
 * get_bpb - Get BIOS parameter block
 * Returns a pointer to the BIOS Parameter Block for
 * the specified drive in D0.L.  If necessary, it
 * should read boot header information from the media
 * in the drive to determine BPB values.
 *
 * Arguments:
 *  drive - drive  (0 = A:, 1 = B:, etc)
 */

LONG getbpb(WORD drive)
{
    return protect_w(hdv_bpb, drive);
}

#if DBGBIOS
static LONG bios_7(WORD drive)
{
    return getbpb(drive);
}
#endif


/**
 * bcostat - Read status of output device
 *
 * Returns status in D0.L:
 * -1   device is ready
 * 0    device is not ready
 */

/* handle  = 0:PRT 1:AUX 2:CON 3:MID 4:KEYB */

LONG bcostat(WORD handle)       /* GEMBIOS character_output_status */
{
#if BCONMAP_AVAILABLE
    WORD map_index;
#endif

    if (!(boot_status & CHARDEV_AVAILABLE))
        return 0;

#if BCONMAP_AVAILABLE
    map_index = handle - BCONMAP_START_HANDLE;
    if (map_index >= bconmap_root.maptabsize)
        return 0L;
    if (map_index >= 0)
        return protect_v(bconmap_root.maptab[map_index].Bcostat);
#endif

    if ((handle >= 0) && (handle <= 7))
        return protect_v(bcostat_vec[handle]);
    return 0L;
}

#if DBGBIOS
static LONG bios_8(WORD handle)
{
    return bcostat(handle);
}
#endif



/**
 * bios_9 - (mediach) See if floppy has changed
 *
 * Returns media change status for specified drive in D0.L:
 *   0  Media definitely has not changed
 *   1  Media may have changed
 *   2  Media definitely has changed
 * where "changed" = "removed"
 */

LONG mediach(WORD drv)
{
    return protect_w(hdv_mediach, drv);
}

#if DBGBIOS
static LONG bios_9(WORD drv)
{
    return mediach(drv);
}
#endif

/**
 * bios_a - (drvmap) Read drive bitmap
 *
 * Returns a long containing a bit map of logical drives on the system,
 * with bit 0, the least significant bit, corresponding to drive A.
 * Note that if the BIOS supports logical drives A and B on a single
 * physical drive, it should return both bits set if a floppy drive is
 * present.
 */

LONG drvmap(void)
{
    return blkdev_drvmap();
}

#if DBGBIOS
static LONG bios_a(void)
{
    return drvmap();
}
#endif

/*
 *  bios_b - (kbshift)  Shift Key mode get/set.
 *
 *  two descriptions:
 *      o       If 'mode' is non-negative, sets the keyboard shift bits
 *              accordingly and returns the old shift bits.  If 'mode' is
 *              less than zero, returns the IBM0PC compatible state of the
 *              shift keys on the keyboard, as a bit vector in the low byte
 *              of D0
 *      o       The flag parameter is used to control the operation of
 *              this function.  If flag is not -1, it is copied into the
 *              state variable(s) for the shift, control and alt keys,
 *              and the previous key states are returned in D0.L.  If
 *              flag is -1, then only the inquiry is done.
 */

#if DBGBIOS
static LONG bios_b(WORD flag)
{
    return kbshift(flag);
}
#endif


/**
 * bios_vecs - the table of bios command vectors.
 */

#if DBGBIOS
#define VEC(wrapper, direct) (PFLONG) wrapper
#else
#define VEC(wrapper, direct) (PFLONG) direct
#endif

const PFLONG bios_vecs[] = {
    VEC(bios_0, getmpb),
    VEC(bios_1, bconstat),
    VEC(bios_2, bconin),
    VEC(bios_3, bconout),
    VEC(bios_4, lrwabs),
    VEC(bios_5, setexc),
    VEC(bios_6, tickcal),
    VEC(bios_7, getbpb),
    VEC(bios_8, bcostat),
    VEC(bios_9, mediach),
    VEC(bios_a, drvmap),
    VEC(bios_b, kbshift),
};

const UWORD bios_ent = ARRAY_SIZE(bios_vecs);

#if CONF_WITH_EXTENDED_MOUSE

/* Does this pointer belong to our TEXT segment? */
BOOL is_text_pointer(const void *p)
{
    UBYTE *pb = (UBYTE *)p;
    return (pb >= (UBYTE *)&os_header) && (pb < _etext);
}

#endif /* CONF_WITH_EXTENDED_MOUSE */
