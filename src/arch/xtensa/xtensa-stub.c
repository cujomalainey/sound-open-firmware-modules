/****************************************************************************

		THIS SOFTWARE IS NOT COPYRIGHTED

   HP offers the following for use in the public domain.  HP makes no
   warranty with regard to the software or it's performance and the
   user accepts the software "AS IS" with all faults.

   HP DISCLAIMS ANY WARRANTIES, EXPRESS OR IMPLIED, WITH REGARD
   TO THIS SOFTWARE INCLUDING BUT NOT LIMITED TO THE WARRANTIES
   OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.

****************************************************************************/

/****************************************************************************
 *  Header: remcom.c,v 1.34 91/03/09 12:29:49 glenne Exp $
 *
 *  Module name: remcom.c $
 *  Revision: 1.34 $
 *  Date: 91/03/09 12:29:49 $
 *  Contributor:     Lake Stevens Instrument Division$
 *
 *  Description:     low level support for gdb debugger. $
 *
 *  Considerations:  only works on target hardware $
 *
 *  Written by:      Glenn Engel $
 *  ModuleState:     Experimental $
 *
 *  NOTES:           See Below $
 *
 *  Modified for SPARC by Stu Grossman, Cygnus Support.
 *
 *  This code has been extensively tested on the Fujitsu SPARClite demo board.
 *
 *  To enable debugger support, two things need to happen.  One, a
 *  call to set_debug_traps() is necessary in order to allow any breakpoints
 *  or error conditions to be properly intercepted and reported to gdb.
 *  Two, a breakpoint needs to be generated to begin communication.  This
 *  is most easily accomplished by a call to breakpoint().  Breakpoint()
 *  simulates a breakpoint by executing a trap #1.
 *
 *************
 *
 *    The following gdb commands are supported:
 *
 * command          function                               Return value
 *
 *    g             return the value of the CPU registers  hex data or ENN
 *    G             set the value of the CPU registers     OK or ENN
 *
 *    mAA..AA,LLLL  Read LLLL bytes at address AA..AA      hex data or ENN
 *    MAA..AA,LLLL: Write LLLL bytes at address AA.AA      OK or ENN
 *
 *    c             Resume at current address              SNN   ( signal NN)
 *    cAA..AA       Continue at address AA..AA             SNN
 *
 *    s             Step one instruction                   SNN
 *    sAA..AA       Step one instruction from AA..AA       SNN
 *
 *    k             kill
 *
 *    ?             What was the last sigval ?             SNN   (signal NN)
 *
 * All commands and responses are sent with a packet which includes a
 * checksum.  A packet consists of
 *
 * $<packet info>#<checksum>.
 *
 * where
 * <packet info> :: <characters representing the command or response>
 * <checksum>    :: < two hex digits computed as modulo 256 sum of <packetinfo>>
 *
 * When a packet is received, it is first acknowledged with either '+' or '-'.
 * '+' indicates a successful transfer.  '-' indicates a failed transfer.
 *
 * Example:
 *
 * Host:                  Reply:
 * $m0,10#2a               +$00010203040506070809101112131415#42
 *
 ****************************************************************************/
#include <string.h>
#include <signal.h>
#include <stdint.h>
#include <xtensa/xtruntime.h>
#include <reef/intel-ipc.h>
#include "xtensa-defs.h"

/************************************************************************
 *
 * external low-level support routines
 */

extern void putDebugChar(char c);	/* write a single character      */
extern int getDebugChar(void);	/* read and return a single char */

/************************************************************************/
/* BUFMAX defines the maximum number of characters in inbound/outbound buffers*/
/* at least NUMREGBYTES*2 are needed for register packets */
#define BUFMAX 256
#define USE_GDBSTUB
#ifdef USE_GDBSTUB
#define bulk_data __attribute__((section (".ddr0.data")))
#else
#define bulk_data
#endif
uint32_t stack[STACK_SIZE / sizeof(uint32_t)] bulk_data;
static uint8_t sregs_read[32] bulk_data;
static uint8_t sregs_mod[32] bulk_data;
static uint8_t sregs_late[32] bulk_data;
uint32_t sregs[256] bulk_data;
// TODO the following array should use XCHAL_NUM_AREGS to size
// but it is currently pulling in the wrong define
uint32_t aregs[32] bulk_data;
static char remcomInBuffer[BUFMAX] bulk_data;
static char remcomOutBuffer[BUFMAX] bulk_data;

