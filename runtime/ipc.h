// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
// Message framing per the HIPC, CMIF and TIPC wire layouts.
// In-process Horizon IPC: HIPC messages with CMIF or TIPC payloads, answered in
// place in the caller's message buffer. Services are host implementations looked up by name through sm:.
#pragma once
#include <stdint.h>

#include "host.h"

#define IPC_MAX_BUF 8
#define IPC_MSG_BYTES 0x100
#define IPC_R_BAD_HANDLE 0xE401u           // kernel: invalid handle
#define IPC_R_UNKNOWN_COMMAND 0xF601u      // sf: unknown command (module 10, description 123)

typedef struct { uint64_t addr, size; unsigned index; } IpcBuffer;

typedef struct {
    unsigned type, cmd;                    // HIPC type (4/5/6/7 CMIF, >= 16 TIPC) and command id
    int has_pid;
    uint64_t pid;
    uint32_t copy[8], move[8];
    unsigned ncopy, nmove;
    IpcBuffer x[IPC_MAX_BUF], a[IPC_MAX_BUF], b[IPC_MAX_BUF], w[IPC_MAX_BUF], c[IPC_MAX_BUF];
    unsigned nx, na, nb, nw, nc;
    const uint8_t *data;                   // command arguments after the CMIF header (TIPC: after the HIPC header)
    uint32_t data_size;
} IpcRequest;

typedef struct {
    uint32_t result;
    uint8_t data[0x100];
    uint32_t data_size;
    uint32_t copy[8], move[8];
    unsigned ncopy, nmove;
    IpcBuffer xout[IPC_MAX_BUF];           // pointer data the service placed into the client's C buffers
    unsigned nxout;
    int domain;                            // the reply carries a domain message header
    unsigned domain_objects;               // objects the service appended to the client's domain
} IpcResponse;

typedef struct IpcSession IpcSession;
typedef void (*IpcHandler)(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);

// One server object. Every kernel handle to it (the first one, CloneCurrentObject copies) holds a reference, and so
// does a request on it while it runs; the last reference calls close(state). A domain's table holds one on each of
// its objects, and each object one on its domain (held_root), until the client closes the object's id
// (CloseVirtualHandle) or its last handle to the domain itself. ipc.c's domain lock guards objects, nobjects, subs
// and dom_closed.
struct IpcSession {
    const char *name;
    IpcHandler handler;
    void *state;
    void (*close)(void *state);
    int32_t refs;
    int is_domain;                         // ConvertCurrentObjectToDomain ran: requests carry a domain header
    IpcSession *root;                      // the domain session every object id belongs to (objects share it)
    IpcSession *objects[32];               // object 1 is this session, then what the service handed out; NULL: closed
    int nobjects;                          // slots used so far (closed ones are given out again)
    uint32_t event;                        // readable event handed to the client (applet state changes)
    IpcSession *held_root;                 // the domain this object holds a reference on (ipc_domain_add)
    int32_t subs;                          // as a domain: the references its objects hold on it
    int dom_closed;                        // as a domain: its client closed it and its table let the objects go
};

// Register an object inside a domain: the table takes the object's first reference (the caller's), the object one on
// the domain. The 1-based id, 0 when the table is full or the domain closed.
unsigned ipc_domain_add(IpcSession *domain, IpcSession *object);

uint32_t ipc_session_new(const char *name, IpcHandler handler, void *state, void (*close)(void *state));   // handle
void ipc_out(IpcResponse *rs, const void *p, uint32_t n);
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n);
void *ipc_guest(uint64_t addr, uint64_t size);   // a client buffer as a host pointer (identity map), range checked
__attribute__((noreturn)) void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq);
#define IPC_OUT(rs, v) ipc_out((rs), &(v), sizeof(v))
#define IPC_ARG(rq, T, off) (*(const T *)ipc_arg((rq), (off), sizeof(T)))

// Transfer memory from svcCreateTransferMemory (0x15), a KERN_TMEM handle object.
typedef struct { uint64_t addr, size; uint32_t perm; } IpcTransferMemory;

// Server objects alive now and closed so far (the probe checks CloseHandle releases them).
long ipc_sessions_live(void);
uint64_t ipc_sessions_released(void);

// The guest frame chain of the request being served, for fatal reports from services.
void ipc_backtrace(void);
void ipc_applet_event_init(void);   // call once at boot, before the guest starts
void ipc_applet_push_message(uint32_t msg);   // queue an AppletMessage and signal the applet event
