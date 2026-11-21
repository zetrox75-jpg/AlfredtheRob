#ifndef WM_H
#define WM_H

#include <ntddk.h>

/*
 * Wire contract for the WmPort transport.
 *
 * Request numbers use ((type << 16) | (function << 2)) with METHOD_BUFFERED
 * and FILE_ANY_ACCESS. Keep this file, 2_src_port/rr_mem.c and
 * 2_src_port/aio_launcher.c in step: a mismatch shows up as rejected requests
 * or misinterpreted fields, and a successful call alone does not prove
 * compatibility.
 *
 * The field order of every structure below intentionally differs from the
 * earlier transport so an old client and this driver cannot silently agree.
 */

#define WM_TYPE        0x29UL
#define WM_CTL(fn)     ((ULONG)((WM_TYPE << 16) | (((ULONG)(fn)) << 2)))

#define WM_IO_READ     WM_CTL(0x8A1)   /* read target memory                */
#define WM_IO_BASE     WM_CTL(0x8A2)   /* image base of the target          */
#define WM_IO_UNLOAD   WM_CTL(0x8A3)   /* drop the DOS link                 */
#define WM_IO_WRITE    WM_CTL(0x8A4)   /* write target memory               */
#define WM_IO_PEB      WM_CTL(0x8A5)   /* PEB of the target                 */
#define WM_IO_REGION   WM_CTL(0x8A6)   /* one region descriptor             */
#define WM_IO_BATCH    WM_CTL(0x8A7)   /* up to 64 reads in one call        */
#define WM_IO_VEIL     WM_CTL(0x8A8)   /* unlink / relink a process         */
#define WM_IO_ALLOC    WM_CTL(0x8A9)   /* allocate in the target            */
#define WM_IO_CONTEXT  WM_CTL(0x8AD)   /* thread register snapshot          */

#define WM_BATCH_MAX      64
#define WM_MAX_TRANSFER   0x10000
#define WM_VEIL_MAX       16

#pragma pack(push, 1)

/* Common request for READ, WRITE, BASE, PEB, REGION and UNLOAD.
 * Order: key, size, pid, pad, address. */
typedef struct _WM_PACKET {
    ULONG     key;
    ULONG     size;
    ULONG     pid;
    ULONG     pad;
    ULONGLONG address;
} WM_PACKET, *PWM_PACKET;

/* One batch item: offset in the reply, byte count, target address. */
typedef struct _WM_ITEM {
    ULONG     offset;
    ULONG     size;
    ULONGLONG address;
} WM_ITEM, *PWM_ITEM;

typedef struct _WM_BATCH {
    ULONG   key;
    ULONG   count;
    ULONG   total;                  /* reply data bytes from buffer offset 0 */
    ULONG   pid;
    WM_ITEM items[WM_BATCH_MAX];
    /* The reply is `total` data bytes followed by `count` status bytes.
     * The reply overwrites the request; this header is not retained. */
} WM_BATCH, *PWM_BATCH;

/* REGION reply: order state, protect, type, pad, base, size. */
typedef struct _WM_REGION {
    ULONG     state;
    ULONG     protect;
    ULONG     type;
    ULONG     pad;
    ULONGLONG base;
    ULONGLONG size;
} WM_REGION, *PWM_REGION;

/* CONTEXT request and reply. */
typedef struct _WM_CTX_REQ {
    ULONG key;
    ULONG tid;
} WM_CTX_REQ, *PWM_CTX_REQ;

typedef struct _WM_REGS {
    ULONG     tid;
    ULONG     ok;
    ULONGLONG rflags;
    ULONGLONG rbp;
    ULONGLONG rsp;
    ULONGLONG rip;
} WM_REGS, *PWM_REGS;

/* VEIL request: action != 0 unlinks, action == 0 relinks. */
typedef struct _WM_VEIL_REQ {
    ULONG key;
    ULONG pid;
    ULONG action;
    ULONG pad;
} WM_VEIL_REQ, *PWM_VEIL_REQ;

/* ALLOC request; the reply is a single ULONG64 address. */
typedef struct _WM_ALLOC_REQ {
    ULONG     key;
    ULONG     pid;
    ULONG     protect;
    ULONG     pad;
    ULONGLONG size;
} WM_ALLOC_REQ, *PWM_ALLOC_REQ;

#pragma pack(pop)

#endif /* WM_H */