static const char hexchars[]="0123456789abcdef";

extern void putDebugChar(char c);
extern int getDebugChar(void);

// https://opensource.apple.com/source/Libc/Libc-167/gen.subproj/ppc.subproj/strncmp.c
int strncmp (const char *s1, const char *s2, size_t n)
{
  for ( ; n > 0; s1++, s2++, --n)
		if (*s1 != *s2)
	    return ((*(unsigned char *)s1 < *(unsigned char *)s2) ? -1 : +1);
		else if (*s1 == '\0')
	    return 0;
   return 0;
}

/* Convert ch from a hex digit to an int */

static int hex(unsigned char ch)
{
	if (ch >= 'a' && ch <= 'f')
		return ch-'a'+10;
	if (ch >= '0' && ch <= '9')
		return ch-'0';
	if (ch >= 'A' && ch <= 'F')
		return ch-'A'+10;
	return -1;
}

/* scan for the sequence $<data>#<checksum>     */

char *getpacket(void)
{
	char *buffer = &remcomInBuffer[0];
	unsigned char checksum;
	unsigned char xmitcsum;
	int count;
	char ch;
	while (1) {
		/* wait around for the start character, ignore all other characters */
		while ((ch = getDebugChar ()) != '$')
			;

retry:
		checksum = 0;
		xmitcsum = -1;
		count = 0;

		/* now, read until a # or end of buffer is found */
		while (count < BUFMAX - 1) {
			ch = getDebugChar ();
			if (ch == '$')
				goto retry;
			if (ch == '#')
				break;
			checksum = checksum + ch;
			buffer[count] = ch;
			count = count + 1;
		}
		buffer[count] = 0;

		if (ch == '#') {
			ch = getDebugChar ();
			xmitcsum = hex (ch) << 4;
			ch = getDebugChar ();
			xmitcsum += hex (ch);

			if (checksum != xmitcsum) {
				putDebugChar ('-');	/* failed checksum */
			} else {
				putDebugChar ('+');	/* successful transfer */

				/* if a sequence char is present, reply the sequence ID */
				if (buffer[2] == ':') {
					putDebugChar (buffer[0]);
					putDebugChar (buffer[1]);

					return &buffer[3];
				}

				return &buffer[0];
			}
		}
	}
}

/* send the packet in buffer.  */
static void putpacket(char *buffer)
{
	unsigned char checksum;
	int count;
	unsigned char ch;
	/*  $<packet info>#<checksum>. */
	do {
		putDebugChar('$');
		checksum = 0;
		count = 0;

		while ((ch = buffer[count])) {
			putDebugChar(ch);
			checksum += ch;
			count += 1;
		}

		putDebugChar('#');
		putDebugChar(hexchars[checksum >> 4]);
		putDebugChar(hexchars[checksum & 0xf]);
		ipc_flush_gdb_tx_buffer();
	} while (getDebugChar() != '+');
}

/* Indicate to caller of mem2hex or hex2mem that there has been an
   error.  */
volatile int mem_err = 0;

/* Convert the memory pointed to by mem into hex, placing result in buf.
 * Return a pointer to the last char put in buf (null), in case of mem fault,
 * return 0.
 */

static char *mem2hex(const void *mem_, char *buf, int count)
{
	const unsigned char *mem = mem_;
	unsigned char ch;
	mem_err = 0;
	while (count-- > 0) {
#ifdef __XTENSA__
		unsigned long v;
		unsigned long addr = (unsigned long)mem;
		asm volatile ("_l32i	%0, %1, 0\n"
			      : "=r"(v)
			      : "r"(addr & ~3)
			      : "memory");
		ch = v >> (addr & 3) * 8;
#endif
		mem++;
		if (mem_err)
			return NULL;
		*buf++ = hexchars[ch >> 4];
		*buf++ = hexchars[ch & 0xf];
	}

	*buf = 0;

	return buf;
}

/* convert the hex array pointed to by buf into binary to be placed in mem
 * return a pointer to the character AFTER the last byte written */

