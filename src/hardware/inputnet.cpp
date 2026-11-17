/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/* Shared input emulation ("input net").
 *
 * A great many DOS games only ever offered "two players, one PC, one keyboard"
 * as their multiplayer mode: no IPX, no serial, no TCP/UDP, just two sets of
 * keys on the same keyboard. This module makes those games playable over a
 * network by running two (or more) DOSBox-X instances as one virtual machine:
 *
 *  - one instance acts as the server (INPUTNET STARTSERVER), the others connect
 *    to it (INPUTNET CONNECT <address>),
 *  - when a client joins, the server hands its complete emulator save state
 *    over to it, so that every instance starts from bit-identical state,
 *  - from then on the instances run in lockstep: emulated time is divided into
 *    fixed "frames" and no instance may execute frame N until it knows the
 *    input every participant produced for frame N,
 *  - all keyboard and mouse input of every participant is applied to every
 *    instance at the same emulated time.
 *
 * Because the emulated machines are deterministic and are fed exactly the same
 * input at exactly the same emulated time, they stay in sync while each player
 * sees the result on their own screen. No video is transmitted, so the network
 * load is tiny (a couple of KB/s) compared to a remote desktop / VNC approach.
 *
 * The design intentionally mirrors the IPX over UDP tunneling support: a UDP
 * socket, a server that the other instances dial into, and an INPUTNET.COM
 * command on drive Z: with the same sub-command layout as IPXNET.COM.
 *
 * Requirements for a session to stay in sync:
 *  - both sides must run the same DOSBox-X build,
 *  - "cycles" should be a fixed value (cycles=max/auto make the emulated
 *    machine's behaviour depend on the host CPU speed),
 *  - only the shared input may drive the guest; anything else that differs
 *    between the instances (different mounts, different config) will desync.
 */

#include "dosbox.h"
#include "inputnet.h"
#include "remoteplay.h"

/* Fast path flag. Defined unconditionally so that the keyboard/mouse code does
 * not need to know whether networking support was compiled in at all. */
bool inputnet_capture_active = false;

/* RollbackNet ("mode = rollback"): a session is running, and frames are being replayed after a correction. Checked by
 * the main loop (dosbox.cpp) and the mixer. */
bool inputnet_rollback_active = false;
bool inputnet_rollback_resim = false;

#if C_INPUTNET

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <algorithm>
#include <functional>
#if defined(__linux__) && defined(__GLIBC__)
#include <execinfo.h>
#include <unistd.h>
#endif
#include <cfloat>
#include <sstream>
#include <string>
#include <vector>

#include "SDL.h"
#include "callback.h"
#include "control.h"
#include "cross.h"
#include "logging.h"
#include "mouse.h"
#include "cpu.h"
#include "mem.h"
#include "paging.h"
#include "vga.h"
#include "pic.h"
#include <zlib.h>
#include "regs.h"
#include "programs.h"
#include "setup.h"
#include "support.h"
#include "timer.h"
#include "video.h"
#include "../dos/drives.h"

#if defined(USE_TTF)
# include "render.h"
#endif

#if defined(C_SDL2_NET) && C_SDL2_NET
# include <SDL2/SDL_net.h>
#else
# include "SDL_net.h"
#endif

#ifndef SDLNet_GetError
# define SDLNet_GetError SDL_GetError
#endif

/* RollbackNet's guest RAM write tracking (RBT_*) uses the host's page protection */
#if defined(WIN32)
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
#else
# include <signal.h>
# include <sys/mman.h>
# include <time.h>
# include <unistd.h>
#endif

/* ------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* ------------------------------------------------------------------------- */

uint32_t CALLBACK_LayoutSignature(void);
extern bool savestate_keep_local_drives;
void CMOS_GetSharedClock(uint8_t out[16]);
void CMOS_SetSharedClock(const uint8_t in[16]);
/* The real-time clock and the DOS date are not part of a save state: a joining machine takes
 * the server's, sent with the WELCOME, and applies them with the server's emulator state. */
static uint8_t inputnet_pending_clock[16];
static uint16_t inputnet_pending_year;
static uint8_t inputnet_pending_month, inputnet_pending_day;
static bool inputnet_have_pending_clock = false;
/* Neither is the time stamp counter's base: the server rebases it at the snapshot and sends the
 * count, and the client rebases to that count at the same emulated moment. */
extern uint64_t rdtsc_count_base;
extern pic_tickindex_t rdtsc_pic_base;
void RDTSC_rebase();
static uint64_t inputnet_pending_tsc = 0;
static bool inputnet_have_pending_tsc = false;
/* Nor is the VGA's frame pacing reference (an absolute emulated time): without it the joining
 * machine schedules its vertical retrace up to 0.1 ms away from the server's. */
extern double vga_fps, vga_mode_time_base;
extern int vga_mode_frames_since_time_base;
uint8_t VGA_GetOnDemandFlags(void);         /* vga_draw.cpp: how frames are drawn, not in save states */
void VGA_SetOnDemandFlags(uint8_t f);
static double inputnet_pending_vga_fps = 0, inputnet_pending_vga_base = 0;
static uint32_t inputnet_pending_vga_frames = 0;
static bool inputnet_have_pending_vga = false;
static uint64_t INPUTNET_DoubleBits(double d) { uint64_t u; memcpy(&u,&d,8); return u; }
static double INPUTNET_BitsDouble(uint64_t u) { double d; memcpy(&d,&u,8); return d; }
#include "dos_inc.h"

/* Host volume labels (read from the host disk on Windows) differ between computers. Shared input
 * machines must answer the guest identically, so local folder drives show no label in a session. */
static void INPUTNET_ClearHostVolumeLabels(void) {
    for (unsigned i=0;i < DOS_DRIVES;i++)
        if (Drives[i] && !strncmp(Drives[i]->GetInfo(),"local directory ",16)) Drives[i]->SetLabel("",false,true);
}

#define INPUTNET_DEFAULT_PORT   2130u   /* not an assigned port, but adjacent to IPX's 213 */
#define INPUTNET_MAX_PEERS      8u
#define INPUTNET_RING           1024u   /* input frames kept per peer, power of two (RollbackNet replays after a resync) */
#define INPUTNET_MAX_PACKET     1400u   /* stay below the usual 1500 byte MTU */
#define INPUTNET_MAX_EVENT_SIZE 18u     /* the largest an encoded event can get */
#define INPUTNET_MAX_FRAME_EVENTS 64u   /* so that one frame always fits a datagram */
#define INPUTNET_MAGIC          0x4E495844u /* "DXIN" */
#define INPUTNET_PROTOVER       1u

/* Save state hand-off */
#define INPUTNET_STATE_CHUNK    1024u
#define INPUTNET_STATE_WINDOW   32u

/* Packet types */
enum {
    INPKT_HELLO     = 1,    /* client -> server: request to join */
    INPKT_WELCOME   = 2,    /* server -> client: accepted, here are the parameters */
    INPKT_INPUT     = 3,    /* any: input for a range of frames */
    INPKT_BYE       = 4,    /* any: leaving */
    INPKT_PING      = 5,
    INPKT_PONG      = 6,
    INPKT_STATE     = 7,    /* server -> client: save state chunk */
    INPKT_STATEACK  = 8,    /* client -> server: cumulative save state acknowledge */
    INPKT_READY     = 9,    /* client -> server: state applied, ready to run */
    INPKT_START     = 10,   /* server -> client: (re)start lockstep at frame 0 */
    INPKT_CHECK     = 11,   /* any: machine checksum for a frame, to detect a desync */
    INPKT_RBINPUT   = 12,   /* any, mode=rollback: input frames, acknowledgements, frame and machine hash */
    INPKT_RESYNC    = 13,   /* server -> client, mode=rollback: take this machine state over (after a desync) */
    INPKT_FILEREQ   = 14,   /* client -> server, mode=rollback, joining: the server's game files it needs */
    INPKT_FILES     = 15,   /* server -> client, mode=rollback, joining: those files (chunks, acknowledged like the state) */
    /* mode=rollback clusters, between a computer's leader and its followers over 127.0.0.1 */
    INPKT_CL_HELLO  = 16,   /* follower -> leader: u8 copy */
    INPKT_CL_WELCOME= 17,   /* leader -> follower */
    INPKT_CL_CMD    = 18,   /* leader -> follower: u32 seq, u8 command (a blob with id seq follows some) */
    INPKT_CL_CMDACK = 19,   /* follower -> leader: u32 seq */
    INPKT_CL_BULK   = 20,   /* either: u32 blob id, u32 total, u32 offset, u16 length, bytes */
    INPKT_CL_BULKACK= 21,   /* either: u32 blob id, u32 bytes received in order */
    INPKT_CL_AT     = 22,   /* follower -> leader: u32 frame, u32 epoch, u32 hash hi, lo, u16 packets, (u16 len, bytes)... */
    INPKT_CL_GO     = 23,   /* leader -> follower: u32 frame, u32 epoch, u16 events, events, u16 packets, (u16 len, bytes)... */
    INPKT_CL_REWIND = 24    /* leader -> follower: u32 target frame, u32 real-time frame, u32 epoch */
};

/* Input event types */
enum {
    INEV_KEY         = 0,
    INEV_MOUSEBUTTON = 1,
    INEV_MOUSEMOVE   = 2,
    INEV_MOUSEWHEEL  = 3,
    /* Not input, but it travels with it: the server publishes changes to the
     * emulated CPU speed this way so that they land at the same emulated time
     * on every participant. */
    INEV_CYCLEMAX    = 4
};

/* How the emulated CPU speed is kept identical across participants. */
enum {
    INCYCLES_AUTO = 0,      /* the server publishes every change it makes */
    INCYCLES_FREEZE,        /* locked to the server's value for the session */
    INCYCLES_OFF            /* not managed at all */
};

/* ------------------------------------------------------------------------- */
/* Packet (de)serialization                                                   */
/* ------------------------------------------------------------------------- */

class InputNetWriter {
public:
    InputNetWriter(uint8_t *buf,size_t buflen) : b(buf), cap(buflen) { }

    void u8(uint8_t v) {
        if (o+1 > cap) { err = true; return; }
        b[o++] = v;
    }
    void u16(uint16_t v) {
        if (o+2 > cap) { err = true; return; }
        b[o++] = (uint8_t)(v & 0xFFu);
        b[o++] = (uint8_t)(v >> 8u);
    }
    void u32(uint32_t v) {
        if (o+4 > cap) { err = true; return; }
        b[o++] = (uint8_t)( v         & 0xFFu);
        b[o++] = (uint8_t)((v >>  8u) & 0xFFu);
        b[o++] = (uint8_t)((v >> 16u) & 0xFFu);
        b[o++] = (uint8_t)((v >> 24u) & 0xFFu);
    }
    void i16(int16_t v) { u16((uint16_t)v); }
    void f32(float v) { uint32_t r; memcpy(&r,&v,4); u32(r); }
    void bytes(const void *d,size_t n) {
        if (o+n > cap) { err = true; return; }
        memcpy(b+o,d,n); o += n;
    }

    size_t size(void) const { return o; }
    bool overflow(void) const { return err; }

private:
    uint8_t *b;
    size_t cap;
    size_t o = 0;
    bool err = false;
};

class InputNetReader {
public:
    InputNetReader(const uint8_t *buf,size_t buflen) : b(buf), len(buflen) { }

    uint8_t u8(void) {
        if (o+1 > len) { err = true; return 0; }
        return b[o++];
    }
    uint16_t u16(void) {
        if (o+2 > len) { err = true; return 0; }
        uint16_t v = (uint16_t)b[o] | ((uint16_t)b[o+1] << 8u);
        o += 2; return v;
    }
    uint32_t u32(void) {
        if (o+4 > len) { err = true; return 0; }
        uint32_t v = (uint32_t)b[o] | ((uint32_t)b[o+1] << 8u) |
                     ((uint32_t)b[o+2] << 16u) | ((uint32_t)b[o+3] << 24u);
        o += 4; return v;
    }
    int16_t i16(void) { return (int16_t)u16(); }
    float f32(void) { uint32_t v = u32(); float f; memcpy(&f,&v,4); return f; }
    const uint8_t *bytes(size_t n) {
        if (o+n > len) { err = true; return NULL; }
        const uint8_t *p = b+o; o += n; return p;
    }

    bool bad(void) const { return err; }
    size_t remain(void) const { return (o < len) ? (len-o) : 0; }

private:
    const uint8_t *b;
    size_t len;
    size_t o = 0;
    bool err = false;
};

/* ------------------------------------------------------------------------- */
/* Session state                                                              */
/* ------------------------------------------------------------------------- */

struct InputNetEvent {
    uint8_t type = 0;
    uint8_t pressed = 0;
    uint8_t button = 0;
    uint8_t emulate = 0;
    uint16_t key = 0;
    int16_t wheel = 0;
    float xrel = 0, yrel = 0, x = 0, y = 0;
    uint32_t cyclemax = 0;
};

/* One machine checksum, kept in a small ring per participant. */
struct InputNetCheck {
    uint32_t frame = 0;
    uint32_t hash = 0;
    bool valid = false;
};

#define INPUTNET_CHECK_RING 16u

struct InputNetFrame {
    uint32_t frame = 0;
    bool valid = false;
    std::vector<InputNetEvent> events;
};

struct InputNetPeer {
    bool active = false;        /* slot in use */
    bool ready = false;         /* taking part in the lockstep */
    bool local = false;         /* this is us */
    IPaddress addr = { 0, 0 };
    uint32_t confirmed = 0;     /* highest frame this peer has published input for */
    uint32_t delivered = 0;     /* highest frame already applied to the emulated machine */
    uint32_t lastheard = 0;     /* host time (ms) of the last packet from this peer */
    uint32_t rtt = 0;           /* last measured round trip time, ms */
    std::string name;
    InputNetFrame ring[INPUTNET_RING];
    InputNetCheck checks[INPUTNET_CHECK_RING];

    void resetFrames(void) {
        confirmed = 0;
        delivered = 0;
        for (unsigned i=0;i < INPUTNET_RING;i++) {
            ring[i].valid = false;
            ring[i].frame = 0;
            ring[i].events.clear();
        }
        for (unsigned i=0;i < INPUTNET_CHECK_RING;i++)
            checks[i].valid = false;
    }
};

/* Configuration */
static bool inputnet_enabled = false;
static bool inputnet_remote = false;        /* "mode = remote": stream from the host instead of lockstep */
static bool inputnet_rollback = false;      /* "mode = rollback": predict the others' input, correct by rolling back */
static unsigned int inputnet_rbwindow = 16; /* mode=rollback: frames that may run ahead of the others' input */
/* mode=rollback, cycles=max (RB_MaxCycles): the load the busiest computer runs at, in percent; this computer's speed
 * proposal (cycles per ms, 0: none yet), what the server heard from the others, and the measurement under way */
static unsigned int rb_maxload = 85;
static uint32_t rb_cap_local = 0;
static uint32_t rb_cap_peer[INPUTNET_MAX_PEERS], rb_cap_peer_at[INPUTNET_MAX_PEERS];
static uint64_t rb_ml_t0 = 0, rb_ml_wait = 0, rb_ml_sleep0 = 0, rb_ml_core0 = 0;
static uint32_t rb_ml_changed_at = 0, rb_ml_told_at = 0;
/* the last second's load, the CPU core's share of it and the average speed meanwhile; averages over a few seconds of
 * the core's time per cycle and of the rest of the load (which does not change with the speed) */
static double rb_ml_load = 0, rb_ml_core = 0, rb_ml_speed = 0, rb_ml_cost_s = -1, rb_ml_fixed_s = -1;
static double rb_ml_csum = 0;
static uint32_t rb_ml_cn = 0;
/* the CPU time the emulation thread got, at the start of the measurement, and the share of the busy time it ran (the
 * rest, other programs had the CPU: not a cost of this speed) */
static uint64_t rb_ml_cpu0 = 0;
static double rb_ml_run = 1;
/* how often the picture changed meanwhile (frames a second, a window's process only) and the screen's refresh rate */
static unsigned int rb_ml_pic0 = 0;
static double rb_ml_pictures = 0, rb_ml_refresh = 70;
extern unsigned int render_changed_frames;   /* render.cpp */
/* the latest replays (RB_Propose's second limit): frames replayed, time taken, the CPU core's part of it, the speed,
 * when it ended */
struct RBReplayRec { uint32_t depth, us, core_us, speed, at; };
static RBReplayRec rb_rp[64];
static unsigned rb_rp_n = 0, rb_rp_pos = 0;
static uint64_t rb_rp_t0 = 0, rb_rp_core0 = 0;
static uint32_t rb_rp_depth = 0;
static const double RB_REPLAY_BUDGET_US = 20000.0;  /* a replay should fit in this (with its frame: 3 frames)... */
static const unsigned RB_REPLAY_WINDOW_MS = 10000u, RB_REPLAY_LONG_ALLOWED = 5u;  /* ...all but 5 in 10 seconds */
static void RB_ReplayBegin(uint32_t depth);
static void RB_ReplayEnd(void);
/* hitches: real-time frames that started more than 30 ms after the one before (a long replay, a wait for the others) */
static uint64_t rb_hitch_last = 0, rb_stat_maxgap_us = 0;
static uint32_t rb_stat_hitches = 0;
extern uint64_t dosbox_core_ns;             /* dosbox.cpp: what the CPU core ran, while dosbox_core_timing */
extern bool dosbox_core_timing;
extern uint64_t dosbox_sleep_us;            /* dosbox.cpp: what increaseticks() slept */
static unsigned int inputnet_rbplayers = 0; /* mode=rollback server: participants to wait for before typing autorun */
static std::string inputnet_rbautorun;      /* mode=rollback server: the command typed then */
static bool inputnet_rbautorun_done = false;
static bool inputnet_share_keyboard = true;
static bool inputnet_share_mouse = true;
static bool inputnet_sync_state = true;
static uint16_t inputnet_default_port = (uint16_t)INPUTNET_DEFAULT_PORT;
static unsigned int inputnet_frame_ms = 10;
static unsigned int inputnet_delay = 3;
static unsigned int inputnet_redundancy = 4;
static unsigned int inputnet_timeout = 10000;
static unsigned int inputnet_cyclesync = INCYCLES_AUTO;
static unsigned int inputnet_checkevery = 100;

/* Auto start, from the config file or the command line */
static std::string inputnet_auto_connect;
static int inputnet_auto_server = -1;
static bool inputnet_autostart_done = false;

/* Link */
static UDPsocket inputnet_socket = NULL;
static bool inputnet_is_server = false;
static bool inputnet_link_up = false;       /* socket open and talking to someone */
static bool inputnet_session = false;       /* lockstep running */
static IPaddress inputnet_server_addr = { 0, 0 };
static uint16_t inputnet_active_port = 0;
static uint8_t inputnet_selfid = 0;
static InputNetPeer inputnet_peers[INPUTNET_MAX_PEERS];

/* Lockstep bookkeeping */
static uint32_t inputnet_epoch = 0;         /* bumped by the server on every (re)start */
static uint32_t inputnet_frame = 0;
static uint32_t inputnet_local_confirmed = 0;
static unsigned int inputnet_tickaccum = 0;
static bool inputnet_busy = false;          /* a blocking operation is in progress */
static bool inputnet_in_tick = false;       /* running inside INPUTNET_TickHandler */
static bool inputnet_tick_installed = false;

/* Pending save state to apply at the next safe point */
static bool inputnet_pending_stateload = false;
static std::vector<uint8_t> inputnet_pending_state;

/* Keys/buttons we handed to the emulated machine, so they can be released
 * cleanly when the session ends. */
static bool inputnet_keydown[KBD_LAST];
static uint32_t inputnet_buttondown = 0;

/* Statistics, shown by INPUTNET STATUS */
static uint32_t inputnet_stat_stallms = 0;
static uint32_t inputnet_stat_frames = 0;
static uint32_t inputnet_stat_desyncs = 0;
static uint32_t inputnet_stat_cycleupdates = 0;

/* Emulated CPU speed the whole session is running at. */
static uint32_t inputnet_cyclemax = 0;
static InputNetCheck inputnet_localchecks[INPUTNET_CHECK_RING];

static UDPpacket *inputnet_sendpkt = NULL;
static UDPpacket *inputnet_recvpkt = NULL;

/* Re-entrancy guard: accepting a participant polls the socket, which can turn
 * up another join request. */
static bool inputnet_accepting = false;

/* Set while INPUTNET.COM is running. Replacing the emulator state (or freezing
 * the machine to hand it over) is only safe once the DOS program has returned:
 * until then the host still has emulated-CPU frames on its stack that expect to
 * resume the machine we are about to throw away. */
static bool inputnet_in_program = false;

extern bool SDLNetInited;

static void INPUTNET_EndSession(const char *reason,bool notify_peers);
static void INPUTNET_CloseLink(const char *reason,bool notify_peers);
static void INPUTNET_PollSocket(void);
static void RB_HandleInput(InputNetPeer &p,unsigned peerid,InputNetReader &r);
static void RB_HandleResync(InputNetReader &r,const IPaddress &from,unsigned peerid);
static std::string RB_Fingerprint(void);
static std::string RB_FingerprintDiff(const std::string &theirs, const std::string &mine);
/* clusters (IPX/NullModem-over-RollbackNet) */
static unsigned int rbc_copies = 0;       /* copies of the game per computer, 0: no cluster */
static unsigned int rbc_copy = 0;         /* the player this copy plays for (its peer id) */
static bool rbc_leader = false;
static unsigned int rbc_port = 0;
static bool rbc_attached = false;         /* IPX_ClusterAttach done */
static bool rbc_ready = false;            /* leader: every follower is here */
static bool rb_joining = false;           /* a joining computer, until it has taken the server's machines over */
static uint32_t rb_start_frame = 0;       /* the frame a session starts at (a joining computer starts at the server's) */
static bool rbj_server_prompt = true;     /* joining: the server's game was at the DOS prompt when we joined */
static std::string rbj_autorun;           /* joining: what the server typed to start the game */
extern void IPX_ClusterAttach(unsigned int copy);
extern bool IPX_ClusterActive(void);
static void RBC_PollLeader(void);
static void RBC_ApplyInputs(uint32_t frame);
static void RBC_LeaderRewind(uint32_t target);
static bool RBC_FollowersCanLoad(uint32_t f, uint32_t now);
static void RBC_ServerDrop(unsigned id, const char *why);
static void RBC_ServerHello(const IPaddress &addr, const std::string &name, unsigned copies, unsigned want);
static bool RBC_ClientJoin(uint32_t total, uint32_t server_frame, uint32_t welcomed_at, std::string &errmsg);
static void RBC_FollowerTick(void);
static void RBC_LeaderGather(void);
static void RB_SessionStarted(void);
static void RB_SessionEnded(void);

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

static bool INPUTNET_SameAddr(const IPaddress &a,const IPaddress &b) {
    return a.host == b.host && a.port == b.port;
}

static InputNetPeer *INPUTNET_PeerByAddr(const IPaddress &addr) {
    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
        if (inputnet_peers[i].active && !inputnet_peers[i].local &&
            INPUTNET_SameAddr(inputnet_peers[i].addr,addr))
            return &inputnet_peers[i];
    }
    return NULL;
}

static unsigned INPUTNET_PeerCount(void) {
    unsigned n = 0;
    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++)
        if (inputnet_peers[i].active) n++;
    return n;
}

/* Keep the host side of the emulator responsive while emulation is stalled
 * waiting for the other side. */
static void INPUTNET_Stall(uint32_t &last_events) {
    const uint32_t now = GetTicks();

    if ((now - last_events) >= 10u) {
        GFX_Events();
        last_events = now;
    }

    SDL_Delay(1);
}

static bool INPUTNET_SendRaw(const IPaddress &addr,const uint8_t *data,size_t len) {
    if (inputnet_socket == NULL || inputnet_sendpkt == NULL) return false;
    if (len > (size_t)inputnet_sendpkt->maxlen) return false;

    memcpy(inputnet_sendpkt->data,data,len);
    inputnet_sendpkt->len = (int)len;
    inputnet_sendpkt->address = addr;

    /* channel -1 means "use the address stored in the packet" */
    return SDLNet_UDP_Send(inputnet_socket,-1,inputnet_sendpkt) != 0;
}

/* Send to every peer except ourselves, and except "skip" if given. A client
 * only ever talks to the server; the server talks to everybody. */
static void INPUTNET_SendToPeers(const uint8_t *data,size_t len,const InputNetPeer *skip = NULL) {
    if (!inputnet_is_server) {
        INPUTNET_SendRaw(inputnet_server_addr,data,len);
        return;
    }

    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || p.local || &p == skip) continue;
        INPUTNET_SendRaw(p.addr,data,len);
    }
}

static void INPUTNET_BeginPacket(InputNetWriter &w,uint8_t type) {
    w.u32(INPUTNET_MAGIC);
    w.u8((uint8_t)INPUTNET_PROTOVER);
    w.u8(type);
    w.u8(inputnet_selfid);
    w.u8(0); /* reserved */
}

static InputNetFrame &INPUTNET_FrameSlot(InputNetPeer &p,uint32_t frame) {
    InputNetFrame &s = p.ring[frame % INPUTNET_RING];
    if (!s.valid || s.frame != frame) {
        s.frame = frame;
        s.valid = true;
        s.events.clear();
    }
    return s;
}

static InputNetFrame *INPUTNET_FrameSlotIfValid(InputNetPeer &p,uint32_t frame) {
    InputNetFrame &s = p.ring[frame % INPUTNET_RING];
    if (s.valid && s.frame == frame) return &s;
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Local input capture                                                        */
/* ------------------------------------------------------------------------- */

/* The frame a local event produced right now must be scheduled for. Normally
 * that is "current frame + input delay", but we must never add input to a frame
 * we have already published (the other instances may have executed it), which
 * can happen for events picked up while emulation is stalled. */
static unsigned int RB_EffectiveDelay(void);

static uint32_t INPUTNET_ScheduleFrame(void) {
    uint32_t f = inputnet_frame + (inputnet_rollback ? RB_EffectiveDelay() : inputnet_delay);
    if (f <= inputnet_local_confirmed) f = inputnet_local_confirmed + 1u;
    return f;
}

static void INPUTNET_QueueLocal(const InputNetEvent &ev) {
    /* a cluster's follower is played by somebody elsewhere (RBC_ApplyInputs); a joining computer waits for the machines */
    if (rbc_copies && (!rbc_leader || rb_joining)) return;
    InputNetPeer &self = inputnet_peers[inputnet_selfid];
    InputNetFrame &slot = INPUTNET_FrameSlot(self,INPUTNET_ScheduleFrame());

    /* Only the newest speed proposal for a frame is worth keeping. */
    if (ev.type == INEV_CYCLEMAX) {
        for (size_t i=0;i < slot.events.size();i++) {
            if (slot.events[i].type == INEV_CYCLEMAX) {
                slot.events[i].cyclemax = ev.cyclemax;
                slot.events[i].pressed = slot.events[i].pressed || ev.pressed;
                return;
            }
        }
    }

    /* Mouse motion within one frame is accumulated into a single event: that is
     * what a real mouse would report anyway, and it keeps the packets small. */
    if (ev.type == INEV_MOUSEMOVE) {
        for (size_t i=0;i < slot.events.size();i++) {
            if (slot.events[i].type == INEV_MOUSEMOVE) {
                slot.events[i].xrel += ev.xrel;
                slot.events[i].yrel += ev.yrel;
                slot.events[i].x = ev.x;
                slot.events[i].y = ev.y;
                slot.events[i].emulate = ev.emulate;
                return;
            }
        }
    }

    /* Runaway protection. The limit is chosen so that one frame's worth of
     * events always fits in a single datagram: 64 * 18 bytes plus the header is
     * comfortably below INPUTNET_MAX_PACKET. */
    if (slot.events.size() >= INPUTNET_MAX_FRAME_EVENTS) {
        LOG(LOG_MISC,LOG_WARN)("INPUTNET: too many input events in one frame, dropping");
        return;
    }

    slot.events.push_back(ev);
}

bool INPUTNET_InterceptKey(KBD_KEYS keytype,bool pressed) {
    if (!inputnet_capture_active || !inputnet_share_keyboard) return false;
    if ((unsigned)keytype >= (unsigned)KBD_LAST) return false;

    InputNetEvent ev;
    ev.type = INEV_KEY;
    ev.key = (uint16_t)keytype;
    ev.pressed = pressed ? 1 : 0;
    INPUTNET_QueueLocal(ev);
    return true;
}

bool INPUTNET_InterceptMouseMotion(float xrel,float yrel,float x,float y,bool emulate) {
    if (!inputnet_capture_active || !inputnet_share_mouse) return false;

    InputNetEvent ev;
    ev.type = INEV_MOUSEMOVE;
    ev.xrel = xrel; ev.yrel = yrel; ev.x = x; ev.y = y;
    ev.emulate = emulate ? 1 : 0;
    INPUTNET_QueueLocal(ev);
    return true;
}

bool INPUTNET_InterceptMouseButton(uint8_t button,bool pressed) {
    if (!inputnet_capture_active || !inputnet_share_mouse) return false;

    InputNetEvent ev;
    ev.type = INEV_MOUSEBUTTON;
    ev.button = button;
    ev.pressed = pressed ? 1 : 0;
    INPUTNET_QueueLocal(ev);
    return true;
}

bool INPUTNET_InterceptMouseWheel(int32_t scroll) {
    if (!inputnet_capture_active || !inputnet_share_mouse) return false;
    if (scroll < -32768 || scroll > 32767) return false;

    InputNetEvent ev;
    ev.type = INEV_MOUSEWHEEL;
    ev.wheel = (int16_t)scroll;
    INPUTNET_QueueLocal(ev);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Applying input to the emulated machine                                     */
/* ------------------------------------------------------------------------- */

static void INPUTNET_ApplyEvent(const InputNetEvent &ev) {
    switch (ev.type) {
        case INEV_KEY:  /* "pressed": bit 0 down, bit 1 typed by the RollbackNet server's autorun */
            if (ev.key < (uint16_t)KBD_LAST) {
                inputnet_keydown[ev.key] = (ev.pressed & 1u) != 0;
                KEYBOARD_AddKeyRaw((KBD_KEYS)ev.key,(ev.pressed & 1u) != 0);
            }
            break;
        case INEV_MOUSEBUTTON:
            if (ev.pressed) {
                if (ev.button < 32) inputnet_buttondown |= (1u << ev.button);
                Mouse_ButtonPressedRaw(ev.button);
            }
            else {
                if (ev.button < 32) inputnet_buttondown &= ~(1u << ev.button);
                Mouse_ButtonReleasedRaw(ev.button);
            }
            break;
        case INEV_MOUSEMOVE:
            if (inputnet_rollback) {
                /* RollbackNet: as motion from a remote play viewer, relative only. Otherwise the motion would be
                 * applied by this computer's own state (whether its window has the mouse, fullscreen, where its host
                 * pointer is), which differs on every computer. */
                extern bool mouse_motion_is_relative;
                mouse_motion_is_relative = true;
                Mouse_CursorMovedRaw(ev.xrel,ev.yrel,ev.x,ev.y,true);
                mouse_motion_is_relative = false;
            }
            else Mouse_CursorMovedRaw(ev.xrel,ev.yrel,ev.x,ev.y,ev.emulate != 0);
            break;
        case INEV_MOUSEWHEEL:
            Mouse_WheelMovedRaw(ev.wheel);
            break;
        case INEV_CYCLEMAX:
            /* Every participant reaches this at the same emulated time, so the
             * emulated machines keep executing the same amount of guest code
             * per emulated millisecond. */
            if (ev.cyclemax != 0 && inputnet_cyclesync != INCYCLES_OFF) {
                inputnet_cyclemax = ev.cyclemax;
                inputnet_stat_cycleupdates++;
                DOSBOX_ApplyCycleMax((cpu_cycles_count_t)ev.cyclemax,ev.pressed != 0);
            }
            break;
        default:
            break;
    }
}

/* Apply the input of every participant for one frame. The peers are walked in
 * a fixed order so that every instance applies the events in the same sequence. */
static void INPUTNET_DeliverFrame(uint32_t frame) {
    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready) continue;

        InputNetFrame *slot = INPUTNET_FrameSlotIfValid(p,frame);
        if (slot != NULL) {
            for (size_t e=0;e < slot->events.size();e++)
                INPUTNET_ApplyEvent(slot->events[e]);
            slot->valid = false;
            slot->events.clear();
        }

        if (frame > p.delivered) p.delivered = frame;
    }
}

/* Let go of everything the remote side left pressed. */
static void INPUTNET_ReleaseHeldInput(void) {
    for (unsigned k=0;k < (unsigned)KBD_LAST;k++) {
        if (inputnet_keydown[k]) {
            inputnet_keydown[k] = false;
            KEYBOARD_AddKeyRaw((KBD_KEYS)k,false);
        }
    }
    for (unsigned b=0;b < 32;b++) {
        if (inputnet_buttondown & (1u << b)) {
            inputnet_buttondown &= ~(1u << b);
            Mouse_ButtonReleasedRaw((uint8_t)b);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Emulated CPU speed authority                                               */
/* ------------------------------------------------------------------------- */

/* Called from the emulator whenever it wants to change how many cycles are
 * executed per emulated millisecond - which "cycles=max" and "cycles=auto" do
 * continuously, based on how fast the local host happens to be.
 *
 * Returning true means "not yours to apply". The server turns the change into
 * an event that rides along with its input stream, so every participant applies
 * it at the same emulated time; a client simply discards its own guess and
 * waits to be told. Because the participants run in lockstep, the server's
 * automatic guessing measures the speed of the whole group and settles on
 * something the slowest host can sustain. */
bool INPUTNET_ProposeCycleMax(uint32_t newmax,bool reset_counters) {
    if (!inputnet_capture_active) return false;      /* no session: business as usual */
    if (inputnet_cyclesync == INCYCLES_OFF) return false;

    /* Clients never decide the speed, and in "freeze" mode nobody does. */
    if (!inputnet_is_server || inputnet_cyclesync != INCYCLES_AUTO) return true;
    if (newmax == 0 || newmax == inputnet_cyclemax) return true;

    InputNetEvent ev;
    ev.type = INEV_CYCLEMAX;
    ev.cyclemax = newmax;
    ev.pressed = reset_counters ? 1 : 0;
    INPUTNET_QueueLocal(ev);

    /* Remember it right away so that the guessing, which runs far more often
     * than we publish, does not queue the same value over and over. */
    inputnet_cyclemax = newmax;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Desync detection                                                           */
/* ------------------------------------------------------------------------- */

/* A cheap hash of the parts of the emulated machine that would differ first if
 * the participants ever stopped executing the same code: the CPU registers and
 * conventional memory. FNV-1a, computed at a frame boundary so that every
 * participant hashes the exact same emulated moment. */
static uint32_t INPUTNET_MachineHash(void) {
    uint32_t h = 2166136261u;

    const uint8_t *regs = (const uint8_t*)&cpu_regs;
    for (size_t i=0;i < sizeof(cpu_regs);i++) { h ^= regs[i]; h *= 16777619u; }

    const uint8_t *segs = (const uint8_t*)&Segs;
    for (size_t i=0;i < sizeof(Segs);i++) { h ^= segs[i]; h *= 16777619u; }

    if (MemBase != NULL) {
        size_t bytes = (size_t)MEM_TotalPages() * 4096u;
        if (bytes > 640u*1024u) bytes = 640u*1024u;   /* conventional memory is enough */

        /* Word at a time: byte at a time over 640KB would be noticeable. */
        const uint32_t *p32 = (const uint32_t*)MemBase;
        for (size_t i=0;i < bytes/4u;i++) { h ^= p32[i]; h *= 16777619u; }
    }

    return h;
}

static void INPUTNET_StoreCheck(InputNetCheck *ring,uint32_t frame,uint32_t hash) {
    InputNetCheck &c = ring[frame % INPUTNET_CHECK_RING];
    c.frame = frame;
    c.hash = hash;
    c.valid = true;
}

static const InputNetCheck *INPUTNET_FindCheck(const InputNetCheck *ring,uint32_t frame) {
    const InputNetCheck &c = ring[frame % INPUTNET_CHECK_RING];
    if (c.valid && c.frame == frame) return &c;
    return NULL;
}

static void INPUTNET_ReportDesync(const InputNetPeer &p,uint32_t frame) {
    inputnet_stat_desyncs++;
    LOG_MSG("INPUTNET: WARNING - the emulated machine no longer matches participant \"%s\" at frame %u.",
        p.name.empty() ? "(peer)" : p.name.c_str(),(unsigned)frame);
    LOG_MSG("INPUTNET: the sessions have drifted apart; reconnect to pick the state up again.");
    if (inputnet_cyclesync == INCYCLES_OFF)
        LOG_MSG("INPUTNET: \"cycle sync\" is off - with cycles=max or cycles=auto this is expected.");
}

static void INPUTNET_CompareCheck(InputNetPeer &p,uint32_t frame,uint32_t hash) {
    INPUTNET_StoreCheck(p.checks,frame,hash);

    const InputNetCheck *mine = INPUTNET_FindCheck(inputnet_localchecks,frame);
    if (mine != NULL && mine->hash != hash) INPUTNET_ReportDesync(p,frame);
}

/* Hash our own machine for this frame, publish it, and compare against anything
 * the other participants already sent for the same frame. */
static void INPUTNET_RunCheck(uint32_t frame) {
    const uint32_t hash = INPUTNET_MachineHash();
    INPUTNET_StoreCheck(inputnet_localchecks,frame,hash);

    uint8_t buf[32];
    InputNetWriter w(buf,sizeof(buf));
    INPUTNET_BeginPacket(w,INPKT_CHECK);
    w.u32(frame);
    w.u32(hash);
    INPUTNET_SendToPeers(buf,w.size());

    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || p.local || !p.ready) continue;

        const InputNetCheck *theirs = INPUTNET_FindCheck(p.checks,frame);
        if (theirs != NULL && theirs->hash != hash) INPUTNET_ReportDesync(p,frame);
    }
}

/* ------------------------------------------------------------------------- */
/* Input packets                                                              */
/* ------------------------------------------------------------------------- */

static void INPUTNET_WriteEvent(InputNetWriter &w,const InputNetEvent &ev) {
    w.u8(ev.type);
    switch (ev.type) {
        case INEV_KEY:
            w.u16(ev.key);
            w.u8(ev.pressed);
            break;
        case INEV_MOUSEBUTTON:
            w.u8(ev.button);
            w.u8(ev.pressed);
            break;
        case INEV_MOUSEMOVE:
            w.f32(ev.xrel); w.f32(ev.yrel); w.f32(ev.x); w.f32(ev.y);
            w.u8(ev.emulate);
            break;
        case INEV_MOUSEWHEEL:
            w.i16(ev.wheel);
            break;
        case INEV_CYCLEMAX:
            w.u32(ev.cyclemax);
            w.u8(ev.pressed); /* reused: reset the cycle counters as well */
            break;
        default:
            break;
    }
}

static bool INPUTNET_ReadEvent(InputNetReader &r,InputNetEvent &ev) {
    ev = InputNetEvent();
    ev.type = r.u8();
    switch (ev.type) {
        case INEV_KEY:
            ev.key = r.u16();
            ev.pressed = r.u8();
            break;
        case INEV_MOUSEBUTTON:
            ev.button = r.u8();
            ev.pressed = r.u8();
            break;
        case INEV_MOUSEMOVE:
            ev.xrel = r.f32(); ev.yrel = r.f32(); ev.x = r.f32(); ev.y = r.f32();
            ev.emulate = r.u8();
            break;
        case INEV_MOUSEWHEEL:
            ev.wheel = r.i16();
            break;
        case INEV_CYCLEMAX:
            ev.cyclemax = r.u32();
            ev.pressed = r.u8();
            break;
        default:
            return false; /* unknown event: the rest of the packet cannot be parsed */
    }
    return !r.bad();
}

/* Publish our own input up to (current frame + input delay).
 *
 * Every packet repeats the last few frames of input. UDP datagrams do get lost
 * and a lost input frame would stall the whole session until it timed out, so
 * instead of adding retransmit machinery the recent history simply rides along
 * - it is only a handful of bytes. */
static void INPUTNET_SendLocalInput(void) {
    InputNetPeer &self = inputnet_peers[inputnet_selfid];
    const uint32_t confirmed = inputnet_frame + inputnet_delay;

    /* Make sure the slot exists even when nothing was pressed: an empty frame is
     * still information the other side is waiting for. */
    INPUTNET_FrameSlot(self,confirmed);
    self.confirmed = confirmed;
    inputnet_local_confirmed = confirmed;

    /* Decide how many frames of history fit into one datagram, newest first.
     * The newest frame is never dropped - it is the one the other participants
     * are waiting for - and the event cap above guarantees that it fits. */
    uint32_t first = confirmed;
    size_t budget = INPUTNET_MAX_PACKET - 16u; /* header, frame number, block count */

    for (unsigned back=0;back < inputnet_redundancy;back++) {
        if (confirmed < back) break;

        const uint32_t f = confirmed - back;
        InputNetFrame *slot = INPUTNET_FrameSlotIfValid(self,f);
        size_t need = 1;
        if (slot != NULL) need += slot->events.size() * INPUTNET_MAX_EVENT_SIZE;

        if (back != 0 && need > budget) break;
        budget = (need < budget) ? (budget - need) : 0;
        first = f;
    }

    uint8_t buf[INPUTNET_MAX_PACKET];
    InputNetWriter w(buf,sizeof(buf));
    INPUTNET_BeginPacket(w,INPKT_INPUT);
    w.u32(confirmed);
    w.u8((uint8_t)(confirmed - first + 1u));

    for (uint32_t f=first;f <= confirmed;f++) {
        InputNetFrame *slot = INPUTNET_FrameSlotIfValid(self,f);
        const size_t n = (slot != NULL) ? slot->events.size() : 0;
        w.u8((uint8_t)n);
        for (size_t i=0;i < n;i++)
            INPUTNET_WriteEvent(w,slot->events[i]);
    }

    if (w.overflow()) {
        LOG(LOG_MISC,LOG_WARN)("INPUTNET: input packet overflow, dropping history");
        return;
    }

    INPUTNET_SendToPeers(buf,w.size());
}

static void INPUTNET_HandleInputPacket(InputNetPeer &p,InputNetReader &r) {
    const uint32_t confirmed = r.u32();
    const unsigned blocks = r.u8();
    if (r.bad() || blocks == 0) return;

    const uint32_t first = (confirmed >= (blocks-1u)) ? (confirmed - (blocks-1u)) : 0u;

    for (uint32_t f=first;f <= confirmed;f++) {
        const unsigned n = r.u8();
        if (r.bad()) return;

        /* Already applied, or far enough in the future that it cannot be real. */
        const bool stale = (f <= p.delivered);
        const bool absurd = (f > inputnet_frame + INPUTNET_RING/2u);
        const bool have = (INPUTNET_FrameSlotIfValid(p,f) != NULL);

        if (stale || absurd || have) {
            /* Still need to walk past the events to stay in sync with the reader. */
            for (unsigned i=0;i < n;i++) {
                InputNetEvent ev;
                if (!INPUTNET_ReadEvent(r,ev)) return;
            }
            continue;
        }

        InputNetFrame &slot = INPUTNET_FrameSlot(p,f);
        for (unsigned i=0;i < n;i++) {
            InputNetEvent ev;
            if (!INPUTNET_ReadEvent(r,ev)) return;
            slot.events.push_back(ev);
        }
    }

    if (confirmed > p.confirmed) p.confirmed = confirmed;
}

/* ------------------------------------------------------------------------- */
/* Save state hand-off                                                        */
/* ------------------------------------------------------------------------- */

extern bool use_save_file, noremark_save_state, force_load_state;
extern std::string savefilename;
extern std::string capturedir;

static std::string INPUTNET_TempStatePath(void) {
    std::string path;
    bool Get_Custom_SaveDir(std::string& savedir);

    if (Get_Custom_SaveDir(path)) {
        path += CROSS_FILESPLIT;
    }
    else {
        const size_t last_slash_idx = capturedir.find_last_of("\\/");
        path = (std::string::npos != last_slash_idx) ? capturedir.substr(0,last_slash_idx) : std::string(".");
        path += CROSS_FILESPLIT;
        path += "save";
        Cross::CreateDir(path);
        path += CROSS_FILESPLIT;
    }

    path += "inputnet-transfer.sav";
    return path;
}

/* Save state settings are global and driven by the menu; borrow them for the
 * duration of one save/load and put them back afterwards. */
class InputNetSaveStateOverride {
public:
    InputNetSaveStateOverride(const std::string &path,bool force_load) {
        old_use = use_save_file;
        old_name = savefilename;
        old_noremark = noremark_save_state;
        old_force = force_load_state;

        use_save_file = true;
        savefilename = path;
        noremark_save_state = true;
        if (force_load) force_load_state = true;
        old_keep = savestate_keep_local_drives;
        if (force_load) savestate_keep_local_drives = true;
    }
    ~InputNetSaveStateOverride() {
        use_save_file = old_use;
        savefilename = old_name;
        noremark_save_state = old_noremark;
        force_load_state = old_force;
        savestate_keep_local_drives = old_keep;
    }
private:
    bool old_use, old_noremark, old_force, old_keep;
    std::string old_name;
};

static bool INPUTNET_SnapshotState(std::vector<uint8_t> &out) {
    const std::string path = INPUTNET_TempStatePath();

    out.clear();
    remove(path.c_str());

    try {
        InputNetSaveStateOverride ov(path,false);
        SaveState::instance().save(0);
    }
    catch (const SaveState::Error &err) {
        LOG_MSG("INPUTNET: unable to save emulator state: %s",err.c_str());
        return false;
    }

    FILE *fp = fopen(path.c_str(),"rb");
    if (fp == NULL) {
        LOG_MSG("INPUTNET: emulator state was not written to %s",path.c_str());
        return false;
    }

    uint8_t buf[16384];
    size_t n;
    while ((n = fread(buf,1,sizeof(buf),fp)) > 0)
        out.insert(out.end(),buf,buf+n);
    fclose(fp);
    remove(path.c_str());

    if (out.empty()) {
        LOG_MSG("INPUTNET: emulator state is empty");
        return false;
    }

    LOG_MSG("INPUTNET: emulator state snapshot is %lu bytes",(unsigned long)out.size());
    return true;
}

static bool INPUTNET_ApplyStateSnapshot(const std::vector<uint8_t> &in) {
    if (in.empty()) return false;

    const std::string path = INPUTNET_TempStatePath();

    FILE *fp = fopen(path.c_str(),"wb");
    if (fp == NULL) {
        LOG_MSG("INPUTNET: cannot write %s: %s",path.c_str(),strerror(errno));
        return false;
    }
    const size_t written = fwrite(&in[0],1,in.size(),fp);
    fclose(fp);
    if (written != in.size()) {
        LOG_MSG("INPUTNET: short write while storing the received emulator state");
        remove(path.c_str());
        return false;
    }

    bool ok = true;
    if (inputnet_have_pending_vga) {    /* (before the load too: see RB_LoadResyncState) */
        vga_fps = inputnet_pending_vga_fps;
        vga_mode_time_base = inputnet_pending_vga_base;
        vga_mode_frames_since_time_base = (int)inputnet_pending_vga_frames;
    }
    try {
        InputNetSaveStateOverride ov(path,true);
        SaveState::instance().load(0);
    }
    catch (const SaveState::Error &err) {
        LOG_MSG("INPUTNET: unable to load the received emulator state: %s",err.c_str());
        ok = false;
    }

#if defined(USE_TTF)
    if (ok && ttf.inUse) resetFontSize();
#endif

    /* The save state keeps each memory page's handler only when it is one of a short list; the video memory window's
     * others (unmapped, the slow, text and other modes' variants) would stay whatever this machine had before the load,
     * a machine that was in another video mode (a joining computer at its DOS prompt, say) then maps it differently. The
     * VGA state that was loaded says how it is mapped. */
    if (ok) VGA_SetupHandlers();
    if (ok) { extern void RENDER_EndUpdate(bool abort); RENDER_EndUpdate(true); }   /* (the window's frame in progress was another machine's) */
    if (ok) INPUTNET_ClearHostVolumeLabels();
    if (ok && inputnet_have_pending_clock) {
        CMOS_SetSharedClock(inputnet_pending_clock);
        dos.date.year = inputnet_pending_year;
        dos.date.month = inputnet_pending_month;
        dos.date.day = inputnet_pending_day;
        inputnet_have_pending_clock = false;
    }
    if (ok && inputnet_have_pending_tsc) {
        rdtsc_count_base = inputnet_pending_tsc;
        rdtsc_pic_base = PIC_FullIndex();
        inputnet_have_pending_tsc = false;
    }
    if (ok && inputnet_have_pending_vga) {
        vga_fps = inputnet_pending_vga_fps;
        vga_mode_time_base = inputnet_pending_vga_base;
        vga_mode_frames_since_time_base = (int)inputnet_pending_vga_frames;
        inputnet_have_pending_vga = false;
    }
    remove(path.c_str());
    return ok;
}

/* Server side: push the snapshot to one joining client. Emulation is frozen for
 * the duration - the state must not move on before the client has it. */
/* --- RollbackNet: the server's game files ---------------------------------------------------------------------
 * The disk is part of the machine: every file a game reads (its configuration with the player names and controls,
 * high scores, saved games) has to be the same on every computer, or the machines go apart as soon as it is read.
 * So the server's files are used: with the state, the server sends a list of the files on its mounted folders (drive,
 * DOS path, size, hash; FILEMOUNT mappings as the guest sees them). The joining machine compares it with its own,
 * asks for every file that differs or is missing, and maps what it gets over its own (FILEMOUNT /K, from a temporary
 * folder): its own files stay as they are. A file that differs and is over RB_SYNC_FILE_MAX means another version of
 * the game: the join is refused, naming it. Only 8.3 names are compared (what DOS programs use). */

host_cnv_char_t *CodePageGuestToHost(const char *s);     /* dos/drives.cpp */
char *CodePageHostToGuest(const host_cnv_char_t *s);

#define RB_SYNC_FILE_MAX   (16u << 20)
#define RB_SYNC_TOTAL_MAX  (64u << 20)
#define RB_MANIFEST_MAGIC  0x4D464252u  /* "RBFM" */
#define RB_MANIFEST_END    0x45464252u  /* "RBFE", the trailer after the save state */

struct RBFile {
    uint8_t drive = 0;
    std::string path;           /* DOS path without the drive, upper case: "SUB\\FILE.EXT" */
    uint64_t size = 0, hash = 0;
    std::string host;           /* where to read it: a guest code page path, or a mapped file's host path */
    bool mapped = false;
};

static std::vector<RBFile> rb_srv_manifest;         /* server: the list sent to the joining participant */
static std::vector<uint8_t> rb_filereq_bits;        /* server: what it asked for (one bit per manifest entry) */
static std::vector<bool> rb_filereq_part;           /* server: which request packets arrived */
static bool rb_filereq_ready = false;
static std::vector<RBFile> rb_cli_need;             /* client: the server's files it asked for */
static std::vector<std::pair<int,std::string> > rb_sync_mapped;   /* client: (drive, DOS path) mapped for the session */
static std::vector<std::string> rb_sync_temp;       /* client: their temporary copies */
static std::string rb_sync_dir;                     /* client: the temporary folder they are in */
static std::vector<std::pair<int,std::string> > rb_sync_dirs;     /* client: (drive, DOS folder) made for them */

static bool RB_Is83(const char *n) {
    size_t base = 0, ext = 0;
    bool dot = false;
    if (!*n || !strcmp(n, ".") || !strcmp(n, "..")) return false;
    for (const char *c = n; *c; c++) {
        const unsigned char ch = (unsigned char)*c;
        if (ch == '.') { if (dot || base == 0) return false; dot = true; continue; }
        if (ch <= ' ' || ch >= 0x7f || strchr("\"*+,/:;<=>?[\\]|", ch)) return false;
        if (dot) { if (++ext > 3) return false; } else if (++base > 8) return false;
    }
    return true;
}

static FILE *RB_OpenRead(const RBFile &e) {
    if (e.mapped) return fopen(e.host.c_str(), "rb");
    const host_cnv_char_t *h = CodePageGuestToHost(e.host.c_str());
    if (h == NULL) return NULL;
#if defined(host_cnv_use_wchar)
    return _wfopen(h, L"rb");
#else
    return fopen(h, "rb");
#endif
}

/* 64-bit hash of a file: all of it up to 8 MB, else its first and last megabyte (enough to tell versions apart). */
static bool RB_HashFile(const RBFile &e, uint64_t &size, uint64_t &hash) {
    FILE *f = RB_OpenRead(e);
    if (f == NULL) return false;
    fseek(f, 0, SEEK_END);
    const long end = ftell(f);
    if (end < 0) { fclose(f); return false; }
    size = (uint64_t)end;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ size;
    std::vector<uint8_t> buf(1u << 16);
    auto eat = [&](uint64_t from, uint64_t len) {
        fseek(f, (long)from, SEEK_SET);
        while (len > 0) {
            const size_t want = (size_t)std::min<uint64_t>(len, buf.size());
            const size_t got = fread(&buf[0], 1, want, f);
            if (got == 0) break;
            for (size_t i = 0; i < got; i++) h = (h ^ buf[i]) * 0x100000001B3ull;
            len -= got;
        }
    };
    if (size <= (8u << 20)) eat(0, size);
    else { eat(0, 1u << 20); eat(size - (1u << 20), 1u << 20); }
    fclose(f);
    hash = h;
    return true;
}

static void RB_WalkDir(uint8_t drive, const std::string &guestdir, const std::string &dosprefix, int depth, std::vector<RBFile> &out) {
    if (depth > 8 || out.size() > 20000u) return;
    const host_cnv_char_t *hd = CodePageGuestToHost(guestdir.c_str());
    if (hd == NULL) return;
    dir_information *dir = open_directoryw(hd);
    if (dir == NULL) return;
    host_cnv_char_t name[CROSS_LEN * 2], sname[CROSS_LEN * 2];
    bool isdir = false;
    std::vector<std::pair<std::string,bool> > entries;
    for (bool more = read_directory_firstw(dir, name, sname, isdir); more; more = read_directory_nextw(dir, name, sname, isdir)) {
        const char *g = CodePageHostToGuest(name);
        if (g == NULL) continue;
        entries.push_back(std::make_pair(std::string(g), isdir));
    }
    close_directory(dir);
    for (size_t i = 0; i < entries.size(); i++) {
        const std::string &n = entries[i].first;
        if (!RB_Is83(n.c_str())) continue;
        std::string up = n;
        for (size_t k = 0; k < up.size(); k++) up[k] = (char)toupper((unsigned char)up[k]);
        const std::string dos = dosprefix.empty() ? up : dosprefix + "\\" + up;
        const std::string guest = guestdir + n;
        if (entries[i].second) { RB_WalkDir(drive, guest + CROSS_FILESPLIT, dos, depth + 1, out); continue; }
        RBFile e;
        e.drive = drive; e.path = dos; e.host = guest;
        if (RB_HashFile(e, e.size, e.hash)) out.push_back(e);
    }
}

/* The files of every mounted folder, as the guest sees them, sorted. */
static void RB_BuildManifest(std::vector<RBFile> &out) {
    out.clear();
    for (int d = 0; d < DOS_DRIVES; d++) {
        if (Drives[d] == NULL || d == 25) continue;                     /* Z: is DOSBox-X's own */
        localDrive *ld = dynamic_cast<localDrive*>(Drives[d]);
        if (ld == NULL || strncmp(Drives[d]->GetInfo(), "local directory", 15) != 0) continue;
        std::string base = ld->getBasedir();
        if (!base.empty() && base[base.size() - 1] != CROSS_FILESPLIT) base += CROSS_FILESPLIT;
        const size_t first = out.size();
        RB_WalkDir((uint8_t)d, base, "", 0, out);
        for (auto it = ld->GetMountedFiles().begin(); it != ld->GetMountedFiles().end(); ++it) {
            RBFile e;
            e.drive = (uint8_t)d; e.path = it->first; e.host = it->second; e.mapped = true;
            if (!RB_HashFile(e, e.size, e.hash)) continue;      /* a /K file the guest has not created yet */
            bool replaced = false;
            for (size_t k = first; k < out.size(); k++)
                if (out[k].drive == e.drive && out[k].path == e.path) { out[k] = e; replaced = true; break; }
            if (!replaced) out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end(), [](const RBFile &a, const RBFile &b) {
        return a.drive != b.drive ? a.drive < b.drive : a.path < b.path; });
}

static void RB_PutManifest(std::vector<uint8_t> &out, const std::vector<RBFile> &m) {
    auto put = [&out](const void *v, size_t n) { const uint8_t *b = (const uint8_t*)v; out.insert(out.end(), b, b + n); };
    const size_t start = out.size();
    const uint32_t magic = RB_MANIFEST_MAGIC, count = (uint32_t)m.size();
    put(&magic, 4); put(&count, 4);
    for (size_t i = 0; i < m.size(); i++) {
        const uint16_t len = (uint16_t)m[i].path.size();
        put(&m[i].drive, 1); put(&len, 2); put(m[i].path.data(), len); put(&m[i].size, 8); put(&m[i].hash, 8);
    }
    const uint32_t mlen = (uint32_t)(out.size() - start), endmagic = RB_MANIFEST_END;
    put(&mlen, 4); put(&endmagic, 4);
}

/* Splits "blob" (save state + manifest + trailer) into the save state, left in "blob", and the manifest. */
static bool RB_TakeManifest(std::vector<uint8_t> &blob, std::vector<RBFile> &m) {
    m.clear();
    if (blob.size() < 16) return false;
    uint32_t mlen, endmagic;
    memcpy(&mlen, &blob[blob.size() - 8], 4); memcpy(&endmagic, &blob[blob.size() - 4], 4);
    if (endmagic != RB_MANIFEST_END || mlen + 8u > blob.size() || mlen < 8u) return false;
    const size_t start = blob.size() - 8u - mlen;
    size_t pos = start;
    auto get = [&blob, &pos, start, mlen](void *v, size_t n) -> bool {
        if (pos + n > start + mlen) return false;
        memcpy(v, &blob[pos], n); pos += n; return true; };
    uint32_t magic, count;
    if (!get(&magic, 4) || !get(&count, 4) || magic != RB_MANIFEST_MAGIC || count > 100000u) return false;
    for (uint32_t i = 0; i < count; i++) {
        RBFile e; uint16_t len;
        if (!get(&e.drive, 1) || !get(&len, 2) || pos + len > start + mlen) return false;
        e.path.assign((const char*)&blob[pos], len); pos += len;
        if (!get(&e.size, 8) || !get(&e.hash, 8)) return false;
        m.push_back(e);
    }
    blob.resize(start);
    return true;
}

/* Client: which of the server's files it needs (bits), or false with "err" when one cannot be taken over. "mine": our
 * own files (of the same copy, in a cluster). */
static bool RB_CompareManifestsWith(const std::vector<RBFile> &theirs, const std::vector<RBFile> &mine, std::vector<uint8_t> &bits, uint64_t &total, std::string &err) {
    bits.assign((theirs.size() + 7u) / 8u, 0);
    total = 0;
    unsigned differ = 0, missing = 0;
    for (size_t i = 0; i < theirs.size(); i++) {
        const RBFile &t = theirs[i];
        const RBFile *my = NULL;
        for (size_t k = 0; k < mine.size(); k++) if (mine[k].drive == t.drive && mine[k].path == t.path) { my = &mine[k]; break; }
        if (my != NULL && my->size == t.size && my->hash == t.hash) continue;
        if (t.size > RB_SYNC_FILE_MAX) {
            err = std::string(1, (char)('A' + t.drive)) + ":\\" + t.path + " (" + std::to_string((unsigned long long)(t.size >> 10)) +
                " KB) " + (my ? "differs from the server's" : "is missing here") + ": every computer needs the same version of the game";
            return false;
        }
        if (my) differ++; else missing++;
        total += t.size;
        bits[i / 8u] |= (uint8_t)(1u << (i % 8u));
    }
    if (total > RB_SYNC_TOTAL_MAX) { err = "the files that differ from the server's are too large to take over: every computer needs the same version of the game"; return false; }
    unsigned extra = 0;
    for (size_t k = 0; k < mine.size(); k++) {
        bool found = false;
        for (size_t i = 0; i < theirs.size() && !found; i++) found = (theirs[i].drive == mine[k].drive && theirs[i].path == mine[k].path);
        if (!found) extra++;
    }
    LOG_MSG("INPUTNET: the server's files: %u, %u differ here and %u are missing (%llu KB to take over); %u files only here",
        (unsigned)theirs.size(), differ, missing, (unsigned long long)(total >> 10), extra);
    return true;
}

static bool RB_CompareManifests(const std::vector<RBFile> &theirs, std::vector<uint8_t> &bits, uint64_t &total, std::string &err) {
    std::vector<RBFile> mine;
    RB_BuildManifest(mine);
    return RB_CompareManifestsWith(theirs, mine, bits, total, err);
}

static std::vector<uint8_t> rb_cli_bits;
static uint32_t rb_cli_manifest_count = 0;

/* Client: (again) the request for the files it needs. */
static void RB_SendFileRequest(void) {
    const size_t per = 1000u;   /* bytes of bits per packet */
    for (size_t off = 0; off < rb_cli_bits.size() || off == 0; off += per) {
        const size_t n = std::min(per, rb_cli_bits.size() - std::min(off, rb_cli_bits.size()));
        uint8_t buf[1100];
        InputNetWriter w(buf, sizeof(buf));
        INPUTNET_BeginPacket(w, INPKT_FILEREQ);
        w.u32(rb_cli_manifest_count);
        w.u32((uint32_t)off);
        w.u16((uint16_t)n);
        if (n) w.bytes(&rb_cli_bits[off], n);
        INPUTNET_SendRaw(inputnet_server_addr, buf, w.size());
        if (rb_cli_bits.empty()) break;
    }
}

/* Server: a part of a joining participant's file request. */
static void RB_HandleFileRequest(InputNetReader &r) {
    const uint32_t count = r.u32(), off = r.u32();
    const unsigned n = r.u16();
    const uint8_t *b = n ? r.bytes(n) : NULL;
    if (r.bad() || count != rb_srv_manifest.size() || rb_filereq_ready) return;
    const size_t need = (count + 7u) / 8u;
    if (rb_filereq_bits.size() != need) {
        rb_filereq_bits.assign(need, 0);
        rb_filereq_part.assign(std::max<size_t>(1u, (need + 999u) / 1000u), false);
    }
    if ((off % 1000u) != 0 || off > need || n > need - off || (n == 0 && need != 0)) return;
    if (n) memcpy(&rb_filereq_bits[off], b, n);
    rb_filereq_part[off / 1000u] = true;
    bool all = true;
    for (size_t i = 0; i < rb_filereq_part.size(); i++) all = all && rb_filereq_part[i];
    rb_filereq_ready = all;
}

/* Server: the requested files, one after the other in the list's order. */
static bool RB_BuildFileBlob(std::vector<uint8_t> &out) {
    out.clear();
    for (size_t i = 0; i < rb_srv_manifest.size(); i++) {
        if (!(rb_filereq_bits[i / 8u] & (1u << (i % 8u)))) continue;
        const RBFile &e = rb_srv_manifest[i];
        FILE *f = RB_OpenRead(e);
        if (f == NULL) return false;
        const size_t at = out.size();
        out.resize(at + (size_t)e.size);
        const size_t got = e.size ? fread(&out[at], 1, (size_t)e.size, f) : 0;
        fclose(f);
        if (got != e.size) return false;
    }
    return true;
}

/* Client: write the files that came and map each over its DOS path for the session. In a cluster an entry's drive
 * byte carries its copy in bits 5-7: the other copies' files are written and listed in "others" (their followers map
 * them: RBC_CMD_FILEMAP). */
static bool RB_MapReceivedFiles(const std::vector<uint8_t> &blob, std::string &err, std::vector<std::vector<RBFile> > *others = NULL) {
    /* the system's temporary folder (the save folder can be the game's own) */
    std::string dir;
#if defined(WIN32)
    char tmp[MAX_PATH + 1];
    const DWORD tl = GetTempPathA(sizeof(tmp), tmp);
    dir = (tl > 0 && tl < sizeof(tmp)) ? std::string(tmp) : std::string(".\\");
#else
    const char *tmp = getenv("TMPDIR");
    dir = (tmp != NULL && *tmp) ? std::string(tmp) : std::string("/tmp");
#endif
    if (!dir.empty() && dir[dir.size() - 1] != '/' && dir[dir.size() - 1] != '\\') dir += CROSS_FILESPLIT;
    char sub[64];
    snprintf(sub, sizeof(sub), "dosbox-x-nn-rollbacknet-%u", (unsigned)(GetTicks() ^ (uint32_t)(uintptr_t)&blob));
    dir += sub;
    Cross::CreateDir(dir);
    rb_sync_dir = dir;
    size_t pos = 0;
    for (size_t i = 0; i < rb_cli_need.size(); i++) {
        RBFile e = rb_cli_need[i];
        const unsigned copy = e.drive >> 5;
        e.drive &= 31u;
        const bool elsewhere = others != NULL && copy != rbc_copy && copy < others->size();
        localDrive *ld = (e.drive < DOS_DRIVES) ? dynamic_cast<localDrive*>(Drives[e.drive]) : NULL;
        if ((ld == NULL && !elsewhere) || pos + e.size > blob.size()) { err = "the server's files did not arrive whole"; return false; }
        const size_t bs = e.path.find_last_of('\\');
        const std::string name = (bs == std::string::npos) ? e.path : e.path.substr(bs + 1);
        const std::string host = dir + CROSS_FILESPLIT + std::to_string((unsigned long)i) + "-" + name;
        FILE *f = fopen(host.c_str(), "wb");
        if (f == NULL) { err = "cannot write " + host; return false; }
        const size_t put = e.size ? fwrite(&blob[pos], 1, (size_t)e.size, f) : 0;
        fclose(f);
        if (put != e.size) { err = "cannot write " + host; return false; }
        pos += (size_t)e.size;
        rb_sync_temp.push_back(host);
        if (elsewhere) { e.host = host; e.mapped = true; (*others)[copy].push_back(e); continue; }
        /* its folder has to exist for real */
        if (bs != std::string::npos) {
            std::string parent;
            size_t from = 0;
            while (from <= bs) {
                const size_t next = e.path.find('\\', from);
                if (next == std::string::npos || next > bs) break;
                parent = e.path.substr(0, next);
                if (!ld->TestDir(parent.c_str()) && ld->MakeDir(parent.c_str()))
                    rb_sync_dirs.push_back(std::make_pair((int)e.drive, parent));
                from = next + 1;
            }
        }
        if (!ld->AddMountedFile(e.path.c_str(), host.c_str(), true)) { err = "cannot map " + e.path; return false; }
        rb_sync_mapped.push_back(std::make_pair((int)e.drive, e.path));
    }
    return true;
}

/* Client, session over: the server's files are unmapped and their copies deleted. */
static void RB_UnmapReceivedFiles(void) {
    for (size_t i = 0; i < rb_sync_mapped.size(); i++) {
        const int d = rb_sync_mapped[i].first;
        localDrive *ld = (d >= 0 && d < DOS_DRIVES) ? dynamic_cast<localDrive*>(Drives[d]) : NULL;
        if (ld != NULL) ld->RemoveMountedFile(rb_sync_mapped[i].second.c_str());
    }
    rb_sync_mapped.clear();
    for (size_t i = 0; i < rb_sync_temp.size(); i++) remove(rb_sync_temp[i].c_str());
    rb_sync_temp.clear();
    if (!rb_sync_dir.empty()) {
#if defined(WIN32)
        RemoveDirectoryA(rb_sync_dir.c_str());
#else
        rmdir(rb_sync_dir.c_str());
#endif
        rb_sync_dir.clear();
    }
    /* folders made for them, newest first, where still empty */
    for (size_t i = rb_sync_dirs.size(); i-- > 0; ) {
        const int d = rb_sync_dirs[i].first;
        if (d >= 0 && d < DOS_DRIVES && Drives[d] != NULL) Drives[d]->RemoveDir(rb_sync_dirs[i].second.c_str());
    }
    rb_sync_dirs.clear();
}

static uint8_t inputnet_xfer_type = 7;  /* INPKT_STATE or INPKT_FILES: the transfer acknowledgements count for */

static bool INPUTNET_SendStateTo(InputNetPeer &p,const std::vector<uint8_t> &state,uint8_t type = INPKT_STATE) {
    const uint32_t total = (uint32_t)state.size();
    inputnet_xfer_type = type;
    p.confirmed = 0;
    uint32_t acked = 0;
    uint32_t lastprogress = GetTicks();
    uint32_t last_events = GetTicks();

    while (acked < total) {
        if (!p.active) return false; /* the participant went away mid-transfer */

        uint32_t off = acked;
        for (unsigned i=0;i < INPUTNET_STATE_WINDOW && off < total;i++) {
            const uint32_t n = std::min((uint32_t)INPUTNET_STATE_CHUNK,total-off);

            uint8_t buf[INPUTNET_STATE_CHUNK+32u];
            InputNetWriter w(buf,sizeof(buf));
            INPUTNET_BeginPacket(w,type);
            w.u32(total);
            w.u32(off);
            w.u16((uint16_t)n);
            w.bytes(&state[off],n);
            if (!w.overflow()) INPUTNET_SendRaw(p.addr,buf,w.size());

            off += n;
        }

        /* Wait for the client to tell us how far it got. */
        const uint32_t waitstart = GetTicks();
        while ((GetTicks() - waitstart) < 250u) {
            const uint32_t before = acked;
            INPUTNET_PollSocket();
            /* INPUTNET_PollSocket() updates the ack through the shared variable
             * below; re-read it. */
            acked = p.confirmed; /* reused as the state transfer ack while joining */
            if (acked != before) { lastprogress = GetTicks(); break; }
            if (acked >= total) break;
            INPUTNET_Stall(last_events);
        }

        if ((GetTicks() - lastprogress) > inputnet_timeout) {
            LOG_MSG("INPUTNET: timed out transferring the emulator state (%u of %u bytes)",
                (unsigned)acked,(unsigned)total);
            return false;
        }
    }

    return true;
}

/* Client side: collect the snapshot the server is pushing at us. */
static bool INPUTNET_ReceiveState(std::vector<uint8_t> &state,uint32_t total,void (*nudge)(void) = NULL,uint8_t type = INPKT_STATE) {
    if (total == 0) return true;
    uint32_t lastnudge = 0;

    state.assign(total,0);

    const uint32_t nchunks = (total + INPUTNET_STATE_CHUNK - 1u) / INPUTNET_STATE_CHUNK;
    std::vector<bool> got(nchunks,false);
    uint32_t have = 0;              /* bytes received in order */
    uint32_t lastprogress = GetTicks();
    uint32_t lastack = 0;
    uint32_t last_events = GetTicks();

    while (have < total) {
        bool advanced = false;

        for (;;) {
            if (inputnet_socket == NULL) return false;
            if (SDLNet_UDP_Recv(inputnet_socket,inputnet_recvpkt) <= 0) break;

            InputNetReader r((const uint8_t*)inputnet_recvpkt->data,(size_t)inputnet_recvpkt->len);
            if (r.u32() != INPUTNET_MAGIC) continue;
            if (r.u8() != (uint8_t)INPUTNET_PROTOVER) continue;
            const uint8_t ptype = r.u8();
            r.u8(); /* peer id */
            r.u8(); /* reserved */
            if (r.bad() || ptype != type) continue;

            const uint32_t pkttotal = r.u32();
            const uint32_t off = r.u32();
            const uint32_t n = r.u16();
            if (r.bad() || pkttotal != total) continue;
            if (off > total || n > total - off) continue;
            if ((off % INPUTNET_STATE_CHUNK) != 0) continue;

            const uint8_t *data = r.bytes(n);
            if (data == NULL) continue;

            const uint32_t idx = off / INPUTNET_STATE_CHUNK;
            if (idx < nchunks && !got[idx]) {
                memcpy(&state[off],data,n);
                got[idx] = true;
                advanced = true;
            }
        }

        if (advanced) {
            while (have < total && got[have / INPUTNET_STATE_CHUNK])
                have = std::min(total,have + INPUTNET_STATE_CHUNK);
            lastprogress = GetTicks();
        }

        /* Acknowledge often enough that the server keeps the window moving. */
        const uint32_t now = GetTicks();
        if (advanced || (now - lastack) >= 50u) {
            uint8_t buf[32];
            InputNetWriter w(buf,sizeof(buf));
            INPUTNET_BeginPacket(w,INPKT_STATEACK);
            w.u32(have);
            w.u8(type);
            INPUTNET_SendRaw(inputnet_server_addr,buf,w.size());
            lastack = now;
        }

        if (have >= total) break;

        /* until the first bytes come: repeat what made the server send them */
        if (nudge != NULL && have == 0 && (now - lastnudge) >= 300u) { nudge(); lastnudge = now; }

        if ((GetTicks() - lastprogress) > inputnet_timeout) {
            LOG_MSG("INPUTNET: timed out receiving the emulator state (%u of %u bytes)",
                (unsigned)have,(unsigned)total);
            return false;
        }

        INPUTNET_Stall(last_events);
    }

    /* Final acknowledge, repeated a few times because it may be lost. */
    for (unsigned i=0;i < 5;i++) {
        uint8_t buf[32];
        InputNetWriter w(buf,sizeof(buf));
        INPUTNET_BeginPacket(w,INPKT_STATEACK);
        w.u32(total);
        w.u8(type);
        INPUTNET_SendRaw(inputnet_server_addr,buf,w.size());
    }

    return true;
}

/* ------------------------------------------------------------------------- */
/* Session control                                                            */
/* ------------------------------------------------------------------------- */

static void INPUTNET_ResetSessionFrames(void) {
    inputnet_frame = 0;
    inputnet_local_confirmed = 0;
    inputnet_tickaccum = 0;
    inputnet_stat_stallms = 0;
    inputnet_stat_frames = 0;

    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++)
        if (inputnet_peers[i].active) inputnet_peers[i].resetFrames();
}

static void INPUTNET_StartSession(void) {
    INPUTNET_ResetSessionFrames();

    for (unsigned i=0;i < INPUTNET_CHECK_RING;i++) inputnet_localchecks[i].valid = false;
    inputnet_stat_desyncs = 0;
    inputnet_stat_cycleupdates = 0;

    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++)
        if (inputnet_peers[i].active) inputnet_peers[i].ready = true;

    memset(inputnet_keydown,0,sizeof(inputnet_keydown));
    inputnet_buttondown = 0;

    inputnet_session = true;
    inputnet_capture_active = true;

    LOG_MSG("INPUTNET: session running with %u participant(s), %u ms frames, %u frame input delay",
        INPUTNET_PeerCount(),inputnet_frame_ms,inputnet_delay);
    if (inputnet_rollback) RB_SessionStarted();
}

static void INPUTNET_EndSession(const char *reason,bool notify_peers) {
    if (notify_peers && inputnet_link_up) {
        uint8_t buf[16];
        InputNetWriter w(buf,sizeof(buf));
        INPUTNET_BeginPacket(w,INPKT_BYE);
        INPUTNET_SendToPeers(buf,w.size());
    }

    if (inputnet_session) {
        inputnet_session = false;
        inputnet_capture_active = false;
        INPUTNET_ReleaseHeldInput();
    }
    RB_SessionEnded();

    inputnet_capture_active = false;

    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (p.local) { p.ready = false; continue; }
        p.active = false;
        p.ready = false;
        p.resetFrames();
    }

    inputnet_pending_stateload = false;
    inputnet_pending_state.clear();

    if (reason != NULL) LOG_MSG("INPUTNET: session ended (%s)",reason);
}

static void INPUTNET_CloseLink(const char *reason,bool notify_peers) {
    INPUTNET_EndSession(reason,notify_peers);

    if (inputnet_socket != NULL) {
        SDLNet_UDP_Close(inputnet_socket);
        inputnet_socket = NULL;
    }

    inputnet_link_up = false;
    inputnet_is_server = false;
    inputnet_selfid = 0;
    inputnet_epoch = 0;

    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
        inputnet_peers[i].active = false;
        inputnet_peers[i].local = false;
        inputnet_peers[i].ready = false;
        inputnet_peers[i].name.clear();
        inputnet_peers[i].resetFrames();
    }
}

/* ------------------------------------------------------------------------- */
/* Packet dispatch                                                            */
/* ------------------------------------------------------------------------- */

/* Server: a new instance wants to join. Snapshot the machine, hand it over and
 * restart the lockstep with everybody at frame 0. Emulation stands still for
 * the duration, which is exactly what we want: the state must not move while it
 * is being copied. */
static void INPUTNET_ServerAcceptClient(const IPaddress &addr,const std::string &name) {
    if (inputnet_accepting) return;

    /* Not now: the client keeps saying hello and we pick it up a moment later. */
    if (inputnet_in_program) return;

    InputNetPeer *p = INPUTNET_PeerByAddr(addr);

    if (p == NULL) {
        for (unsigned i=1;i < INPUTNET_MAX_PEERS;i++) {
            if (!inputnet_peers[i].active) { p = &inputnet_peers[i]; break; }
        }
        if (p == NULL) {
            LOG_MSG("INPUTNET: no free slot for a new participant");
            return;
        }
        p->active = true;
        p->local = false;
        p->addr = addr;
    }
    else if (p->ready) {
        /* Already taking part: a duplicate HELLO, ignore it. */
        return;
    }

    inputnet_accepting = true;

    const uint8_t peerid = (uint8_t)(p - inputnet_peers);
    p->name = name;
    p->ready = false;
    p->lastheard = GetTicks();
    p->resetFrames();

    LOG_MSG("INPUTNET: participant \"%s\" joining from %d.%d.%d.%d as peer %u",
        name.c_str(),
        (int)( addr.host        & 0xFF),(int)((addr.host >>  8u) & 0xFF),
        (int)((addr.host >> 16u) & 0xFF),(int)((addr.host >> 24u) & 0xFF),
        (unsigned)peerid);

    /* Everybody already running has to restart at frame 0 together with the new
     * participant, so stop the lockstep first. */
    if (inputnet_session) {
        inputnet_session = false;
        inputnet_capture_active = false;
        INPUTNET_ReleaseHeldInput();
    }

    /* From here on the emulated machine stands still until the new participant
     * has taken over our state: it must not move on while it is being copied. */
    std::vector<uint8_t> state;
    if (inputnet_sync_state && !INPUTNET_SnapshotState(state)) {
        LOG_MSG("INPUTNET: state hand-off failed, dropping the new participant");
        p->active = false;
        inputnet_accepting = false;
        if (INPUTNET_PeerCount() > 1) INPUTNET_StartSession();
        return;
    }

    /* Continue from the very bytes the new participant gets. A save state does
     * not round-trip every detail exactly (event times are stored at a lower
     * precision than they run at, and loading cuts the current round of timer
     * tick handlers short), so a machine that kept running from its live state
     * would drift a few CPU cycles away from one that loaded the copy. */
    if (!state.empty() && !INPUTNET_ApplyStateSnapshot(state)) {
        LOG_MSG("INPUTNET: could not reload the emulator state, dropping the new participant");
        p->active = false;
        inputnet_accepting = false;
        if (INPUTNET_PeerCount() > 1) INPUTNET_StartSession();
        return;
    }

    /* RollbackNet: the list of our files goes after the save state (the joining machine takes what differs) */
    rb_filereq_ready = false;
    rb_filereq_bits.clear();
    rb_filereq_part.clear();
    if (inputnet_rollback && !state.empty()) {
        RB_BuildManifest(rb_srv_manifest);
        RB_PutManifest(state, rb_srv_manifest);
        LOG_MSG("INPUTNET: %u files on the mounted folders listed for the joining participant",(unsigned)rb_srv_manifest.size());
    }

    uint8_t welcome[1024];   /* RollbackNet adds its settings fingerprint */
    InputNetWriter w(welcome,sizeof(welcome));
    INPUTNET_BeginPacket(w,INPKT_WELCOME);
    w.u8(peerid);
    w.u16((uint16_t)inputnet_frame_ms);
    w.u16((uint16_t)inputnet_delay);
    w.u16((uint16_t)inputnet_redundancy);
    w.u8(inputnet_share_keyboard ? 1 : 0);
    w.u8(inputnet_share_mouse ? 1 : 0);
    w.u8((uint8_t)inputnet_cyclesync);
    w.u32((uint32_t)state.size());
    /* Lets the client refuse a state it would run differently (see CALLBACK_LayoutSignature). */
    w.u32(CALLBACK_LayoutSignature());
    LOG_MSG("INPUTNET: callback layout %08x",(unsigned int)CALLBACK_LayoutSignature());
    {
        uint8_t clock[16];
        CMOS_GetSharedClock(clock);
        w.bytes(clock,sizeof(clock));
        w.u16(dos.date.year);
        w.u8(dos.date.month);
        w.u8(dos.date.day);
        RDTSC_rebase();
        w.u32((uint32_t)(rdtsc_count_base >> 32u));
        w.u32((uint32_t)rdtsc_count_base);
        const uint64_t fps = INPUTNET_DoubleBits(vga_fps), base = INPUTNET_DoubleBits(vga_mode_time_base);
        w.u32((uint32_t)(fps >> 32u)); w.u32((uint32_t)fps);
        w.u32((uint32_t)(base >> 32u)); w.u32((uint32_t)base);
        w.u32((uint32_t)vga_mode_frames_since_time_base);
    }
    w.u8(inputnet_rollback ? 1 : 0);
    w.u8((uint8_t)inputnet_rbwindow);
    if (inputnet_rollback) {    /* what has to be the same on every computer for the machines to run identically */
        const std::string fp = RB_Fingerprint();
        w.u16((uint16_t)fp.size());
        w.bytes((const uint8_t*)fp.data(), fp.size());
    }
    const size_t welcomelen = w.size();
    INPUTNET_SendRaw(p->addr,welcome,welcomelen);

    /* "confirmed" doubles as the save state transfer acknowledge while joining. */
    p->confirmed = 0;

    bool ok = true;

    if (!state.empty()) {
        /* Repeat the welcome for a moment in case it was lost: without it the
         * client does not know how much state to expect and never acknowledges. */
        uint32_t last_events = GetTicks();
        const uint32_t wstart = GetTicks();
        while (p->confirmed == 0 && (GetTicks() - wstart) < 1000u) {
            INPUTNET_SendRaw(p->addr,welcome,welcomelen);
            const uint32_t round = GetTicks();
            while ((GetTicks() - round) < 100u && p->confirmed == 0) {
                INPUTNET_PollSocket();
                INPUTNET_Stall(last_events);
            }
        }

        ok = INPUTNET_SendStateTo(*p,state);
    }

    /* Wait for the client to report that it applied the state and is ready. */
    if (ok) {
        uint32_t last_events = GetTicks();
        uint32_t lastwelcome = GetTicks();
        const uint32_t waitstart = GetTicks();

        bool files_sent = false;
        while (!p->ready) {
            INPUTNET_PollSocket();
            if (p->ready) break;

            if (rb_filereq_ready && !files_sent) {     /* RollbackNet: the files it asked for */
                files_sent = true;
                std::vector<uint8_t> files;
                if (!RB_BuildFileBlob(files)) {
                    LOG_MSG("INPUTNET: could not read the game files \"%s\" asked for",p->name.c_str());
                    ok = false;
                    break;
                }
                LOG_MSG("INPUTNET: sending %u KB of game files to \"%s\"",(unsigned)(files.size() >> 10),p->name.c_str());
                if (!INPUTNET_SendStateTo(*p,files,INPKT_FILES)) { ok = false; break; }
                continue;
            }

            if ((GetTicks() - waitstart) > inputnet_timeout) {
                LOG_MSG("INPUTNET: participant \"%s\" never became ready",p->name.c_str());
                ok = false;
                break;
            }

            /* A client that never saw the welcome keeps saying hello; repeating
             * the welcome is what gets it moving again. */
            if (state.empty() && (GetTicks() - lastwelcome) >= 250u) {
                INPUTNET_SendRaw(p->addr,welcome,welcomelen);
                lastwelcome = GetTicks();
            }

            INPUTNET_Stall(last_events);
        }
    }

    if (!ok) {
        p->active = false;
        p->ready = false;
        inputnet_accepting = false;
        if (INPUTNET_PeerCount() > 1) INPUTNET_StartSession();
        return;
    }

    /* Everybody starts over at frame 0, with the full list of participants so
     * that nobody races ahead of a peer it has not heard from yet. The start is
     * repeated a few times because a lost one would leave a participant waiting
     * forever; the epoch makes the repeats harmless. */
    {
        inputnet_epoch++;

        /* The save state does not carry the cycle count, so it is sent here:
         * everybody has to start out executing the same amount of guest code
         * per emulated millisecond. */
        inputnet_cyclemax = (uint32_t)CPU_CycleMax;

        uint8_t sbuf[64];
        InputNetWriter sw(sbuf,sizeof(sbuf));
        INPUTNET_BeginPacket(sw,INPKT_START);
        sw.u32(inputnet_epoch);
        sw.u32(inputnet_cyclemax);
        /* "cycles=auto" restores this when a DOS program exits, which the guest
         * triggers at the same emulated moment everywhere - but only produces
         * the same result if the saved value matches too. */
        sw.u32((uint32_t)CPU_OldCycleMax);
        sw.u8((uint8_t)INPUTNET_PeerCount());
        for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++)
            if (inputnet_peers[i].active) sw.u8((uint8_t)i);

        for (unsigned i=0;i < 4;i++) INPUTNET_SendToPeers(sbuf,sw.size());
    }

    inputnet_accepting = false;
    INPUTNET_StartSession();
}

static void INPUTNET_HandlePacket(const IPaddress &from,const uint8_t *data,size_t len) {
    InputNetReader r(data,len);

    if (r.u32() != INPUTNET_MAGIC) return;
    if (r.u8() != (uint8_t)INPUTNET_PROTOVER) return;
    const uint8_t type = r.u8();
    const uint8_t peerid = r.u8();
    r.u8(); /* reserved */
    if (r.bad()) return;

    switch (type) {
        case INPKT_HELLO: {
            if (!inputnet_is_server) return;
            const uint8_t namelen = r.u8();
            std::string name;
            const uint8_t *nb = r.bytes(namelen);
            if (nb != NULL) name.assign((const char*)nb,namelen);
            if (r.bad()) return;
            std::string token;
            if (r.remain() >= 1) { const unsigned tl = r.u8(); const uint8_t *tb = r.bytes(tl); if (tb != NULL) token.assign((const char*)tb,tl); }
            unsigned copies = 0, want = 0;  /* a cluster's computer: its copies, and the one its player plays */
            if (!r.bad() && r.remain() >= 2) { copies = r.u8(); want = r.u8(); }
            {
                /* NukemNet's room token (see REMOTEPLAY_AuthToken): only the room's players may join */
                extern std::string REMOTEPLAY_AuthToken(const std::string &secret,const std::string &name);
                extern bool REMOTEPLAY_SameSecret(const std::string &a,const std::string &b);
                static const char *secret_env = getenv("NN_RP_AUTH_SECRET");
                static const std::string secret = (secret_env != NULL) ? secret_env : "";
                if (secret.size() >= 16) {
                    if (name.empty() || !REMOTEPLAY_SameSecret(token,REMOTEPLAY_AuthToken(secret,name))) {
                        static uint32_t last_told = 0;
                        if ((GetTicks() - last_told) > 5000u) {
                            LOG_MSG("INPUTNET: refused a join as \"%s\": not invited to this room (no valid token)",name.c_str());
                            last_told = GetTicks();
                        }
                        return;
                    }
                }
            }
            /* Only take joins between timer ticks, where the state is saved and
             * reloaded on both sides at the same point; the client says hello
             * again until it is answered. */
            if (rbc_copies) { RBC_ServerHello(from,name,copies,want); break; }
            if (copies != 0) return;        /* a cluster's computer: this session is not one */
            if (!inputnet_in_tick) return;
            INPUTNET_ServerAcceptClient(from,name);
            break;
        }

        case INPKT_STATEACK: {
            if (!inputnet_is_server) return;
            InputNetPeer *p = INPUTNET_PeerByAddr(from);
            if (p == NULL) return;
            const uint32_t ack = r.u32();
            if (r.bad()) return;
            if (r.remain() >= 1 && r.u8() != inputnet_xfer_type) return;    /* the other transfer's, late */
            if (!p->ready && ack > p->confirmed) p->confirmed = ack;
            p->lastheard = GetTicks();
            break;
        }

        case INPKT_FILEREQ: {
            if (!inputnet_is_server || !inputnet_rollback) return;
            InputNetPeer *p = INPUTNET_PeerByAddr(from);
            if (p == NULL || p->ready) return;
            RB_HandleFileRequest(r);
            p->lastheard = GetTicks();
            break;
        }

        case INPKT_READY: {
            if (!inputnet_is_server) return;
            InputNetPeer *p = INPUTNET_PeerByAddr(from);
            if (p == NULL) return;
            p->ready = true;
            p->lastheard = GetTicks();
            break;
        }

        case INPKT_START: {
            if (inputnet_is_server) return;
            if (!INPUTNET_SameAddr(from,inputnet_server_addr)) return;

            const uint32_t epoch = r.u32();
            const uint32_t cyclemax = r.u32();
            const uint32_t oldcyclemax = r.u32();
            const unsigned npeers = r.u8();
            if (r.bad() || npeers == 0 || npeers > INPUTNET_MAX_PEERS) return;

            uint8_t ids[INPUTNET_MAX_PEERS];
            for (unsigned i=0;i < npeers;i++) {
                ids[i] = r.u8();
                if (ids[i] >= INPUTNET_MAX_PEERS) return;
            }
            if (r.bad()) return;

            /* The server repeats the start; only act on the first copy, or the
             * frame numbering would be reset out from under a running session. */
            if (epoch == inputnet_epoch) break;
            inputnet_epoch = epoch;

            if (inputnet_session) {
                inputnet_session = false;
                inputnet_capture_active = false;
                INPUTNET_ReleaseHeldInput();
            }

            for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
                inputnet_peers[i].active = false;
                inputnet_peers[i].ready = false;
                inputnet_peers[i].local = false;
                inputnet_peers[i].resetFrames();
            }
            for (unsigned i=0;i < npeers;i++) {
                InputNetPeer &p = inputnet_peers[ids[i]];
                p.active = true;
                p.local = (ids[i] == inputnet_selfid);
                /* Everything reaches us through the server. */
                if (!p.local) p.addr = inputnet_server_addr;
            }

            /* Adopt the server's emulated CPU speed before the first frame runs. */
            if (cyclemax != 0 && inputnet_cyclesync != INCYCLES_OFF) {
                inputnet_cyclemax = cyclemax;
                if (oldcyclemax != 0) CPU_OldCycleMax = (cpu_cycles_count_t)oldcyclemax;
                DOSBOX_ApplyCycleMax((cpu_cycles_count_t)cyclemax,true);
            }

            /* We must always be in our own roster, or our input would go nowhere. */
            if (inputnet_selfid < INPUTNET_MAX_PEERS) {
                InputNetPeer &self = inputnet_peers[inputnet_selfid];
                self.active = true;
                self.local = true;
            }

            INPUTNET_StartSession();
            break;
        }

        case INPKT_INPUT: {
            if (peerid >= INPUTNET_MAX_PEERS) return;
            if (peerid == inputnet_selfid) return;

            InputNetPeer &p = inputnet_peers[peerid];
            if (!p.active) {
                /* A client learns about the other participants from the packets
                 * the server relays to it. */
                if (inputnet_is_server) return;
                p.active = true;
                p.local = false;
                p.ready = true;
                p.addr = inputnet_server_addr;
                p.resetFrames();
            }
            p.lastheard = GetTicks();
            INPUTNET_HandleInputPacket(p,r);

            /* The server is the hub: pass it on to everybody else. */
            if (inputnet_is_server) {
                InputNetPeer *src = INPUTNET_PeerByAddr(from);
                INPUTNET_SendToPeers(data,len,src);
            }
            break;
        }

        case INPKT_RBINPUT: {
            if (!inputnet_rollback || !inputnet_session) return;
            if (peerid >= INPUTNET_MAX_PEERS || peerid == inputnet_selfid) return;
            InputNetPeer &p = inputnet_peers[peerid];
            if (!p.active) {
                if (inputnet_is_server) return;
                p.active = true;
                p.local = false;
                p.ready = true;
                p.addr = inputnet_server_addr;
                p.resetFrames();
            }
            p.lastheard = GetTicks();
            RB_HandleInput(p,peerid,r);
            if (inputnet_is_server) {   /* the hub: pass it on to the other clients */
                InputNetPeer *src = INPUTNET_PeerByAddr(from);
                INPUTNET_SendToPeers(data,len,src);
            }
            break;
        }

        case INPKT_RESYNC: {
            if (!inputnet_rollback || !inputnet_session) return;
            if (!inputnet_is_server && !INPUTNET_SameAddr(from,inputnet_server_addr)) return;
            RB_HandleResync(r,from,peerid);
            break;
        }

        case INPKT_CHECK: {
            if (peerid >= INPUTNET_MAX_PEERS || peerid == inputnet_selfid) return;
            InputNetPeer &p = inputnet_peers[peerid];
            if (!p.active) return;

            const uint32_t frame = r.u32();
            const uint32_t hash = r.u32();
            if (r.bad()) return;

            INPUTNET_CompareCheck(p,frame,hash);

            /* The server is the hub for these too. */
            if (inputnet_is_server) {
                InputNetPeer *src = INPUTNET_PeerByAddr(from);
                INPUTNET_SendToPeers(data,len,src);
            }
            break;
        }

        case INPKT_PING: {
            const uint32_t token = r.u32();
            if (r.bad()) return;
            uint8_t buf[32];
            InputNetWriter w(buf,sizeof(buf));
            INPUTNET_BeginPacket(w,INPKT_PONG);
            w.u32(token);
            INPUTNET_SendRaw(from,buf,w.size());
            break;
        }

        case INPKT_PONG: {
            const uint32_t token = r.u32();
            if (r.bad()) return;
            InputNetPeer *p = INPUTNET_PeerByAddr(from);
            if (p != NULL) {
                const uint32_t now = GetTicks();
                p->rtt = (now >= token) ? (now - token) : 0;
                p->lastheard = now;
            }
            break;
        }

        case INPKT_BYE: {
            if (inputnet_is_server) {
                InputNetPeer *p = INPUTNET_PeerByAddr(from);
                if (p == NULL) break;
                if (rbc_copies) { RBC_ServerDrop((unsigned)(p - inputnet_peers), "left"); break; }

                LOG_MSG("INPUTNET: participant \"%s\" left",p->name.c_str());
                p->active = false;
                p->ready = false;
                p->resetFrames();

                /* Once somebody drops out the remaining machines may have
                 * applied different amounts of that participant's input, so the
                 * session cannot simply carry on: everybody has to rejoin and
                 * pick the state up again. The server itself stays available. */
                INPUTNET_EndSession("a participant left",true);
            }
            else if (INPUTNET_SameAddr(from,inputnet_server_addr)) {
                INPUTNET_CloseLink("server closed the session",false);
            }
            break;
        }

        default:
            break;
    }
}

static void INPUTNET_PollSocket(void) {
    RBC_PollLeader();
    if (inputnet_socket == NULL || inputnet_recvpkt == NULL) return;

    /* Bound the amount of work per call so a flood cannot lock us up. */
    for (unsigned i=0;i < 256;i++) {
        const int got = SDLNet_UDP_Recv(inputnet_socket,inputnet_recvpkt);
        if (got <= 0) break;
        INPUTNET_HandlePacket(inputnet_recvpkt->address,
            (const uint8_t*)inputnet_recvpkt->data,(size_t)inputnet_recvpkt->len);
    }
}

/* Block until every participant has published its input for "frame". This is
 * the actual lockstep: it is what keeps the emulated machines from drifting
 * apart when one host is faster than the other. */
static bool INPUTNET_WaitForPeers(uint32_t frame) {
    const uint32_t start = GetTicks();
    uint32_t last_events = start;

    for (;;) {
        INPUTNET_PollSocket();

        bool waiting = false;
        for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
            InputNetPeer &p = inputnet_peers[i];
            if (!p.active || !p.ready || p.local) continue;
            if (p.confirmed < frame) { waiting = true; break; }
        }
        if (!waiting) break;

        if (!inputnet_session) return false; /* torn down underneath us */

        if ((GetTicks() - start) > inputnet_timeout) {
            /* The server stays up so that a participant can rejoin; a client has
             * nothing left to talk to and drops the link entirely. */
            if (inputnet_is_server)
                INPUTNET_EndSession("timed out waiting for the other participant(s)",true);
            else
                INPUTNET_CloseLink("timed out waiting for the server",true);
            return false;
        }

        INPUTNET_Stall(last_events);
    }

    inputnet_stat_stallms += GetTicks() - start;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Client side connect                                                        */
/* ------------------------------------------------------------------------- */

static bool INPUTNET_OpenSocket(uint16_t port) {
    if (!SDLNetInited) {
        if (SDLNet_Init() == -1) {
            LOG_MSG("INPUTNET: SDLNet_Init failed: %s",SDLNet_GetError());
            return false;
        }
        SDLNetInited = true;
    }

    /* Must hold the largest packet we send or receive: an input packet may be up
     * to INPUTNET_MAX_PACKET, a save state chunk a little over the chunk size. */
    const int pktsize = (int)std::max((size_t)INPUTNET_MAX_PACKET,(size_t)INPUTNET_STATE_CHUNK+64u);
    if (inputnet_sendpkt == NULL) inputnet_sendpkt = SDLNet_AllocPacket(pktsize);
    if (inputnet_recvpkt == NULL) inputnet_recvpkt = SDLNet_AllocPacket(pktsize);
    if (inputnet_sendpkt == NULL || inputnet_recvpkt == NULL) {
        LOG_MSG("INPUTNET: out of memory allocating packet buffers");
        return false;
    }

    inputnet_socket = SDLNet_UDP_Open(port);
    if (inputnet_socket == NULL) {
        LOG_MSG("INPUTNET: unable to open UDP port %u: %s",(unsigned)port,SDLNet_GetError());
        return false;
    }

    inputnet_active_port = port;
    return true;
}

static bool INPUTNET_StartServer(uint16_t port) {
    if (inputnet_link_up) return false;

    if (!INPUTNET_OpenSocket(port)) return false;

    inputnet_is_server = true;
    inputnet_link_up = true;
    inputnet_selfid = 0;

    InputNetPeer &self = inputnet_peers[0];
    self.active = true;
    self.local = true;
    self.ready = false;
    self.name = "server";
    self.resetFrames();

    INPUTNET_ClearHostVolumeLabels();
    LOG_MSG("INPUTNET: waiting for participants on UDP port %u",(unsigned)port);
    /* a cluster's copies run from now on, nobody else there or not (the others join it: RBC_ServerHello) */
    if (rbc_copies) { rb_start_frame = 0; INPUTNET_StartSession(); }
    return true;
}

static bool INPUTNET_Connect(const char *host,uint16_t port,std::string &errmsg) {
    if (inputnet_link_up) { errmsg = "already connected"; return false; }

    if (!INPUTNET_OpenSocket(0)) { errmsg = "unable to open a UDP socket"; return false; }

    if (SDLNet_ResolveHost(&inputnet_server_addr,host,port) != 0) {
        errmsg = "unable to resolve the server address";
        SDLNet_UDP_Close(inputnet_socket);
        inputnet_socket = NULL;
        return false;
    }

    inputnet_is_server = false;
    inputnet_link_up = true;
    inputnet_selfid = 0xFF; /* until the server assigns one */

    /* Say hello until the server answers. The name and the room token come from NukemNet (as for the remote play
     * viewer): a server started with NN_RP_AUTH_SECRET takes only a name with its token. */
    extern std::string REMOTEPLAY_CleanName(const std::string &name);
    uint8_t buf[160];
    InputNetWriter w(buf,sizeof(buf));
    INPUTNET_BeginPacket(w,INPKT_HELLO);
    std::string me = "client";
    {
        const char *u = getenv("USERNAME");
        if (u == NULL || !*u) u = getenv("USER");
        if (u != NULL && *u) { const std::string clean = REMOTEPLAY_CleanName(u); if (!clean.empty()) me = clean; }
    }
    const char *tok = getenv("NN_RP_AUTH_TOKEN");
    const std::string token = (tok != NULL) ? std::string(tok).substr(0, 64) : std::string();
    w.u8((uint8_t)me.size());
    w.bytes(me.data(),me.size());
    w.u8((uint8_t)token.size());
    if (!token.empty()) w.bytes(token.data(),token.size());
    if (rbc_copies) { w.u8((uint8_t)rbc_copies); w.u8((uint8_t)rbc_copy); }   /* a cluster: the copy our player plays */

    uint32_t total = 0, server_frame = 0, welcomed_at = 0;
    bool welcomed = false;
    uint32_t last_events = GetTicks();
    const uint32_t start = GetTicks();

    while (!welcomed) {
        INPUTNET_SendRaw(inputnet_server_addr,buf,w.size());

        const uint32_t round = GetTicks();
        while (!welcomed && (GetTicks() - round) < 250u) {
            if (SDLNet_UDP_Recv(inputnet_socket,inputnet_recvpkt) > 0) {
                InputNetReader r((const uint8_t*)inputnet_recvpkt->data,(size_t)inputnet_recvpkt->len);
                if (r.u32() == INPUTNET_MAGIC && r.u8() == (uint8_t)INPUTNET_PROTOVER) {
                    const uint8_t type = r.u8();
                    r.u8(); r.u8();
                    if (type == INPKT_WELCOME) {
                        const uint8_t myid = r.u8();
                        const uint16_t fms = r.u16();
                        const uint16_t dly = r.u16();
                        const uint16_t red = r.u16();
                        const uint8_t shk = r.u8();
                        const uint8_t shm = r.u8();
                        const uint8_t csy = r.u8();
                        total = r.u32();
                        /* Older servers do not send the callback layout signature. */
                        if (!r.bad() && r.remain() >= 4) {
                            const uint32_t theirs = r.u32(), ours = CALLBACK_LayoutSignature();
                            if (theirs != ours) {
                                LOG_MSG("INPUTNET: callback layout %08x differs from the server's %08x",
                                    (unsigned int)ours,(unsigned int)theirs);
                                errmsg = "this DOSBox-X build is not compatible with the server's (its callback layout differs); use the same DOSBox-X build on every computer";
                                INPUTNET_CloseLink(NULL,false);
                                return false;
                            }
                            LOG_MSG("INPUTNET: callback layout %08x matches the server",(unsigned int)ours);
                        }
                        if (!r.bad() && r.remain() >= 20) {
                            const uint8_t *clock = r.bytes(16);
                            if (clock != NULL) memcpy(inputnet_pending_clock,clock,16);
                            inputnet_pending_year = r.u16();
                            inputnet_pending_month = r.u8();
                            inputnet_pending_day = r.u8();
                            inputnet_have_pending_clock = (clock != NULL && !r.bad());
                        }
                        inputnet_have_pending_tsc = false;
                        if (!r.bad() && r.remain() >= 8) {
                            const uint32_t hi = r.u32(), lo = r.u32();
                            inputnet_pending_tsc = ((uint64_t)hi << 32u) | lo;
                            inputnet_have_pending_tsc = !r.bad();
                        }
                        inputnet_have_pending_vga = false;
                        if (!r.bad() && r.remain() >= 20) {
                            uint64_t fps = (uint64_t)r.u32() << 32u; fps |= r.u32();
                            uint64_t base = (uint64_t)r.u32() << 32u; base |= r.u32();
                            inputnet_pending_vga_frames = r.u32();
                            inputnet_pending_vga_fps = INPUTNET_BitsDouble(fps);
                            inputnet_pending_vga_base = INPUTNET_BitsDouble(base);
                            inputnet_have_pending_vga = !r.bad();
                        }
                        {
                            /* RollbackNet and the lockstep cannot share a session */
                            uint8_t theirs_rb = 0, win = 0;
                            if (!r.bad() && r.remain() >= 2) { theirs_rb = r.u8(); win = r.u8(); }
                            if (theirs_rb != (inputnet_rollback ? 1 : 0)) {
                                errmsg = theirs_rb ? "the server runs mode=rollback, this DOSBox-X does not"
                                                   : "this DOSBox-X runs mode=rollback, the server does not";
                                LOG_MSG("INPUTNET: %s",errmsg.c_str());
                                INPUTNET_CloseLink(NULL,false);
                                return false;
                            }
                            if (inputnet_rollback && win >= 2 && win <= 48) inputnet_rbwindow = win;
                            if (inputnet_rollback) {
                                std::string theirs;
                                if (!r.bad() && r.remain() >= 2) {
                                    const unsigned n = r.u16();
                                    const uint8_t *b = r.bytes(n);
                                    if (b != NULL) theirs.assign((const char*)b, n);
                                }
                                const std::string mine = RB_Fingerprint();
                                if (theirs != mine) {
                                    errmsg = "RollbackNet needs the same emulator and settings on every computer; different here: " +
                                        RB_FingerprintDiff(theirs, mine);
                                    LOG_MSG("INPUTNET: %s",errmsg.c_str());
                                    INPUTNET_CloseLink(NULL,false);
                                    return false;
                                }
                                if (rbc_copies) {   /* a cluster: the server's runs as many copies */
                                    unsigned theirs_c = 0;
                                    if (!r.bad() && r.remain() >= 1) theirs_c = r.u8();
                                    if (theirs_c != rbc_copies) {
                                        errmsg = "the server runs " + std::to_string(theirs_c) + " copies of the game, this computer " + std::to_string(rbc_copies);
                                        LOG_MSG("INPUTNET: %s",errmsg.c_str());
                                        INPUTNET_CloseLink(NULL,false);
                                        return false;
                                    }
                                    server_frame = r.u32();
                                    rbj_server_prompt = r.u8() != 0;
                                    const unsigned al = r.u16();
                                    const uint8_t *ab = r.bytes(al);
                                    rbj_autorun = (ab != NULL) ? std::string((const char*)ab, al) : std::string();
                                    /* (the machines bring their own clock etc. later: RBCExtras) */
                                    inputnet_have_pending_clock = inputnet_have_pending_tsc = inputnet_have_pending_vga = false;
                                    welcomed_at = GetTicks();
                                }
                            }
                        }
                        if (!r.bad() && myid < INPUTNET_MAX_PEERS) {
                            /* Clamp: these drive our ring buffer and packet sizes. */
                            inputnet_selfid = myid;
                            inputnet_frame_ms = (fms >= 1 && fms <= 100) ? fms : 10u;
                            inputnet_delay = (dly <= 64 && (dly >= 1 || inputnet_rollback)) ? dly : 3u;
                            inputnet_redundancy = (red >= 1 && red <= 32) ? red : 4u;
                            inputnet_share_keyboard = (shk != 0);
                            inputnet_share_mouse = (shm != 0);
                            /* The whole session has to follow one cycle policy. */
                            inputnet_cyclesync = (csy <= INCYCLES_OFF) ? csy : INCYCLES_AUTO;
                            welcomed = true;
                        }
                    }
                }
            }
            if (!welcomed) INPUTNET_Stall(last_events);
        }

        /* Generous: before answering, the server has to snapshot its complete
         * emulator state, which on a large machine takes a few seconds. */
        if (!welcomed && (GetTicks() - start) > 30000u) {
            errmsg = "no answer from the server";
            INPUTNET_CloseLink(NULL,false);
            return false;
        }
    }

    /* Register ourselves and the server as participants. */
    for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
        inputnet_peers[i].active = false;
        inputnet_peers[i].local = false;
        inputnet_peers[i].ready = false;
        inputnet_peers[i].resetFrames();
    }

    InputNetPeer &self = inputnet_peers[inputnet_selfid];
    self.active = true;
    self.local = true;
    self.name = "client";

    InputNetPeer &srv = inputnet_peers[0];
    if (inputnet_selfid != 0) {
        srv.active = true;
        srv.local = false;
        srv.addr = inputnet_server_addr;
        srv.name = "server";
    }

    LOG_MSG("INPUTNET: joined as peer %u, %u ms frames, %u frame input delay",
        (unsigned)inputnet_selfid,inputnet_frame_ms,inputnet_delay);

    if (rbc_copies) {
        if (!RBC_ClientJoin(total, server_frame, welcomed_at, errmsg)) {
            LOG_MSG("INPUTNET: %s", errmsg.c_str());
            INPUTNET_CloseLink(NULL, true);
            return false;
        }
        return true;
    }

    if (total != 0) {
        LOG_MSG("INPUTNET: receiving %u bytes of emulator state from the server",(unsigned)total);
        std::vector<uint8_t> state;
        if (!INPUTNET_ReceiveState(state,total)) {
            errmsg = "the emulator state transfer failed";
            INPUTNET_CloseLink(NULL,true);
            return false;
        }
        if (inputnet_rollback) {
            /* RollbackNet: the server's game files, wherever ours differ */
            std::vector<RBFile> theirs;
            std::vector<uint8_t> bits;
            uint64_t need = 0;
            std::string err;
            if (!RB_TakeManifest(state,theirs)) err = "the server sent no list of its game files";
            else if (RB_CompareManifests(theirs,bits,need,err)) {
                rb_cli_need.clear();
                for (size_t i = 0; i < theirs.size(); i++)
                    if (bits[i / 8u] & (1u << (i % 8u))) rb_cli_need.push_back(theirs[i]);
                rb_cli_bits.swap(bits);
                rb_cli_manifest_count = (uint32_t)theirs.size();
                if (!rb_cli_need.empty()) {
                    std::vector<uint8_t> files;
                    RB_SendFileRequest();
                    if (!INPUTNET_ReceiveState(files,(uint32_t)need,RB_SendFileRequest,INPKT_FILES))
                        err = "the server's game files did not arrive";
                    else if (RB_MapReceivedFiles(files,err))
                        LOG_MSG("INPUTNET: using %u of the server's files in place of ours for this session",(unsigned)rb_cli_need.size());
                }
            }
            if (!err.empty()) {
                errmsg = err;
                LOG_MSG("INPUTNET: %s",errmsg.c_str());
                INPUTNET_CloseLink(NULL,true);
                return false;
            }
        }
        /* The state cannot be applied from here: we are running as a DOS
         * program inside the machine that is about to be replaced. Hand it to
         * the tick handler, which runs between CPU blocks. */
        inputnet_pending_state.swap(state);
        inputnet_pending_stateload = true;
    }
    else {
        inputnet_pending_stateload = true; /* nothing to load, but still announce readiness */
    }

    return true;
}

/* Client side: apply the state we were given (if any), announce readiness and
 * wait for the server to start the lockstep. Runs from the tick handler. */
static void INPUTNET_FinishJoin(void) {
    inputnet_pending_stateload = false;

    if (!inputnet_pending_state.empty()) {
        LOG_MSG("INPUTNET: applying the emulator state received from the server");
        const bool ok = INPUTNET_ApplyStateSnapshot(inputnet_pending_state);
        inputnet_pending_state.clear();
        if (!ok) {
            INPUTNET_CloseLink("could not apply the received emulator state",true);
            return;
        }
    }

    uint8_t buf[32];
    InputNetWriter w(buf,sizeof(buf));
    INPUTNET_BeginPacket(w,INPKT_READY);

    uint32_t last_events = GetTicks();
    const uint32_t start = GetTicks();

    while (!inputnet_session) {
        INPUTNET_SendRaw(inputnet_server_addr,buf,w.size());

        const uint32_t round = GetTicks();
        while (!inputnet_session && (GetTicks() - round) < 200u) {
            INPUTNET_PollSocket();
            if (inputnet_session) break;
            INPUTNET_Stall(last_events);
        }

        if (!inputnet_session && (GetTicks() - start) > inputnet_timeout) {
            INPUTNET_CloseLink("the server never started the session",true);
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Tick handler - the heartbeat of the lockstep                               */
/* ------------------------------------------------------------------------- */

static void INPUTNET_TickHandler(void) {
    if (inputnet_busy) return;
    inputnet_busy = true;
    struct InTick { InTick() { inputnet_in_tick = true; } ~InTick() { inputnet_in_tick = false; } } in_tick;

    if (rbc_copies) {               /* a cluster: the copies find each other at their DOS prompts first */
        extern bool dos_shell_reading_input;
        if (!rbc_attached && dos_shell_reading_input && rbc_copies >= 2) { IPX_ClusterAttach(rbc_copy); rbc_attached = true; }
        if (!rbc_leader) { RBC_FollowerTick(); inputnet_busy = false; return; }
        if (!rbc_ready) { RBC_LeaderGather(); if (!rbc_ready) { inputnet_busy = false; return; } }
    }

    /* Deferred start from the config file or the command line. Wait until the
     * shell sits idle at its prompt: joining swaps in the server's emulator
     * state, which only unwinds safely when this machine is parked in the
     * same quiescent input wait instead of midway through a booting program
     * (e.g. the welcome screen). */
    extern bool dos_shell_reading_input;
    if (!inputnet_autostart_done && dos_shell_reading_input) {
        inputnet_autostart_done = true;

        if (inputnet_auto_server >= 0) {
            INPUTNET_StartServer((uint16_t)inputnet_auto_server);
        }
        else if (!inputnet_auto_connect.empty()) {
            std::string host = inputnet_auto_connect;
            uint16_t port = inputnet_default_port;
            const size_t colon = host.find_last_of(':');
            if (colon != std::string::npos) {
                port = (uint16_t)atoi(host.substr(colon+1).c_str());
                host = host.substr(0,colon);
                if (port == 0) port = inputnet_default_port;
            }

            std::string err;
            if (!INPUTNET_Connect(host.c_str(),port,err))
                LOG_MSG("INPUTNET: auto connect to %s failed: %s",host.c_str(),err.c_str());
        }
    }

    if (!inputnet_link_up) { inputnet_busy = false; return; }

    if (inputnet_pending_stateload) {
        /* Wait until INPUTNET.COM has returned and the shell is parked at its
         * prompt before replacing the machine. */
        if (!inputnet_in_program && dos_shell_reading_input) INPUTNET_FinishJoin();
        inputnet_busy = false;
        return;
    }

    if (!inputnet_session) {
        /* Not running yet: keep servicing the socket so that joins, pings and
         * state transfers make progress. */
        INPUTNET_PollSocket();
        inputnet_busy = false;
        return;
    }

    /* RollbackNet saves, restores and runs its frames from the main loop (INPUTNET_RollbackTick). */
    if (inputnet_rollback) {
        inputnet_busy = false;
        return;
    }

    if (++inputnet_tickaccum < inputnet_frame_ms) {
        INPUTNET_PollSocket();
        inputnet_busy = false;
        return;
    }
    inputnet_tickaccum = 0;

    inputnet_frame++;
    inputnet_stat_frames++;

    INPUTNET_SendLocalInput();

    if (INPUTNET_WaitForPeers(inputnet_frame)) {
        INPUTNET_DeliverFrame(inputnet_frame);

        /* Every participant reaches this point for the same frame having applied
         * exactly the same input, so the machines must look identical here. */
        if (inputnet_checkevery != 0 && (inputnet_frame % inputnet_checkevery) == 0)
            INPUTNET_RunCheck(inputnet_frame);
    }

    inputnet_busy = false;
}

//save state support
void *INPUTNET_TickHandler_PIC_Timer = (void*)((uintptr_t)INPUTNET_TickHandler);

/* ------------------------------------------------------------------------- */
/* RollbackNet ("mode = rollback")                                            */
/* ------------------------------------------------------------------------- */

/* Every participant runs the same machine with everyone's input, like the
 * lockstep, but nobody waits for the others' input. A frame whose input from
 * another participant has not arrived yet runs with a prediction: that player
 * pressed nothing new, so the keys they hold stay held (input is a list of key
 * and button events, and "no events" is exactly that). When their real input
 * for such a frame arrives and does hold events, the machine goes back to the
 * start of that frame and the frames since run again with the real input, at
 * full speed and without queueing their sound, before the next real-time frame
 * starts. Local input is applied after "delay" frames (0: in the next frame),
 * so a player never waits for the network to see their own input.
 *
 * Every frame start is saved: the devices as a lean save state (no guest RAM,
 * no video memory) and, as an undo log, the RAM and video memory pages that
 * changed during the frame before. Pages are found by comparing against a
 * shadow copy rather than by trapping writes, because some emulator code writes
 * guest memory directly. Per-page hashes, updated only for changed pages, give a
 * whole-machine hash for every frame at little cost: the participants compare it
 * for frames whose input everybody has, and a client whose machine differs takes
 * the server's state over (RESYNC), so the machines can never stay apart.
 *
 * Saving, restoring and replaying happen at the start of an emulated
 * millisecond (INPUTNET_RollbackTick, called by the main loop right after
 * TIMER_AddTick): every tick handler has run, the mixer has finished the
 * millisecond before, and no device is half way through anything. */

#define RB_RING 64u                 /* saved frames; more than the largest window */
#define RB_PAGE 4096u
#define RB_RESYNC_CHUNK 1024u

extern void MIXER_SetRollbackDiscard(bool on);

struct RBRegion {
    uint8_t *base = NULL;
    size_t pages = 0;
    std::vector<uint8_t> shadow;    /* the region at the newest saved frame start */
    std::vector<uint64_t> hash;     /* per page hash of shadow */
};

struct RBSnap {
    bool valid = false;
    uint32_t frame = 0;
    uint32_t tick = 0;
    uint64_t context = 0;           /* RB_HostContext() when saved */
    uint64_t hash = 0;              /* the whole machine at this frame start (a cluster's leader: all its copies) */
    uint64_t own = 0;               /* this copy's alone */
    std::string devices;            /* lean save state */
    std::string ipx;                /* a cluster copy's links: IPX driver and null modem (RB_SaveLinks) */
    std::vector<uint32_t> undo_page;/* (region << 28) | page: changed during the frame before this one */
    std::vector<uint64_t> undo_hash;/* their hashes before */
    std::vector<uint8_t> undo_data; /* their contents before, RB_PAGE each */
};

struct RBCheck {
    uint32_t frame = 0;
    uint64_t hash = 0;
    bool valid = false;
};

static RBRegion rb_region[2];       /* 0: guest RAM, 1: video memory */
static uint32_t rb_last_load_frame = UINT32_MAX;    /* the frame of the newest resync state loaded (NN_RB_DUMP=+n) */
bool rb_evlog_on = false;           /* NN_RB_EVLOG: pic.cpp logs every event of the two frames after a resync's load */
static RBSnap rb_snap[RB_RING];
static uint64_t rb_memhash = 0;     /* sum over every page of RB_Mix(key, page hash) */
static uint32_t rb_tick = 0;        /* emulated ms since the session started (restored with the machine) */
static bool rb_have_first = false;
static uint32_t rb_target = 0;      /* the frame a replay ends at */
static uint32_t rb_realframe = 0;   /* the frame real time is at */
static uint32_t rb_predicted[INPUTNET_MAX_PEERS][RB_RING]; /* frame+1 that ran with predicted input from this peer */
static uint32_t rb_have[INPUTNET_MAX_PEERS];      /* frames 0..rb_have-1 of this peer's input are here */
static uint32_t rb_checked[INPUTNET_MAX_PEERS];   /* frames below this were checked against what we predicted */
static uint32_t rb_acked[INPUTNET_MAX_PEERS];     /* frames 0..rb_acked-1 of OUR input this peer has */
static uint32_t rb_peerframe[INPUTNET_MAX_PEERS]; /* the real-time frame the peer last said it was at */
static uint32_t rb_peerframe_at[INPUTNET_MAX_PEERS]; /* host ms when that arrived */
static uint32_t rb_peerstamp[INPUTNET_MAX_PEERS]; /* the peer's send time in its newest packet, echoed back */
static uint32_t rb_peerstamp_at[INPUTNET_MAX_PEERS];
static uint32_t rb_rtt[INPUTNET_MAX_PEERS];
static RBCheck rb_peercheck[INPUTNET_MAX_PEERS][INPUTNET_CHECK_RING];
static double rb_advantage = 0;     /* smoothed: how many frames we run ahead of the others */
static uint32_t rb_last_slowdown = 0;
static bool rb_desynced = false;    /* client: asked for, or receiving, the server's state */
static uint16_t rb_epoch = 0;       /* resyncs loaded: checksums from before one are never compared */
static uint32_t rb_resync_last_sent = 0;
/* RESYNC transfer (client side) */
static std::vector<uint8_t> rb_resync_buf;
static uint32_t rb_resync_id = 0, rb_resync_total = 0, rb_resync_got = 0, rb_resync_frame = 0;
static std::vector<bool> rb_resync_have;

/* statistics, logged every few seconds */
static uint32_t rb_stat_rollbacks = 0, rb_stat_replayed = 0, rb_stat_maxdepth = 0, rb_stat_slowdowns = 0;
static uint32_t rb_stat_stalls = 0, rb_stat_checks = 0, rb_stat_desyncs = 0, rb_stat_resyncs = 0;
static uint64_t rb_stat_capture_us = 0, rb_stat_captures = 0, rb_stat_pages = 0;
static uint64_t rb_replay_started = 0;  /* host us when the running replay began */
static uint64_t rb_stat_scan_us = 0, rb_stat_lean_us = 0;
static uint32_t rb_stat_lockstep = 0;   /* frames run in lockstep since the last log (see RB_UpdateLockstep) */
static uint32_t rb_stat_ctxchanges = 0; /* host context changes seen at frame starts since the last log */
static uint64_t rb_stat_replay_us = 0, rb_stat_replay_max_us = 0, rb_stat_replays = 0;
static uint32_t rb_stat_last_log = 0;

extern bool dos_shell_reading_input;
extern uint32_t dos_shell_input_generation;
extern uint32_t dosbox_runmachine_context;

/* What the host's own stack is doing: the innermost emulation loop that runs (dosbox.cpp) and how often the DOS shell
 * started or stopped reading a command line. The same value at two frame starts means the same host stack, so a
 * machine state saved at the first can be loaded at the second. */
static inline uint64_t RB_HostContext(void) { return ((uint64_t)dos_shell_input_generation << 32u) | dosbox_runmachine_context; }

/* Frames run with everybody's real input (no prediction, so nothing to roll back) where a rollback could not correct a
 * guess: at the DOS prompt, and while no saved frame from the oldest one whose input is still unknown back to the
 * window's start was saved in the host context that runs now (DOS waiting for a key: every wait is a new nested
 * emulation loop; a program that has just started). */
static bool rb_lockstep = false;
static uint64_t rb_lockstep_ctx = 0;
static uint32_t rb_unrestorable = 0;    /* frame starts in a row at which nothing could be rolled back to */
static uint32_t RB_MinHave(void);
static uint32_t RB_RestorableAtOrBefore(uint32_t frame, uint32_t lowest);
static void RB_UpdateLockstep(uint32_t frame) {
    const uint64_t ctx = RB_HostContext();
    if (ctx != rb_lockstep_ctx) { rb_lockstep_ctx = ctx; rb_stat_ctxchanges++; }
    const uint32_t oldest = std::min(RB_MinHave(), frame);
    const uint32_t lowest = (frame > inputnet_rbwindow) ? frame - inputnet_rbwindow : 0;
    if (RB_RestorableAtOrBefore(oldest, lowest) == UINT32_MAX) rb_unrestorable++;
    else rb_unrestorable = 0;
    /* One such frame start alone is a nested loop of a moment (a BIOS call DOS makes for the console): the next one is
     * back where it was, and a correction waits for it (RB_Correct). Two in a row: DOS waits for a key, a program has
     * just started. */
    rb_lockstep = dos_shell_reading_input || rb_unrestorable >= 2;
    if (rb_lockstep) rb_stat_lockstep++;
}

/* Remote frames that ran with a prediction their real input contradicts (RB_FindMisprediction): applied late when no
 * rollback can reach them. */
struct RBLate { unsigned peer; uint32_t frame; };
static std::vector<RBLate> rb_late;
/* The earliest frame a wrong guess was found for and that is not corrected yet, or UINT32_MAX (RB_Correct). */
static uint32_t rb_pending_wrong = UINT32_MAX;

static uint64_t RB_MicroTicks(void) {
#if defined(C_SDL2)
    return (uint64_t)((double)SDL_GetPerformanceCounter() * 1000000.0 / (double)SDL_GetPerformanceFrequency());
#else
    return (uint64_t)SDL_GetTicks() * 1000u;
#endif
}

static inline uint64_t RB_HashPage(const uint8_t *p) {
    uint64_t h1 = 0x9E3779B97F4A7C15ull, h2 = 0xC2B2AE3D27D4EB4Full;
    for (size_t i = 0; i < RB_PAGE; i += 16) {
        uint64_t a, b;
        memcpy(&a, p + i, 8);
        memcpy(&b, p + i + 8, 8);
        h1 = (h1 ^ a) * 0xff51afd7ed558ccdull; h1 ^= h1 >> 29;
        h2 = (h2 ^ b) * 0xc4ceb9fe1a85ec53ull; h2 ^= h2 >> 31;
    }
    return h1 ^ (h2 * 0x9E3779B97F4A7C15ull);
}

static inline uint64_t RB_Mix(uint32_t key, uint64_t h) {
    uint64_t x = h ^ ((uint64_t)key * 0x9E3779B97F4A7C15ull);
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

static size_t RB_RegionPages(unsigned r) {
    if (r == 0) return (MemBase != NULL) ? (size_t)MEM_TotalPages() : 0u;
    return (vga.mem.linear != NULL) ? (size_t)(vga.mem.memsize / RB_PAGE) : 0u;
}

static uint8_t *RB_RegionBase(unsigned r) {
    return (r == 0) ? MemBase : vga.mem.linear;
}

/* --- Guest RAM write tracking -----------------------------------------------------------------------------------
 * Comparing all of guest RAM with its copy at every frame start took most of a frame's saving time (16 MB: about
 * 1.2 ms of 1.4). Instead the host's page protection tells which pages were written: guest RAM is write-protected,
 * the first write to a page faults, and the handler makes the page writable again and notes it. Written pages stay
 * writable and are compared at every frame start; one unchanged for RBT_IDLE frame starts is protected again. Every
 * kind of write is caught: the CPU's, DMA's, the DOS kernel's and BIOS's (host code), loading devices. Only system
 * calls must not write into guest RAM while it is protected (they would fail instead of faulting): none do once the
 * machine has started (ROM images are read at boot). Video memory (a megabyte or so) is still compared whole.
 * Off, so that everything is compared, where the host cannot do it (guest RAM not aligned to host pages) or with
 * NN_RB_NOTRACK=1. */
static const uint16_t RBT_IDLE = 8;
static bool rbt_on = false;
static uint8_t *rbt_lo = NULL, *rbt_hi = NULL;   /* the protected part of guest RAM: whole host pages */
static size_t rbt_hp = 4096, rbt_n = 0;          /* host page size; host pages in [rbt_lo, rbt_hi) */
static volatile uint8_t *rbt_state = NULL;       /* per host page: 0 write-protected, 1 writable */
static uint32_t *rbt_fault = NULL;               /* host pages made writable since they were last collected */
static volatile size_t rbt_nfault = 0;
static std::vector<uint32_t> rbt_work;           /* writable host pages: compared at every frame start */
static std::vector<uint16_t> rbt_idle;           /* frame starts a writable host page has been unchanged */

static bool RBT_SetWritable(size_t p, size_t count, bool writable) {
#if defined(WIN32)
    DWORD old;
    return VirtualProtect(rbt_lo + p * rbt_hp, count * rbt_hp, writable ? PAGE_READWRITE : PAGE_READONLY, &old) != 0;
#else
    return mprotect(rbt_lo + p * rbt_hp, count * rbt_hp, writable ? (PROT_READ | PROT_WRITE) : PROT_READ) == 0;
#endif
}

/* A write fault at "addr" (in the fault handler): true when it was one of ours and the page is writable now. */
static bool RBT_Fault(void *addr) {
    uint8_t *a = (uint8_t*)addr;
    if (!rbt_on || a < rbt_lo || a >= rbt_hi) return false;
    const size_t p = (size_t)(a - rbt_lo) / rbt_hp;
    if (rbt_state[p] != 0 || !RBT_SetWritable(p, 1, true)) return false;
    rbt_state[p] = 1;
    const size_t k = rbt_nfault;
    rbt_fault[k] = (uint32_t)p;
    rbt_nfault = k + 1u;
    return true;
}

#if defined(WIN32)
static LONG CALLBACK RBT_Exception(PEXCEPTION_POINTERS ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2 && er->ExceptionInformation[0] == 1 &&
        RBT_Fault((void*)er->ExceptionInformation[1]))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static struct sigaction rbt_old_segv, rbt_old_bus;
static void RBT_Signal(int sig, siginfo_t *si, void *uc) {
    if (RBT_Fault(si->si_addr)) return;
    /* not ours: whoever had the signal before, or the default action (the fault repeats once we return) */
    const struct sigaction &old = (sig == SIGBUS) ? rbt_old_bus : rbt_old_segv;
    if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction != NULL) { old.sa_sigaction(sig, si, uc); return; }
    if (!(old.sa_flags & SA_SIGINFO) && old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN && old.sa_handler != NULL) {
        old.sa_handler(sig);
        return;
    }
#if defined(__linux__) && defined(__GLIBC__)
    {   /* a real crash: where (to the console log, which NukemNet keeps) */
        static bool told = false;
        if (!told) {
            told = true;
            void *bt[64];
            const int n = backtrace(bt, 64);
            static const char m[] = "\n*** DOSBox-X-NN crashed (segmentation fault); backtrace:\n";
            if (write(2, m, sizeof(m) - 1) < 0) { /* nothing to do */ }
            backtrace_symbols_fd(bt, n, 2);
        }
    }
#endif
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
}
#endif

static bool RBT_InstallHandler(void) {
    static bool installed = false, ok = false;
    if (installed) return ok;
    installed = true;
#if defined(WIN32)
    ok = AddVectoredExceptionHandler(1, RBT_Exception) != NULL;
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = RBT_Signal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    ok = sigaction(SIGSEGV, &sa, &rbt_old_segv) == 0 && sigaction(SIGBUS, &sa, &rbt_old_bus) == 0;   /* macOS: SIGBUS */
#endif
    return ok;
}

/* Everything writable again, tracking off (before a whole machine is loaded, and at the end of a session). */
static void RBT_Stop(void) {
    if (rbt_on) {
        rbt_on = false;
        RBT_SetWritable(0, rbt_n, true);
    }
    rbt_work.clear();
    rbt_nfault = 0;
}

/* Guest RAM equals its copy now (RB_InitRegions): protect all of it. */
static void RBT_Start(uint8_t *base, size_t bytes) {
    RBT_Stop();
    const char *off = getenv("NN_RB_NOTRACK");
    if ((off != NULL && *off) || base == NULL || bytes == 0) return;
#if defined(WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    rbt_hp = si.dwPageSize;
#else
    rbt_hp = (size_t)sysconf(_SC_PAGESIZE);
#endif
    if (rbt_hp == 0 || (rbt_hp % RB_PAGE) != 0 || ((uintptr_t)base % rbt_hp) != 0) {
        static bool told = false;
        if (!told) LOG_MSG("INPUTNET: guest RAM is not aligned to host pages; every page is compared at every frame start");
        told = true;
        return;
    }
    const size_t n = bytes / rbt_hp;
    if (n == 0 || !RBT_InstallHandler()) return;
    if (n != rbt_n || rbt_state == NULL) {
        free((void*)rbt_state); free(rbt_fault);
        rbt_state = (volatile uint8_t*)calloc(n, 1);
        rbt_fault = (uint32_t*)calloc(n, sizeof(uint32_t));
        if (rbt_state == NULL || rbt_fault == NULL) { rbt_n = 0; return; }
    }
    rbt_n = n;
    rbt_lo = base;
    rbt_hi = base + n * rbt_hp;
    memset((void*)rbt_state, 0, n);
    rbt_idle.assign(n, 0);
    rbt_work.clear();
    rbt_work.reserve(n);
    rbt_nfault = 0;
    rbt_on = true;
    if (!RBT_SetWritable(0, n, false)) {
        rbt_on = false;
        LOG_MSG("INPUTNET: cannot write-protect guest RAM; every page is compared at every frame start");
    }
}

/* Pages that became writable since the last call join the compared ones. */
static void RBT_Collect(void) {
    const size_t nf = rbt_nfault;
    for (size_t k = 0; k < nf; k++) {
        rbt_work.push_back(rbt_fault[k]);
        rbt_idle[rbt_fault[k]] = 0;
    }
    rbt_nfault = 0;
}

/* A guest RAM page we are about to write ourselves (restoring): writable now, and compared from now on. */
static inline void RBT_Touch(size_t pg) {
    if (!rbt_on) return;
    const size_t p = pg * RB_PAGE / rbt_hp;
    if (p >= rbt_n || rbt_state[p] != 0 || !RBT_SetWritable(p, 1, true)) return;
    rbt_state[p] = 1;
    const size_t k = rbt_nfault;
    rbt_fault[k] = (uint32_t)p;
    rbt_nfault = k + 1u;
}

static void RB_InitRegions(void) {
    rb_memhash = 0;
    for (unsigned r = 0; r < 2; r++) {
        RBRegion &g = rb_region[r];
        g.base = RB_RegionBase(r);
        g.pages = RB_RegionPages(r);
        if (g.pages == 0) { g.shadow.clear(); g.hash.clear(); continue; }
        g.shadow.assign(g.base, g.base + g.pages * RB_PAGE);
        g.hash.resize(g.pages);
        for (size_t pg = 0; pg < g.pages; pg++) {
            g.hash[pg] = RB_HashPage(g.base + pg * RB_PAGE);
            rb_memhash += RB_Mix(((uint32_t)r << 28) | (uint32_t)pg, g.hash[pg]);
        }
    }
    for (unsigned i = 0; i < RB_RING; i++) rb_snap[i].valid = false;
    RBT_Start(rb_region[0].base, rb_region[0].pages * RB_PAGE);
}

static bool RB_RegionsMoved(void) {
    for (unsigned r = 0; r < 2; r++)
        if (rb_region[r].base != RB_RegionBase(r) || rb_region[r].pages != RB_RegionPages(r)) return true;
    return false;
}

/* The registers are hashed field by field: the structures hold host-sized fields and padding. */
static uint64_t RB_MachineHash(void) {
    uint64_t f = 1469598103934665603ull;
    auto add = [&f](uint32_t v) { for (unsigned b = 0; b < 4; b++) { f ^= (v >> (8u * b)) & 0xFFu; f *= 1099511628211ull; } };
    for (unsigned i = 0; i < 8; i++) add(cpu_regs.regs[i].dword[0]);
    add(cpu_regs.ip.dword[0]);
    add((uint32_t)cpu_regs.flags);
    for (unsigned i = 0; i < 6; i++) {
        add((uint32_t)Segs.val[i]);
        add((uint32_t)Segs.phys[i]);
        add((uint32_t)Segs.limit[i]);
        add(Segs.expanddown[i] ? 1u : 0u);
    }
    return rb_memhash ^ RB_Mix(0x7FFFFFFFu, f);
}

/* A cluster copy's links to the others as a saved frame holds them: its IPX driver and its RollbackNet null modem. */
extern void IPX_SaveLean(std::string &out);
extern bool IPX_LoadLean(const uint8_t *&p, const uint8_t *end);
extern bool SERIAL_RBPresent(void);
extern void SERIAL_RBTakeOutbox(std::vector<uint8_t> &bytes, uint8_t &lines);
extern void SERIAL_RBDeliver(const uint8_t *data, size_t n, uint8_t lines);
extern void SERIAL_RBSave(std::string &out);
extern bool SERIAL_RBLoad(const uint8_t *&p, const uint8_t *end);

static void RB_SaveLinks(std::string &out) {
    std::string a, b;
    IPX_SaveLean(a);
    SERIAL_RBSave(b);
    out.clear();
    if (a.empty() && b.empty()) return;
    const uint32_t la = (uint32_t)a.size(), lb = (uint32_t)b.size();
    out.append((const char*)&la, 4); out.append(a);
    out.append((const char*)&lb, 4); out.append(b);
}

static bool RB_LoadLinks(const uint8_t *p, size_t n) {
    if (n == 0) return true;
    const uint8_t *end = p + n;
    uint32_t la = 0, lb = 0;
    if (n < 4) return false;
    memcpy(&la, p, 4); p += 4;
    if ((size_t)(end - p) < la) return false;
    const uint8_t *a = p; p += la;
    if ((size_t)(end - p) < 4) return false;
    memcpy(&lb, p, 4); p += 4;
    if ((size_t)(end - p) < lb) return false;
    const uint8_t *b = p;
    bool ok = true;
    if (la) { const uint8_t *q = a; ok = IPX_LoadLean(q, a + la) && ok; }
    if (lb) { const uint8_t *q = b; ok = SERIAL_RBLoad(q, b + lb) && ok; }
    return ok;
}

/* Save the start of "frame". */
/* One page at a frame start: when it differs from its copy of the last frame start, the copy goes into the frame's
 * undo log and the page's hash is updated. Returns whether it differed. */
static inline bool RB_DiffPage(RBSnap &s, unsigned r, size_t pg) {
    RBRegion &g = rb_region[r];
    const uint8_t *cur = g.base + pg * RB_PAGE;
    uint8_t *sh = &g.shadow[pg * RB_PAGE];
    if (memcmp(cur, sh, RB_PAGE) == 0) return false;
    const uint32_t key = ((uint32_t)r << 28) | (uint32_t)pg;
    s.undo_page.push_back(key);
    s.undo_hash.push_back(g.hash[pg]);
    s.undo_data.insert(s.undo_data.end(), sh, sh + RB_PAGE);
    memcpy(sh, cur, RB_PAGE);
    const uint64_t nh = RB_HashPage(cur);
    rb_memhash += RB_Mix(key, nh) - RB_Mix(key, g.hash[pg]);
    g.hash[pg] = nh;
    return true;
}

/* Guest RAM with write tracking: only the writable pages can differ. */
static void RBT_DiffRAM(RBSnap &s) {
    RBT_Collect();
    const size_t per = rbt_hp / RB_PAGE;
    size_t keep = 0;
    for (size_t w = 0; w < rbt_work.size(); w++) {
        const uint32_t p = rbt_work[w];
        bool changed = false;
        for (size_t q = 0; q < per; q++) changed |= RB_DiffPage(s, 0, (size_t)p * per + q);
        if (changed) rbt_idle[p] = 0;
        else if (++rbt_idle[p] >= RBT_IDLE && RBT_SetWritable(p, 1, false)) { rbt_state[p] = 0; continue; }
        rbt_work[keep++] = p;
    }
    rbt_work.resize(keep);
    for (size_t pg = rbt_n * per; pg < rb_region[0].pages; pg++) RB_DiffPage(s, 0, pg);   /* past the last whole host page */
}

static void RB_Capture(uint32_t frame) {
    const uint64_t t0 = RB_MicroTicks();
    if (rb_evlog_on && rb_last_load_frame != UINT32_MAX) {     /* NN_RB_EVLOG=<frames> (2 by default) */
        static const char *ev = getenv("NN_RB_EVLOG");
        const uint32_t n = (ev != NULL && atoi(ev) > 0) ? (uint32_t)atoi(ev) : 2u;
        if (frame >= rb_last_load_frame + n) rb_evlog_on = false;
    }
    if (RB_RegionsMoved()) RB_InitRegions();    /* video memory reallocated etc.: older frames are gone */

    RBSnap &s = rb_snap[frame % RB_RING];
    s.valid = true;
    s.frame = frame;
    s.tick = rb_tick;
    s.context = RB_HostContext();
    s.undo_page.clear();
    s.undo_hash.clear();
    s.undo_data.clear();

    if (rbt_on) RBT_DiffRAM(s);
    for (unsigned r = rbt_on ? 1u : 0u; r < 2; r++) {
        RBRegion &g = rb_region[r];
        for (size_t pg = 0; pg < g.pages; pg++) RB_DiffPage(s, r, pg);
    }

    const uint64_t t1 = RB_MicroTicks();
    SaveState::instance().saveLean(s.devices);
    if (rbc_copies) RB_SaveLinks(s.ipx);
    const uint64_t t2 = RB_MicroTicks();
    rb_stat_scan_us += t1 - t0;
    rb_stat_lean_us += t2 - t1;
    s.own = s.hash = RB_MachineHash();

    /* debugging desyncs: NN_RB_TRACE=1 logs every frame start (compare two participants' logs line by line) */
    static const bool trace = getenv("NN_RB_TRACE") != NULL && *getenv("NN_RB_TRACE");
    if (trace) {
        extern void SBLASTER_DebugState(char *out, size_t n);
        char sbs[200];
        SBLASTER_DebugState(sbs, sizeof(sbs));
        LOG_MSG("RBSB f=%u %s%s", (unsigned)frame, sbs, inputnet_rollback_resim ? " replay" : "");
        uint64_t devh = 1469598103934665603ull;
        LOG_MSG("RBTRACE f=%u t=%u ctx=%u pic=%u cs:eip=%04x:%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esp=%08x fl=%08x mem=%08x%08x h=%08x%08x%s",
            (unsigned)frame,(unsigned)rb_tick,(unsigned)(s.context & 0xFFFFFFFFu),(unsigned)PIC_Ticks,(unsigned)Segs.val[cs],(unsigned)cpu_regs.ip.dword[0],
            (unsigned)cpu_regs.regs[0].dword[0],(unsigned)cpu_regs.regs[3].dword[0],(unsigned)cpu_regs.regs[1].dword[0],(unsigned)cpu_regs.regs[2].dword[0],
            (unsigned)cpu_regs.regs[4].dword[0],(unsigned)cpu_regs.flags,(unsigned)(rb_memhash >> 32u),(unsigned)rb_memhash,
            (unsigned)(s.hash >> 32u),(unsigned)s.hash,inputnet_rollback_resim ? " replay" : "");
        (void)devh;
    }

    /* debugging desyncs: NN_RB_DUMP=<frame> writes that frame start (RAM, video memory, devices) to the working dir;
     * NN_RB_DUMP=+<n>: the n-th frame after the newest resync's frame (each load of a resync's state) */
    static const char *dump_env = getenv("NN_RB_DUMP");
    if (dump_env != NULL && *dump_env && (*dump_env == '+' ? (rb_last_load_frame != UINT32_MAX && rb_last_load_frame + (uint32_t)strtoul(dump_env + 1, NULL, 10) == frame)
                                                          : (uint32_t)strtoul(dump_env, NULL, 10) == frame)) {
        char name[64];
        for (unsigned r = 0; r < 2; r++) {
            snprintf(name, sizeof(name), "rbdump-%u-%s.bin", (unsigned)frame, r ? "vram" : "ram");
            FILE *f = fopen(name, "wb");
            if (f) { fwrite(rb_region[r].base, 1, rb_region[r].pages * RB_PAGE, f); fclose(f); }
        }
        snprintf(name, sizeof(name), "rbdump-%u-devices.bin", (unsigned)frame);
        FILE *f = fopen(name, "wb");
        if (f) { fwrite(s.devices.data(), 1, s.devices.size(), f); fclose(f); }
        LOG_MSG("INPUTNET: dumped frame %u (hash %08x%08x)", (unsigned)frame, (unsigned)(s.hash >> 32u), (unsigned)s.hash);
    }

    /* debugging: NN_RB_FULLDUMP=+<n>[,+<m>...] also writes the portable save state of the frames that many after the newest
     * resync's (compare the computers' files component by component) */
    static const char *fulldump_env = getenv("NN_RB_FULLDUMP");
    if (fulldump_env != NULL && *fulldump_env && rb_last_load_frame != UINT32_MAX) {
        for (const char *q = fulldump_env; q != NULL && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : NULL) {
            if (*q != '+' || rb_last_load_frame + (uint32_t)strtoul(q + 1, NULL, 10) != frame) continue;
            std::vector<uint8_t> st;
            if (INPUTNET_SnapshotState(st)) {
                char name[64];
                snprintf(name, sizeof(name), "rbfull-%u.sav", (unsigned)frame);
                FILE *f = fopen(name, "wb");
                if (f) { fwrite(&st[0], 1, st.size(), f); fclose(f); }
            }
        }
    }

    rb_stat_capture_us += RB_MicroTicks() - t0;
    rb_stat_captures++;
    rb_stat_pages += s.undo_page.size();
}

/* Put the machine back to the start of "target"; "newest" is the newest saved frame (saved just now). */
static bool RB_Restore(uint32_t target, uint32_t newest) {
    for (uint32_t f = target; f <= newest; f++) {
        const RBSnap &s = rb_snap[f % RB_RING];
        if (!s.valid || s.frame != f) return false;
    }
    /* The frame we load has to have been saved with the host's stack as it is now (the frames in between only give
     * back memory pages; the replay makes their nested loops again). */
    if (rb_snap[target % RB_RING].context != RB_HostContext()) {
        LOG_MSG("INPUTNET: frame %u was saved in another host context",(unsigned)target);
        return false;
    }

    for (uint32_t f = newest; f > target; f--) {
        RBSnap &s = rb_snap[f % RB_RING];
        for (size_t k = 0; k < s.undo_page.size(); k++) {
            const uint32_t key = s.undo_page[k];
            RBRegion &g = rb_region[key >> 28];
            const size_t pg = key & 0x0FFFFFFFu;
            const uint8_t *old = &s.undo_data[k * RB_PAGE];
            if ((key >> 28) == 0) RBT_Touch(pg);
            memcpy(g.base + pg * RB_PAGE, old, RB_PAGE);
            memcpy(&g.shadow[pg * RB_PAGE], old, RB_PAGE);
            rb_memhash += RB_Mix(key, s.undo_hash[k]) - RB_Mix(key, g.hash[pg]);
            g.hash[pg] = s.undo_hash[k];
        }
        s.valid = false;
    }

    if (!SaveState::instance().loadLean(rb_snap[target % RB_RING].devices)) {
        LOG_MSG("INPUTNET: rollback could not load the saved devices of frame %u",(unsigned)target);
        return false;
    }
    VGA_SetupHandlers();            /* (the video memory window's handlers: see INPUTNET_ApplyStateSnapshot) */
    {   /* the window's frame in progress belongs to the frames gone: the next one starts over (no more lines than it has) */
        extern void RENDER_EndUpdate(bool abort);
        RENDER_EndUpdate(true);
    }
    if (rbc_copies) {
        const std::string &ln = rb_snap[target % RB_RING].ipx;
        if (!RB_LoadLinks((const uint8_t*)ln.data(), ln.size())) LOG_MSG("INPUTNET: rollback could not load the links of frame %u",(unsigned)target);
    }
    /* Loading devices may touch guest memory (video modes, fonts): make it exactly the saved frame again. With write
     * tracking only writable pages can differ. */
    auto put_back = [](unsigned r, size_t pg) {
        RBRegion &g = rb_region[r];
        uint8_t *cur = g.base + pg * RB_PAGE;
        const uint8_t *sh = &g.shadow[pg * RB_PAGE];
        if (memcmp(cur, sh, RB_PAGE) != 0) memcpy(cur, sh, RB_PAGE);
    };
    if (rbt_on) {
        RBT_Collect();
        const size_t per = rbt_hp / RB_PAGE;
        for (size_t w = 0; w < rbt_work.size(); w++)
            for (size_t q = 0; q < per; q++) put_back(0, (size_t)rbt_work[w] * per + q);
        for (size_t pg = rbt_n * per; pg < rb_region[0].pages; pg++) put_back(0, pg);
    }
    for (unsigned r = rbt_on ? 1u : 0u; r < 2; r++)
        for (size_t pg = 0; pg < rb_region[r].pages; pg++) put_back(r, pg);

    rb_tick = rb_snap[target % RB_RING].tick;
    PAGING_ClearTLB();

    /* self-test: loading and saving again must give the same bytes, or a component does not restore exactly */
    if (getenv("NN_RB_SELFTEST") != NULL && *getenv("NN_RB_SELFTEST")) {
        static const char *names[] = { "CMOS","CPU","DMA","Dos","EMS","FPU","IO handler","Int10","IntTimer10","Joystick",
            "Keyboard","Memory","Mixer","Mouse","Pic","Vga","XMS" };
        std::string again;
        SaveState::instance().saveLean(again);
        const std::string &x = rb_snap[target % RB_RING].devices;
        size_t px = 0, py = 0; unsigned idx = 0;
        while (px + 4 <= x.size() && py + 4 <= again.size()) {
            uint32_t lx, ly; memcpy(&lx, x.data() + px, 4); memcpy(&ly, again.data() + py, 4);
            px += 4; py += 4;
            if (lx != ly || memcmp(x.data() + px, again.data() + py, lx) != 0) {
                size_t first = 0;
                while (first < lx && first < ly && x[px + first] == again[py + first]) first++;
                LOG_MSG("RBSELFTEST round trip: %s differs after load+save (%u/%u bytes), first at byte %u: %02x%02x%02x%02x -> %02x%02x%02x%02x",
                    idx < 17 ? names[idx] : "?", (unsigned)lx, (unsigned)ly, (unsigned)first,
                    (uint8_t)x[px+first], (uint8_t)x[px+first+1], (uint8_t)x[px+first+2], (uint8_t)x[px+first+3],
                    (uint8_t)again[py+first], (uint8_t)again[py+first+1], (uint8_t)again[py+first+2], (uint8_t)again[py+first+3]);
            }
            px += lx; py += ly; idx++;
        }
    }
    return true;
}

/* Apply everyone's input for "frame"; remote input that has not arrived yet is predicted as "nothing new". */
static void RB_ApplyInputs(uint32_t frame) {
    if (rbc_copies && rbc_leader) { RBC_ApplyInputs(frame); return; }
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready) continue;
        if (!p.local) {
            if (frame >= rb_have[i]) {
                rb_predicted[i][frame % RB_RING] = frame + 1u;
                continue;
            }
            rb_predicted[i][frame % RB_RING] = 0;
        }
        InputNetFrame *slot = INPUTNET_FrameSlotIfValid(p,frame);
        if (slot == NULL) continue;
        for (size_t e = 0; e < slot->events.size(); e++) {
            const InputNetEvent &ev = slot->events[e];
            /* At the DOS prompt every machine would run what anybody types (DEL included, in everybody's game
             * folder): only the server's autorun types there. Releases always go. */
            if (dos_shell_reading_input && ev.type == INEV_KEY && (ev.pressed & 1u) && !(ev.pressed & 2u)) continue;
            INPUTNET_ApplyEvent(ev);
        }
    }
}

/* The earliest frame before "frame" that ran with a prediction the real input now contradicts, or UINT32_MAX. */
static uint32_t RB_FindMisprediction(uint32_t frame) {
    uint32_t earliest = UINT32_MAX;
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready || p.local) continue;
        const uint32_t upto = std::min(rb_have[i], frame);   /* frames below "frame" have run */
        for (uint32_t f = rb_checked[i]; f < upto; f++) {
            if (rb_predicted[i][f % RB_RING] != f + 1u) continue;
            rb_predicted[i][f % RB_RING] = 0;
            InputNetFrame *slot = INPUTNET_FrameSlotIfValid(p,f);
            if (slot != NULL && !slot->events.empty()) {
                if (f < earliest) earliest = f;
                rb_late.push_back(RBLate{ i, f });
            }
        }
        if (upto > rb_checked[i]) rb_checked[i] = upto;
    }
    return earliest;
}

static uint32_t RB_MinHave(void) {
    uint32_t m = UINT32_MAX;
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready || p.local) continue;
        m = std::min(m, rb_have[i]);
    }
    return m;
}

/* The newest frame at or before "frame" (not before "lowest") saved in the host context that runs now, or UINT32_MAX. */
static uint32_t RB_RestorableAtOrBefore(uint32_t frame, uint32_t lowest) {
    const uint64_t ctx = RB_HostContext();
    for (uint32_t f = frame + 1u; f-- > lowest; ) {
        const RBSnap &s = rb_snap[f % RB_RING];
        if (s.valid && s.frame == f && s.context == ctx && (!rbc_copies || !rbc_leader || RBC_FollowersCanLoad(f, inputnet_frame))) return f;
        if (f == 0) break;
    }
    return UINT32_MAX;
}

static bool RB_Rollback(uint32_t target, uint32_t frame) {
    /* A frame saved inside a nested emulation loop that has ended since (a BIOS call DOS made for the console, say)
     * cannot be loaded: go back to the newest one before it that can, and replay a little more. */
    const uint32_t lowest = (frame > inputnet_rbwindow + 2u) ? frame - (inputnet_rbwindow + 2u) : 0;
    const uint32_t usable = RB_RestorableAtOrBefore(target, lowest);
    if (usable != UINT32_MAX) target = usable;
    else if (rbc_copies && rbc_leader) return false;    /* (a frame some copy cannot load) */
    if (frame - target > inputnet_rbwindow + 2u || !RB_Restore(target, frame)) {
        LOG_MSG("INPUTNET: cannot roll back from frame %u to %u",(unsigned)frame,(unsigned)target);
        return false;
    }
    const uint32_t depth = frame - target;
    rb_stat_rollbacks++;
    rb_stat_replayed += depth;
    if (depth > rb_stat_maxdepth) rb_stat_maxdepth = depth;

    inputnet_rollback_resim = true;
    rb_target = frame;
    rb_replay_started = RB_MicroTicks();
    RB_ReplayBegin(depth);
    MIXER_SetRollbackDiscard(true);
    DOSBOX_AddTicksRemain(depth * inputnet_frame_ms);
    inputnet_frame = target;
    if (rbc_copies && rbc_leader) RBC_LeaderRewind(target);
    RB_ApplyInputs(target);
    return true;
}

/* The local input delay. At the DOS prompt the frames run with everybody's real input only (RB_WaitForInputs), so
 * our input goes out far enough ahead to arrive before the others need it: typing a command does not need the lowest
 * latency, and nobody waits a round trip for every frame. */
static unsigned int RB_EffectiveDelay(void) {
    if (!rb_lockstep) return inputnet_delay;
    uint32_t rtt = 0;
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) rtt = std::max(rtt, rb_rtt[i]);
    const unsigned int need = (unsigned int)((rtt / 2u + 15u) / inputnet_frame_ms) + 1u;
    return std::min(std::max(inputnet_delay, need), 32u);
}

/* Our input up to frame+delay is final now: send it, from the oldest frame anybody lacks. */
static void RB_SendInput(uint32_t frame) {
    if (rb_joining) return;         /* (nothing of ours counts before we have the server's machines) */
    InputNetPeer &self = inputnet_peers[inputnet_selfid];
    const uint32_t newest = std::max(frame + RB_EffectiveDelay(), inputnet_local_confirmed);
    INPUTNET_FrameSlot(self,newest);
    self.confirmed = newest;
    inputnet_local_confirmed = newest;

    uint32_t lack = newest;     /* the oldest frame of ours somebody does not have */
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready || p.local) continue;
        lack = std::min(lack, rb_acked[i]);
    }
    if (newest - lack >= INPUTNET_RING / 2u) lack = newest - (INPUTNET_RING / 2u - 1u);

    /* newest first, as many older frames as fit */
    size_t budget = INPUTNET_MAX_PACKET - 96u;
    uint32_t first = newest;
    for (uint32_t f = newest + 1u; f-- > lack; ) {
        InputNetFrame *slot = INPUTNET_FrameSlotIfValid(self,f);
        const size_t need = 1u + ((slot != NULL) ? slot->events.size() * INPUTNET_MAX_EVENT_SIZE : 0u);
        if (f != newest && need > budget) break;
        budget = (need < budget) ? (budget - need) : 0u;
        first = f;
        if (f == 0) break;
    }

    uint8_t buf[INPUTNET_MAX_PACKET];
    InputNetWriter w(buf,sizeof(buf));
    INPUTNET_BeginPacket(w,INPKT_RBINPUT);
    w.u32(newest);
    w.u32(rb_realframe);
    w.u32(GetTicks());
    w.u16((uint16_t)std::min<uint32_t>(newest - first + 1u, 0xFFFFu));
    for (uint32_t f = first; f <= newest; f++) {
        InputNetFrame *slot = INPUTNET_FrameSlotIfValid(self,f);
        const size_t n = (slot != NULL) ? slot->events.size() : 0u;
        w.u8((uint8_t)n);
        for (size_t e = 0; e < n; e++) INPUTNET_WriteEvent(w,slot->events[e]);
    }

    /* what we have of everybody else's input, and when their newest packet was sent (for their round trip time) */
    uint8_t nacks = 0;
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++)
        if (inputnet_peers[i].active && inputnet_peers[i].ready && !inputnet_peers[i].local) nacks++;
    w.u8(nacks);
    const uint32_t now = GetTicks();
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready || p.local) continue;
        w.u8((uint8_t)i);
        w.u32(rb_have[i]);
        w.u32(rb_peerstamp[i] + (now - rb_peerstamp_at[i]));  /* their clock, plus how long we held it */
    }

    /* the newest frame whose input everybody has, and our machine's hash at its start; the number of resyncs we
     * loaded, since a checksum from before one compares machines that were made equal since */
    w.u16(rb_epoch);
    /* (frames after a wrong guess that waits for its correction are not final either) */
    const uint32_t final_frame = std::min(std::min(RB_MinHave(), frame), rb_pending_wrong);
    const RBSnap &cs = rb_snap[final_frame % RB_RING];
    if (final_frame != UINT32_MAX && cs.valid && cs.frame == final_frame) {
        w.u32(final_frame);
        w.u32((uint32_t)(cs.hash >> 32u));
        w.u32((uint32_t)cs.hash);
    }
    else {
        w.u32(UINT32_MAX); w.u32(0); w.u32(0);
    }

    w.u32(rb_cap_local);            /* this computer's speed proposal at cycles=max (RB_MaxCycles; 0: none) */

    if (w.overflow()) {
        LOG(LOG_MISC,LOG_WARN)("INPUTNET: rollback input packet overflow");
        return;
    }
    INPUTNET_SendToPeers(buf,w.size());
}

static void RB_Desync(InputNetPeer &p, unsigned peerid, uint32_t frame, uint64_t mine, uint64_t theirs);

static void RB_CompareCheck(InputNetPeer &p, unsigned peerid, uint32_t frame) {
    const RBCheck &c = rb_peercheck[peerid][frame % INPUTNET_CHECK_RING];
    if (!c.valid || c.frame != frame) return;
    if (frame >= std::min(RB_MinHave(), rb_realframe) + 1u) return;  /* not final here yet */
    /* Final only once the input of every frame before it was compared with what ran (RB_FindMisprediction, at a frame
     * start) and a rollback, if one was needed, has replayed it: input that has merely arrived may still contradict
     * the prediction this frame start was computed with. */
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &q = inputnet_peers[i];
        if (q.active && q.ready && !q.local && rb_checked[i] < frame) return;
    }
    if (inputnet_rollback_resim) return;
    if (rb_pending_wrong != UINT32_MAX && frame > rb_pending_wrong) return;   /* waits for its correction */
    const RBSnap &s = rb_snap[frame % RB_RING];
    if (!s.valid || s.frame != frame) return;
    rb_stat_checks++;
    if (s.hash != c.hash) RB_Desync(p, peerid, frame, s.hash, c.hash);
    rb_peercheck[peerid][frame % INPUTNET_CHECK_RING].valid = false;
}

static void RB_HandleInput(InputNetPeer &p, unsigned peerid, InputNetReader &r) {
    const uint32_t newest = r.u32();
    const uint32_t theirframe = r.u32();
    const uint32_t stamp = r.u32();
    const unsigned nframes = r.u16();
    if (r.bad() || nframes == 0 || nframes > newest + 1u) return;

    const uint32_t first = newest + 1u - nframes;
    for (uint32_t f = first; f <= newest; f++) {
        const unsigned n = r.u8();
        if (r.bad()) return;
        const bool have = (f < rb_have[peerid]) || (INPUTNET_FrameSlotIfValid(p,f) != NULL);
        const bool absurd = (f > rb_realframe + INPUTNET_RING / 2u);
        if (have || absurd) {
            for (unsigned e = 0; e < n; e++) { InputNetEvent ev; if (!INPUTNET_ReadEvent(r,ev)) return; }
            continue;
        }
        InputNetFrame &slot = INPUTNET_FrameSlot(p,f);
        for (unsigned e = 0; e < n; e++) {
            InputNetEvent ev;
            if (!INPUTNET_ReadEvent(r,ev)) return;
            slot.events.push_back(ev);
        }
    }
    while (INPUTNET_FrameSlotIfValid(p,rb_have[peerid]) != NULL) rb_have[peerid]++;
    if (newest > p.confirmed) p.confirmed = newest;

    const uint32_t now = GetTicks();
    rb_peerframe[peerid] = theirframe;
    rb_peerframe_at[peerid] = now;
    rb_peerstamp[peerid] = stamp;
    rb_peerstamp_at[peerid] = now;

    const unsigned nacks = r.u8();
    for (unsigned k = 0; k < nacks; k++) {
        const uint8_t who = r.u8();
        const uint32_t have = r.u32();
        const uint32_t echo = r.u32();
        if (r.bad()) return;
        if (who == inputnet_selfid) {
            if (have > rb_acked[peerid]) rb_acked[peerid] = have;
            const uint32_t rtt = now - echo;
            if (rtt < 5000u) rb_rtt[peerid] = rb_rtt[peerid] ? (rb_rtt[peerid] * 7u + rtt) / 8u : rtt;
        }
    }

    const uint16_t epoch = r.u16();
    const uint32_t cframe = r.u32();
    const uint32_t chi = r.u32(), clo = r.u32();
    if (!r.bad() && cframe != UINT32_MAX && epoch == rb_epoch) {
        RBCheck &c = rb_peercheck[peerid][cframe % INPUTNET_CHECK_RING];
        c.frame = cframe;
        c.hash = ((uint64_t)chi << 32u) | clo;
        c.valid = true;
        RB_CompareCheck(p, peerid, cframe);
    }
    if (r.remain() >= 4) {
        const uint32_t cap = r.u32();
        if (!r.bad() && cap != 0) { rb_cap_peer[peerid] = cap; rb_cap_peer_at[peerid] = now; }
    }
}

/* Wait while we are more than the window ahead of somebody's input. */
/* Client, waiting for input: while the server is heard (it sends its own input while it waits as well), it is another
 * participant that is missing, and the server drops that one after its own timeout and hands the machines over. So the
 * client waits for that (up to six timeouts) instead of leaving: with three players, the one who did not quit timed out
 * first and left too. */
static bool RB_ClientWaitsOn(uint32_t start) {
    if (inputnet_is_server) return false;
    const uint32_t now = GetTicks();
    const InputNetPeer &srv = inputnet_peers[0];
    return srv.active && now - srv.lastheard < inputnet_timeout && now - start < 6u * inputnet_timeout;
}

/* Time spent waiting for the other computers' input: not this computer's load (RB_MaxCycles). */
struct RBWaitTimer {
    uint64_t t0;
    RBWaitTimer() : t0(RB_MicroTicks()) {}
    ~RBWaitTimer() { rb_ml_wait += RB_MicroTicks() - t0; }
};

/* How busy this process was over the last second: wall time less what increaseticks() slept and what it waited for
 * other computers or other copies (RB_MaxCycles). True when a new value is in rb_ml_load. */
/* The CPU time this thread has had, in microseconds (0: not known here). */
static uint64_t RB_ThreadCpuUs(void) {
#if defined(WIN32)
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    const uint64_t kt = ((uint64_t)k.dwHighDateTime << 32u) | k.dwLowDateTime, ut = ((uint64_t)u.dwHighDateTime << 32u) | u.dwLowDateTime;
    return (kt + ut) / 10u;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#else
    return 0;
#endif
}

static bool RB_MeasureLoad(void) {
    dosbox_core_timing = true;
    const uint64_t now = RB_MicroTicks();
    if (rb_ml_t0 == 0 || now < rb_ml_t0) {
        rb_ml_t0 = now; rb_ml_wait = 0; rb_ml_sleep0 = dosbox_sleep_us; rb_ml_core0 = dosbox_core_ns;
        rb_ml_csum = 0; rb_ml_cn = 0; rb_ml_cpu0 = RB_ThreadCpuUs(); rb_ml_pic0 = render_changed_frames;
        return false;
    }
    rb_ml_csum += (double)CPU_CycleMax; rb_ml_cn++;     /* (the speed can change during the second) */
    const uint64_t wall = now - rb_ml_t0;
    if (wall < 1000000u) return false;
    const uint64_t idle = (dosbox_sleep_us - rb_ml_sleep0) + rb_ml_wait;
    rb_ml_load = (idle >= wall) ? 0.0 : (double)(wall - idle) / (double)wall;
    rb_ml_core = std::min(rb_ml_load, (double)(dosbox_core_ns - rb_ml_core0) / 1000.0 / (double)wall);
    rb_ml_speed = rb_ml_cn ? rb_ml_csum / (double)rb_ml_cn : (double)CPU_CycleMax;
    const uint64_t cpu = RB_ThreadCpuUs();
    rb_ml_run = (cpu > rb_ml_cpu0 && rb_ml_cpu0 != 0 && rb_ml_load > 0.02)
        ? std::max(0.05, std::min(1.0, (double)(cpu - rb_ml_cpu0) / ((double)wall * rb_ml_load))) : 1.0;
    rb_ml_pictures = (double)(render_changed_frames - rb_ml_pic0) * 1000000.0 / (double)wall;
    rb_ml_refresh = (vga.draw.delay.vtotal > 1.0) ? 1000.0 / (double)vga.draw.delay.vtotal : 70.0;
    rb_ml_t0 = now; rb_ml_wait = 0; rb_ml_sleep0 = dosbox_sleep_us; rb_ml_core0 = dosbox_core_ns;
    rb_ml_csum = 0; rb_ml_cn = 0; rb_ml_cpu0 = cpu; rb_ml_pic0 = render_changed_frames;
    return true;
}

/* This process's speed proposal (rb_cap_local) from the last measurement: the speed at which the CPU core would get
 * what "rollback max load" leaves after the rest (at least a third of it, should the rest take more). The core's time
 * per cycle and the rest are averaged over a few seconds: replays come and go with the players' input, and another
 * program on the computer can take a second or two (lowering the speed for every player for longer would not make that
 * second any better). Only a process that could not keep up (over 90% busy) asks for less at once. While the core
 * barely runs (a halted guest) its time says little about faster speeds: no higher proposal then. */
static void RB_ReplayBegin(uint32_t depth) {
    rb_rp_t0 = (CPU_CycleAutoAdjust && depth > 0) ? RB_MicroTicks() : 0;
    rb_rp_core0 = dosbox_core_ns;
    rb_rp_depth = depth;
}

static void RB_ReplayEnd(void) {
    if (rb_rp_t0 == 0) return;
    const uint64_t now = RB_MicroTicks();
    RBReplayRec &r = rb_rp[rb_rp_pos];
    r.depth = rb_rp_depth;
    r.us = (uint32_t)std::min<uint64_t>(now - rb_rp_t0, 10000000u);
    r.core_us = (uint32_t)std::min<uint64_t>((dosbox_core_ns - rb_rp_core0) / 1000u, r.us);
    r.speed = (uint32_t)CPU_CycleMax;
    r.at = GetTicks();
    rb_rp_pos = (rb_rp_pos + 1u) % 64u;
    if (rb_rp_n < 64u) rb_rp_n++;
    rb_rp_t0 = 0;
}

/* The speed at which every replay of the last RB_REPLAY_WINDOW_MS but the RB_REPLAY_LONG_ALLOWED deepest takes
 * RB_REPLAY_BUDGET_US: a replay runs the frames since the corrected one again before the next real-time frame, and a
 * long one is a hitch (a few now and then are fine, many are not). Per frame, saving it and the rest take the same time
 * at any speed, the CPU core's part follows the speed (from all the latest replays). 0: no limit. */
static double RB_ReplayCap(void) {
    uint32_t depths[64];
    unsigned n = 0;
    double frames = 0, fixed_us = 0, core_per_cycle = 0;
    const uint32_t now = GetTicks();
    for (unsigned i = 0; i < rb_rp_n; i++) {
        const RBReplayRec &r = rb_rp[i];
        frames += r.depth;
        fixed_us += (double)(r.us - r.core_us);
        core_per_cycle += (double)r.core_us / (double)std::max<uint32_t>(r.speed, 1000u);
        if (now - r.at < RB_REPLAY_WINDOW_MS) depths[n++] = r.depth;
    }
    if (n <= RB_REPLAY_LONG_ALLOWED || frames <= 0) return 0;
    std::sort(depths, depths + n);
    const double depth = std::max(1.0, (double)depths[n - 1u - RB_REPLAY_LONG_ALLOWED]);
    /* (as for the load: time another program had the CPU is not this speed's cost) */
    const double per_frame_fixed = fixed_us / frames * rb_ml_run, per_frame_core = core_per_cycle / frames * rb_ml_run;
    const double room = RB_REPLAY_BUDGET_US / depth - per_frame_fixed;
    if (per_frame_core <= 0) return 0;
    return std::max(room, 0.0) / per_frame_core;
}

static void RB_Propose(void) {
    /* (what ran while another program had the CPU took no CPU of its own: count only the share that ran) */
    const double fixed = std::max(0.0, rb_ml_load - rb_ml_core) * rb_ml_run;
    const double cost = rb_ml_core * rb_ml_run / std::max(rb_ml_speed, 1000.0);
    rb_ml_cost_s = (rb_ml_cost_s < 0) ? cost : rb_ml_cost_s * 0.6 + cost * 0.4;
    rb_ml_fixed_s = (rb_ml_fixed_s < 0) ? fixed : rb_ml_fixed_s * 0.6 + fixed * 0.4;
    const double target = (double)rb_maxload / 100.0;
    const double avail = std::max(target - rb_ml_fixed_s, target / 3.0);
    const double limit = (CPU_CycleLimit > 0) ? (double)CPU_CycleLimit : 1000000.0;
    /* (replays are part of the load measured; how long one takes is not a limit of its own: a game at a low speed
     * draws few frames a second, which shows far more than a replay of a few tens of milliseconds now and then.
     * Measured Nov 2026, Duke Nukem 3D at 800x600: 3 frames a second at 20,000 cycles, 10 at 70,000, 22 at 160,000) */
    double cap = avail / std::max(rb_ml_cost_s, 1e-9);
    if (rb_ml_core < 0.03) cap = std::min(cap, (double)CPU_CycleMax);
    /* a game whose picture already changes at nearly every screen refresh shows nothing more at a higher speed, and
     * every replay would take longer: no higher proposal (Duke Nukem 3D at 320x200 reaches it near 50,000 cycles) */
    if (rb_ml_pictures >= rb_ml_refresh * 0.9) cap = std::min(cap, (double)CPU_CycleMax);
    if (rb_ml_load > 0.97) cap = std::min(cap, (double)CPU_CycleMax * 0.8);
    rb_cap_local = (uint32_t)std::max(2000.0, std::min(cap, limit));
}

static bool RB_WaitForWindow(uint32_t frame) {
    RBWaitTimer waited;
    uint32_t start = 0, last_events = 0, lastresend = GetTicks();
    for (;;) {
        bool ok = true;
        for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
            InputNetPeer &p = inputnet_peers[i];
            if (!p.active || !p.ready || p.local) continue;
            if (frame >= rb_have[i] + inputnet_rbwindow) { ok = false; break; }
        }
        if (ok) break;
        if (start == 0) { start = GetTicks(); last_events = start; rb_stat_stalls++; }
        if (!inputnet_session) return false;
        if ((GetTicks() - start) > inputnet_timeout && inputnet_is_server && rbc_copies) {
            for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
                InputNetPeer &p = inputnet_peers[i];
                if (p.active && p.ready && !p.local && frame >= rb_have[i] + inputnet_rbwindow) RBC_ServerDrop(i, "fell too far behind");
            }
            continue;
        }
        if ((GetTicks() - lastresend) >= 30u) { RB_SendInput(frame); lastresend = GetTicks(); }  /* (they may wait for ours) */
        if ((GetTicks() - start) > inputnet_timeout && !RB_ClientWaitsOn(start)) {
            if (inputnet_is_server)
                INPUTNET_EndSession("timed out waiting for the other participant(s)",true);
            else
                INPUTNET_CloseLink("timed out waiting for the server",true);
            return false;
        }
        INPUTNET_Stall(last_events);
        const uint32_t before = GetTicks();
        INPUTNET_PollSocket();
        if (GetTicks() - before > 500u) start = GetTicks();     /* (a resync's transfer came meanwhile) */
    }
    if (start != 0) inputnet_stat_stallms += GetTicks() - start;
    return true;
}

/* Run no further ahead of the others than the network makes us: one emulated millisecond is skipped now and then
 * while we are ahead, and the others do the same when they are. */
static void RB_TimeSync(uint32_t frame) {
    double worst = 0;
    bool any = false;
    const uint32_t now = GetTicks();
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready || p.local || rb_peerframe_at[i] == 0) continue;
        const double theirs = (double)rb_peerframe[i] +
            ((double)(now - rb_peerframe_at[i]) + (double)rb_rtt[i] / 2.0) / (double)inputnet_frame_ms;
        const double adv = (double)frame - theirs;
        if (!any || adv > worst) worst = adv;
        any = true;
    }
    if (!any) return;
    rb_advantage = rb_advantage * 0.9 + worst * 0.1;
    if (rb_advantage > 0.75 && (frame - rb_last_slowdown) >= 2u) {
        if (DOSBOX_DropTickRemain()) { rb_stat_slowdowns++; rb_last_slowdown = frame; }
    }
}

/* At the DOS prompt the shell (native code) reads the keys: frames there run with everybody's real input only, so
 * no correction ever has to put the machine back into the prompt. */
static bool RB_WaitForInputs(uint32_t frame) {
    RBWaitTimer waited;
    uint32_t start = 0, last_events = 0, lastresend = GetTicks();
    while (RB_MinHave() <= frame) {
        if (start == 0) { start = GetTicks(); last_events = start; }
        if (!inputnet_session) return false;
        if ((GetTicks() - start) > inputnet_timeout && inputnet_is_server && rbc_copies) {
            for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
                InputNetPeer &p = inputnet_peers[i];
                if (p.active && p.ready && !p.local && rb_have[i] <= frame) RBC_ServerDrop(i, "fell too far behind");
            }
            continue;
        }
        if ((GetTicks() - lastresend) >= 30u) { RB_SendInput(frame); lastresend = GetTicks(); }  /* (they may wait for ours) */
        if ((GetTicks() - start) > inputnet_timeout && !RB_ClientWaitsOn(start)) {
            if (inputnet_is_server)
                INPUTNET_EndSession("timed out waiting for the other participant(s)",true);
            else
                INPUTNET_CloseLink("timed out waiting for the server",true);
            return false;
        }
        INPUTNET_Stall(last_events);
        const uint32_t before = GetTicks();
        INPUTNET_PollSocket();
        if (GetTicks() - before > 500u) start = GetTicks();     /* (a resync's transfer came meanwhile) */
    }
    return true;
}

static void RB_LogStats(void) {
    const uint32_t now = GetTicks();
    if ((now - rb_stat_last_log) < 5000u) return;
    rb_stat_last_log = now;
    uint32_t rtt = 0;
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) rtt = std::max(rtt, rb_rtt[i]);
    LOG_MSG("INPUTNET: rollback frame %u: %u rollbacks (%u frames replayed, deepest %u), %u slowdowns, %u stalls, "
        "%u checks, %u desyncs, %u resyncs; rtt %u ms, ahead %.2f frames; saving a frame takes %.0f us, %.1f pages; "
        "a replay takes %.1f ms (at most %.1f); memory scan %.0f us, devices %.0f us; %u frames in lockstep, %u host context changes",
        (unsigned)rb_realframe,(unsigned)rb_stat_rollbacks,(unsigned)rb_stat_replayed,(unsigned)rb_stat_maxdepth,
        (unsigned)rb_stat_slowdowns,(unsigned)rb_stat_stalls,(unsigned)rb_stat_checks,(unsigned)rb_stat_desyncs,
        (unsigned)rb_stat_resyncs,(unsigned)rtt,rb_advantage,
        rb_stat_captures ? (double)rb_stat_capture_us / (double)rb_stat_captures : 0.0,
        rb_stat_captures ? (double)rb_stat_pages / (double)rb_stat_captures : 0.0,
        rb_stat_replays ? (double)rb_stat_replay_us / (double)rb_stat_replays / 1000.0 : 0.0, (double)rb_stat_replay_max_us / 1000.0,
        rb_stat_captures ? (double)rb_stat_scan_us / (double)rb_stat_captures : 0.0,
        rb_stat_captures ? (double)rb_stat_lean_us / (double)rb_stat_captures : 0.0,
        (unsigned)rb_stat_lockstep, (unsigned)rb_stat_ctxchanges);
    if (CPU_CycleAutoAdjust) LOG_MSG("INPUTNET: speed %u cycles per ms (cycles=max), this computer %.0f%% busy (CPU core %.0f%%, "
        "running %.0f%% of it), picture %.0f of %.0f a second, proposing %u", (unsigned)CPU_CycleMax, rb_ml_load * 100.0, rb_ml_core * 100.0,
        rb_ml_run * 100.0, rb_ml_pictures, rb_ml_refresh, rb_cap_local);
    {
        extern unsigned int vga_shown_flips, render_changed_frames;
        static unsigned int flips_last = 0, changed_last = 0;
        static uint32_t flips_at = 0;
        const uint32_t tnow = GetTicks();
        const double secs = flips_at ? (double)(tnow - flips_at) / 1000.0 : 0.0;
        LOG_MSG("INPUTNET: %u hitches (frames more than 30 ms late), the longest gap %.0f ms; the game showed %.1f frames/s (page flips, "
            "frames copied through the bank window), the picture changed %.1f times/s",
            (unsigned)rb_stat_hitches, (double)rb_stat_maxgap_us / 1000.0, secs > 0 ? (double)(vga_shown_flips - flips_last) / secs : 0.0,
            secs > 0 ? (double)(render_changed_frames - changed_last) / secs : 0.0);
        flips_last = vga_shown_flips; changed_last = render_changed_frames; flips_at = tnow;
    }
    rb_stat_hitches = 0; rb_stat_maxgap_us = 0;
    rb_stat_scan_us = 0; rb_stat_lean_us = 0; rb_stat_lockstep = 0; rb_stat_ctxchanges = 0;
    rb_stat_capture_us = 0; rb_stat_captures = 0; rb_stat_pages = 0;
    rb_stat_replay_us = 0; rb_stat_replays = 0; rb_stat_replay_max_us = 0;
}

/* --- Desync handling ---------------------------------------------------------
 * The server's machine is the reference. When a frame's hashes differ (either
 * side may notice), the server waits at a frame start until it has every
 * participant's input for the frames before it, which makes that frame start
 * final, saves the machine in the portable save state format (the one the join
 * uses: the lean snapshots hold host pointers), loads that state itself and
 * sends it. The client loads it and replays the frames from there to its
 * real-time frame with the input it already has, so both run from the same
 * state again. Both machines stand still while this happens. */

static bool rb_resync_wanted = false;   /* server: a client asked for our state, or we saw a desync */
static std::vector<uint8_t> rb_resync_ready;    /* client: the server's state, complete, waiting for a frame start */
static uint32_t rb_resync_ready_raw = 0;
static uint32_t rb_resync_acked = 0;    /* server: chunks acknowledged */
static bool rb_resync_applied = false;  /* client: the server's state was just loaded */

static void RB_Desync(InputNetPeer &p, unsigned peerid, uint32_t frame, uint64_t mine, uint64_t theirs) {
    (void)peerid;
    rb_stat_desyncs++;
    LOG_MSG("INPUTNET: RollbackNet machines differ at frame %u (%08x%08x here, %08x%08x at \"%s\"); %s",
        (unsigned)frame,(unsigned)(mine >> 32u),(unsigned)mine,(unsigned)(theirs >> 32u),(unsigned)theirs,
        p.name.empty() ? "peer" : p.name.c_str(),
        inputnet_is_server ? "sending our state" : "taking the server's state over");
    if (inputnet_is_server) rb_resync_wanted = true;
    else { rb_desynced = true; rb_resync_last_sent = 0; }
}

static void RB_SendResyncAck(uint32_t id, uint32_t count) {
    uint8_t buf[32];
    InputNetWriter w(buf,sizeof(buf));
    INPUTNET_BeginPacket(w,INPKT_RESYNC);
    w.u8(3);
    w.u32(id);
    w.u32(count);
    INPUTNET_SendToPeers(buf,w.size());
}

/* What a resync carries besides the save state: the frame it is, the session's tick, and the VGA frame clock the save
 * state format does not hold (INPUTNET_ApplyStateSnapshot's join sets the same from the WELCOME). */
struct RBResyncInfo {
    uint32_t frame = 0, tick = 0;
    double vfps = 0, vbase = 0;
    int32_t vframes = 0;
    uint16_t epoch = 0;             /* the server's resyncs, this one included (rb_epoch) */
    uint8_t draw = 0;               /* VGA_GetOnDemandFlags */
};
static const size_t RB_RESYNC_INFO_SIZE = 4 + 4 + 8 + 8 + 4 + 2 + 1;

static void RB_PutResyncInfo(std::vector<uint8_t> &raw, const RBResyncInfo &h) {
    auto put = [&raw](const void *v, size_t n) { const uint8_t *b = (const uint8_t*)v; raw.insert(raw.end(), b, b + n); };
    put(&h.frame, 4); put(&h.tick, 4); put(&h.vfps, 8); put(&h.vbase, 8); put(&h.vframes, 4); put(&h.epoch, 2);  /* little-endian hosts */
    put(&h.draw, 1);
}

static bool RB_GetResyncInfo(const std::vector<uint8_t> &raw, RBResyncInfo &h) {
    if (raw.size() < RB_RESYNC_INFO_SIZE) return false;
    const uint8_t *b = &raw[0];
    memcpy(&h.frame, b, 4); memcpy(&h.tick, b + 4, 4); memcpy(&h.vfps, b + 8, 8); memcpy(&h.vbase, b + 16, 8); memcpy(&h.vframes, b + 24, 4);
    memcpy(&h.epoch, b + 28, 2);
    h.draw = b[30];
    return true;
}

/* Both ends of a resync: load the save state, then start the saved frames over from it. The server loads the very
 * state it sends, so both go on from identical machines (the save state format rounds some values, the PIC's event
 * times among them). Nothing before this frame can be rolled back to afterwards. */
static bool RB_LoadResyncState(const RBResyncInfo &h, const std::vector<uint8_t> &state, const std::function<void()> &after = std::function<void()>()) {
    RBT_Stop();                     /* the load writes all of guest RAM */
    /* before too: the VGA's load compares its mode's rate with vga_fps (VGA_SetupDrawing), and with this machine's own
     * (a joining computer's text mode at 70 Hz, a game's VESA mode at 60) it would restart the frame clock and the
     * vertical retrace where the other computers do not */
    vga_fps = h.vfps;
    vga_mode_time_base = h.vbase;
    vga_mode_frames_since_time_base = (int)h.vframes;
    if (!INPUTNET_ApplyStateSnapshot(state)) { RB_InitRegions(); return false; }
    vga_fps = h.vfps;
    vga_mode_time_base = h.vbase;
    vga_mode_frames_since_time_base = (int)h.vframes;
    VGA_SetOnDemandFlags(h.draw);
    if (after) after();             /* (a cluster copy's clock, time stamp counter, IPX driver) */
    PAGING_ClearTLB();
    rb_tick = h.tick;
    rb_epoch = h.epoch;
    rb_last_load_frame = h.frame;
    rb_ml_t0 = 0;                   /* (the load's own time is not play) */
    rb_rp_t0 = 0;
    rb_evlog_on = getenv("NN_RB_EVLOG") != NULL;
    RB_InitRegions();               /* every older frame start is gone */
    inputnet_frame = h.frame;
    RB_Capture(h.frame);
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        for (unsigned k = 0; k < RB_RING; k++) rb_predicted[i][k] = 0;
        rb_checked[i] = h.frame;    /* frames before this one are never compared again */
        for (unsigned k = 0; k < INPUTNET_CHECK_RING; k++) rb_peercheck[i][k].valid = false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* RollbackNet clusters: IPX/NullModem-over-RollbackNet                      */
/* ------------------------------------------------------------------------- */
/* For the games played over IPX (or a null modem) between computers. Every computer runs one copy of the game for
 * every player, the copies linked by IPX as on a LAN, and the copies run identically on every computer: the whole
 * LAN is the machine RollbackNet keeps in step. Copy c is played by participant c (the host's is copy 0): that
 * participant's input goes to copy c on every computer, and a copy whose player is not there (yet) runs without
 * input. So a player can join later and take a free copy over, as with IPX-over-InputNet.
 *
 * On each computer one copy is the leader (the one its player plays: the RollbackNet participant, with the input,
 * the predictions, the rollbacks and the checksums); the others are its followers, driven over 127.0.0.1 ("cluster
 * port"). Every copy saves every frame start. At a frame start each follower tells the leader what it sent over IPX
 * during the frame before, its checksum, and which saved frames it could load (AT); the leader routes the packets (in
 * copy order, then in the order sent) and sends every follower the packets addressed to it and its player's input
 * for the frame (GO). Packets sent during a frame so reach their addressees at the start of the next one: the same
 * moment on every computer. A rollback restores every copy (REWIND) and they replay together; the packets routed at
 * the frame gone back to are kept by the leader and delivered again, the later ones are routed again.
 *
 * Joining: the host's session runs from the start, alone if need be. A computer that joins gets the host's game
 * files where its own differ (per copy), starts the game on its copies itself when the host's game is running
 * already (so that every copy's emulator is inside the same nested loops as the host's when it takes the host's
 * machine over), then asks for the host's state: at a frame start the host saves all of its copies, loads them back
 * (a save state rounds a few values), and sends them to every computer of the session, which all load them
 * (RB_ServerResync); the joining computer's player is a participant from that frame on. A participant who leaves is
 * taken out the same way, and the same transfer puts every computer back on the host's machines should a checksum
 * ever differ. */

enum { RBC_CMD_SAVE = 1, RBC_CMD_MANIFEST = 2, RBC_CMD_FILEMAP = 3, RBC_CMD_LOADRESYNC = 4, RBC_CMD_START = 5, RBC_CMD_END = 6,
       RBC_CMD_SETCOPY = 7 };
enum { RBC_ROUTE = 0, RBC_REWIND = 1, RBC_HOLD = 2 };
#define RBC_CHUNK      1200u
#define RBC_MAXPKT     65000
#define RBC_AT_STABLE  1u           /* the copy's host stack is as it was at the frame start before */
#define RBC_AT_PROMPT  2u           /* its DOS shell is reading a command line */
#define RBC_CLX_MAGIC  0x58434252u  /* "RBCX": the cluster's copies, after the leader's save state */
#define RBC_CLX_END    0x45434252u  /* "RBCE" */

extern void IPX_ClusterTakeOutbox(std::vector<std::vector<uint8_t> > &out);
extern void IPX_ClusterDeliver(const uint8_t *data, size_t len);
extern uint64_t rdtsc_count_base;
extern pic_tickindex_t rdtsc_pic_base;

static UDPsocket rbc_sock = NULL;
static UDPpacket *rbc_in = NULL, *rbc_out = NULL;
static SDLNet_SocketSet rbc_set = NULL;
static IPaddress rbc_leader_addr;
static uint32_t rbc_epoch = 0;              /* rewinds and loads so far: an AT or GO of an earlier one is old */
static uint32_t rbc_last_heard = 0;         /* follower: the leader's newest packet (host ms) */
static uint32_t rbc_last_beat = 0;          /* leader: its newest heartbeat */

struct RBCFollower {
    bool present = false;
    IPaddress addr;
    uint32_t at_frame = UINT32_MAX, at_epoch = 0;
    uint8_t at_flags = 0;
    uint64_t at_loadable = 0, at_hash = 0;
    std::vector<std::vector<uint8_t> > at_out;  /* its packets of the frame before at_frame */
    std::vector<uint8_t> at_ser;                /* ...and what its null modem sent, and its RTS/DTR (SERIAL_RBTakeOutbox) */
    uint8_t at_lines = 0;
    uint32_t got = 0, done = 0;                 /* the newest command it received, carried out */
    std::vector<uint8_t> go;                    /* the newest GO sent to it, again should its AT repeat */
    uint32_t go_frame = UINT32_MAX, go_epoch = 0;
    uint32_t cap = 0, cap_at = 0;               /* its speed proposal (RB_Propose, 0: none yet), and when it said */
};
static RBCFollower rbc_f[INPUTNET_MAX_PEERS];   /* leader: by copy */
static uint32_t rbc_seq = 0;
/* The command RBC_Command waits for (one at a time), and the copy it went to: its acknowledgements count for that
 * follower whatever copy they name (a SETCOPY changes it in between). */
static uint32_t rbc_inflight_seq = 0;
static unsigned rbc_inflight_c = 0;
static std::vector<std::vector<uint8_t> > rbc_inbox[RB_RING][INPUTNET_MAX_PEERS];  /* leader: each copy's packets at a frame start */
static uint32_t rbc_inbox_frame[RB_RING];
static std::vector<uint8_t> rbc_ser_inbox[RB_RING][INPUTNET_MAX_PEERS];  /* leader: what each copy's null modem receives */
static uint8_t rbc_ser_lines[RB_RING][INPUTNET_MAX_PEERS];               /* ...and the other side's RTS/DTR */

/* a blob coming in (either side) */
static uint32_t rbc_blob_id = 0, rbc_blob_total = 0, rbc_blob_have = 0;
static std::vector<uint8_t> rbc_blob;
static std::vector<bool> rbc_blob_got;
static bool rbc_blob_done = false;
static IPaddress rbc_blob_from;
static uint32_t rbc_ack_id = 0, rbc_ack_have = 0;  /* a blob going out: how much of it the other end has */

/* follower */
struct RBCCmd { uint32_t seq; uint8_t cmd; uint32_t a, b, c; };
static bool rbcf_welcomed = false;
static std::vector<RBCCmd> rbcf_cmds;           /* received, not carried out yet */
static uint32_t rbcf_last_cmd = 0;
static uint32_t rbcf_cur = UINT32_MAX;          /* the frame start it waits at */
static bool rbcf_go = false;
static uint32_t rbcf_go_frame = 0, rbcf_go_epoch = 0;
static std::vector<InputNetEvent> rbcf_go_events;
static std::vector<std::vector<uint8_t> > rbcf_go_packets;
static std::vector<uint8_t> rbcf_go_ser;
static uint8_t rbcf_go_lines = 0;
static bool rbcf_rewind = false;
static uint32_t rbcf_rw_target = 0, rbcf_rw_real = 0, rbcf_rw_epoch = 0;
static bool rbcf_reloaded = false;              /* SAVE or LOADRESYNC made the frame start over */

/* a joining computer's leader */
static bool rbj_typed = false;
static uint32_t rbj_steady = 0;                 /* frames in a row every copy has been where it can take the state */
static uint32_t rbj_last_request = 0;
static std::vector<std::vector<InputNetEvent> > rbj_typing;     /* our own typing on our copies, a frame each */
static size_t rbj_typing_pos = 0;

/* client: the server's machines (uncompressed) until they can be loaded */
static std::vector<uint8_t> rbc_resync_raw;
static uint8_t rbc_resync_prompt[INPUTNET_MAX_PEERS];
static uint32_t rbc_resync_since = 0;

/* server: joining and leaving participants */
static bool rbs_joining[INPUTNET_MAX_PEERS];    /* asked to join, not in the session yet */
static bool rbs_wants[INPUTNET_MAX_PEERS];      /* ...and ready to take the machines over */
static std::vector<uint8_t> rbs_left;           /* left since the last transfer */
static uint8_t rbs_join_copy[INPUTNET_MAX_PEERS];   /* the copy a joining participant asked for (RBC_ServerHello) */

/* Seats (clusters): the participant who plays each copy, 0xFF for nobody, the same on every computer. Participant c
 * plays copy c until a seat changes (INPUTNET_RollbackSeat: the server's control channel, "rbseat"); seats change only
 * with a resync, which carries them, so every computer switches at the same frame. A computer whose player gets
 * another copy switches its own copies too (RBC_SwitchCopy): the leader, with the window and the sound, always runs
 * the copy its player plays. */
static uint8_t rb_seat[INPUTNET_MAX_PEERS];
static uint8_t rbs_seat_next[INPUTNET_MAX_PEERS];   /* server: the seats the next resync gives */
static bool rbs_seat_change = false;
static uint8_t rbc_resync_seat[INPUTNET_MAX_PEERS]; /* client: the seats of the resync being received */
static bool rbc_resync_has_seats = false;

static void RB_SeatsReset(void) {
    for (unsigned c = 0; c < INPUTNET_MAX_PEERS; c++) rb_seat[c] = (uint8_t)c;
    rbs_seat_change = false;
    rbc_resync_has_seats = false;
}

/* The copy a participant plays in "seats", or -1. */
static int RB_CopyOf(const uint8_t *seats, unsigned participant) {
    for (unsigned c = 0; c < rbc_copies; c++) if (seats[c] == participant) return (int)c;
    return -1;
}

/* Server: tells NukemNet who plays which copy now (env NN_RB_SEAT_REPORT, a local UDP port), after every join, leave
 * and seat change: {"seats":["@host","nick",null,...]}, a copy's player by name ("@host": the server's own), null for
 * nobody. */
static void RB_ReportSeats(void) {
    static int port = -1;
    if (port < 0) { const char *e = getenv("NN_RB_SEAT_REPORT"); port = (e != NULL) ? atoi(e) : 0; }
    if (port <= 0 || port > 65535 || rbc_copies < 2) return;
    std::string j = "{\"seats\":[";
    for (unsigned c = 0; c < rbc_copies; c++) {
        if (c) j += ',';
        const uint8_t s = rb_seat[c];
        if (s >= INPUTNET_MAX_PEERS || !inputnet_peers[s].active) { j += "null"; continue; }
        const std::string name = inputnet_peers[s].local ? std::string("@host") : inputnet_peers[s].name;
        j += '"';
        for (char ch : name) { if (ch == '"' || ch == '\\') j += '\\'; if ((unsigned char)ch >= 0x20) j += ch; }
        j += '"';
    }
    j += "]}";
    IPaddress to;
    SDLNet_Write32(0x7F000001u, &to.host);
    SDLNet_Write16((uint16_t)port, &to.port);
    INPUTNET_SendRaw(to, (const uint8_t*)j.data(), j.size());
}

/* Whether nobody in the session plays copy c in "seats". */
static bool RB_CopyFree(const uint8_t *seats, unsigned c) {
    const uint8_t s = seats[c];
    return s >= INPUTNET_MAX_PEERS || !inputnet_peers[s].active;
}
static int rbs_filereq_peer = -1;               /* whose file request rb_filereq_* hold */

/* What a save state leaves out of a copy: its clock, the processor's time stamp counter, the VGA frame clock. */
struct RBCExtras {
    uint8_t clock[16];
    uint16_t year = 0;
    uint8_t month = 0, day = 0;
    uint64_t tsc = 0;
    double picbase = 0, vfps = 0, vbase = 0;
    int32_t vframes = 0;
    uint8_t draw = 0;               /* VGA_GetOnDemandFlags */
};
static const size_t RBC_EXTRAS_SIZE = 16 + 2 + 1 + 1 + 8 + 8 + 8 + 8 + 4 + 1;

static void RBC_GetExtras(RBCExtras &x) {
    CMOS_GetSharedClock(x.clock);
    x.year = dos.date.year; x.month = dos.date.month; x.day = dos.date.day;
    x.tsc = rdtsc_count_base;
    x.picbase = (double)rdtsc_pic_base;
    x.vfps = vga_fps; x.vbase = vga_mode_time_base; x.vframes = (int32_t)vga_mode_frames_since_time_base;
    x.draw = VGA_GetOnDemandFlags();
}

static void RBC_SetExtras(const RBCExtras &x) {
    CMOS_SetSharedClock(x.clock);
    dos.date.year = x.year; dos.date.month = x.month; dos.date.day = x.day;
    rdtsc_count_base = x.tsc;
    rdtsc_pic_base = (pic_tickindex_t)x.picbase;
    vga_fps = x.vfps; vga_mode_time_base = x.vbase; vga_mode_frames_since_time_base = (int)x.vframes;
    VGA_SetOnDemandFlags(x.draw);
}

static void RBC_Put(std::vector<uint8_t> &out, const void *v, size_t n) { const uint8_t *b = (const uint8_t*)v; out.insert(out.end(), b, b + n); }
static bool RBC_Get(const uint8_t *&p, const uint8_t *end, void *v, size_t n) { if ((size_t)(end - p) < n) return false; memcpy(v, p, n); p += n; return true; }

static void RBC_PutExtras(std::vector<uint8_t> &out, const RBCExtras &x) {
    RBC_Put(out, x.clock, 16); RBC_Put(out, &x.year, 2); RBC_Put(out, &x.month, 1); RBC_Put(out, &x.day, 1);
    RBC_Put(out, &x.tsc, 8); RBC_Put(out, &x.picbase, 8); RBC_Put(out, &x.vfps, 8); RBC_Put(out, &x.vbase, 8); RBC_Put(out, &x.vframes, 4);
    RBC_Put(out, &x.draw, 1);
}

static bool RBC_GetExtrasFrom(const uint8_t *&p, const uint8_t *end, RBCExtras &x) {
    return RBC_Get(p, end, x.clock, 16) && RBC_Get(p, end, &x.year, 2) && RBC_Get(p, end, &x.month, 1) && RBC_Get(p, end, &x.day, 1) &&
        RBC_Get(p, end, &x.tsc, 8) && RBC_Get(p, end, &x.picbase, 8) && RBC_Get(p, end, &x.vfps, 8) && RBC_Get(p, end, &x.vbase, 8) &&
        RBC_Get(p, end, &x.vframes, 4) && RBC_Get(p, end, &x.draw, 1);
}

/* ---- the link between a computer's copies ---- */

static bool RBC_Send(const IPaddress &to, const uint8_t *data, size_t len) {
    if (rbc_sock == NULL || rbc_out == NULL || len > (size_t)rbc_out->maxlen) return false;
    memcpy(rbc_out->data, data, len);
    rbc_out->len = (int)len;
    rbc_out->address = to;
    return SDLNet_UDP_Send(rbc_sock, -1, rbc_out) != 0;
}

static void RBC_BeginPacket(InputNetWriter &w, uint8_t type) {
    w.u32(INPUTNET_MAGIC);
    w.u8((uint8_t)INPUTNET_PROTOVER);
    w.u8(type);
    w.u8((uint8_t)rbc_copy);
    w.u8(0);
}

static void RBC_Dispatch(void);

/* Everything that has come in; with "wait_ms", waits up to that long for the first packet. */
static void RBC_Poll(unsigned int wait_ms = 0) {
    if (rbc_sock == NULL) return;
    if (wait_ms && rbc_set != NULL) SDLNet_CheckSockets(rbc_set, wait_ms);
    while (SDLNet_UDP_Recv(rbc_sock, rbc_in) > 0) RBC_Dispatch();
}

/* Leader: the followers know it is there (a follower gives up after a long silence). */
static void RBC_Beat(void) {
    const uint32_t now = GetTicks();
    if (now - rbc_last_beat < 500u) return;
    rbc_last_beat = now;
    uint8_t buf[16];
    InputNetWriter w(buf, sizeof(buf));
    RBC_BeginPacket(w, INPKT_CL_WELCOME);
    for (unsigned c = 0; c < rbc_copies; c++) if (c != rbc_copy && rbc_f[c].present) RBC_Send(rbc_f[c].addr, buf, w.size());
}

static void RBC_PollLeader(void) {
    if (!rbc_copies || !rbc_leader || rbc_sock == NULL) return;
    RBC_Poll();
    RBC_Beat();
}

static void RBC_SendBlobAck(const IPaddress &to) {
    uint8_t buf[32];
    InputNetWriter w(buf, sizeof(buf));
    RBC_BeginPacket(w, INPKT_CL_BULKACK);
    w.u32(rbc_blob_id);
    w.u32(rbc_blob_have);
    RBC_Send(to, buf, w.size());
}

static void RBC_Dispatch(void) {
    InputNetReader r((const uint8_t*)rbc_in->data, (size_t)rbc_in->len);
    if (r.u32() != INPUTNET_MAGIC || r.u8() != (uint8_t)INPUTNET_PROTOVER) return;
    const uint8_t type = r.u8();
    const unsigned from = r.u8();
    r.u8();
    if (r.bad()) return;
    const IPaddress addr = rbc_in->address;
    if (addr.host != 0x0100007Fu) return;               /* 127.0.0.1 only */
    if (!rbc_leader) {
        if (addr.port != rbc_leader_addr.port) return;  /* the leader only */
        rbc_last_heard = GetTicks();
    }
    switch (type) {
        case INPKT_CL_HELLO: {
            if (!rbc_leader || from >= rbc_copies || from == rbc_copy) return;
            RBCFollower &f = rbc_f[from];
            if (!f.present) LOG_MSG("INPUTNET: cluster: the copy for player %u is here", from + 1u);
            f.present = true;
            f.addr = addr;
            uint8_t buf[16];
            InputNetWriter w(buf, sizeof(buf));
            RBC_BeginPacket(w, INPKT_CL_WELCOME);
            RBC_Send(addr, buf, w.size());
            break;
        }
        case INPKT_CL_WELCOME:                          /* (also the leader's heartbeat) */
            if (!rbc_leader) rbcf_welcomed = true;
            break;
        case INPKT_CL_CMD: {
            RBCCmd c;
            c.seq = r.u32(); c.cmd = r.u8(); c.a = r.u32(); c.b = r.u32(); c.c = r.u32();
            if (r.bad() || rbc_leader) return;
            if (c.seq > rbcf_last_cmd) { rbcf_cmds.push_back(c); rbcf_last_cmd = c.seq; }
            uint8_t buf[24];
            InputNetWriter w(buf, sizeof(buf));
            RBC_BeginPacket(w, INPKT_CL_CMDACK);
            w.u32(c.seq);
            w.u8(0);                                    /* received */
            RBC_Send(addr, buf, w.size());
            break;
        }
        case INPKT_CL_CMDACK: {
            const uint32_t seq = r.u32();
            const uint8_t done = r.u8();
            if (r.bad() || !rbc_leader || from >= rbc_copies) return;
            const unsigned t = (seq == rbc_inflight_seq && rbc_inflight_c < rbc_copies) ? rbc_inflight_c : from;
            if (seq > rbc_f[t].got) rbc_f[t].got = seq;
            if (done && seq > rbc_f[t].done) rbc_f[t].done = seq;
            break;
        }
        case INPKT_CL_BULK: {
            const uint32_t id = r.u32(), total = r.u32(), off = r.u32();
            const unsigned len = r.u16();
            const uint8_t *data = len ? r.bytes(len) : NULL;
            if (r.bad() || total > (512u << 20) || off > total || len > total - off || (off % RBC_CHUNK) != 0) return;
            if (id != rbc_blob_id) {                    /* a new blob */
                rbc_blob_id = id; rbc_blob_total = total; rbc_blob_have = 0; rbc_blob_done = (total == 0);
                rbc_blob.assign(total, 0);
                rbc_blob_got.assign((total + RBC_CHUNK - 1u) / RBC_CHUNK, false);
            }
            if (total != rbc_blob_total) return;
            rbc_blob_from = addr;
            const uint32_t idx = off / RBC_CHUNK;
            if (idx < rbc_blob_got.size() && !rbc_blob_got[idx]) {
                if (len) memcpy(&rbc_blob[off], data, len);
                rbc_blob_got[idx] = true;
                while (rbc_blob_have < rbc_blob_total && rbc_blob_got[rbc_blob_have / RBC_CHUNK])
                    rbc_blob_have = std::min(rbc_blob_total, rbc_blob_have + RBC_CHUNK);
                if (rbc_blob_have >= rbc_blob_total) rbc_blob_done = true;
            }
            RBC_SendBlobAck(addr);
            break;
        }
        case INPKT_CL_BULKACK: {
            const uint32_t id = r.u32(), have = r.u32();
            if (r.bad()) return;
            if (id == rbc_ack_id && have > rbc_ack_have) rbc_ack_have = have;
            break;
        }
        case INPKT_CL_AT: {
            const uint32_t frame = r.u32(), epoch = r.u32(), hi = r.u32(), lo = r.u32();
            const uint8_t flags = r.u8();
            const uint32_t lhi = r.u32(), llo = r.u32();
            const unsigned n = r.u16();
            if (r.bad() || !rbc_leader || from >= rbc_copies || from == rbc_copy) return;
            RBCFollower &f = rbc_f[from];
            if (!f.present || epoch < rbc_epoch) return;
            if (f.go_frame == frame && f.go_epoch == epoch && !f.go.empty()) {   /* its GO was lost: again */
                RBC_Send(f.addr, &f.go[0], f.go.size());
                return;
            }
            std::vector<std::vector<uint8_t> > out;
            for (unsigned i = 0; i < n; i++) {
                const unsigned len = r.u16();
                const uint8_t *b = r.bytes(len);
                if (r.bad() || b == NULL) return;
                out.push_back(std::vector<uint8_t>(b, b + len));
            }
            std::vector<uint8_t> ser;
            uint8_t lines = 0;
            if (r.remain() >= 3) {
                const unsigned ns = r.u16();
                const uint8_t *b = ns ? r.bytes(ns) : NULL;
                lines = r.u8();
                if (r.bad()) return;
                if (ns) ser.assign(b, b + ns);
            }
            if (r.remain() >= 4) { const uint32_t cap = r.u32(); if (!r.bad()) { f.cap = cap; f.cap_at = GetTicks(); } }
            f.at_ser.swap(ser); f.at_lines = lines;
            f.at_frame = frame; f.at_epoch = epoch; f.at_flags = flags;
            f.at_hash = ((uint64_t)hi << 32u) | lo;
            f.at_loadable = ((uint64_t)lhi << 32u) | llo;
            f.at_out.swap(out);
            break;
        }
        case INPKT_CL_GO: {
            const uint32_t frame = r.u32(), epoch = r.u32();
            if (r.bad() || rbc_leader) return;
            std::vector<InputNetEvent> evs;
            const unsigned ne = r.u16();
            for (unsigned i = 0; i < ne; i++) { InputNetEvent ev; if (!INPUTNET_ReadEvent(r, ev)) return; evs.push_back(ev); }
            std::vector<std::vector<uint8_t> > pk;
            const unsigned np = r.u16();
            for (unsigned i = 0; i < np; i++) {
                const unsigned len = r.u16();
                const uint8_t *b = r.bytes(len);
                if (r.bad() || b == NULL) return;
                pk.push_back(std::vector<uint8_t>(b, b + len));
            }
            std::vector<uint8_t> ser;
            uint8_t lines = 0;
            if (r.remain() >= 3) {
                const unsigned ns = r.u16();
                const uint8_t *b = ns ? r.bytes(ns) : NULL;
                lines = r.u8();
                if (r.bad()) return;
                if (ns) ser.assign(b, b + ns);
            }
            rbcf_go = true; rbcf_go_frame = frame; rbcf_go_epoch = epoch;
            rbcf_go_events.swap(evs); rbcf_go_packets.swap(pk);
            rbcf_go_ser.swap(ser); rbcf_go_lines = lines;
            break;
        }
        case INPKT_CL_REWIND: {
            const uint32_t target = r.u32(), real = r.u32(), epoch = r.u32();
            if (r.bad() || rbc_leader || epoch <= rbc_epoch) return;
            rbcf_rewind = true; rbcf_rw_target = target; rbcf_rw_real = real; rbcf_rw_epoch = epoch;
            break;
        }
        default:
            break;
    }
}

/* A blob to the other end, in chunks it acknowledges. */
static bool RBC_SendBlob(const IPaddress &to, uint32_t id, const std::vector<uint8_t> &blob) {
    const uint32_t total = (uint32_t)blob.size();
    rbc_ack_id = id;
    rbc_ack_have = 0;
    uint32_t lastprogress = GetTicks(), last_events = GetTicks();
    const uint32_t nchunks = std::max(1u, (total + RBC_CHUNK - 1u) / RBC_CHUNK);
    bool first = true;
    while (rbc_ack_have < total || (total == 0 && first)) {
        const uint32_t from = rbc_ack_have / RBC_CHUNK;
        for (uint32_t c = from; c < nchunks && c < from + 64u; c++) {
            const uint32_t off = c * RBC_CHUNK, len = (total > off) ? std::min(RBC_CHUNK, total - off) : 0u;
            static uint8_t buf[RBC_CHUNK + 64];
            InputNetWriter w(buf, sizeof(buf));
            RBC_BeginPacket(w, INPKT_CL_BULK);
            w.u32(id); w.u32(total); w.u32(off); w.u16((uint16_t)len);
            if (len) w.bytes(&blob[off], len);
            RBC_Send(to, buf, w.size());
        }
        first = false;
        if (total == 0) break;
        const uint32_t before = rbc_ack_have, t0 = GetTicks();
        while (GetTicks() - t0 < 30u && rbc_ack_have == before) RBC_Poll(2);
        if (rbc_ack_have != before) lastprogress = GetTicks();
        if (GetTicks() - lastprogress > 15000u) { LOG_MSG("INPUTNET: cluster: a transfer between the copies timed out"); return false; }
        if (GetTicks() - last_events >= 10u) { GFX_Events(); last_events = GetTicks(); }
    }
    return true;
}

/* Waits for the blob "id" from the other end. */
static bool RBC_ReceiveBlob(uint32_t id, std::vector<uint8_t> &out, uint32_t timeout_ms) {
    const uint32_t start = GetTicks();
    uint32_t last_events = start, lastack = 0;
    for (;;) {
        RBC_Poll(2);
        if (rbc_blob_id == id && rbc_blob_done) {
            out.swap(rbc_blob);
            std::vector<uint8_t>().swap(rbc_blob);
            rbc_blob_done = false;
            return true;
        }
        const uint32_t now = GetTicks();
        if (now - start > timeout_ms) { LOG_MSG("INPUTNET: cluster: a transfer between the copies timed out"); return false; }
        if (rbc_blob_id == id && now - lastack > 50u) { RBC_SendBlobAck(rbc_blob_from); lastack = now; }
        if (now - last_events >= 10u) { GFX_Events(); last_events = now; }
    }
}

/* Leader: a command for follower "c" (a, b, cc its numbers). The payload blob goes after the follower received the
 * command, the reply blob comes back under the command's number. True once it has carried the command out. */
static bool RBC_Command(unsigned c, uint8_t cmd, uint32_t a, uint32_t b, uint32_t cc, const std::vector<uint8_t> *payload, std::vector<uint8_t> *reply) {
    RBCFollower &f = rbc_f[c];
    if (!f.present) return false;
    const uint32_t seq = ++rbc_seq;
    rbc_inflight_seq = seq; rbc_inflight_c = c;
    uint32_t start = GetTicks(), lastsend = 0, last_events = start;
    while (f.got < seq) {
        const uint32_t now = GetTicks();
        if (lastsend == 0 || now - lastsend > 50u) {
            uint8_t buf[40];
            InputNetWriter w(buf, sizeof(buf));
            RBC_BeginPacket(w, INPKT_CL_CMD);
            w.u32(seq); w.u8(cmd); w.u32(a); w.u32(b); w.u32(cc);
            RBC_Send(f.addr, buf, w.size());
            lastsend = now ? now : 1u;
        }
        if (now - start > 20000u) { LOG_MSG("INPUTNET: cluster: the copy for player %u does not answer", c + 1u); return false; }
        RBC_Poll(2);
        if (now - last_events >= 10u) { GFX_Events(); last_events = now; }
    }
    if (payload != NULL && !RBC_SendBlob(f.addr, seq, *payload)) return false;
    if (reply != NULL && !RBC_ReceiveBlob(seq, *reply, 60000u)) return false;
    start = GetTicks();
    while (f.done < seq) {
        if (GetTicks() - start > 60000u) { LOG_MSG("INPUTNET: cluster: the copy for player %u did not finish", c + 1u); return false; }
        RBC_Poll(2);
        if (GetTicks() - last_events >= 10u) { GFX_Events(); last_events = GetTicks(); }
    }
    return true;
}

static bool RBC_OpenSocket(uint16_t port) {
    if (rbc_sock != NULL) return true;
    if (!SDLNetInited) {
        if (SDLNet_Init() == -1) { LOG_MSG("INPUTNET: cluster: SDLNet_Init failed: %s", SDLNet_GetError()); return false; }
        SDLNetInited = true;
    }
    rbc_sock = SDLNet_UDP_Open(port);
    if (rbc_sock == NULL) { LOG_MSG("INPUTNET: cluster: cannot open UDP port %u: %s", (unsigned)port, SDLNet_GetError()); return false; }
    if (rbc_in == NULL) rbc_in = SDLNet_AllocPacket(RBC_MAXPKT);
    if (rbc_out == NULL) rbc_out = SDLNet_AllocPacket(RBC_MAXPKT);
    if (rbc_set == NULL) rbc_set = SDLNet_AllocSocketSet(1);
    if (rbc_set != NULL) SDLNet_UDP_AddSocket(rbc_set, rbc_sock);
    return rbc_in != NULL && rbc_out != NULL;
}

/* Leader, at its DOS prompt before the network session: the followers come (RBC_FollowerTick). */
static void RBC_LeaderGather(void) {
    if (rbc_copies < 2) { rbc_ready = true; return; }   /* (the game alone: nobody to wait for) */
    static bool failed = false;
    if (rbc_sock == NULL) {
        if (failed) return;
        if (!RBC_OpenSocket((uint16_t)rbc_port)) { failed = true; return; }
        for (unsigned k = 0; k < RB_RING; k++) rbc_inbox_frame[k] = UINT32_MAX;
        LOG_MSG("INPUTNET: cluster: player %u's copy waits for the other %u on port %u", rbc_copy + 1u, rbc_copies - 1u, rbc_port);
    }
    RBC_Poll();
    unsigned here = 1;
    for (unsigned c = 0; c < rbc_copies; c++) if (c != rbc_copy && rbc_f[c].present) here++;
    if (here < rbc_copies) return;
    rbc_ready = true;
    LOG_MSG("INPUTNET: cluster of %u copies ready (this one is player %u's)", rbc_copies, rbc_copy + 1u);
}

/* Leader: copy c's input and packets at "frame". */
static void RBC_SendGo(unsigned c, uint32_t frame, const std::vector<InputNetEvent> &events, const std::vector<std::vector<uint8_t> > &packets,
                       const std::vector<uint8_t> &ser, uint8_t lines) {
    RBCFollower &f = rbc_f[c];
    if (!f.present) return;
    std::vector<uint8_t> buf(RBC_MAXPKT);
    InputNetWriter w(&buf[0], buf.size());
    RBC_BeginPacket(w, INPKT_CL_GO);
    w.u32(frame); w.u32(rbc_epoch);
    w.u16((uint16_t)events.size());
    for (size_t i = 0; i < events.size(); i++) INPUTNET_WriteEvent(w, events[i]);
    w.u16((uint16_t)packets.size());
    for (size_t i = 0; i < packets.size(); i++) { w.u16((uint16_t)packets[i].size()); w.bytes(&packets[i][0], packets[i].size()); }
    w.u16((uint16_t)ser.size());
    if (!ser.empty()) w.bytes(&ser[0], ser.size());
    w.u8(lines);
    if (w.overflow()) { LOG_MSG("INPUTNET: cluster: too much for one frame of the copy for player %u", c + 1u); return; }
    buf.resize(w.size());
    f.go.swap(buf);
    f.go_frame = frame; f.go_epoch = rbc_epoch;
    RBC_Send(f.addr, &f.go[0], f.go.size());
}

/* (At the DOS prompt only the server's autorun types: RB_ApplyInputs.) */
static inline bool RB_GuardDrops(const InputNetEvent &e) {
    return dos_shell_reading_input && e.type == INEV_KEY && (e.pressed & 1u) && !(e.pressed & 2u);
}

/* Leader: everyone's input for "frame", each participant's to the copy it plays (rb_seat; the server's autorun typing to every copy), then
 * this copy's packets and input, and every follower's GO. A joining computer runs its own typing only. */
static void RBC_ApplyInputs(uint32_t frame) {
    std::vector<InputNetEvent> ev[INPUTNET_MAX_PEERS];
    if (rb_joining) {
        if (rbj_typing_pos < rbj_typing.size()) {
            for (unsigned c = 0; c < rbc_copies; c++) ev[c] = rbj_typing[rbj_typing_pos];
            rbj_typing_pos++;
        }
    }
    else for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready) continue;
        if (!p.local) {
            if (frame >= rb_have[i]) { rb_predicted[i][frame % RB_RING] = frame + 1u; continue; }
            rb_predicted[i][frame % RB_RING] = 0;
        }
        InputNetFrame *slot = INPUTNET_FrameSlotIfValid(p, frame);
        if (slot == NULL) continue;
        for (size_t e = 0; e < slot->events.size(); e++) {
            const InputNetEvent &x = slot->events[e];
            if ((x.type == INEV_KEY && (x.pressed & 2u)) || x.type == INEV_CYCLEMAX) { for (unsigned c = 0; c < rbc_copies; c++) ev[c].push_back(x); }
            else if (rbc_copies == 1) ev[0].push_back(x);   /* (the game alone: everybody plays it) */
            else { const int c = RB_CopyOf(rb_seat, i); if (c >= 0) ev[c].push_back(x); }
        }
    }
    const unsigned k = frame % RB_RING;
    static const std::vector<std::vector<uint8_t> > none;
    static const std::vector<uint8_t> noser;
    const bool have = (rbc_inbox_frame[k] == frame);
    if (have) {
        for (size_t i = 0; i < rbc_inbox[k][rbc_copy].size(); i++) IPX_ClusterDeliver(&rbc_inbox[k][rbc_copy][i][0], rbc_inbox[k][rbc_copy][i].size());
        const std::vector<uint8_t> &ser = rbc_ser_inbox[k][rbc_copy];
        if (!ser.empty() || rbc_ser_lines[k][rbc_copy]) SERIAL_RBDeliver(ser.empty() ? NULL : &ser[0], ser.size(), rbc_ser_lines[k][rbc_copy]);
    }
    for (size_t i = 0; i < ev[rbc_copy].size(); i++) if (!RB_GuardDrops(ev[rbc_copy][i])) INPUTNET_ApplyEvent(ev[rbc_copy][i]);
    for (unsigned c = 0; c < rbc_copies; c++)
        if (c != rbc_copy) RBC_SendGo(c, frame, ev[c], have ? rbc_inbox[k][c] : none, have ? rbc_ser_inbox[k][c] : noser, have ? rbc_ser_lines[k][c] : 0);
}

/* Which saved frames before "frame" this copy could load now (bit i: frame - 1 - i). */
static uint64_t RBC_Loadable(uint32_t frame) {
    const uint64_t ctx = RB_HostContext();
    uint64_t bits = 0;
    for (uint32_t i = 0; i < 64u && i < frame; i++) {
        const uint32_t f = frame - 1u - i;
        const RBSnap &s = rb_snap[f % RB_RING];
        if (s.valid && s.frame == f && s.context == ctx) bits |= 1ull << i;
    }
    return bits;
}

/* Leader: whether every follower could load frame "f" (they said so at the frame start "now"). */
static bool RBC_FollowersCanLoad(uint32_t f, uint32_t now) {
    for (unsigned c = 0; c < rbc_copies; c++) {
        if (c == rbc_copy || !rbc_f[c].present) continue;
        if (rbc_f[c].at_frame != now) return false;
        if (f == now) continue;
        if (f > now) return false;
        const uint32_t i = now - 1u - f;
        if (i >= 64u || !((rbc_f[c].at_loadable >> i) & 1u)) return false;
    }
    return true;
}

/* This copy's host stack is as it was at the frame start before "frame" (a machine saved now can be loaded by one
 * that is inside the same loops). */
static bool RBC_SelfStable(uint32_t frame) {
    if (frame == 0) return false;
    const RBSnap &s = rb_snap[(frame - 1u) % RB_RING];
    return s.valid && s.frame == frame - 1u && s.context == RB_HostContext();
}

/* Leader: every copy at "frame" is stable; "prompt" gets whether each is at its DOS prompt. */
static bool RBC_AllStable(uint32_t frame, uint8_t *prompt = NULL) {
    if (!RBC_SelfStable(frame)) return false;
    if (prompt != NULL) prompt[rbc_copy] = dos_shell_reading_input ? 1 : 0;
    for (unsigned c = 0; c < rbc_copies; c++) {
        if (c == rbc_copy) continue;
        if (!rbc_f[c].present || rbc_f[c].at_frame != frame || !(rbc_f[c].at_flags & RBC_AT_STABLE)) return false;
        if (prompt != NULL) prompt[c] = (rbc_f[c].at_flags & RBC_AT_PROMPT) ? 1 : 0;
    }
    return true;
}

/* Leader, at a frame start: every follower there (RBC_REWIND: put back to "frame" first; RBC_HOLD: at the epoch it
 * moved to by a command), their packets of the frame before routed (RBC_ROUTE only), and the cluster's checksum.
 * False when a copy stopped answering (the session is over then). */
static bool RBC_LeaderBarrier(uint32_t frame, int mode) {
    RBWaitTimer waited;             /* (the copies report how busy they are themselves) */
    std::vector<std::vector<uint8_t> > own;
    std::vector<uint8_t> own_ser;
    uint8_t own_lines = 0;
    if (mode == RBC_ROUTE) { IPX_ClusterTakeOutbox(own); SERIAL_RBTakeOutbox(own_ser, own_lines); }
    const uint32_t start = GetTicks();
    uint32_t last_events = start, lastrw = 0;
    for (;;) {
        bool all = true;
        for (unsigned c = 0; c < rbc_copies; c++) {
            if (c == rbc_copy || !rbc_f[c].present) continue;
            if (rbc_f[c].at_frame != frame || rbc_f[c].at_epoch != rbc_epoch) { all = false; break; }
        }
        if (all) break;
        const uint32_t now = GetTicks();
        if (mode == RBC_REWIND && (lastrw == 0 || now - lastrw >= 5u)) {
            for (unsigned c = 0; c < rbc_copies; c++) {
                if (c == rbc_copy || !rbc_f[c].present || rbc_f[c].at_epoch == rbc_epoch) continue;
                uint8_t buf[32];
                InputNetWriter w(buf, sizeof(buf));
                RBC_BeginPacket(w, INPKT_CL_REWIND);
                w.u32(frame); w.u32(inputnet_rollback_resim ? rb_target : frame); w.u32(rbc_epoch);
                RBC_Send(rbc_f[c].addr, buf, w.size());
            }
            lastrw = now ? now : 1u;
        }
        if (now - start > 20000u) {
            LOG_MSG("INPUTNET: cluster: a copy stopped at frame %u", (unsigned)frame);
            INPUTNET_EndSession("a copy of the game stopped answering", true);
            return false;
        }
        RBC_Poll(1);
        RBC_Beat();
        if (now - last_events >= 10u) { GFX_Events(); last_events = now; }
    }
    const unsigned k = frame % RB_RING;
    if (mode == RBC_ROUTE) {
        for (unsigned c = 0; c < rbc_copies; c++) { rbc_inbox[k][c].clear(); rbc_ser_inbox[k][c].clear(); rbc_ser_lines[k][c] = 0; }
        if (!rb_joining && rbc_copies >= 2) {   /* the null modem links copies 0 and 1 */
            for (unsigned src = 0; src < 2; src++) {
                const unsigned dst = 1u - src;
                rbc_ser_inbox[k][dst] = (src == rbc_copy) ? own_ser : rbc_f[src].at_ser;
                rbc_ser_lines[k][dst] = (src == rbc_copy) ? own_lines : rbc_f[src].at_lines;
            }
        }
        if (!rb_joining) {                  /* (a joining computer's copies stay apart until they take the server's) */
            for (unsigned src = 0; src < rbc_copies; src++) {
                const std::vector<std::vector<uint8_t> > &out = (src == rbc_copy) ? own : rbc_f[src].at_out;
                for (size_t i = 0; i < out.size(); i++) {
                    const std::vector<uint8_t> &pkt = out[i];
                    if (pkt.size() < 30) continue;
                    const uint8_t *node = &pkt[10];          /* the IPX header's destination node */
                    bool bcast = true;
                    for (unsigned b = 0; b < 6; b++) if (node[b] != 0xFF) bcast = false;
                    for (unsigned dst = 0; dst < rbc_copies; dst++) {
                        if (dst == src) continue;   /* (the sender has its own broadcasts already: ipx.cpp sendPacket) */
                        const bool mine = node[0] == 10 && node[1] == 0 && node[2] == 0 && node[3] == dst + 1u && node[4] == 0x55 && node[5] == 0x55;
                        if (bcast || mine) rbc_inbox[k][dst].push_back(pkt);
                    }
                }
            }
        }
        rbc_inbox_frame[k] = frame;
    }
    RBSnap &s = rb_snap[k];
    if (s.valid && s.frame == frame) {
        uint64_t h = 1469598103934665603ull;
        for (unsigned c = 0; c < rbc_copies; c++) {
            const uint64_t hc = (c == rbc_copy) ? s.own : (rbc_f[c].present ? rbc_f[c].at_hash : 0u);
            h = (h ^ hc) * 1099511628211ull;
            h ^= h >> 29u;
        }
        s.hash = h;
    }
    return true;
}

/* Leader, having gone back to "target" itself: the followers too. */
static void RBC_LeaderRewind(uint32_t target) {
    rbc_epoch++;
    RBC_LeaderBarrier(target, RBC_REWIND);
}

/* Leader: the session starts at "frame"; so do the followers. */
static void RBC_LeaderStart(uint32_t frame, bool joining) {
    rbc_epoch = 0;
    for (unsigned k = 0; k < RB_RING; k++) rbc_inbox_frame[k] = UINT32_MAX;
    for (unsigned c = 0; c < rbc_copies; c++) {
        if (c == rbc_copy) continue;
        RBCFollower &f = rbc_f[c];
        f.at_frame = UINT32_MAX; f.at_epoch = 0; f.go_frame = UINT32_MAX; f.go.clear(); f.at_out.clear();
        if (f.present && !RBC_Command(c, RBC_CMD_START, frame, joining ? 1u : 0u, 0, NULL, NULL))
            LOG_MSG("INPUTNET: cluster: the copy for player %u did not start", c + 1u);
    }
}

/* Leader: the session is over; so it is for the followers (they do not wait for it). */
static void RBC_LeaderEnd(void) {
    for (unsigned c = 0; c < rbc_copies; c++) {
        if (c == rbc_copy || !rbc_f[c].present) continue;
        const uint32_t seq = ++rbc_seq;
        uint8_t buf[40];
        InputNetWriter w(buf, sizeof(buf));
        RBC_BeginPacket(w, INPKT_CL_CMD);
        w.u32(seq); w.u8(RBC_CMD_END); w.u32(0); w.u32(0); w.u32(0);
        for (unsigned i = 0; i < 3; i++) RBC_Send(rbc_f[c].addr, buf, w.size());
    }
}

/* ---- follower ---- */

static void RBC_FollowerDone(uint32_t seq) {
    uint8_t buf[24];
    InputNetWriter w(buf, sizeof(buf));
    RBC_BeginPacket(w, INPKT_CL_CMDACK);
    w.u32(seq); w.u8(1);
    for (unsigned i = 0; i < 3; i++) RBC_Send(rbc_leader_addr, buf, w.size());
}

/* A manifest with each file's host path (for the other copies on the same computer). */
static void RBC_PutManifestFull(std::vector<uint8_t> &out, const std::vector<RBFile> &m) {
    const uint32_t count = (uint32_t)m.size();
    RBC_Put(out, &count, 4);
    for (size_t i = 0; i < m.size(); i++) {
        const uint16_t len = (uint16_t)m[i].path.size(), hlen = (uint16_t)m[i].host.size();
        const uint8_t mapped = m[i].mapped ? 1 : 0;
        RBC_Put(out, &m[i].drive, 1); RBC_Put(out, &len, 2); RBC_Put(out, m[i].path.data(), len);
        RBC_Put(out, &m[i].size, 8); RBC_Put(out, &m[i].hash, 8);
        RBC_Put(out, &mapped, 1); RBC_Put(out, &hlen, 2); RBC_Put(out, m[i].host.data(), hlen);
    }
}

static bool RBC_TakeManifestFull(const uint8_t *&p, const uint8_t *end, std::vector<RBFile> &m) {
    uint32_t count = 0;
    if (!RBC_Get(p, end, &count, 4) || count > 100000u) return false;
    m.clear();
    for (uint32_t i = 0; i < count; i++) {
        RBFile e; uint16_t len = 0, hlen = 0; uint8_t mapped = 0;
        if (!RBC_Get(p, end, &e.drive, 1) || !RBC_Get(p, end, &len, 2) || (size_t)(end - p) < len) return false;
        e.path.assign((const char*)p, len); p += len;
        if (!RBC_Get(p, end, &e.size, 8) || !RBC_Get(p, end, &e.hash, 8) || !RBC_Get(p, end, &mapped, 1) ||
            !RBC_Get(p, end, &hlen, 2) || (size_t)(end - p) < hlen) return false;
        e.host.assign((const char*)p, hlen); p += hlen;
        e.mapped = mapped != 0;
        m.push_back(e);
    }
    return true;
}

/* Loads a copy's machine at "h.frame" (the leader's, or a follower's), then saves that frame start: the extras and
 * the IPX driver's state go back after the save state. */
static bool RBC_LoadCopy(const RBResyncInfo &h, const std::vector<uint8_t> &state, const uint8_t *ipx, size_t ipxlen, const RBCExtras &x) {
    RBResyncInfo hh = h;
    hh.vfps = x.vfps; hh.vbase = x.vbase; hh.vframes = x.vframes; hh.draw = x.draw;
    return RB_LoadResyncState(hh, state, [&]() {
        RBC_SetExtras(x);
        if (!RB_LoadLinks(ipx, ipxlen)) LOG_MSG("INPUTNET: cluster: the links' state (IPX, null modem) did not load");
    });
}

/* Follower: the leader's commands. At a frame start ("rbcf_cur" set) or, outside a session, between timer ticks. */
static void RBC_FollowerCommands(void) {
    while (!rbcf_cmds.empty()) {
        const RBCCmd c = rbcf_cmds.front();
        rbcf_cmds.erase(rbcf_cmds.begin());
        switch (c.cmd) {
            case RBC_CMD_SAVE: {            /* at frame start c.a: this copy's machine, then the same loaded back */
                if (!inputnet_session || rbcf_cur != c.a) { LOG_MSG("INPUTNET: cluster: asked to save frame %u at %u", (unsigned)c.a, (unsigned)rbcf_cur); break; }
                std::vector<uint8_t> state, blob;
                if (!INPUTNET_SnapshotState(state)) state.clear();
                std::string ipx;
                RB_SaveLinks(ipx);
                RBCExtras x;
                RBC_GetExtras(x);
                const uint32_t sl = (uint32_t)state.size(), il = (uint32_t)ipx.size();
                const uint8_t prompt = dos_shell_reading_input ? 1 : 0;
                RBC_Put(blob, &sl, 4); if (sl) RBC_Put(blob, &state[0], sl);
                RBC_Put(blob, &il, 4); if (il) RBC_Put(blob, ipx.data(), il);
                RBC_Put(blob, &prompt, 1);
                RBC_PutExtras(blob, x);
                RBC_SendBlob(rbc_leader_addr, c.seq, blob);
                if (sl) {
                    RBResyncInfo h;
                    h.frame = c.a; h.tick = rb_tick; h.epoch = c.c;
                    if (!RBC_LoadCopy(h, state, (const uint8_t*)ipx.data(), ipx.size(), x)) LOG_MSG("INPUTNET: cluster: could not load this copy's own state");
                }
                rbc_epoch = c.b;
                rbcf_reloaded = true;
                break;
            }
            case RBC_CMD_MANIFEST: {        /* this copy's files */
                std::vector<RBFile> m;
                std::vector<uint8_t> blob;
                RB_BuildManifest(m);
                RBC_PutManifestFull(blob, m);
                RBC_SendBlob(rbc_leader_addr, c.seq, blob);
                break;
            }
            case RBC_CMD_FILEMAP: {         /* the server's files for this copy, written by the leader */
                std::vector<uint8_t> blob;
                if (!RBC_ReceiveBlob(c.seq, blob, 60000u)) break;
                const uint8_t *p = blob.empty() ? NULL : &blob[0], *end = p + blob.size();
                std::vector<RBFile> m;
                if (p == NULL || !RBC_TakeManifestFull(p, end, m)) break;
                for (size_t i = 0; i < m.size(); i++) {
                    localDrive *ld = (m[i].drive < DOS_DRIVES) ? dynamic_cast<localDrive*>(Drives[m[i].drive]) : NULL;
                    if (ld == NULL) continue;
                    const size_t bs = m[i].path.find_last_of('\\');
                    if (bs != std::string::npos) {
                        size_t from = 0;
                        while (from <= bs) {
                            const size_t next = m[i].path.find('\\', from);
                            if (next == std::string::npos || next > bs) break;
                            const std::string parent = m[i].path.substr(0, next);
                            if (!ld->TestDir(parent.c_str()) && ld->MakeDir(parent.c_str()))
                                rb_sync_dirs.push_back(std::make_pair((int)m[i].drive, parent));
                            from = next + 1;
                        }
                    }
                    if (ld->AddMountedFile(m[i].path.c_str(), m[i].host.c_str(), true))
                        rb_sync_mapped.push_back(std::make_pair((int)m[i].drive, m[i].path));
                }
                LOG_MSG("INPUTNET: cluster: using %u of the server's files in this copy", (unsigned)m.size());
                break;
            }
            case RBC_CMD_LOADRESYNC: {      /* at a frame start: the server's machine for this copy at frame c.a */
                std::vector<uint8_t> blob;
                if (!RBC_ReceiveBlob(c.seq, blob, 120000u)) break;
                const uint8_t *p = blob.empty() ? NULL : &blob[0], *end = p + blob.size();
                RBResyncInfo h;
                uint32_t sl = 0, il = 0;
                RBCExtras x;
                std::vector<uint8_t> raw(p, end);
                if (p == NULL || !RB_GetResyncInfo(raw, h)) break;
                p += RB_RESYNC_INFO_SIZE;
                if (!RBC_Get(p, end, &sl, 4) || (size_t)(end - p) < sl) break;
                const std::vector<uint8_t> state(p, p + sl); p += sl;
                if (!RBC_Get(p, end, &il, 4) || (size_t)(end - p) < il) break;
                const uint8_t *ipx = p; p += il;
                if (!RBC_GetExtrasFrom(p, end, x)) break;
                if (!RBC_LoadCopy(h, state, ipx, il, x)) { LOG_MSG("INPUTNET: cluster: could not load the server's copy"); break; }
                const uint32_t real = c.b;
                if (real > h.frame) {       /* replay to where real time is, with the leader */
                    inputnet_rollback_resim = true;
                    rb_target = real;
                    MIXER_SetRollbackDiscard(true);
                    DOSBOX_AddTicksRemain((real - h.frame) * inputnet_frame_ms);
                }
                rbc_epoch = c.c;
                rbcf_cur = h.frame;
                rbcf_reloaded = true;
                LOG_MSG("INPUTNET: cluster: took the server's copy for player %u over at frame %u", rbc_copy + 1u, (unsigned)h.frame);
                break;
            }
            case RBC_CMD_START:             /* the session starts at frame c.a */
                if (inputnet_session) { inputnet_session = false; RB_SessionEnded(); }
                for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) { inputnet_peers[i].active = false; inputnet_peers[i].ready = false; }
                rb_start_frame = c.a;
                rbc_epoch = 0;
                rbcf_go = false; rbcf_rewind = false;
                inputnet_session = true;
                inputnet_capture_active = true;  /* this window's own input goes nowhere (INPUTNET_QueueLocal) */
                RB_SessionStarted();
                break;
            case RBC_CMD_END:
                if (inputnet_session) { inputnet_session = false; inputnet_capture_active = false; RB_SessionEnded(); }
                break;
            case RBC_CMD_SETCOPY:           /* this process runs copy c.a from now on (its machine comes with a LOADRESYNC) */
                if (c.a < rbc_copies && c.a != rbc_copy) {
                    LOG_MSG("INPUTNET: cluster: this process runs the copy for player %u from now on (it ran player %u's)", c.a + 1u, rbc_copy + 1u);
                    rbc_copy = c.a;
                    if (IPX_ClusterActive()) IPX_ClusterAttach(rbc_copy);
                }
                break;
            default:
                break;
        }
        RBC_FollowerDone(c.seq);
    }
}

/* Follower, between timer ticks outside a session: the leader, once the DOS prompt is reached, then its commands. */
static void RBC_FollowerTick(void) {
    if (inputnet_session) return;           /* (in a session everything happens at the frame starts) */
    if (rbc_sock == NULL) {
        if (!dos_shell_reading_input) return;
        if (!RBC_OpenSocket(0)) { rbc_copies = 0; return; }
        SDLNet_ResolveHost(&rbc_leader_addr, "127.0.0.1", (uint16_t)rbc_port);
        rbc_last_heard = GetTicks();
        LOG_MSG("INPUTNET: cluster: player %u's copy looks for its leader on port %u", rbc_copy + 1u, rbc_port);
    }
    static uint32_t lasthello = 0;
    if (!rbcf_welcomed && GetTicks() - lasthello > 200u) {
        uint8_t buf[16];
        InputNetWriter w(buf, sizeof(buf));
        RBC_BeginPacket(w, INPKT_CL_HELLO);
        RBC_Send(rbc_leader_addr, buf, w.size());
        lasthello = GetTicks();
    }
    RBC_Poll();
    RBC_FollowerCommands();
}

/* Follower, at a frame start: tell the leader, then do what it says. */
static void RBC_FollowerBoundary(uint32_t frame) {
    inputnet_frame = frame;
    if (inputnet_rollback_resim && frame >= rb_target) {
        inputnet_rollback_resim = false;
        MIXER_SetRollbackDiscard(false);
        extern void RENDER_RedrawAll(void);
        RENDER_RedrawAll();
        RB_ReplayEnd();
    }
    RB_Capture(frame);
    std::vector<std::vector<uint8_t> > out;
    IPX_ClusterTakeOutbox(out);
    std::vector<uint8_t> ser;
    uint8_t lines = 0;
    SERIAL_RBTakeOutbox(ser, lines);
    rbcf_cur = frame;
    rbcf_reloaded = false;
    if (CPU_CycleAutoAdjust && RB_MeasureLoad()) {
        RB_Propose();
        static unsigned told = 0;
        if (++told % 5u == 0)
            LOG_MSG("INPUTNET: cluster copy %u: speed %u cycles per ms, %.0f%% busy (CPU core %.0f%%), replays allow %.0f, proposing %u",
                rbc_copy + 1u, (unsigned)CPU_CycleMax, rb_ml_load * 100.0, rb_ml_core * 100.0, RB_ReplayCap(), rb_cap_local);
    }
    RBWaitTimer waited;             /* (waiting for the leader is not this copy's load) */
    bool with_packets = true;
    uint32_t lastsend = 0, last_events = GetTicks();
    for (;;) {
        const uint32_t now = GetTicks();
        if (lastsend == 0 || now - lastsend >= 4u) {
            std::vector<uint8_t> buf(RBC_MAXPKT);
            InputNetWriter w(&buf[0], buf.size());
            RBC_BeginPacket(w, INPKT_CL_AT);
            const RBSnap &s = rb_snap[rbcf_cur % RB_RING];
            const uint64_t lb = RBC_Loadable(rbcf_cur);
            const uint8_t flags = (RBC_SelfStable(rbcf_cur) ? RBC_AT_STABLE : 0u) | (dos_shell_reading_input ? RBC_AT_PROMPT : 0u);
            w.u32(rbcf_cur); w.u32(rbc_epoch); w.u32((uint32_t)(s.own >> 32u)); w.u32((uint32_t)s.own);
            w.u8(flags); w.u32((uint32_t)(lb >> 32u)); w.u32((uint32_t)lb);
            const size_t n = with_packets ? out.size() : 0u;
            w.u16((uint16_t)n);
            for (size_t i = 0; i < n; i++) { w.u16((uint16_t)out[i].size()); w.bytes(&out[i][0], out[i].size()); }
            const size_t ns = with_packets ? ser.size() : 0u;
            w.u16((uint16_t)ns);
            if (ns) w.bytes(&ser[0], ns);
            w.u8(lines);
            w.u32(rb_cap_local);    /* this copy's speed proposal (RB_MaxCycles) */
            if (!w.overflow()) RBC_Send(rbc_leader_addr, &buf[0], w.size());
            lastsend = now ? now : 1u;
        }
        RBC_Poll(1);
        RBC_FollowerCommands();
        if (!inputnet_session) { rbcf_cur = UINT32_MAX; return; }   /* END */
        if (rbcf_reloaded) { rbcf_reloaded = false; with_packets = false; lastsend = 0; continue; }
        if (rbcf_rewind && rbcf_rw_epoch > rbc_epoch) {
            rbcf_rewind = false;
            rbc_epoch = rbcf_rw_epoch;
            const uint32_t target = rbcf_rw_target;
            if (target <= rbcf_cur && RB_Restore(target, rbcf_cur)) {
                if (rbcf_rw_real > target) {
                    RB_ReplayBegin(rbcf_rw_real - target);
                    inputnet_rollback_resim = true;
                    rb_target = rbcf_rw_real;
                    MIXER_SetRollbackDiscard(true);
                    DOSBOX_AddTicksRemain((rbcf_rw_real - rbcf_cur) * inputnet_frame_ms + (rbcf_cur - target) * inputnet_frame_ms);
                }
                inputnet_frame = target;
                rbcf_cur = target;
            }
            else LOG_MSG("INPUTNET: cluster: the copy for player %u cannot go back to frame %u", rbc_copy + 1u, (unsigned)target);
            with_packets = false;           /* the leader keeps what was routed at "target" */
            lastsend = 0;
            continue;
        }
        if (rbcf_go && rbcf_go_frame == rbcf_cur && rbcf_go_epoch == rbc_epoch) {
            rbcf_go = false;
            for (size_t i = 0; i < rbcf_go_packets.size(); i++) IPX_ClusterDeliver(&rbcf_go_packets[i][0], rbcf_go_packets[i].size());
            if (!rbcf_go_ser.empty() || rbcf_go_lines) SERIAL_RBDeliver(rbcf_go_ser.empty() ? NULL : &rbcf_go_ser[0], rbcf_go_ser.size(), rbcf_go_lines);
            for (size_t i = 0; i < rbcf_go_events.size(); i++) if (!RB_GuardDrops(rbcf_go_events[i])) INPUTNET_ApplyEvent(rbcf_go_events[i]);
            rbcf_cur = UINT32_MAX;
            return;
        }
        if (GetTicks() - rbc_last_heard > 15000u) {   /* (a command may have taken a while since "now") */
            LOG_MSG("INPUTNET: cluster: the leader is gone");
            inputnet_session = false;
            inputnet_capture_active = false;
            RB_SessionEnded();
            rbcf_cur = UINT32_MAX;
            return;
        }
        if (now - last_events >= 10u) { GFX_Events(); last_events = now; }
    }
}

static void RBC_FollowerRollbackTick(void) {
    if (!inputnet_rollback_active || !inputnet_session) return;
    if (!rb_have_first) {
        rb_have_first = true;
        rb_tick = rb_start_frame * inputnet_frame_ms;
        RBC_FollowerBoundary(rb_start_frame);
        return;
    }
    rb_tick++;
    if ((rb_tick % inputnet_frame_ms) == 0) RBC_FollowerBoundary(rb_tick / inputnet_frame_ms);
}

/* ---- the machines of a cluster, for the transfer (RB_ServerResync) ---- */

/* Server, at frame start "frame" (every copy there and stable): every copy's machine, each loaded back (RBC_LoadCopy),
 * as the resync's raw blob: info, this copy's save state, then the cluster block (RBC_CLX_*). */
struct RBCMachine {
    uint8_t prompt = 0;
    const uint8_t *state = NULL; size_t statelen = 0;
    const uint8_t *ipx = NULL; size_t ipxlen = 0;
    RBCExtras x;
    std::vector<std::vector<uint8_t> > inbox;
    std::vector<uint8_t> ser;
    uint8_t ser_lines = 0;
};

/* Splits the resync's raw blob of a cluster: the info, and every copy's machine (pointers into "raw"). */
static bool RBC_ParseMachines(const std::vector<uint8_t> &raw, RBResyncInfo &info, std::vector<RBCMachine> &m) {
    if (!RB_GetResyncInfo(raw, info) || raw.size() < RB_RESYNC_INFO_SIZE + 8u) return false;
    uint32_t clxlen = 0, endmagic = 0;
    memcpy(&clxlen, &raw[raw.size() - 8u], 4); memcpy(&endmagic, &raw[raw.size() - 4u], 4);
    if (endmagic != RBC_CLX_END || (size_t)clxlen + 8u + RB_RESYNC_INFO_SIZE > raw.size()) return false;
    const uint8_t *base = &raw[0];
    const uint8_t *main_state = base + RB_RESYNC_INFO_SIZE;
    const uint8_t *p = base + raw.size() - 8u - clxlen, *end = base + raw.size() - 8u;
    const size_t mainlen = (size_t)(p - main_state);
    uint32_t magic = 0; uint8_t copies = 0, srv = 0;
    if (!RBC_Get(p, end, &magic, 4) || magic != RBC_CLX_MAGIC || !RBC_Get(p, end, &copies, 1) || !RBC_Get(p, end, &srv, 1)) return false;
    if (copies != rbc_copies || srv >= copies) return false;
    m.assign(copies, RBCMachine());
    for (unsigned c = 0; c < copies; c++) {
        RBCMachine &x = m[c];
        uint32_t sl = 0, il = 0;
        uint16_t np = 0;
        if (!RBC_Get(p, end, &x.prompt, 1) || !RBC_Get(p, end, &sl, 4) || (size_t)(end - p) < sl) return false;
        x.state = p; x.statelen = sl; p += sl;
        if (!RBC_Get(p, end, &il, 4) || (size_t)(end - p) < il) return false;
        x.ipx = p; x.ipxlen = il; p += il;
        if (!RBC_GetExtrasFrom(p, end, x.x) || !RBC_Get(p, end, &np, 2)) return false;
        for (uint16_t i = 0; i < np; i++) {
            uint16_t len = 0;
            if (!RBC_Get(p, end, &len, 2) || (size_t)(end - p) < len) return false;
            x.inbox.push_back(std::vector<uint8_t>(p, p + len));
            p += len;
        }
        uint16_t ns = 0;
        if (!RBC_Get(p, end, &ns, 2) || (size_t)(end - p) < ns) return false;
        x.ser.assign(p, p + ns); p += ns;
        if (!RBC_Get(p, end, &x.ser_lines, 1)) return false;
        if (c == srv) { x.state = main_state; x.statelen = mainlen; }
        if (x.statelen == 0) return false;
    }
    return true;
}

/* Leader: this computer's player plays copy "nc" from now on. The follower that ran it runs the leader's former copy
 * instead, and the leader that one (the machines themselves are loaded right after, every copy's into the process
 * that runs it now). */
static bool RBC_SwitchCopy(unsigned nc) {
    const unsigned old = rbc_copy;
    if (nc >= rbc_copies || nc == old || !rbc_f[nc].present) return false;
    if (!RBC_Command(nc, RBC_CMD_SETCOPY, old, 0, 0, NULL, NULL)) return false;
    rbc_f[old] = rbc_f[nc];
    rbc_f[nc] = RBCFollower();
    rbc_copy = nc;
    if (IPX_ClusterActive()) IPX_ClusterAttach(rbc_copy);
    LOG_MSG("INPUTNET: cluster: this computer's player plays copy %u from now on (was %u)", nc + 1u, old + 1u);
    return true;
}

/* A copy's machine for a follower's LOADRESYNC. */
static void RBC_PutLoadPayload(std::vector<uint8_t> &payload, const RBResyncInfo &info, const uint8_t *state, size_t sl, const uint8_t *ipx, size_t il, const RBCExtras &x) {
    RB_PutResyncInfo(payload, info);
    const uint32_t sl32 = (uint32_t)sl, il32 = (uint32_t)il;
    RBC_Put(payload, &sl32, 4); RBC_Put(payload, state, sl);
    RBC_Put(payload, &il32, 4); if (il) RBC_Put(payload, ipx, il);
    RBC_PutExtras(payload, x);
}

static bool RBC_BuildMachines(uint32_t frame, uint16_t epoch, std::vector<uint8_t> &raw, const uint8_t *seats) {
    RBResyncInfo info;
    std::vector<uint8_t> main_state;
    if (!INPUTNET_SnapshotState(main_state)) return false;
    info.frame = frame; info.tick = rb_tick; info.epoch = epoch;
    info.vfps = vga_fps; info.vbase = vga_mode_time_base; info.vframes = (int32_t)vga_mode_frames_since_time_base;
    info.draw = VGA_GetOnDemandFlags();
    std::string own_ipx;
    RB_SaveLinks(own_ipx);
    RBCExtras own_x;
    RBC_GetExtras(own_x);

    std::vector<uint8_t> clx;
    const uint32_t magic = RBC_CLX_MAGIC;
    const uint8_t copies = (uint8_t)rbc_copies, me = (uint8_t)rbc_copy;
    RBC_Put(clx, &magic, 4); RBC_Put(clx, &copies, 1); RBC_Put(clx, &me, 1);
    const uint32_t newepoch = rbc_epoch + 1u;
    const unsigned k = frame % RB_RING;
    for (unsigned c = 0; c < rbc_copies; c++) {
        std::vector<uint8_t> state, ipx;
        uint8_t prompt = 0;
        RBCExtras x;
        if (c == rbc_copy) {
            prompt = dos_shell_reading_input ? 1 : 0;
            ipx.assign(own_ipx.begin(), own_ipx.end());
            x = own_x;
        }
        else {
            std::vector<uint8_t> reply;
            if (!RBC_Command(c, RBC_CMD_SAVE, frame, newepoch, epoch, NULL, &reply)) return false;
            const uint8_t *p = reply.empty() ? NULL : &reply[0], *end = p + reply.size();
            uint32_t sl = 0, il = 0;
            if (p == NULL || !RBC_Get(p, end, &sl, 4) || sl == 0 || (size_t)(end - p) < sl) return false;
            state.assign(p, p + sl); p += sl;
            if (!RBC_Get(p, end, &il, 4) || (size_t)(end - p) < il) return false;
            ipx.assign(p, p + il); p += il;
            if (!RBC_Get(p, end, &prompt, 1) || !RBC_GetExtrasFrom(p, end, x)) return false;
        }
        const uint32_t sl = (uint32_t)state.size(), il = (uint32_t)ipx.size();
        RBC_Put(clx, &prompt, 1);
        RBC_Put(clx, &sl, 4); if (sl) RBC_Put(clx, &state[0], sl);
        RBC_Put(clx, &il, 4); if (il) RBC_Put(clx, &ipx[0], il);
        RBC_PutExtras(clx, x);
        const std::vector<std::vector<uint8_t> > &in = rbc_inbox[k][c];
        const bool have = (rbc_inbox_frame[k] == frame);
        const uint16_t np = have ? (uint16_t)in.size() : 0;
        RBC_Put(clx, &np, 2);
        for (uint16_t i = 0; i < np; i++) { const uint16_t len = (uint16_t)in[i].size(); RBC_Put(clx, &len, 2); RBC_Put(clx, &in[i][0], len); }
        const uint16_t ns = have ? (uint16_t)rbc_ser_inbox[k][c].size() : 0;
        const uint8_t slines = have ? rbc_ser_lines[k][c] : 0;
        RBC_Put(clx, &ns, 2);
        if (ns) RBC_Put(clx, &rbc_ser_inbox[k][c][0], ns);
        RBC_Put(clx, &slines, 1);
    }
    const uint32_t clxlen = (uint32_t)clx.size(), endmagic = RBC_CLX_END;
    RBC_Put(clx, &clxlen, 4); RBC_Put(clx, &endmagic, 4);

    raw.clear();
    RB_PutResyncInfo(raw, info);
    raw.insert(raw.end(), main_state.begin(), main_state.end());
    raw.insert(raw.end(), clx.begin(), clx.end());

    /* this copy goes on from the very bytes it sends (the followers did the same) */
    const int nc = (seats != NULL) ? RB_CopyOf(seats, inputnet_selfid) : -1;
    if (nc >= 0 && (unsigned)nc != rbc_copy) {
        /* the server's player plays another copy now: the leader and the follower that ran it trade machines */
        std::vector<RBCMachine> m;
        RBResyncInfo pinfo;
        if (!RBC_ParseMachines(raw, pinfo, m)) return false;
        const unsigned old = rbc_copy;
        if (!RBC_SwitchCopy((unsigned)nc)) return false;
        if (!RBC_LoadCopy(info, std::vector<uint8_t>(m[nc].state, m[nc].state + m[nc].statelen), m[nc].ipx, m[nc].ipxlen, m[nc].x)) return false;
        std::vector<uint8_t> payload;
        RBC_PutLoadPayload(payload, info, m[old].state, m[old].statelen, m[old].ipx, m[old].ipxlen, m[old].x);
        if (!RBC_Command(old, RBC_CMD_LOADRESYNC, frame, frame, newepoch, &payload, NULL)) return false;
    }
    else if (!RBC_LoadCopy(info, main_state, (const uint8_t*)own_ipx.data(), own_ipx.size(), own_x)) return false;
    rbc_epoch = newepoch;
    return RBC_LeaderBarrier(frame, RBC_HOLD);
}

/* Client, at frame start "frame" (every copy stable and at the DOS prompt where the server's was): every copy takes
 * the server's machine over, then the frames replay to real time. */
static bool RBC_ApplyMachines(uint32_t frame, bool joining) {
    RBResyncInfo info;
    std::vector<RBCMachine> m;
    if (!RBC_ParseMachines(rbc_resync_raw, info, m)) { LOG_MSG("INPUTNET: resync: the server's machines did not arrive whole"); return false; }
    if (rbc_resync_has_seats) {         /* who plays which copy from this frame on */
        memcpy(rb_seat, rbc_resync_seat, sizeof(rb_seat));
        rbc_resync_has_seats = false;
        const int nc = RB_CopyOf(rb_seat, inputnet_selfid);
        if (nc >= 0 && (unsigned)nc != rbc_copy && !RBC_SwitchCopy((unsigned)nc)) { LOG_MSG("INPUTNET: cluster: could not switch to copy %d", nc + 1); return false; }
    }
    const RBCMachine &own = m[rbc_copy];
    if (!RBC_LoadCopy(info, std::vector<uint8_t>(own.state, own.state + own.statelen), own.ipx, own.ipxlen, own.x)) return false;
    /* (a joining computer goes on from the machines' frame: its own count was a guess) */
    const uint32_t real = joining ? info.frame : std::max(rb_realframe, frame);
    const uint32_t newepoch = rbc_epoch + 1u;
    for (unsigned c = 0; c < rbc_copies; c++) {
        if (c == rbc_copy) continue;
        std::vector<uint8_t> payload;
        RBC_PutLoadPayload(payload, info, m[c].state, m[c].statelen, m[c].ipx, m[c].ipxlen, m[c].x);
        if (!RBC_Command(c, RBC_CMD_LOADRESYNC, info.frame, real, newepoch, &payload, NULL)) return false;
    }
    rbc_epoch = newepoch;
    const unsigned k = info.frame % RB_RING;
    for (unsigned c = 0; c < rbc_copies; c++) { rbc_inbox[k][c] = m[c].inbox; rbc_ser_inbox[k][c] = m[c].ser; rbc_ser_lines[k][c] = m[c].ser_lines; }
    rbc_inbox_frame[k] = info.frame;
    std::vector<uint8_t>().swap(rbc_resync_raw);
    if (!RBC_LeaderBarrier(info.frame, RBC_HOLD)) return false;
    return true;
}

/* Client: whether the server's machines can be loaded at "frame": every copy stable, and each at the DOS prompt
 * exactly where the server's was (else its emulator is inside other nested loops). */
static bool RBC_CanApplyMachines(uint32_t frame) {
    uint8_t prompt[INPUTNET_MAX_PEERS];
    if (!RBC_AllStable(frame, prompt)) return false;
    for (unsigned c = 0; c < rbc_copies; c++) {
        if (prompt[c] == rbc_resync_prompt[c]) continue;
        if (rbc_resync_since && GetTicks() - rbc_resync_since > 10000u) {
            static uint32_t told = 0;
            if (GetTicks() - told > 10000u) {
                LOG_MSG("INPUTNET: resync: the copy for player %u is %s, the server's was %s", c + 1u,
                    prompt[c] ? "at the DOS prompt" : "running a program", rbc_resync_prompt[c] ? "at the DOS prompt" : "running a program");
                told = GetTicks();
            }
        }
        return false;
    }
    return true;
}

/* The DOS prompt flags of the machines in "raw" (read when the transfer is complete). */
static void RBC_NoteMachines(void) {
    RBResyncInfo info;
    std::vector<RBCMachine> m;
    memset(rbc_resync_prompt, 0, sizeof(rbc_resync_prompt));
    if (RBC_ParseMachines(rbc_resync_raw, info, m))
        for (unsigned c = 0; c < m.size(); c++) rbc_resync_prompt[c] = m[c].prompt;
    rbc_resync_since = GetTicks();
}

/* ---- joining ---- */

/* The key events that type "text" and Enter, a frame per character (the autorun flag set: RB_ApplyInputs). */
static void RB_TypingFrames(const std::string &text, std::vector<std::vector<InputNetEvent> > &frames) {
    static const char *plain = "`1234567890-=[]\\;',./";
    static const char *shifted = "~!@#$%^&*()_+{}|:\"<>?";
    static const KBD_KEYS punct[] = { KBD_grave,KBD_1,KBD_2,KBD_3,KBD_4,KBD_5,KBD_6,KBD_7,KBD_8,KBD_9,KBD_0,KBD_minus,KBD_equals,
        KBD_leftbracket,KBD_rightbracket,KBD_backslash,KBD_semicolon,KBD_quote,KBD_comma,KBD_period,KBD_slash };
    static const KBD_KEYS letters[26] = { KBD_a,KBD_b,KBD_c,KBD_d,KBD_e,KBD_f,KBD_g,KBD_h,KBD_i,KBD_j,KBD_k,KBD_l,KBD_m,
        KBD_n,KBD_o,KBD_p,KBD_q,KBD_r,KBD_s,KBD_t,KBD_u,KBD_v,KBD_w,KBD_x,KBD_y,KBD_z };
    auto press = [&frames](KBD_KEYS k, bool shift) {
        std::vector<InputNetEvent> f;
        InputNetEvent ev;
        ev.type = INEV_KEY;
        if (shift) { ev.key = (uint16_t)KBD_leftshift; ev.pressed = 3; f.push_back(ev); }
        ev.key = (uint16_t)k; ev.pressed = 3; f.push_back(ev);
        ev.pressed = 2; f.push_back(ev);
        if (shift) { ev.key = (uint16_t)KBD_leftshift; ev.pressed = 2; f.push_back(ev); }
        frames.push_back(f);
    };
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c >= 'a' && c <= 'z') press(letters[c - 'a'], false);
        else if (c >= 'A' && c <= 'Z') press(letters[c - 'A'], true);
        else if (c == ' ') press(KBD_space, false);
        else for (unsigned j = 0; plain[j]; j++) {
            if (plain[j] == c) { press(punct[j], false); break; }
            if (shifted[j] == c) { press(punct[j], true); break; }
        }
    }
    press(KBD_enter, false);
}

/* Joining computer, at a frame start before it has the server's machines: start the game on every copy when the
 * server's runs already, and once every copy has been where the server's machines can be loaded for a while, ask for
 * them (again every few seconds until they come). */
static void RBC_JoinStep(uint32_t frame) {
    uint8_t prompt[INPUTNET_MAX_PEERS];
    memset(prompt, 0, sizeof(prompt));
    const bool stable = RBC_AllStable(frame, prompt);
    /* (where each copy is, stable or not: the followers' newest frame start) */
    bool all_prompt = dos_shell_reading_input, all_game = !dos_shell_reading_input, here = true;
    for (unsigned c = 0; c < rbc_copies; c++) {
        if (c == rbc_copy) continue;
        if (rbc_f[c].at_frame != frame) { here = false; continue; }
        if (rbc_f[c].at_flags & RBC_AT_PROMPT) all_game = false; else all_prompt = false;
    }
    if (!here) all_prompt = all_game = false;
    if (!rbj_server_prompt && !rbj_typed && stable && all_prompt) {
        if (rbj_autorun.empty()) LOG_MSG("INPUTNET: joining: the server's game runs, but it did not say what starts it");
        else {
            LOG_MSG("INPUTNET: joining: starting the game on our copies (\"%s\") to take the server's machines over", rbj_autorun.c_str());
            RB_TypingFrames(rbj_autorun, rbj_typing);
        }
        rbj_typed = true;
    }
    /* the machines can come once every copy is where the server's are: at the DOS prompt, or in the game for a while
     * (they are loaded at a frame start where every copy's host stack is steady: RBC_CanApplyMachines) */
    const bool ready = rbj_server_prompt ? all_prompt : (rbj_typed && all_game);
    rbj_steady = ready ? rbj_steady + 1u : 0u;
    static uint32_t told = 0;
    if (GetTicks() - told > 2000u) {
        LOG_MSG("INPUTNET: joining: frame %u, copies %s%s, steady for %u frames", (unsigned)frame,
            all_prompt ? "at the DOS prompt" : all_game ? "in the game" : "in between", stable ? " (stable)" : "", (unsigned)rbj_steady);
        told = GetTicks();
    }
    if (!rb_resync_ready.empty()) return;  /* (the machines are here, waiting for a frame start they can load at) */
    if (rbj_steady >= (rbj_server_prompt ? 5u : 150u) && (rbj_last_request == 0 || GetTicks() - rbj_last_request > 3000u)) {
        uint8_t buf[32];
        InputNetWriter w(buf, sizeof(buf));
        INPUTNET_BeginPacket(w, INPKT_RESYNC);
        w.u8(0);                            /* request */
        w.u8(1);                            /* ...to join (repeated: void once we are in) */
        INPUTNET_SendToPeers(buf, w.size());
        rbj_last_request = GetTicks();
        LOG_MSG("INPUTNET: joining: asking for the server's machines at frame %u", (unsigned)frame);
    }
}

/* Client (cluster): split the server's list of files per copy, compare each with that copy's own files here, and
 * take the server's over where they differ. Then the session starts, joining (RBC_JoinStep). */
static bool RBC_ClientJoin(uint32_t total, uint32_t server_frame, uint32_t welcomed_at, std::string &errmsg) {
    std::vector<uint8_t> blob;
    if (total == 0 || !INPUTNET_ReceiveState(blob, total)) { errmsg = "the server's list of game files did not arrive"; return false; }
    std::vector<RBFile> theirs;
    if (!RB_TakeManifest(blob, theirs)) { errmsg = "the server sent no list of its game files"; return false; }
    std::vector<std::vector<RBFile> > mine(rbc_copies);
    RB_BuildManifest(mine[rbc_copy]);
    for (unsigned c = 0; c < rbc_copies; c++) {
        if (c == rbc_copy) continue;
        std::vector<uint8_t> reply;
        if (!RBC_Command(c, RBC_CMD_MANIFEST, 0, 0, 0, NULL, &reply)) { errmsg = "a copy of the game here did not answer"; return false; }
        const uint8_t *p = reply.empty() ? NULL : &reply[0], *end = p + reply.size();
        if (p == NULL || !RBC_TakeManifestFull(p, end, mine[c])) { errmsg = "a copy of the game here sent no list of its files"; return false; }
    }
    std::vector<uint8_t> bits((theirs.size() + 7u) / 8u, 0);
    uint64_t need = 0;
    for (unsigned c = 0; c < rbc_copies; c++) {
        std::vector<RBFile> part;
        std::vector<size_t> at;
        for (size_t i = 0; i < theirs.size(); i++) {
            if ((unsigned)(theirs[i].drive >> 5) != c) continue;
            RBFile e = theirs[i];
            e.drive &= 31u;
            part.push_back(e);
            at.push_back(i);
        }
        std::vector<uint8_t> pb;
        uint64_t pn = 0;
        std::string err;
        if (!RB_CompareManifestsWith(part, mine[c], pb, pn, err)) { errmsg = err; return false; }
        for (size_t j = 0; j < part.size(); j++)
            if (pb[j / 8u] & (1u << (j % 8u))) bits[at[j] / 8u] |= (uint8_t)(1u << (at[j] % 8u));
        need += pn;
    }
    rb_cli_need.clear();
    for (size_t i = 0; i < theirs.size(); i++) if (bits[i / 8u] & (1u << (i % 8u))) rb_cli_need.push_back(theirs[i]);
    rb_cli_bits.swap(bits);
    rb_cli_manifest_count = (uint32_t)theirs.size();
    RB_SendFileRequest();                   /* (always: the server learns that we need nothing) */
    if (need > 0) {
        if (need > RB_SYNC_TOTAL_MAX) { errmsg = "the files that differ from the server's are too large to take over: every computer needs the same version of the game"; return false; }
        std::vector<uint8_t> files;
        if (!INPUTNET_ReceiveState(files, (uint32_t)need, RB_SendFileRequest, INPKT_FILES)) { errmsg = "the server's game files did not arrive"; return false; }
        std::vector<std::vector<RBFile> > others(rbc_copies);
        std::string err;
        if (!RB_MapReceivedFiles(files, err, &others)) { errmsg = err; return false; }
        for (unsigned c = 0; c < rbc_copies; c++) {
            if (c == rbc_copy || others[c].empty()) continue;
            std::vector<uint8_t> payload;
            RBC_PutManifestFull(payload, others[c]);
            if (!RBC_Command(c, RBC_CMD_FILEMAP, 0, 0, 0, &payload, NULL)) { errmsg = "a copy of the game here did not take the server's files"; return false; }
        }
        LOG_MSG("INPUTNET: using %u of the server's files in place of ours for this session", (unsigned)rb_cli_need.size());
    }
    else RB_SendFileRequest();

    rb_joining = true;
    rbj_typed = false; rbj_steady = 0; rbj_last_request = 0;
    rbj_typing.clear(); rbj_typing_pos = 0;
    std::vector<uint8_t>().swap(rbc_resync_raw);
    rb_start_frame = server_frame + (GetTicks() - welcomed_at) / inputnet_frame_ms;
    LOG_MSG("INPUTNET: joining the session at frame %u (the server's game %s)", (unsigned)rb_start_frame,
        rbj_server_prompt ? "is at the DOS prompt" : "runs");
    INPUTNET_StartSession();
    return true;
}

/* Server: the list of every copy's files (each entry's drive byte carries its copy in bits 5-7) for a joining
 * participant, then its file request is answered at a frame start (RBC_ServerFiles). */
static bool RBC_ServerManifest(std::vector<uint8_t> &out) {
    rb_srv_manifest.clear();
    for (unsigned c = 0; c < rbc_copies; c++) {
        std::vector<RBFile> m;
        if (c == rbc_copy) RB_BuildManifest(m);
        else {
            std::vector<uint8_t> reply;
            if (!RBC_Command(c, RBC_CMD_MANIFEST, 0, 0, 0, NULL, &reply)) return false;
            const uint8_t *p = reply.empty() ? NULL : &reply[0], *end = p + reply.size();
            if (p == NULL || !RBC_TakeManifestFull(p, end, m)) return false;
        }
        for (size_t i = 0; i < m.size(); i++) { m[i].drive = (uint8_t)(m[i].drive | (c << 5)); rb_srv_manifest.push_back(m[i]); }
    }
    out.clear();
    RB_PutManifest(out, rb_srv_manifest);
    rb_filereq_ready = false;
    rb_filereq_bits.clear();
    rb_filereq_part.clear();
    return true;
}

/* Server: someone asks to join (HELLO). Its player's copy is the one it names (the host plays copy 0); it gets the
 * game files now and the machines when it asks for them (RBC_JoinStep). */
static void RBC_ServerHello(const IPaddress &addr, const std::string &name, unsigned copies, unsigned want) {
    if (inputnet_accepting) return;
    InputNetPeer *p = INPUTNET_PeerByAddr(addr);
    if (p != NULL && p->ready) return;                  /* in the session already */
    if (p != NULL && rbs_joining[p - inputnet_peers]) return;   /* has its WELCOME (a hello sent before it came) */
    static uint32_t last_told = 0;
    const bool tell = GetTicks() - last_told > 5000u;
    if (copies != rbc_copies) {
        if (tell) { LOG_MSG("INPUTNET: refused \"%s\": it runs %u copies of the game, the session %u", name.c_str(), copies, rbc_copies); last_told = GetTicks(); }
        return;
    }
    if (rbc_copies == 1) {          /* the game alone: any free participant number */
        want = 0;
        for (unsigned i = 1; i < INPUTNET_MAX_PEERS && want == 0; i++) if (!inputnet_peers[i].active) want = i;
        if (want == 0) {
            if (tell) { LOG_MSG("INPUTNET: refused \"%s\": the session is full", name.c_str()); last_told = GetTicks(); }
            return;
        }
    }
    else if (want >= rbc_copies || want >= INPUTNET_MAX_PEERS || !RB_CopyFree(rbs_seat_change ? rbs_seat_next : rb_seat, want)) {
        if (tell) { LOG_MSG("INPUTNET: refused \"%s\": player %u's copy is taken", name.c_str(), want + 1u); last_told = GetTicks(); }
        return;
    }
    /* its participant number: the copy's, unless someone who moved to another copy has it */
    unsigned id = want;
    if (rbc_copies >= 2 && inputnet_peers[id].active) {
        id = 0;
        for (unsigned i = 1; i < INPUTNET_MAX_PEERS && id == 0; i++) if (!inputnet_peers[i].active && !rbs_joining[i]) id = i;
        if (id == 0) {
            if (tell) { LOG_MSG("INPUTNET: refused \"%s\": the session is full", name.c_str()); last_told = GetTicks(); }
            return;
        }
    }
    const unsigned copy = want;
    want = id;
    rbs_join_copy[want] = (uint8_t)copy;
    inputnet_accepting = true;
    p = &inputnet_peers[want];
    p->active = true; p->local = false; p->ready = false; p->addr = addr; p->name = name; p->lastheard = GetTicks();
    p->resetFrames();
    rbs_joining[want] = true; rbs_wants[want] = false;
    LOG_MSG("INPUTNET: \"%s\" joins from %d.%d.%d.%d as player %u", name.c_str(),
        (int)(addr.host & 0xFF), (int)((addr.host >> 8u) & 0xFF), (int)((addr.host >> 16u) & 0xFF), (int)((addr.host >> 24u) & 0xFF), copy + 1u);

    std::vector<uint8_t> manifest;
    bool ok = RBC_ServerManifest(manifest);
    if (ok) {
        rbs_filereq_peer = (int)want;
        uint8_t welcome[1400];
        InputNetWriter w(welcome, sizeof(welcome));
        INPUTNET_BeginPacket(w, INPKT_WELCOME);
        w.u8((uint8_t)want);
        w.u16((uint16_t)inputnet_frame_ms);
        w.u16((uint16_t)inputnet_delay);
        w.u16((uint16_t)inputnet_redundancy);
        w.u8(inputnet_share_keyboard ? 1 : 0);
        w.u8(inputnet_share_mouse ? 1 : 0);
        w.u8((uint8_t)inputnet_cyclesync);
        w.u32((uint32_t)manifest.size());
        w.u32(CALLBACK_LayoutSignature());
        {   /* (the machines come later, with what a save state leaves out of them: RBCExtras) */
            uint8_t clock[16];
            CMOS_GetSharedClock(clock);
            w.bytes(clock, sizeof(clock));
            w.u16(dos.date.year); w.u8(dos.date.month); w.u8(dos.date.day);
            w.u32((uint32_t)(rdtsc_count_base >> 32u)); w.u32((uint32_t)rdtsc_count_base);
            const uint64_t fps = INPUTNET_DoubleBits(vga_fps), base = INPUTNET_DoubleBits(vga_mode_time_base);
            w.u32((uint32_t)(fps >> 32u)); w.u32((uint32_t)fps);
            w.u32((uint32_t)(base >> 32u)); w.u32((uint32_t)base);
            w.u32((uint32_t)vga_mode_frames_since_time_base);
        }
        w.u8(1);
        w.u8((uint8_t)inputnet_rbwindow);
        const std::string fp = RB_Fingerprint();
        w.u16((uint16_t)fp.size());
        w.bytes((const uint8_t*)fp.data(), fp.size());
        /* the cluster */
        w.u8((uint8_t)rbc_copies);
        w.u32(rb_realframe);
        w.u8(dos_shell_reading_input ? 1 : 0);
        const std::string ar = inputnet_rbautorun.substr(0, 200);
        w.u16((uint16_t)ar.size());
        w.bytes((const uint8_t*)ar.data(), ar.size());
        const size_t wl = w.size();
        p->confirmed = 0;
        uint32_t last_events = GetTicks();
        const uint32_t wstart = GetTicks(), heard = p->lastheard;
        while (p->lastheard == heard && GetTicks() - wstart < 1500u) {     /* (STATEACK: it waits for the list) */
            INPUTNET_SendRaw(p->addr, welcome, wl);
            const uint32_t round = GetTicks();
            while (GetTicks() - round < 100u && p->lastheard == heard) { INPUTNET_PollSocket(); INPUTNET_Stall(last_events); }
        }
        ok = INPUTNET_SendStateTo(*p, manifest, INPKT_STATE);
    }
    if (!ok) {
        LOG_MSG("INPUTNET: \"%s\" could not join", name.c_str());
        p->active = false;
        rbs_joining[want] = false;
        rbs_filereq_peer = -1;
    }
    inputnet_accepting = false;
}

/* Server, at a frame start: the files a joining participant asked for. */
static void RBC_ServerFiles(void) {
    if (!rb_filereq_ready || rbs_filereq_peer < 0) return;
    const unsigned id = (unsigned)rbs_filereq_peer;
    rb_filereq_ready = false;
    rbs_filereq_peer = -1;
    InputNetPeer &p = inputnet_peers[id];
    if (!p.active || !rbs_joining[id]) return;
    bool any = false;
    for (size_t i = 0; i < rb_filereq_bits.size(); i++) any = any || rb_filereq_bits[i] != 0;
    if (!any) return;
    std::vector<uint8_t> files;
    if (!RB_BuildFileBlob(files)) { LOG_MSG("INPUTNET: could not read the game files \"%s\" asked for", p.name.c_str()); return; }
    LOG_MSG("INPUTNET: sending %u KB of game files to \"%s\"", (unsigned)(files.size() >> 10), p.name.c_str());
    inputnet_accepting = true;
    if (!INPUTNET_SendStateTo(p, files, INPKT_FILES)) {
        LOG_MSG("INPUTNET: \"%s\" did not take the game files", p.name.c_str());
        p.active = false;
        rbs_joining[id] = false;
    }
    inputnet_accepting = false;
}

/* Server: a participant is gone (BYE, or too far behind): its copy runs on without input, and every computer takes
 * the server's machines over (the others may have run with different amounts of its input). */
static void RBC_ServerDrop(unsigned id, const char *why) {
    InputNetPeer &p = inputnet_peers[id];
    if (!p.active || p.local) return;
    LOG_MSG("INPUTNET: \"%s\" (player %u) %s", p.name.c_str(), id + 1u, why);
    const bool was_in = p.ready;
    p.active = false; p.ready = false; p.resetFrames();
    rbs_joining[id] = false; rbs_wants[id] = false;
    if (was_in) { rbs_left.push_back((uint8_t)id); rb_resync_wanted = true; }
}

/* Server: the machine at the start of "frame" (= now) in the portable save state format, compressed; "state" gets the
 * save state alone. The live machine must be at that frame start. */
static bool RB_BuildResyncBlob(uint32_t frame, std::vector<uint8_t> &out, uint32_t &rawsize, std::vector<uint8_t> &state, RBResyncInfo &h) {
    if (!INPUTNET_SnapshotState(state)) return false;
    h.frame = frame;
    h.tick = rb_tick;
    h.epoch = (uint16_t)(rb_epoch + 1u);
    h.vfps = vga_fps;
    h.vbase = vga_mode_time_base;
    h.vframes = (int32_t)vga_mode_frames_since_time_base;
    h.draw = VGA_GetOnDemandFlags();
    std::vector<uint8_t> raw;
    RB_PutResyncInfo(raw, h);
    raw.insert(raw.end(), state.begin(), state.end());
    rawsize = (uint32_t)raw.size();
    uLongf clen = compressBound((uLong)raw.size());
    out.resize(clen);
    if (compress2(&out[0], &clen, &raw[0], (uLong)raw.size(), 1) != Z_OK) return false;
    out.resize(clen);
    return true;
}

static uint32_t rb_resync_acked_by[INPUTNET_MAX_PEERS];   /* server: chunks each participant acknowledged */

/* Server: the resync's blob to every participant, each at its own pace. "joined" and "left" tell the clients who plays
 * from this frame on (a cluster's joins and leaves). */
static bool RB_SendResync(uint32_t frame, const std::vector<uint8_t> &blob, uint32_t rawsize, const std::vector<uint8_t> &joined, const std::vector<uint8_t> &left,
                          const uint8_t *seats) {
    const uint32_t id = (uint32_t)(GetTicks() ^ (frame * 2654435761u)) | 1u;
    const uint32_t nchunks = (uint32_t)((blob.size() + RB_RESYNC_CHUNK - 1u) / RB_RESYNC_CHUNK);
    bool to[INPUTNET_MAX_PEERS];
    uint32_t next[INPUTNET_MAX_PEERS], lastsend[INPUTNET_MAX_PEERS], seen[INPUTNET_MAX_PEERS], progress[INPUTNET_MAX_PEERS];
    unsigned n = 0;
    const uint32_t t0 = GetTicks();
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &p = inputnet_peers[i];
        to[i] = p.active && p.ready && !p.local;
        next[i] = 0; lastsend[i] = 0; seen[i] = 0; progress[i] = t0;
        rb_resync_acked_by[i] = 0;
        if (to[i]) n++;
    }
    rb_resync_acked = 0;
    LOG_MSG("INPUTNET: resync: sending frame %u to %u participant(s), %u KB (%u KB before compression)",
        (unsigned)frame, n, (unsigned)(blob.size() / 1024u), (unsigned)(rawsize / 1024u));
    uint8_t hb[96];
    InputNetWriter hw(hb, sizeof(hb));
    INPUTNET_BeginPacket(hw, INPKT_RESYNC);
    hw.u8(1); hw.u32(id); hw.u32(frame); hw.u32((uint32_t)blob.size()); hw.u32(rawsize);
    hw.u8((uint8_t)joined.size());
    for (size_t j = 0; j < joined.size(); j++) hw.u8(joined[j]);
    hw.u8((uint8_t)left.size());
    for (size_t j = 0; j < left.size(); j++) hw.u8(left[j]);
    hw.u8(seats != NULL ? (uint8_t)rbc_copies : 0);     /* who plays which copy from this frame on (rb_seat) */
    if (seats != NULL) for (unsigned c = 0; c < rbc_copies; c++) hw.u8(seats[c]);
    inputnet_accepting = true;      /* (no join meanwhile) */
    uint32_t last_events = t0, lastkeep = t0;
    for (;;) {
        if (!inputnet_session) { inputnet_accepting = false; return false; }
        bool done = true;
        const uint32_t now = GetTicks();
        /* the others hear the server meanwhile (a transfer through a slow relay takes many seconds, and a participant
         * that has everything, or is in the session already, waits for the server's input: RB_ClientWaitsOn) */
        if (now - lastkeep >= 100u) { RB_SendInput(frame); lastkeep = now; }
        for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
            if (!to[i]) continue;
            InputNetPeer &p = inputnet_peers[i];
            if (!p.active) { to[i] = false; continue; }
            const uint32_t acked = std::min(rb_resync_acked_by[i], nchunks);
            if (acked >= nchunks) continue;
            done = false;
            if (acked > seen[i]) { seen[i] = acked; progress[i] = now; }
            if (now - progress[i] > 30000u) {
                LOG_MSG("INPUTNET: resync: \"%s\" took nothing for 30 seconds", p.name.c_str());
                to[i] = false;
                if (rbc_copies) RBC_ServerDrop(i, "did not take the machines");
                continue;
            }
            if (lastsend[i] == 0 || now - lastsend[i] > 300u) {     /* (again) from what it has */
                next[i] = acked;
                lastsend[i] = now ? now : 1u;
                if (acked == 0) INPUTNET_SendRaw(p.addr, hb, hw.size());
            }
            if (next[i] < acked) next[i] = acked;
            while (next[i] < nchunks && next[i] < acked + 256u) {
                const size_t off = (size_t)next[i] * RB_RESYNC_CHUNK;
                const size_t len = std::min((size_t)RB_RESYNC_CHUNK, blob.size() - off);
                uint8_t cb[RB_RESYNC_CHUNK + 48];
                InputNetWriter w(cb, sizeof(cb));
                INPUTNET_BeginPacket(w, INPKT_RESYNC);
                w.u8(2); w.u32(id); w.u32(next[i]); w.u16((uint16_t)len); w.bytes(&blob[off], len);
                INPUTNET_SendRaw(p.addr, cb, w.size());
                next[i]++;
            }
        }
        if (done) break;
        INPUTNET_PollSocket();
        if (now - last_events >= 10u) { GFX_Events(); last_events = now; }
        SDL_Delay(1);
    }
    inputnet_accepting = false;
    rb_stat_resyncs++;
    LOG_MSG("INPUTNET: resync: frame %u sent in %u ms", (unsigned)frame, (unsigned)(GetTicks() - t0));
    return true;
}

/* Server, at a real-time frame start: send our state to the clients. Returns false if a correction came first. */
static bool RB_ServerResync(uint32_t frame) {
    /* this frame start is final once every input before it is here */
    uint32_t start = GetTicks(), last_events = start;
    while (RB_MinHave() < frame) {
        if (!inputnet_session) return false;
        if ((GetTicks() - start) > inputnet_timeout) { LOG_MSG("INPUTNET: resync: no input from the client(s)"); return false; }
        INPUTNET_Stall(last_events);
        INPUTNET_PollSocket();
        const uint32_t g = RB_FindMisprediction(frame);
        if (g != UINT32_MAX) {                  /* correct it first; try again when the replay is done */
            if (RB_Rollback(g, frame)) { rb_late.clear(); rb_pending_wrong = UINT32_MAX; }
            else rb_pending_wrong = std::min(rb_pending_wrong, g);
            return false;
        }
    }
    std::vector<uint8_t> blob, state;
    uint32_t rawsize = 0;
    std::vector<uint8_t> joined, left;
    uint8_t seats[INPUTNET_MAX_PEERS];
    memcpy(seats, rbs_seat_change ? rbs_seat_next : rb_seat, sizeof(seats));
    if (rbc_copies) {
        for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++)
            if (rbs_joining[i] && rbs_wants[i] && inputnet_peers[i].active) joined.push_back((uint8_t)i);
        /* who plays which copy from this frame on: the joining ones the copies they asked for, nobody those who left */
        for (size_t j = 0; j < joined.size(); j++) if (rbs_join_copy[joined[j]] < rbc_copies) seats[rbs_join_copy[joined[j]]] = joined[j];
        for (size_t j = 0; j < rbs_left.size(); j++) { const int c = RB_CopyOf(seats, rbs_left[j]); if (c >= 0) seats[c] = 0xFF; }
        std::vector<uint8_t> raw;
        if (!RBC_BuildMachines(frame, (uint16_t)(rb_epoch + 1u), raw, seats)) {
            LOG_MSG("INPUTNET: resync: cannot save the cluster's machines at frame %u",(unsigned)frame);
            return false;
        }
        left.swap(rbs_left);
        memcpy(rb_seat, seats, sizeof(rb_seat));
        rbs_seat_change = false;
        RB_ReportSeats();
        rawsize = (uint32_t)raw.size();
        uLongf clen = compressBound((uLong)raw.size());
        blob.resize(clen);
        if (compress2(&blob[0], &clen, &raw[0], (uLong)raw.size(), 1) != Z_OK) return false;
        blob.resize(clen);
        for (size_t j = 0; j < joined.size(); j++) {   /* in the session from this frame on */
            const unsigned i = joined[j];
            InputNetPeer &p = inputnet_peers[i];
            p.ready = true;
            p.lastheard = GetTicks();
            rbs_joining[i] = false; rbs_wants[i] = false;
            rb_have[i] = frame; rb_checked[i] = frame; rb_acked[i] = frame;
            rb_peerframe[i] = 0; rb_peerframe_at[i] = 0; rb_rtt[i] = 0;
            for (unsigned k = 0; k < RB_RING; k++) rb_predicted[i][k] = 0;
            for (unsigned k = 0; k < INPUTNET_CHECK_RING; k++) rb_peercheck[i][k].valid = false;
            LOG_MSG("INPUTNET: \"%s\" plays copy %u from frame %u", p.name.c_str(), (unsigned)(RB_CopyOf(rb_seat, i) + 1), (unsigned)frame);
        }
    }
    else {
        RBResyncInfo info;
        if (!RB_BuildResyncBlob(frame, blob, rawsize, state, info)) { LOG_MSG("INPUTNET: resync: cannot save the machine at frame %u",(unsigned)frame); return false; }
        if (!RB_LoadResyncState(info, state)) { LOG_MSG("INPUTNET: resync: cannot load our own state of frame %u",(unsigned)frame); return false; }
    }
    return RB_SendResync(frame, blob, rawsize, joined, left, rbc_copies >= 2 ? rb_seat : NULL);
}

/* Client: load the server's machine at "frame", then replay to where real time is. */
static bool RB_ApplyResyncBlob(const std::vector<uint8_t> &comp, uint32_t rawsize) {
    std::vector<uint8_t> raw(rawsize);
    uLongf rlen = rawsize;
    if (rawsize == 0 || uncompress(&raw[0], &rlen, &comp[0], (uLong)comp.size()) != Z_OK || rlen != rawsize) return false;
    RBResyncInfo h;
    if (!RB_GetResyncInfo(raw, h)) return false;
    const std::vector<uint8_t> state(raw.begin() + RB_RESYNC_INFO_SIZE, raw.end());
    if (!RB_LoadResyncState(h, state)) return false;
    rb_desynced = false;
    rb_stat_resyncs++;

    const uint32_t frame = h.frame;
    if (rb_realframe > frame) {     /* replay to real time with the input we have */
        const uint32_t depth = rb_realframe - frame;
        inputnet_rollback_resim = true;
        rb_target = rb_realframe;
        MIXER_SetRollbackDiscard(true);
        DOSBOX_AddTicksRemain(depth * inputnet_frame_ms);
    }
    else rb_realframe = frame;
    RB_ApplyInputs(frame);
    LOG_MSG("INPUTNET: resync: took the server's state at frame %u over",(unsigned)frame);
    return true;
}

/* Client (cluster): every copy takes the server's machine over (RBC_ApplyMachines), then the frames replay to real
 * time. A joining computer plays from here on. */
static bool RB_ApplyClusterResync(uint32_t frame) {
    const bool joining = rb_joining;
    if (!RBC_ApplyMachines(frame, joining)) return false;
    const uint32_t r0 = inputnet_frame;     /* the frame the machines are at (RB_LoadResyncState) */
    if (joining) rb_realframe = r0;         /* (our own count was a guess; RB_TimeSync does the rest) */
    rb_desynced = false;
    rb_stat_resyncs++;
    if (joining) {
        rb_joining = false;
        for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
            InputNetPeer &p = inputnet_peers[i];
            if (!p.active || p.local) continue;
            if (rb_have[i] < r0) rb_have[i] = r0;
            while (INPUTNET_FrameSlotIfValid(p, rb_have[i]) != NULL) rb_have[i]++;
            rb_acked[i] = r0;
        }
        inputnet_local_confirmed = r0;
        if (rbc_copies >= 2) LOG_MSG("INPUTNET: joined: playing copy %u from frame %u", rbc_copy + 1u, (unsigned)r0);
        else LOG_MSG("INPUTNET: joined: playing as player %u from frame %u", (unsigned)inputnet_selfid + 1u, (unsigned)r0);
    }
    if (rb_realframe > r0) {
        inputnet_rollback_resim = true;
        rb_target = rb_realframe;
        MIXER_SetRollbackDiscard(true);
        DOSBOX_AddTicksRemain((rb_realframe - r0) * inputnet_frame_ms);
    }
    else rb_realframe = r0;
    RB_ApplyInputs(r0);
    LOG_MSG("INPUTNET: resync: took the server's machines over at frame %u", (unsigned)r0);
    return true;
}

static uint32_t rb_resync_done_id = 0, rb_resync_done_count = 0;  /* client: the last transfer received whole */

static void RB_HandleResync(InputNetReader &r,const IPaddress &from,unsigned peerid) {
    const uint8_t sub = r.u8();
    if (r.bad()) return;
    if (sub == 0) {                 /* request from a client (a cluster's joining computer: it is ready for the machines) */
        if (!inputnet_is_server) return;
        const bool join = r.remain() >= 1 && r.u8() == 1;
        if (rbc_copies) {
            InputNetPeer *p = INPUTNET_PeerByAddr(from);
            const bool joining = p != NULL && rbs_joining[p - inputnet_peers];
            if (join && !joining) return;   /* in already: a repeat of its request to join */
            if (joining) rbs_wants[p - inputnet_peers] = true;
        }
        rb_resync_wanted = true;
        return;
    }
    if (sub == 3) {                 /* acknowledgement */
        if (!inputnet_is_server) return;
        r.u32();
        const uint32_t count = r.u32();
        if (r.bad()) return;
        if (count > rb_resync_acked) rb_resync_acked = count;
        if (peerid < INPUTNET_MAX_PEERS && count > rb_resync_acked_by[peerid]) rb_resync_acked_by[peerid] = count;
        return;
    }
    if (inputnet_is_server) return;
    if (sub == 1) {                 /* header: receive everything now */
        const uint32_t id = r.u32(), frame = r.u32(), total = r.u32(), rawsize = r.u32();
        if (r.bad() || total == 0 || total > (256u << 20) || rawsize > (1024u << 20)) return;
        if (id == rb_resync_done_id) { RB_SendResyncAck(id, rb_resync_done_count); return; }   /* (our acks got lost) */
        if (id == rb_resync_id) return;     /* a repeated header of a transfer we already have */
        std::vector<uint8_t> joined, left;  /* who plays from this frame on */
        if (r.remain() >= 1) { const unsigned nj = r.u8(); for (unsigned j = 0; j < nj; j++) joined.push_back(r.u8()); }
        if (r.remain() >= 1) { const unsigned nl = r.u8(); for (unsigned j = 0; j < nl; j++) left.push_back(r.u8()); }
        uint8_t seats[INPUTNET_MAX_PEERS];
        unsigned ns = 0;
        if (r.remain() >= 1) { ns = r.u8(); for (unsigned j = 0; j < ns; j++) { const uint8_t v = r.u8(); if (j < INPUTNET_MAX_PEERS) seats[j] = v; } }
        if (r.bad()) return;
        if (ns == rbc_copies && ns >= 2 && ns <= INPUTNET_MAX_PEERS) { memcpy(rbc_resync_seat, seats, ns); rbc_resync_has_seats = true; }
        for (size_t j = 0; j < joined.size(); j++) {
            const unsigned i = joined[j];
            if (i >= INPUTNET_MAX_PEERS || i == inputnet_selfid) continue;
            InputNetPeer &p = inputnet_peers[i];
            p.active = true; p.ready = true; p.local = false; p.addr = inputnet_server_addr; p.lastheard = GetTicks();
            p.resetFrames();
            rb_have[i] = frame; rb_checked[i] = frame; rb_acked[i] = frame; rb_peerframe_at[i] = 0; rb_rtt[i] = 0;
            for (unsigned k = 0; k < RB_RING; k++) rb_predicted[i][k] = 0;
            for (unsigned k = 0; k < INPUTNET_CHECK_RING; k++) rb_peercheck[i][k].valid = false;
            LOG_MSG("INPUTNET: player %u joins at frame %u", i + 1u, (unsigned)frame);
        }
        for (size_t j = 0; j < left.size(); j++) {
            const unsigned i = left[j];
            if (i >= INPUTNET_MAX_PEERS || i == inputnet_selfid) continue;
            inputnet_peers[i].active = false; inputnet_peers[i].ready = false; inputnet_peers[i].resetFrames();
            LOG_MSG("INPUTNET: player %u left at frame %u", i + 1u, (unsigned)frame);
        }
        rb_resync_id = id;
        rb_resync_frame = frame;
        rb_resync_total = total;
        rb_resync_buf.assign(total, 0);
        const uint32_t nchunks = (total + RB_RESYNC_CHUNK - 1u) / RB_RESYNC_CHUNK;
        rb_resync_have.assign(nchunks, false);
        rb_resync_got = 0;
        uint32_t contiguous = 0;
        const uint32_t start = GetTicks();
        uint32_t last_events = start, lastack = 0;
        uint32_t progress_at = start;
        while (contiguous < nchunks) {
            /* (30 seconds without a piece: a slow link may take longer than that for the whole of it) */
            if (!inputnet_link_up || (GetTicks() - progress_at) > 30000u) { LOG_MSG("INPUTNET: resync: transfer failed"); rb_resync_id = 0; return; }
            INPUTNET_PollSocket();              /* chunks land in rb_resync_buf (sub == 2 below) */
            const uint32_t was = contiguous;
            while (contiguous < nchunks && rb_resync_have[contiguous]) contiguous++;
            if (contiguous != was) progress_at = GetTicks();
            if ((GetTicks() - lastack) >= 20u) { RB_SendResyncAck(id, contiguous); lastack = GetTicks(); }
            INPUTNET_Stall(last_events);
        }
        RB_SendResyncAck(id, contiguous);
        RB_SendResyncAck(id, contiguous);
        rb_resync_done_id = id; rb_resync_done_count = contiguous;     /* (acknowledged again should the server repeat it) */
        /* loaded at the next frame start where the host's stack is as it was at the one before (RB_FrameBoundary) */
        rb_resync_ready.swap(rb_resync_buf);
        rb_resync_ready_raw = rawsize;
        std::vector<uint8_t>().swap(rb_resync_buf);
        if (rbc_copies) {           /* (the copies' DOS prompt flags say where it can be loaded) */
            rbc_resync_raw.assign(rawsize, 0);
            uLongf rlen = rawsize;
            if (rawsize == 0 || uncompress(&rbc_resync_raw[0], &rlen, &rb_resync_ready[0], (uLong)rb_resync_ready.size()) != Z_OK || rlen != rawsize) {
                LOG_MSG("INPUTNET: resync: the server's machines did not unpack");
                std::vector<uint8_t>().swap(rbc_resync_raw);
                std::vector<uint8_t>().swap(rb_resync_ready);
                return;
            }
            RBC_NoteMachines();
        }
        return;
    }
    if (sub == 2) {                 /* chunk */
        const uint32_t id = r.u32(), index = r.u32();
        const unsigned len = r.u16();
        const uint8_t *data = r.bytes(len);
        if (!r.bad() && id == rb_resync_done_id) { RB_SendResyncAck(id, rb_resync_done_count); return; }
        if (r.bad() || data == NULL || id != rb_resync_id || index >= rb_resync_have.size()) return;
        const size_t off = (size_t)index * RB_RESYNC_CHUNK;
        if (off + len > rb_resync_buf.size() || rb_resync_have[index]) return;
        memcpy(&rb_resync_buf[off], data, len);
        rb_resync_have[index] = true;
        rb_resync_got++;
    }
}


/* Self-test (NN_RB_SELFTEST=<period>): every <period> frames, put the machine back a few frames whose input is all
 * known and replay them; every replayed frame start must equal the original one, or some state is missing from the
 * snapshot. Logs which part differs. */
static uint32_t rb_selftest_period = 0, rb_selftest_end = 0, rb_selftest_first = 0;
static uint64_t rb_selftest_hash[RB_RING];
static uint64_t rb_selftest_mem[RB_RING];
static std::string rb_selftest_dev[RB_RING];
static uint32_t rb_selftest_regs[RB_RING][12];
static uint32_t rb_selftest_fails = 0, rb_selftest_runs = 0;

static void RB_SelfTestRegs(uint32_t *out) {
    for (unsigned i = 0; i < 8; i++) out[i] = cpu_regs.regs[i].dword[0];
    out[8] = cpu_regs.ip.dword[0]; out[9] = (uint32_t)cpu_regs.flags; out[10] = (uint32_t)Segs.val[cs]; out[11] = (uint32_t)PIC_Ticks;
}

extern void POD_Save_Adlib(std::ostream& stream);
extern void POD_Save_Disney(std::ostream& stream);
extern void POD_Save_Gameblaster(std::ostream& stream);
extern void POD_Save_GUS(std::ostream& stream);
extern void POD_Save_MPU401(std::ostream& stream);
extern void POD_Save_PCSpeaker(std::ostream& stream);
extern void POD_Save_Sblaster(std::ostream& stream);
extern void POD_Save_Tandy_Sound(std::ostream& stream);

static size_t rb_mixerpart_start = 0;
/* Which sound device a byte of the mixer component belongs to (self-test reports); rb_mixerpart_start = its start. */
static const char *RB_MixerPart(size_t at) {
    static const char *names[] = { "Adlib", "Disney", "Gameblaster", "GUS", "MPU401", "PCSpeaker", "Sblaster", "Tandy" };
    void (*savers[])(std::ostream&) = { POD_Save_Adlib, POD_Save_Disney, POD_Save_Gameblaster, POD_Save_GUS,
        POD_Save_MPU401, POD_Save_PCSpeaker, POD_Save_Sblaster, POD_Save_Tandy_Sound };
    savestate_lean = true;
    size_t pos = 0;
    const char *found = "?";
    for (unsigned i = 0; i < 8; i++) {
        std::ostringstream os;
        savers[i](os);
        const size_t len = os.str().size();
        if (at >= pos && at < pos + len && found[0] == '?') { found = names[i]; rb_mixerpart_start = pos; }
        pos += len;
    }
    savestate_lean = false;
    return found;
}

static void RB_SelfTestCompare(uint32_t frame) {
    const unsigned k = frame % RB_RING;
    const RBSnap &s = rb_snap[k];
    if (s.hash == rb_selftest_hash[k]) return;
    rb_selftest_fails++;
    uint32_t regs[12]; RB_SelfTestRegs(regs);
    LOG_MSG("RBSELFTEST frame %u differs after replay: memory %s, cs:eip %04x:%08x (was %04x:%08x), eax %08x (was %08x), pic %u (was %u)",
        (unsigned)frame, s.hash == rb_selftest_hash[k] ? "same" : ((rb_memhash == rb_selftest_mem[k]) ? "same" : "DIFFERS"),
        regs[10], regs[8], rb_selftest_regs[k][10], rb_selftest_regs[k][8], regs[0], rb_selftest_regs[k][0], regs[11], rb_selftest_regs[k][11]);
    /* the components, in saveLean order: length-prefixed blobs */
    const std::string &x = rb_selftest_dev[k], &y = s.devices;
    size_t px = 0, py = 0; unsigned idx = 0;
    while (px + 4 <= x.size() && py + 4 <= y.size()) {
        uint32_t lx, ly; memcpy(&lx, x.data() + px, 4); memcpy(&ly, y.data() + py, 4);
        px += 4; py += 4;
        if (lx != ly || memcmp(x.data() + px, y.data() + py, lx) != 0) {
            size_t first = 0;
            while (first < lx && first < ly && x[px + first] == y[py + first]) first++;
            LOG_MSG("RBSELFTEST   component #%u differs (%u/%u bytes), first at byte %u%s%s", idx, (unsigned)lx, (unsigned)ly, (unsigned)first,
                idx == 12 ? ", in " : "", idx == 12 ? RB_MixerPart(first) : "");
            if (idx != 12) {
                unsigned shown = 0;
                for (size_t b = first; b < lx && b < ly && shown < 16; b++)
                    if (x[px + b] != y[py + b]) {
                        LOG_MSG("RBSELFTEST     byte %u: %02x -> %02x", (unsigned)b, (uint8_t)x[px + b], (uint8_t)y[py + b]);
                        shown++;
                    }
            }
            if (idx == 12 && strcmp(RB_MixerPart(first), "Sblaster") == 0) {
                /* every differing byte of the Sound Blaster struct (after its 32 byte name) */
                const size_t base = rb_mixerpart_start + 32u;
                unsigned shown = 0;
                for (size_t b = base; b < lx && b < ly && shown < 12; b++)
                    if (x[px + b] != y[py + b]) {
                        LOG_MSG("RBSELFTEST     SB_INFO+%u: %02x -> %02x", (unsigned)(b - base), (uint8_t)x[px + b], (uint8_t)y[py + b]);
                        shown++;
                    }
            }
        }
        px += lx; py += ly; idx++;
    }
}

/* Input that contradicts a prediction but lies beyond what a rollback can reach (too old, or before a change of the
 * host's context): it is applied now, late, so no key press or release is lost, and the machines are then made equal
 * again from the server's state (the server sends it; a client asks for it). */
static void RB_Unreachable(void) {
    LOG_MSG("INPUTNET: input for %u earlier frame(s) arrived too late to roll back; applying it now",(unsigned)rb_late.size());
    for (size_t k = 0; k < rb_late.size(); k++) {
        InputNetFrame *slot = INPUTNET_FrameSlotIfValid(inputnet_peers[rb_late[k].peer], rb_late[k].frame);
        if (slot != NULL)
            for (size_t e = 0; e < slot->events.size(); e++) INPUTNET_ApplyEvent(slot->events[e]);
    }
    rb_late.clear();
    if (inputnet_is_server) rb_resync_wanted = true;
    else { rb_desynced = true; rb_resync_last_sent = 0; }
}

/* Server: type "text" and Enter as this machine's input, one key a frame from the next frame our input can go into, so
 * that every machine in the session gets the same key presses at the same frames. */
static void RB_QueueTyping(const std::string &text) {
    InputNetPeer &self = inputnet_peers[inputnet_selfid];
    uint32_t f = INPUTNET_ScheduleFrame();
    std::vector<std::vector<InputNetEvent> > frames;
    RB_TypingFrames(text, frames);
    for (size_t i = 0; i < frames.size(); i++) {
        InputNetFrame &slot = INPUTNET_FrameSlot(self, f++);
        slot.events.insert(slot.events.end(), frames[i].begin(), frames[i].end());
    }
}

static unsigned int RB_Participants(void) {
    unsigned int n = 0;
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++)
        if (inputnet_peers[i].active && inputnet_peers[i].ready) n++;
    return n;
}


/* Corrects frames that ran with a wrong guess (found now, or earlier and kept): true when a replay starts. A frame start
 * inside a nested loop of a moment has nothing to go back to; the correction then waits for the next frame start while
 * the frame is still in the window. Input beyond reach is applied late (RB_Unreachable). */
static bool RB_Correct(uint32_t frame) {
    const uint32_t found = RB_FindMisprediction(frame);
    const uint32_t g = std::min(found, rb_pending_wrong);
    if (g == UINT32_MAX) return false;
    if (RB_Rollback(g, frame)) { rb_pending_wrong = UINT32_MAX; rb_late.clear(); return true; }
    const uint32_t lowest = (frame > inputnet_rbwindow) ? frame - inputnet_rbwindow : 0;
    if (g > lowest + 1u && RB_RestorableAtOrBefore(g, lowest) == UINT32_MAX) {
        rb_pending_wrong = g;       /* try again at the next frame start */
        return false;
    }
    rb_pending_wrong = UINT32_MAX;
    RB_Unreachable();
    return false;
}

/* cycles=max in RollbackNet. The speed is part of the shared machine: a guess from one computer's own timing would differ
 * on every computer. So every process measures itself each second (RB_MeasureLoad: wall time less what it slept and
 * what it waited for other computers or copies; the CPU core's part of it) and proposes a speed (RB_Propose: "rollback
 * max load" percent busy, and a usual deep replay within RB_REPLAY_BUDGET_US); a cluster's copies tell their leader,
 * which takes the lowest, and every computer reports its proposal with its input. The server sets the lowest proposal
 * for everybody as an event of its own input (INEV_CYCLEMAX: every machine and every copy applies it at the same frame,
 * a replay too). Called at every real-time frame start of a leader. */
static void RB_MaxCycles(void) {
    if (!CPU_CycleAutoAdjust || rb_joining) { rb_ml_t0 = 0; return; }
    if (!RB_MeasureLoad()) return;
    RB_Propose();
    for (unsigned c = 0; c < rbc_copies; c++)      /* (a cluster: as fast as its slowest copy) */
        if (c != rbc_copy && rbc_f[c].present && rbc_f[c].cap != 0 && GetTicks() - rbc_f[c].cap_at < 5000u)
            rb_cap_local = std::min(rb_cap_local, rbc_f[c].cap);
    const double load = rb_ml_load;
    const double limit = (CPU_CycleLimit > 0) ? (double)CPU_CycleLimit : 1000000.0;
    if (!inputnet_is_server) return;

    /* the server: the lowest proposal; a participant that has not proposed yet keeps the speed from rising */
    const uint32_t tnow = GetTicks();
    uint32_t want = rb_cap_local;
    bool all = true;
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        const InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready || p.local) continue;
        if (rb_cap_peer[i] != 0 && tnow - rb_cap_peer_at[i] < 5000u) want = std::min(want, rb_cap_peer[i]);
        else all = false;
    }
    if (tnow - rb_ml_changed_at < 1000u) return;
    const double cur = (double)CPU_CycleMax;
    double next = cur;
    /* down by at most 40% a second (one busy second is often a burst of replays), up by 15% a second, 25% while far
     * below what everybody proposes; 50% while slow (the start: cycles=max begins at 3000) */
    if ((double)want < cur * 0.95) next = std::max((double)want, cur * 0.6);
    else if (all && (double)want > cur * 1.12)
        next = std::min((double)want, (double)want > cur * 2.0 ? (cur < 20000.0 ? cur * 1.5 : cur * 1.25) : cur * 1.15 + 500.0);
    next = std::max(2000.0, std::min(next, limit));
    const uint32_t n = (uint32_t)next;
    if (n == (uint32_t)CPU_CycleMax) return;
    InputNetEvent ev;
    ev.type = INEV_CYCLEMAX;
    ev.cyclemax = n;
    ev.pressed = 0;
    INPUTNET_QueueLocal(ev);
    rb_ml_changed_at = tnow;
    if (tnow - rb_ml_told_at > 10000u || (double)n < cur * 0.8) {
        LOG_MSG("INPUTNET: speed: %u cycles per ms for everybody (was %u; here %.0f%% busy, proposing %u)",
            n, (unsigned)CPU_CycleMax, load * 100.0, rb_cap_local);
        rb_ml_told_at = tnow;
    }
}

/* A frame starts: save it, correct earlier frames if needed, then run it. */
static void RB_FrameBoundary(uint32_t frame) {
    inputnet_frame = frame;
    RB_Capture(frame);
    if (rbc_copies && rbc_leader && !RBC_LeaderBarrier(frame, RBC_ROUTE)) return;   /* every copy at this frame start */
    if (rb_selftest_end != 0 && frame <= rb_selftest_end) {
        if (frame >= rb_selftest_first) RB_SelfTestCompare(frame);   /* (a replay may start before the frames recorded) */
        if (frame == rb_selftest_end) rb_selftest_end = 0;
    }

    if (inputnet_rollback_resim) {
        if (frame < rb_target) { RB_ApplyInputs(frame); return; }
        inputnet_rollback_resim = false;    /* caught up with real time */
        MIXER_SetRollbackDiscard(false);
        extern void RENDER_RedrawAll(void);
        RENDER_RedrawAll();                 /* nothing of the replayed frames was shown */
        RB_ReplayEnd();
        if (rb_replay_started) {
            const uint64_t us = RB_MicroTicks() - rb_replay_started;
            rb_stat_replay_us += us; rb_stat_replays++;
            if (us > rb_stat_replay_max_us) rb_stat_replay_max_us = us;
            rb_replay_started = 0;
        }
    }
    rb_realframe = frame;

    INPUTNET_PollSocket();
    if (!inputnet_session || !inputnet_rollback_active) return;
    if (rbc_copies) {
        if (!rb_resync_ready.empty() && !rbc_resync_raw.empty() && RBC_CanApplyMachines(frame)) {
            /* client: the server's machines, loaded where every copy's host stack is steady */
            if (RB_ApplyClusterResync(frame)) rb_resync_applied = true;
            else LOG_MSG("INPUTNET: resync: could not load the server's machines");
            std::vector<uint8_t>().swap(rb_resync_ready);
            std::vector<uint8_t>().swap(rbc_resync_raw);
        }
        if (rb_resync_applied) { rb_resync_applied = false; return; }
        if (rb_joining) {           /* not playing yet: our copies get to where they can take the machines over */
            RBC_JoinStep(frame);
            RB_TimeSync(frame);
            RBC_ApplyInputs(frame);
            return;
        }
    }
    else if (!rb_resync_ready.empty() && frame > 0 && rb_snap[(frame - 1u) % RB_RING].valid &&
        rb_snap[(frame - 1u) % RB_RING].frame == frame - 1u && rb_snap[(frame - 1u) % RB_RING].context == RB_HostContext()) {
        /* client: the server's state, loaded where the host's stack is steady */
        if (RB_ApplyResyncBlob(rb_resync_ready, rb_resync_ready_raw)) rb_resync_applied = true;
        else LOG_MSG("INPUTNET: resync: could not load the server's state");
        std::vector<uint8_t>().swap(rb_resync_ready);
    }
    if (rb_resync_applied) { rb_resync_applied = false; return; }   /* now replaying from the server's state */

    if (rb_desynced) {
        /* client: until the server's state arrives, keep asking for it */
        if (!inputnet_is_server && (GetTicks() - rb_resync_last_sent) > 500u) {
            uint8_t buf[32];
            InputNetWriter w(buf,sizeof(buf));
            INPUTNET_BeginPacket(w,INPKT_RESYNC);
            w.u32(0);   /* request */
            INPUTNET_SendToPeers(buf,w.size());
            rb_resync_last_sent = GetTicks();
        }
    }

    RB_UpdateLockstep(frame);
    if (RB_Correct(frame)) return;          /* the main loop now replays the frames at full speed */

    if (!RB_WaitForWindow(frame)) return;
    if (rb_resync_applied) { rb_resync_applied = false; return; }
    if (RB_Correct(frame)) return;          /* input that arrived while waiting */

    {
        static const char *fr = getenv("NN_RB_FORCE_RESYNC");   /* debugging: the server sends its state at this frame */
        if (fr != NULL && *fr && inputnet_is_server && frame == (uint32_t)strtoul(fr, NULL, 10)) rb_resync_wanted = true;
    }
    /* (only where the host's stack is as it was at the last frame start: a state saved inside a nested loop of a
     * moment, a BIOS call DOS makes for the console, cannot be loaded by a machine that is not inside the same call) */
    if (rbc_copies && inputnet_is_server) RBC_ServerFiles();    /* a joining computer's file request */
    const bool rb_stable = frame > 0 && rb_snap[(frame - 1u) % RB_RING].valid && rb_snap[(frame - 1u) % RB_RING].frame == frame - 1u &&
        rb_snap[(frame - 1u) % RB_RING].context == RB_HostContext() && rb_pending_wrong == UINT32_MAX &&
        (!rbc_copies || RBC_AllStable(frame));
    if (inputnet_is_server && rb_resync_wanted && rb_stable && RB_ServerResync(frame)) rb_resync_wanted = false;
    if (inputnet_rollback_resim) return;    /* a correction came first: send our state after the replay */

    {
        static const char *st = getenv("NN_RB_SELFTEST");
        static uint32_t last_test = 0;
        if (st != NULL && *st && rb_selftest_period == 0) rb_selftest_period = (uint32_t)std::max(5ul, strtoul(st, NULL, 10));
        const uint32_t depth = 4;
        if (rb_selftest_period && !rb_lockstep && frame > 20u && (frame % rb_selftest_period) == 0 &&
            rb_selftest_end == 0 && frame != last_test) {
            last_test = frame;
            rb_selftest_first = frame - depth + 1u;
            for (uint32_t f = frame - depth + 1u; f <= frame; f++) {
                const unsigned k = f % RB_RING;
                rb_selftest_hash[k] = rb_snap[k].hash;
                rb_selftest_dev[k] = rb_snap[k].devices;
            }
            /* registers and memory hash of the newest frame are the live ones now */
            RB_SelfTestRegs(rb_selftest_regs[frame % RB_RING]);
            rb_selftest_mem[frame % RB_RING] = rb_memhash;
            rb_selftest_runs++;
            if (RB_Rollback(frame - depth, frame)) { rb_selftest_end = frame; rb_stat_rollbacks--; return; }
        }
        if (rb_selftest_period && (frame % 500u) == 7u)
            LOG_MSG("RBSELFTEST %u replays, %u frames differed", (unsigned)rb_selftest_runs, (unsigned)rb_selftest_fails);
    }

    if (inputnet_is_server && !inputnet_rbautorun_done && inputnet_rbplayers > 0 && !inputnet_rbautorun.empty() &&
        dos_shell_reading_input && RB_Participants() >= inputnet_rbplayers) {
        inputnet_rbautorun_done = true;
        LOG_MSG("INPUTNET: %u participants: typing \"%s\"", RB_Participants(), inputnet_rbautorun.c_str());
        RB_QueueTyping(inputnet_rbautorun);
    }
    RB_SendInput(frame);
    if (rb_lockstep) {
        if (!RB_WaitForInputs(frame)) return;
        if (rb_resync_applied) { rb_resync_applied = false; return; }
        if (RB_Correct(frame)) return;
    }
    RB_ApplyInputs(frame);
    RB_TimeSync(frame);
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        InputNetPeer &p = inputnet_peers[i];
        if (!p.active || !p.ready || p.local) continue;
        for (unsigned k = 0; k < INPUTNET_CHECK_RING; k++)
            if (rb_peercheck[i][k].valid) RB_CompareCheck(p, i, rb_peercheck[i][k].frame);
    }
    {
        const uint64_t t = RB_MicroTicks();
        if (rb_hitch_last != 0 && t > rb_hitch_last) {
            const uint64_t gap = t - rb_hitch_last;
            if (gap > 30000u) rb_stat_hitches++;
            rb_stat_maxgap_us = std::max(rb_stat_maxgap_us, gap);
        }
        rb_hitch_last = t;
    }
    RB_MaxCycles();
    RB_LogStats();
}

void INPUTNET_RollbackTick(void) {
    if (rbc_copies && !rbc_leader) { RBC_FollowerRollbackTick(); return; }
    if (!inputnet_rollback_active || !inputnet_session) return;
    if (!rb_have_first) {
        rb_have_first = true;
        rb_tick = rb_start_frame * inputnet_frame_ms;
        RB_FrameBoundary(rb_start_frame);
        return;
    }
    rb_tick++;
    if ((rb_tick % inputnet_frame_ms) == 0) RB_FrameBoundary(rb_tick / inputnet_frame_ms);
    else if (!inputnet_rollback_resim) {
        INPUTNET_PollSocket();
        if (rb_resync_applied) rb_resync_applied = false;   /* replaying from the server's state already */
    }
}

/* RollbackNet: what makes two machines run differently even from the same state. The joining machine takes the
 * server's whole state over, but these come from each computer's own build and settings: the precision of the
 * emulator's timing arithmetic (long double is 80 bits on x86 builds and 64 or 128 on ARM ones), the CPU core and
 * speed, memory, sound devices (their emulation and how they are driven), the joystick (read from the host). */
static std::string RB_Fingerprint(void) {
    std::string fp;
    char buf[96];
#if defined(HAS_LONG_DOUBLE)
    snprintf(buf, sizeof(buf), "timing=%u/%u", (unsigned)LDBL_MANT_DIG, (unsigned)sizeof(Bitu));
#else
    snprintf(buf, sizeof(buf), "timing=%u/%u", (unsigned)DBL_MANT_DIG, (unsigned)sizeof(Bitu));
#endif
    fp += buf;
    struct Key { const char *section, *name; };
    static const Key keys[] = {
        { "cpu", "core" }, { "cpu", "cputype" }, { "cpu", "cycles" },
        { "dosbox", "machine" }, { "dosbox", "memsize" },
        { "sblaster", "sbtype" }, { "sblaster", "sbbase" }, { "sblaster", "irq" }, { "sblaster", "dma" }, { "sblaster", "hdma" },
        { "sblaster", "oplmode" }, { "sblaster", "oplemu" }, { "sblaster", "force goldplay" },
        { "gus", "gus" }, { "speaker", "pcspeaker" }, { "speaker", "tandy" }, { "speaker", "disney" },
        { "joystick", "joysticktype" },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        Section_prop *sec = static_cast<Section_prop*>(control->GetSection(keys[i].section));
        if (sec == NULL) continue;
        Property *prop = sec->Get_prop(keys[i].name);
        if (prop == NULL) continue;
        fp += ' ';
        fp += keys[i].name;
        fp += '=';
        fp += prop->GetValue().ToString();
    }
    extern Bitu MIXER_GetRate(void);
    snprintf(buf, sizeof(buf), " mixer=%u", (unsigned)MIXER_GetRate());
    fp += buf;
    return fp;
}

/* The entries of two fingerprints that differ, "name (server: x, here: y)". */
static std::string RB_FingerprintDiff(const std::string &theirs, const std::string &mine) {
    auto split = [](const std::string &f) {
        std::vector<std::pair<std::string, std::string> > out;
        size_t pos = 0;
        while (pos < f.size()) {
            /* entries are "name=value" separated by spaces, but names and values may hold spaces themselves: an entry
             * ends where the next known name begins, which is good enough for a message */
            size_t eq = f.find('=', pos);
            if (eq == std::string::npos) break;
            size_t next = f.find('=', eq + 1);
            size_t end = (next == std::string::npos) ? f.size() : f.rfind(' ', next);
            if (end == std::string::npos || end <= eq) end = f.size();
            out.push_back(std::make_pair(f.substr(pos, eq - pos), f.substr(eq + 1, end - eq - 1)));
            pos = (end < f.size()) ? end + 1 : f.size();
        }
        return out;
    };
    const auto a = split(theirs), b = split(mine);
    std::string diff;
    for (size_t i = 0; i < b.size(); i++) {
        std::string other = "(none)";
        for (size_t j = 0; j < a.size(); j++) if (a[j].first == b[i].first) other = a[j].second;
        if (other == b[i].second) continue;
        if (!diff.empty()) diff += ", ";
        diff += b[i].first + " (server: " + other + ", here: " + b[i].second + ")";
    }
    if (diff.empty()) diff = theirs.empty() ? "the server's DOSBox-X has no RollbackNet fingerprint (an older build)" : theirs;
    return diff;
}

static void RB_SessionStarted(void) {
    RB_SeatsReset();
    rb_cap_local = 0; rb_ml_t0 = 0; rb_ml_changed_at = 0; rb_ml_cost_s = -1; rb_ml_fixed_s = -1; rb_rp_n = 0; rb_rp_pos = 0; rb_rp_t0 = 0;
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) { rb_cap_peer[i] = 0; rb_cap_peer_at[i] = 0; }
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS; i++) {
        rb_have[i] = 0; rb_checked[i] = 0; rb_acked[i] = 0;
        rb_peerframe[i] = 0; rb_peerframe_at[i] = 0; rb_peerstamp[i] = 0; rb_peerstamp_at[i] = 0; rb_rtt[i] = 0;
        for (unsigned k = 0; k < RB_RING; k++) rb_predicted[i][k] = 0;
        for (unsigned k = 0; k < INPUTNET_CHECK_RING; k++) rb_peercheck[i][k].valid = false;
    }
    RB_InitRegions();
    rb_have_first = false;
    rb_target = 0;
    rb_realframe = 0;
    rb_advantage = 0;
    rb_last_slowdown = 0;
    rb_desynced = false;
    rb_epoch = 0;
    rb_late.clear();
    rb_pending_wrong = UINT32_MAX;
    rb_unrestorable = 0;
    rb_lockstep = false;
    rb_lockstep_ctx = RB_HostContext();
    rb_resync_id = 0;
    rb_resync_wanted = false;
    std::vector<uint8_t>().swap(rb_resync_ready);
    rb_resync_applied = false;
    rb_stat_rollbacks = rb_stat_replayed = rb_stat_maxdepth = rb_stat_slowdowns = 0;
    rb_stat_stalls = rb_stat_checks = rb_stat_desyncs = rb_stat_resyncs = 0;
    rb_stat_capture_us = rb_stat_captures = rb_stat_pages = 0;
    rb_stat_last_log = GetTicks();
    inputnet_rollback_resim = false;
    MIXER_SetRollbackDiscard(false);
    inputnet_rollback_active = true;
    LOG_MSG("INPUTNET: RollbackNet running: %u ms frames, %u frame local delay, %u frame window, RAM %lu KB, video memory %lu KB",
        inputnet_frame_ms,inputnet_delay,inputnet_rbwindow,
        (unsigned long)(rb_region[0].pages * 4u),(unsigned long)(rb_region[1].pages * 4u));
    if (rbc_copies && rbc_leader) RBC_LeaderStart(rb_start_frame, rb_joining);
}

static void RB_SessionEnded(void) {
    if (rbc_copies && rbc_leader && inputnet_rollback_active) RBC_LeaderEnd();
    rb_joining = false;
    if (inputnet_rollback_active) {
        LOG_MSG("INPUTNET: RollbackNet ended at frame %u: %u rollbacks, %u frames replayed, %u desyncs, %u resyncs",
            (unsigned)rb_realframe,(unsigned)rb_stat_rollbacks,(unsigned)rb_stat_replayed,(unsigned)rb_stat_desyncs,(unsigned)rb_stat_resyncs);
    }
    inputnet_rollback_active = false;
    inputnet_rollback_resim = false;
    MIXER_SetRollbackDiscard(false);
    for (unsigned i = 0; i < RB_RING; i++) {
        rb_snap[i].valid = false;
        std::string().swap(rb_snap[i].devices);
        std::vector<uint8_t>().swap(rb_snap[i].undo_data);
    }
    for (unsigned r = 0; r < 2; r++) { std::vector<uint8_t>().swap(rb_region[r].shadow); rb_region[r].hash.clear(); }
    extern void LEAN_FreeRetiredFiles(void);
    LEAN_FreeRetiredFiles();
    RBT_Stop();
    RB_UnmapReceivedFiles();
    rb_late.clear();
    rb_lockstep = false;
}


/* ------------------------------------------------------------------------- */
/* Public status                                                              */
/* ------------------------------------------------------------------------- */

bool INPUTNET_IsServer(void) { return inputnet_is_server && inputnet_link_up; }
/* (a cluster's copy is one of the session's machines from the start, the followers without a link of their own: they
 * report the same fixed file times, disk and clock as the others: drive_local.cpp, cmos.cpp) */
bool INPUTNET_IsConnected(void) { return inputnet_link_up || rbc_copies != 0; }
/* NN_RB_TRACE: the frame for other components' trace lines (dos.cpp logs every INT 21h call with it) */
uint32_t INPUTNET_TraceFrame(void) { return inputnet_frame; }

/* ------------------------------------------------------------------------- */
/* INPUTNET.COM                                                               */
/* ------------------------------------------------------------------------- */

class INPUTNET : public Program {
public:
    void HelpCommand(const char *helpStr) {
        if (strcasecmp("connect",helpStr) == 0) {
            WriteOut("INPUTNET CONNECT joins the shared input session hosted by another DOSBox-X\n");
            WriteOut("session.  The \"address\" parameter is the IP address or the host name of the\n");
            WriteOut("server computer, and a UDP port may be given as well (default %u).\n\n",(unsigned)inputnet_default_port);
            WriteOut("The server sends its complete emulator state on connect, so this DOSBox-X\n");
            WriteOut("session will continue from wherever the server currently is.\n\n");
            WriteOut("The syntax for INPUTNET CONNECT is:\n\n");
            WriteOut("INPUTNET CONNECT address <port>\n\n");
            return;
        }
        if (strcasecmp("disconnect",helpStr) == 0) {
            WriteOut("INPUTNET DISCONNECT leaves the shared input session.  Local input is applied\n");
            WriteOut("directly again afterwards.\n\n");
            WriteOut("The syntax for INPUTNET DISCONNECT is:\n\n");
            WriteOut("INPUTNET DISCONNECT\n\n");
            return;
        }
        if (strcasecmp("startserver",helpStr) == 0) {
            WriteOut("INPUTNET STARTSERVER hosts a shared input session in this DOSBox-X session.\n");
            WriteOut("Other DOSBox-X sessions can then join with INPUTNET CONNECT.  When one does,\n");
            WriteOut("this session's emulator state is copied over to it and both machines continue\n");
            WriteOut("in lockstep with the keyboard and mouse input of every participant.\n\n");
            WriteOut("The syntax for INPUTNET STARTSERVER is:\n\n");
            WriteOut("INPUTNET STARTSERVER <port>\n\n");
            return;
        }
        if (strcasecmp("stopserver",helpStr) == 0) {
            WriteOut("INPUTNET STOPSERVER stops the shared input session hosted by this DOSBox-X\n");
            WriteOut("session and disconnects every participant.\n\n");
            WriteOut("The syntax for INPUTNET STOPSERVER is:\n\n");
            WriteOut("INPUTNET STOPSERVER\n\n");
            return;
        }
        if (strcasecmp("ping",helpStr) == 0) {
            WriteOut("INPUTNET PING measures the round trip time to the other participants.\n\n");
            WriteOut("The syntax for INPUTNET PING is:\n\n");
            WriteOut("INPUTNET PING\n\n");
            return;
        }
        if (strcasecmp("status",helpStr) == 0) {
            WriteOut("INPUTNET STATUS reports the state of the shared input session, the list of\n");
            WriteOut("participants and how much emulated time was spent waiting for them.\n\n");
            WriteOut("The syntax for INPUTNET STATUS is:\n\n");
            WriteOut("INPUTNET STATUS\n\n");
            return;
        }
        WriteOut("The syntax of this command is:\n\n");
        WriteOut("INPUTNET [ CONNECT | DISCONNECT | STARTSERVER | STOPSERVER | PING | HELP |\n           STATUS ]\n\n");
    }

    /* Marks the shared input layer as "a DOS program is on the stack" for as
     * long as this command runs. */
    class InProgram {
    public:
        InProgram() { inputnet_in_program = true; }
        ~InProgram() { inputnet_in_program = false; }
    };

    /* "mode = remote" */
    void RunRemote(void) {
        if (strcasecmp("startserver",temp_line.c_str()) == 0) {
            if (REMOTEPLAY_ServerRunning()) {
                WriteOut("A remote play server is already running in this DOSBox-X session.\n");
                return;
            }
            uint16_t port = inputnet_default_port;
            if (cmd->FindCommand(1,temp_line,/*remove*/true))
                port = (uint16_t)strtol(temp_line.c_str(),NULL,10);
            if (REMOTEPLAY_StartServer(port)) {
                WriteOut("Remote play server started on UDP port %u.\n",(unsigned)port);
                WriteOut("Other players join by starting DOSBox-X with\n");
                WriteOut("-inputnet-connect <this machine's address>:%u\n",(unsigned)port);
            }
            else {
                WriteOut("Remote play server failed to start.\n");
            }
            return;
        }
        if (strcasecmp("stopserver",temp_line.c_str()) == 0) {
            if (!REMOTEPLAY_ServerRunning()) {
                WriteOut("No remote play server is running in this DOSBox-X session.\n");
                return;
            }
            REMOTEPLAY_StopServer("stopped by the host");
            WriteOut("Remote play server stopped.\n");
            return;
        }
        if (strcasecmp("status",temp_line.c_str()) == 0) {
            REMOTEPLAY_Status(this);
            return;
        }
        WriteOut("This DOSBox-X session uses remote play (\"mode = remote\" in [inputnet]).\n");
        WriteOut("To join a game, start DOSBox-X with -inputnet-connect <address>:<port>.\n");
        WriteOut("Here only INPUTNET STARTSERVER, STOPSERVER and STATUS are available.\n");
    }

    void Run(void) override {
        InProgram inprogram;

        if (cmd->GetCount() == 0) {
            WriteOut("Shared keyboard/mouse input over UDP between DOSBox-X sessions.\n\n");
            WriteOut("The following are valid INPUTNET commands:\n\n");
            WriteOut("INPUTNET CONNECT      INPUTNET DISCONNECT     INPUTNET STARTSERVER\n");
            WriteOut("INPUTNET STOPSERVER   INPUTNET PING           INPUTNET STATUS\n\n");
            WriteOut("A summary of each command is given by:\n\n");
            WriteOut("INPUTNET HELP command\n\n");
            return;
        }

        if (cmd->FindCommand(1,temp_line,/*remove*/true)) {
            if (strcasecmp("help",temp_line.c_str()) == 0) {
                if (!cmd->FindCommand(1,temp_line,/*remove*/true)) {
                    WriteOut("The following are valid INPUTNET commands:\n\n");
                    WriteOut("INPUTNET CONNECT      INPUTNET DISCONNECT     INPUTNET STARTSERVER\n");
                    WriteOut("INPUTNET STOPSERVER   INPUTNET PING           INPUTNET STATUS\n\n");
                    WriteOut("A summary of each command is given by:\n\n");
                    WriteOut("INPUTNET HELP command\n\n");
                }
                else {
                    HelpCommand(temp_line.c_str());
                }
                return;
            }

            if (inputnet_remote) {
                RunRemote();
                return;
            }

            if (strcasecmp("startserver",temp_line.c_str()) == 0) {
                if (INPUTNET_IsConnected()) {
                    WriteOut("A shared input session is already running in this DOSBox-X session.\n");
                    return;
                }

                uint16_t port = inputnet_default_port;
                if (cmd->FindCommand(1,temp_line,/*remove*/true))
                    port = (uint16_t)strtol(temp_line.c_str(),NULL,10);

                if (INPUTNET_StartServer(port)) {
                    WriteOut("Shared input server started on UDP port %u.\n",(unsigned)port);
                    WriteOut("Run \"INPUTNET CONNECT <this machine's address> %u\" on the other machine.\n",(unsigned)port);
                }
                else {
                    WriteOut("Shared input server failed to start.\n");
                    if (port < 1024) WriteOut("Try a port number above 1024. See INPUTNET HELP STARTSERVER.\n");
                }
                return;
            }

            if (strcasecmp("stopserver",temp_line.c_str()) == 0) {
                if (!INPUTNET_IsServer()) {
                    WriteOut("No shared input server is running in this DOSBox-X session.\n");
                    return;
                }
                INPUTNET_CloseLink("stopped by the user",true);
                WriteOut("Shared input server stopped.\n");
                return;
            }

            if (strcasecmp("connect",temp_line.c_str()) == 0) {
                if (INPUTNET_IsConnected()) {
                    WriteOut("This DOSBox-X session is already part of a shared input session.\n");
                    return;
                }
                if (!cmd->FindCommand(1,temp_line,/*remove*/true)) {
                    WriteOut("Server address not specified.\n");
                    return;
                }

                const std::string host = temp_line;
                uint16_t port = inputnet_default_port;
                if (cmd->FindCommand(1,temp_line,/*remove*/true))
                    port = (uint16_t)strtol(temp_line.c_str(),NULL,10);

                WriteOut("Connecting to %s port %u...\n",host.c_str(),(unsigned)port);

                std::string err;
                if (INPUTNET_Connect(host.c_str(),port,err))
                    WriteOut("Connected. The emulator state of the server is being taken over.\n");
                else
                    WriteOut("Failed to connect to %s: %s.\n",host.c_str(),err.c_str());
                return;
            }

            if (strcasecmp("disconnect",temp_line.c_str()) == 0) {
                if (!INPUTNET_IsConnected()) {
                    WriteOut("This DOSBox-X session is not part of a shared input session.\n");
                    return;
                }
                INPUTNET_CloseLink("disconnected by the user",true);
                WriteOut("Left the shared input session.\n");
                return;
            }

            if (strcasecmp("status",temp_line.c_str()) == 0) {
                WriteOut("Shared input status:\n\n");
                WriteOut("Server status: %s\n",INPUTNET_IsServer() ? "ACTIVE" : "INACTIVE");
                WriteOut("Link status:   %s\n",INPUTNET_IsConnected() ? "CONNECTED" : "DISCONNECTED");
                WriteOut("Session:       %s\n",inputnet_session ? "RUNNING" : "NOT RUNNING");
                if (!INPUTNET_IsConnected()) return;

                WriteOut("UDP port:      %u\n",(unsigned)inputnet_active_port);
                WriteOut("Frame:         %u ms, input delay %u frames (%u ms)\n",
                    inputnet_frame_ms,inputnet_delay,inputnet_frame_ms*inputnet_delay);
                WriteOut("Sharing:       %s%s%s\n",
                    inputnet_share_keyboard ? "keyboard" : "",
                    (inputnet_share_keyboard && inputnet_share_mouse) ? " and " : "",
                    inputnet_share_mouse ? "mouse" : "");
                WriteOut("Cycle sync:    %s",
                    inputnet_cyclesync == INCYCLES_AUTO ? "server sets the speed" :
                    inputnet_cyclesync == INCYCLES_FREEZE ? "frozen for the session" : "not managed");
                if (inputnet_cyclemax)
                    WriteOut(" (%u cycles, %u update%s)\n",inputnet_cyclemax,
                        inputnet_stat_cycleupdates,inputnet_stat_cycleupdates == 1 ? "" : "s");
                else
                    WriteOut("\n");
                if (inputnet_checkevery)
                    WriteOut("Desync check:  every %u frames, %u mismatch%s so far\n",
                        inputnet_checkevery,inputnet_stat_desyncs,inputnet_stat_desyncs == 1 ? "" : "es");
                else
                    WriteOut("Desync check:  disabled\n");
                if (inputnet_stat_frames)
                    WriteOut("Waiting:       %u ms over %u frames\n",inputnet_stat_stallms,inputnet_stat_frames);

                WriteOut("\nParticipants:\n\n");
                for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
                    InputNetPeer &p = inputnet_peers[i];
                    if (!p.active) continue;
                    if (p.local) {
                        WriteOut("     %u: this DOSBox-X session\n",i);
                    }
                    else {
                        WriteOut("     %u: %d.%d.%d.%d port %u%s\n",i,
                            (int)( p.addr.host        & 0xFF),(int)((p.addr.host >>  8u) & 0xFF),
                            (int)((p.addr.host >> 16u) & 0xFF),(int)((p.addr.host >> 24u) & 0xFF),
                            (unsigned)SDLNet_Read16(&p.addr.port),
                            p.ready ? "" : " (joining)");
                    }
                }
                WriteOut("\n");
                return;
            }

            if (strcasecmp("ping",temp_line.c_str()) == 0) {
                if (!INPUTNET_IsConnected()) {
                    WriteOut("This DOSBox-X session is not part of a shared input session.\n");
                    return;
                }

                const uint32_t token = GetTicks();
                uint8_t buf[32];
                InputNetWriter w(buf,sizeof(buf));
                INPUTNET_BeginPacket(w,INPKT_PING);
                w.u32(token);
                INPUTNET_SendToPeers(buf,w.size());

                WriteOut("Pinging the other participants:\n\n");

                const uint32_t start = GetTicks();
                while ((GetTicks() - start) < 1500u) {
                    CALLBACK_Idle();
                    INPUTNET_PollSocket();
                }

                bool any = false;
                for (unsigned i=0;i < INPUTNET_MAX_PEERS;i++) {
                    InputNetPeer &p = inputnet_peers[i];
                    if (!p.active || p.local) continue;
                    any = true;
                    WriteOut("Response from %d.%d.%d.%d, port %u time=%ums\n",
                        (int)( p.addr.host        & 0xFF),(int)((p.addr.host >>  8u) & 0xFF),
                        (int)((p.addr.host >> 16u) & 0xFF),(int)((p.addr.host >> 24u) & 0xFF),
                        (unsigned)SDLNet_Read16(&p.addr.port),p.rtt);
                }
                if (!any) WriteOut("No other participants.\n");
                WriteOut("\n");
                return;
            }

            HelpCommand(temp_line.c_str());
        }
    }
};

void INPUTNET_ProgramStart(Program * * make) {
    *make = new INPUTNET;
}

/* ------------------------------------------------------------------------- */
/* Module life cycle                                                          */
/* ------------------------------------------------------------------------- */

bool addinputnet = false;
void VFILE_Remove(const char *name,const char *dir = "");

class INPUTNETModule : public Module_base {
public:
    INPUTNETModule(Section *configuration) : Module_base(configuration) {
        Section_prop *section = static_cast<Section_prop *>(configuration);

        addinputnet = false;
        inputnet_enabled = section->Get_bool("inputnet");

        /* Asking for a session on the command line implies the feature. */
        if (!control->opt_inputnet_connect.empty() || control->opt_inputnet_server >= 0)
            inputnet_enabled = true;

        if (!inputnet_enabled) return;

        int v;

        v = section->Get_int("port");
        inputnet_default_port = (uint16_t)((v > 0 && v < 65536) ? v : (int)INPUTNET_DEFAULT_PORT);

        v = section->Get_int("frame");
        inputnet_frame_ms = (unsigned int)((v >= 1 && v <= 100) ? v : 10);

        inputnet_rollback = std::string(section->Get_string("mode")) == "rollback";

        v = section->Get_int("delay");
        if (inputnet_rollback) inputnet_delay = (unsigned int)((v >= 0 && v <= 8) ? v : 0);
        else inputnet_delay = (unsigned int)((v >= 1 && v <= 64) ? v : 3);

        v = section->Get_int("rollback window");
        inputnet_rbwindow = (unsigned int)((v >= 2 && v <= 48) ? v : 16);
        v = section->Get_int("rollback max load");
        rb_maxload = (unsigned int)((v >= 20 && v <= 95) ? v : 85);
        v = section->Get_int("players");
        inputnet_rbplayers = (unsigned int)((v >= 0 && v <= (int)INPUTNET_MAX_PEERS) ? v : 0);
        inputnet_rbautorun = section->Get_string("autorun");
        inputnet_rbautorun_done = false;
        /* RollbackNet runs a "cluster" of copies on every computer: one (the game itself, every player's input to it), or
         * one per player for IPX/NullModem-over-RollbackNet. Either way the host's session runs from the start and
         * others join it, before or during the game (RBC_ServerHello). */
        v = section->Get_int("cluster");
        rbc_copies = 0; rbc_copy = 0; rbc_leader = false; rbc_port = 0;
        if (inputnet_rollback) {
            rbc_copies = (v >= 2 && v <= (int)INPUTNET_MAX_PEERS) ? (unsigned int)v : 1u;
            if (rbc_copies >= 2) {
                v = section->Get_int("cluster copy");
                rbc_copy = (v >= 0 && v < (int)rbc_copies) ? (unsigned int)v : 0u;
                rbc_leader = std::string(section->Get_string("cluster role")) == "leader";
                v = section->Get_int("cluster port");
                rbc_port = (v > 0 && v < 65536) ? (unsigned int)v : 0u;
                if (rbc_port == 0) { LOG_MSG("INPUTNET: cluster: no \"cluster port\" set, so one copy"); rbc_copies = 1; rbc_copy = 0; }
                else LOG_MSG("INPUTNET: cluster of %u copies: this is player %u's, the %s (port %u)", rbc_copies, rbc_copy + 1u,
                    rbc_leader ? "leader" : "follower", rbc_port);
            }
            if (rbc_copies == 1) rbc_leader = true;
        }

        v = section->Get_int("redundancy");
        inputnet_redundancy = (unsigned int)((v >= 1 && v <= 32) ? v : 4);

        v = section->Get_int("timeout");
        inputnet_timeout = (unsigned int)((v >= 250 && v <= 600000) ? v : 10000);

        v = section->Get_int("desync check");
        inputnet_checkevery = (unsigned int)((v >= 0 && v <= 100000) ? v : 100);

        {
            const std::string cs = section->Get_string("cycle sync");
            if (cs == "freeze")   inputnet_cyclesync = INCYCLES_FREEZE;
            else if (cs == "off") inputnet_cyclesync = INCYCLES_OFF;
            else                  inputnet_cyclesync = INCYCLES_AUTO;
        }

        inputnet_share_keyboard = section->Get_bool("share keyboard");
        inputnet_share_mouse = section->Get_bool("share mouse");
        inputnet_sync_state = section->Get_bool("sync state");

        /* Command line beats the config file. */
        inputnet_auto_connect = control->opt_inputnet_connect;
        inputnet_auto_server = control->opt_inputnet_server;

        if (inputnet_auto_connect.empty() && inputnet_auto_server < 0) {
            const std::string cfg_connect = section->Get_string("connect");
            const std::string cfg_server = section->Get_string("startserver");
            if (!cfg_connect.empty()) {
                inputnet_auto_connect = cfg_connect;
            }
            else if (!cfg_server.empty()) {
                inputnet_auto_server = atoi(cfg_server.c_str());
            }
        }

        /* -1 means "no server requested"; anything else that is not a usable
         * port number falls back to the configured default. */
        if (inputnet_auto_server != -1 && (inputnet_auto_server <= 0 || inputnet_auto_server > 65535))
            inputnet_auto_server = (int)inputnet_default_port;

        memset(inputnet_keydown,0,sizeof(inputnet_keydown));
        inputnet_buttondown = 0;
        inputnet_autostart_done = false;

        /* Remote play: only this machine runs the game; joining players watch
         * its picture and send their input (see remoteplay.cpp). */
        inputnet_remote = std::string(section->Get_string("mode")) == "remote";
        if (inputnet_remote) {
            REMOTEPLAY_Configure(section);
            addinputnet = true;
            if (inputnet_auto_server != -1 && !REMOTEPLAY_StartServer((uint16_t)inputnet_auto_server) &&
                remoteplay_screen_hidden)
                GFX_RemotePlaySetScreenHidden(false); /* nobody can watch: show what happened */
            return;
        }

        if (!inputnet_tick_installed) {
            TIMER_AddTickHandler(&INPUTNET_TickHandler);
            inputnet_tick_installed = true;
        }

        addinputnet = true;

        LOG(LOG_MISC,LOG_DEBUG)("INPUTNET: shared input emulation available (default UDP port %u)",
            (unsigned)inputnet_default_port);
    }

    ~INPUTNETModule() {
        REMOTEPLAY_StopServer(NULL);
        if (inputnet_tick_installed) {
            TIMER_DelTickHandler(&INPUTNET_TickHandler);
            inputnet_tick_installed = false;
        }

        INPUTNET_CloseLink(NULL,true);

        if (inputnet_sendpkt != NULL) { SDLNet_FreePacket(inputnet_sendpkt); inputnet_sendpkt = NULL; }
        if (inputnet_recvpkt != NULL) { SDLNet_FreePacket(inputnet_recvpkt); inputnet_recvpkt = NULL; }

        if (addinputnet) {
            VFILE_Remove("INPUTNET.COM","SYSTEM");
            addinputnet = false;
        }
    }
};

static INPUTNETModule *inputnet_module = NULL;

void INPUTNET_ShutDown(Section *) {
    if (inputnet_module != NULL) {
        delete inputnet_module;
        inputnet_module = NULL;
    }
}

static void INPUTNET_OnReset(Section *) {
    /* A reset of the emulated machine invalidates any synchronization. */
    if (inputnet_link_up) INPUTNET_CloseLink("the emulated machine was reset",true);
}

void INPUTNET_Setup(Section *) {
    if (inputnet_module == NULL) {
        LOG(LOG_MISC,LOG_DEBUG)("Allocating shared input emulation");
        inputnet_module = new INPUTNETModule(control->GetSection("inputnet"));
    }
}

void INPUTNET_Init(void) {
    LOG(LOG_MISC,LOG_DEBUG)("Initializing shared input emulation");

    AddExitFunction(AddExitFunctionFuncPair(INPUTNET_ShutDown),true);
    AddVMEventFunction(VM_EVENT_RESET,AddVMEventFunctionFuncPair(INPUTNET_OnReset));
    REMOTEPLAY_Init();
}

/* The server's control channel ("rbseat <copy> <player>", see remoteplay.cpp): that player plays that copy (0 based)
 * from the next frame start where the machines can be handed over. Whoever played it gets a free copy, or the one the
 * player leaves. "@host" names the server's own player. False when it cannot be done. */
bool INPUTNET_RollbackSeat(const std::string &name, unsigned copy) {
    if (!inputnet_session || !inputnet_rollback || !inputnet_is_server || rbc_copies < 2 || copy >= rbc_copies) return false;
    int pid = -1;
    for (unsigned i = 0; i < INPUTNET_MAX_PEERS && pid < 0; i++) {
        const InputNetPeer &p = inputnet_peers[i];
        if (!p.active) continue;
        if (p.local ? name == "@host" : (p.ready && strcasecmp(p.name.c_str(), name.c_str()) == 0)) pid = (int)i;
    }
    if (pid < 0) { LOG_MSG("INPUTNET: seat: nobody called \"%s\" plays", name.c_str()); return false; }
    uint8_t next[INPUTNET_MAX_PEERS];
    memcpy(next, rbs_seat_change ? rbs_seat_next : rb_seat, sizeof(next));
    const int from = RB_CopyOf(next, (unsigned)pid);
    if (from == (int)copy) return true;
    const uint8_t other = next[copy];
    next[copy] = (uint8_t)pid;
    if (from >= 0) next[from] = 0xFF;
    if (other < INPUTNET_MAX_PEERS && other != (uint8_t)pid && inputnet_peers[other].active) {
        int to = -1;
        for (unsigned c = 0; c < rbc_copies && to < 0; c++) if (c != copy && RB_CopyFree(next, c)) to = (int)c;
        if (to >= 0) {
            next[to] = other;
            LOG_MSG("INPUTNET: seat: \"%s\" goes to copy %u", inputnet_peers[other].local ? "the host" : inputnet_peers[other].name.c_str(), (unsigned)to + 1u);
        }
    }
    memcpy(rbs_seat_next, next, sizeof(next));
    rbs_seat_change = true;
    rb_resync_wanted = true;
    LOG_MSG("INPUTNET: seat: \"%s\" plays copy %u from the next frame the machines can be handed over at", name.c_str(), copy + 1u);
    return true;
}

#else /* !C_INPUTNET */

#include <string>
bool INPUTNET_RollbackSeat(const std::string &, unsigned) { return false; }

/* Networking support was not compiled in: the hooks stay inert. */

bool INPUTNET_InterceptKey(KBD_KEYS,bool) { return false; }
bool INPUTNET_InterceptMouseMotion(float,float,float,float,bool) { return false; }
bool INPUTNET_InterceptMouseButton(uint8_t,bool) { return false; }
bool INPUTNET_InterceptMouseWheel(int32_t) { return false; }

bool INPUTNET_IsServer(void) { return false; }
bool INPUTNET_IsConnected(void) { return false; }
void INPUTNET_RollbackTick(void) { }

void INPUTNET_ShutDown(Section *) { }
void INPUTNET_Setup(Section *) { }
void INPUTNET_Init(void) { }

void INPUTNET_ProgramStart(Program * *) { }

#endif /* C_INPUTNET */