static char *hex2mem(const char *buf, void *mem_, int count)
{
	char *mem = mem_;
	int i;
	unsigned char ch;
	mem_err = 0;
	for (i=0; i<count; i++) {
		ch = hex(*buf++) << 4;
		ch |= hex(*buf++);
#ifdef __XTENSA__
		unsigned long tmp;
		unsigned long addr = (unsigned long)mem;
		asm volatile ("_l32i	%0, %1, 0\n"
			      "and	%0, %0, %2\n"
			      "or	%0, %0, %3\n"
			      "_s32i	%0, %1, 0\n"
			      "dhwb	%1, 0\n"
			      "ihi	%1, 0\n"
			      : "=r"(tmp)
			      : "r"(addr & ~3), "r"(0xffffffff ^ (0xff << (addr & 3) * 8)),
			        "r"(ch << (addr & 3) * 8)
			      : "memory");
#endif
		mem++;
		if (mem_err)
			return NULL;
	}

	return mem;
}

/*
 * While we find nice hex chars, build an int.
 * Return number of chars processed.
 */

static int hexToInt(char **ptr, int *intValue)
{
	int numChars = 0;
	int hexValue;

	*intValue = 0;
	while (**ptr) {
		hexValue = hex(**ptr);
		if (hexValue < 0)
			break;

		*intValue = (*intValue << 4) | hexValue;
		numChars ++;

		(*ptr)++;
	}

	return (numChars);
}

static inline int test_bit(const uint8_t *p, int bit)
{
	return (p[bit / 8] >> (bit & 0x7)) & 1;
}
static inline void set_bit(uint8_t *p, int bit)
{
	p[bit / 8] |= 1u << (bit & 0x7);
}

static inline void mark_read(int sr)
{
	set_bit(sregs_read, sr);
}
static inline int is_read(int sr)
{
	return test_bit(sregs_read, sr);
}
static inline void mark_mod(int sr)
{
	set_bit(sregs_mod, sr);
}
static inline int is_mod(int sr)
{
	return test_bit(sregs_mod, sr);
}
static inline void mark_late(int sr)
{
	set_bit(sregs_late, sr);
}
static inline int is_late(int sr)
{
	return test_bit(sregs_late, sr);
}

static void read_sr(int sr)
{
	if (!is_read(sr)) {
#ifdef __XTENSA__
		uint32_t val;
		asm volatile ("movi	a3, 1f + 1\n"
			      "s8i	%1, a3, 0\n"
			      "dhwb	a3, 0\n"
			      "ihi	a3, 0\n"
			      "isync\n"
			      "1:\n"
			      "rsr	%0, lbeg\n"
			      : "=r"(val)
			      : "r"(sr)
			      : "a3", "memory");
		sregs[sr] = val;
#endif
		mark_read(sr);
	}
}

static void write_sr(int sr)
{
#ifdef __XTENSA__
	asm volatile ("movi	a3, 1f + 1\n"
		      "s8i	%1, a3, 0\n"
		      "dhwb	a3, 0\n"
		      "ihi	a3, 0\n"
		      "isync\n"
		      "1:\n"
		      "wsr	%0, lbeg\n"
		      :
		      : "r"(sregs[sr]), "r"(sr)
		      : "a3", "memory");
#endif
}

static void restore_sr(void)
{
	int i;
	for (i = 0; i < 256; ++i)
		if (is_mod(i) && !is_late(i))
			write_sr(i);
}

extern void *_xtos_exc_handler_table[];
void fault_handler(void);

void handle_exception(void)
{
	int sigval = 0;
	int addr;
	int length;
	char *ptr;
	unsigned i;
	const unsigned windowbase = 4 * sregs[WINDOWBASE];
	char stop_status[4] = "Sxx";
#ifdef LIBC_LEVEL1_HANDLER
	static const int cause[] = {
		EXCCAUSE_LOAD_STORE_ERROR,
		EXCCAUSE_LOAD_STORE_DATA_ERROR,
		EXCCAUSE_LOAD_STORE_ADDR_ERROR,
		EXCCAUSE_DTLB_MISS,
		EXCCAUSE_DTLB_MULTIHIT,
		EXCCAUSE_LOAD_PROHIBITED,
		EXCCAUSE_STORE_PROHIBITED,
	};
	_xtos_handler handler[sizeof(cause) / sizeof(cause[0])];
	for (i = 0; i < ARRAY_SIZE(cause); ++i) {
		handler[i] = _xtos_exc_handler_table[cause[i]];
		_xtos_exc_handler_table[cause[i]] = fault_handler;
	}
#endif
	memcpy(sregs_read, sregs_late, sizeof(sregs_read));
	memset(sregs_mod, 0, sizeof(sregs_mod));

	sigval = 5;
	stop_status[1] = hexchars[sigval >> 4];
	stop_status[2] = hexchars[sigval & 0xf];

	if (sregs[DEBUGCAUSE] & DEBUGCAUSE_ICOUNT_MASK) {
		sregs[ICOUNTLEVEL] = 0;
		mark_mod(ICOUNTLEVEL);
	}
	putpacket(stop_status);

	while (1) {
		remcomOutBuffer[0] = 0;

		ptr = getpacket();
		switch (*ptr++) {
		case '?':
			memcpy(remcomOutBuffer, stop_status, sizeof(stop_status));
			break;

		case 'c':    /* cAA..AA    Continue at address AA..AA(optional) */
			/* try to read optional parameter, pc unchanged if no parm */

			if (hexToInt(&ptr, &addr))
				sregs[DEBUG_PC] = addr;
			goto out;
		case 'g':		/* return the value of the CPU registers */
		  mem2hex (sregs + DEBUG_PC, remcomOutBuffer, sizeof(uint32_t));
		  break;
		case 'G':		/* set the value of the CPU registers - return OK */
		  hex2mem (ptr, (char *) aregs, sizeof(aregs));
		  strcpy (remcomOutBuffer, "OK");
		  break;
		// acknowledge but ignore multi-threading command to appease gdb
		case 'H':
			strcpy(remcomOutBuffer, "E00");
			break;
		case 'm':	  /* mAA..AA,LLLL  Read LLLL bytes at address AA..AA */
			/* Try to read %x,%x.  */

			if (hexToInt(&ptr, &addr) && *ptr++ == ',' &&
			    hexToInt(&ptr, &length)) {
				if (mem2hex((void *)addr, remcomOutBuffer, length))
					break;

				strcpy(remcomOutBuffer, "E03");
			} else {
				strcpy(remcomOutBuffer, "E01");
			}
			break;

		case 'M': /* MAA..AA,LLLL: Write LLLL bytes at address AA.AA return OK */
			/* Try to read '%x,%x:'.  */

			if (hexToInt(&ptr, &addr) && *ptr++ == ',' &&
			    hexToInt(&ptr, &length) && *ptr++ == ':') {
				if (hex2mem(ptr, (void *)addr, length))
					strcpy(remcomOutBuffer, "OK");
				else
					strcpy(remcomOutBuffer, "E03");
			} else {
				strcpy(remcomOutBuffer, "E02");
			}
			break;

		case 'p': /* pAA..AA read register number AA..AA */
			if (hexToInt(&ptr, &addr)) {
				if (addr < 0x10) { /* read address register in the current window */
					mem2hex(aregs + addr, remcomOutBuffer, 4);
				} else if (addr == 0x20) { /* read PC */
					mem2hex(sregs + DEBUG_PC, remcomOutBuffer, 4);
				} else if (addr >= 0x100 && addr < 0x100 + XCHAL_NUM_AREGS) { /* read address register by absolute index */
					mem2hex(aregs + ((addr - windowbase) & 0xff), remcomOutBuffer, 4);
				} else if (addr >= 0x200 && addr < 0x300) { /* read special register */
					addr &= 0xff;
					read_sr(addr);
					mem2hex(sregs + addr, remcomOutBuffer, 4);
				} else if (addr >= 0x300 && addr < 0x400) { /* TODO read user register */
					strcpy(remcomOutBuffer, "deadbabe");
				} else { /* unexpected register number */
					strcpy(remcomOutBuffer, "E00");
				}
			}
			break;

		case 'P': /* PAA..AA=VV..VV  Set register number AA..AA to a value VV..VV */
			if (hexToInt(&ptr, &addr) && *(ptr++) == '=') {
				int ok = 1;

				if (addr < 0x10) {
					hex2mem(ptr, aregs + addr, 4);
				} else if (addr == 0x20) {
					hex2mem(ptr, sregs + DEBUG_PC, 4);
				} else if (addr >= 0x100 && addr < 0x100 + XCHAL_NUM_AREGS) {
					hex2mem(ptr, aregs + ((addr - windowbase) & 0xff), 4);
				} else if (addr >= 0x200 && addr < 0x300) {
					addr &= 0xff;
					hex2mem(ptr, sregs + addr, 4);
					mark_read(addr);
					mark_mod(addr);
				} else {
					ok = 0;
					strcpy(remcomOutBuffer, "E00");
				}
				if (ok)
					strcpy(remcomOutBuffer, "OK");
			}
			break;

		case 'q': /* generic query */
			if (strncmp(ptr, "Supported", 9) == 0)
				strcpy(remcomOutBuffer, "PacketSize=256"); /* must match BUFMAX */
			break;

		case 's': /* s[AA..AA] Single step */
			if (hexToInt(&ptr, &addr))
				sregs[DEBUG_PC] = addr;
			sregs[ICOUNT] = 0xfffffffe;
			mark_mod(ICOUNT);
			sregs[ICOUNTLEVEL] = XCHAL_DEBUGLEVEL;
			mark_mod(ICOUNTLEVEL);
			goto out;

		case 'Z': /* insert HW breakpoint*/
			switch (*ptr++) {
			case '1':
				read_sr(IBREAKENABLE);
				if (*ptr++ == ',' && hexToInt(&ptr, &addr) &&
				    *ptr++ == ',' && hexToInt(&ptr, &length) &&
				    *ptr == 0) {
					for (i = 0; i < XCHAL_NUM_IBREAK; ++i) {
						if (!(sregs[IBREAKENABLE] & (1 << i)) ||
						    sregs[IBREAKA + i] == addr) {
							sregs[IBREAKA + i] = addr;
							mark_mod(IBREAKA + i);
							sregs[IBREAKENABLE] |= (1 << i);
							mark_mod(IBREAKENABLE);
							break;
						}
					}
					if (i == XCHAL_NUM_IBREAK)
						strcpy(remcomOutBuffer, "E02");
					else
						strcpy(remcomOutBuffer, "OK");
				} else {
					strcpy(remcomOutBuffer, "E01");
				}
				break;
			}
			break;

		case 'z': /* remove HW breakpoint */
			switch (*ptr++) {
			case '1':
				read_sr(IBREAKENABLE);
				if (*ptr++ == ',' && hexToInt(&ptr, &addr) &&
				    *ptr++ == ',' && hexToInt(&ptr, &length)) {
					for (i = 0; i < XCHAL_NUM_IBREAK; ++i) {
						read_sr(IBREAKA + i);
						if (sregs[IBREAKENABLE] & (1 << i) &&
						    sregs[IBREAKA + i] == addr) {
							sregs[IBREAKENABLE] &= ~(1 << i);
							mark_mod(IBREAKENABLE);
							break;
						}
					}
					if (i == XCHAL_NUM_IBREAK)
						strcpy(remcomOutBuffer, "E02");
					else
						strcpy(remcomOutBuffer, "OK");
				} else {
					strcpy(remcomOutBuffer, "E01");
				}
				break;
			}
			break;
		}

		/* reply to the request */
		putpacket(remcomOutBuffer);
	}
out:
#ifdef LIBC_LEVEL1_HANDLER
	for (i = 0; i < ARRAY_SIZE(cause); ++i) {
		_xtos_exc_handler_table[cause[i]] = handler[i];
	}
#endif
	restore_sr();
}

void init_debug_entry();

void init_gdbstub(void)
{
	mark_late(LBEG);
	mark_late(LEND);
	mark_late(LCOUNT);
	mark_late(SAR);
	mark_late(WINDOWBASE);
	mark_late(WINDOWSTART);
	mark_late(DEBUG_PC);
	mark_late(EXCSAVE_1);
	mark_late(PS);
	mark_late(EXCCAUSE);
	mark_late(DEBUGCAUSE);
	mark_late(EXCVADDR);
#ifdef __XTENSA__
	init_debug_entry();
#endif
}
