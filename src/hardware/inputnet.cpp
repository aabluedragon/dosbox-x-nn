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

/* Fast path flag. Defined unconditionally so that the keyboard/mouse code does
 * not need to know whether networking support was compiled in at all. */
bool inputnet_capture_active = false;

#if C_INPUTNET

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <algorithm>
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
#include "pic.h"
#include "regs.h"
#include "programs.h"
#include "setup.h"
#include "support.h"
#include "timer.h"
#include "video.h"

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

/* ------------------------------------------------------------------------- */
/* Tunables                                                                   */
/* ------------------------------------------------------------------------- */

#define INPUTNET_DEFAULT_PORT   2130u   /* not an assigned port, but adjacent to IPX's 213 */
#define INPUTNET_MAX_PEERS      8u
#define INPUTNET_RING           256u    /* input frames kept per peer, power of two */
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
    INPKT_CHECK     = 11    /* any: machine checksum for a frame, to detect a desync */
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
static uint32_t INPUTNET_ScheduleFrame(void) {
    uint32_t f = inputnet_frame + inputnet_delay;
    if (f <= inputnet_local_confirmed) f = inputnet_local_confirmed + 1u;
    return f;
}

static void INPUTNET_QueueLocal(const InputNetEvent &ev) {
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
        case INEV_KEY:
            if (ev.key < (uint16_t)KBD_LAST) {
                inputnet_keydown[ev.key] = (ev.pressed != 0);
                KEYBOARD_AddKeyRaw((KBD_KEYS)ev.key,ev.pressed != 0);
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
            Mouse_CursorMovedRaw(ev.xrel,ev.yrel,ev.x,ev.y,ev.emulate != 0);
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
    }
    ~InputNetSaveStateOverride() {
        use_save_file = old_use;
        savefilename = old_name;
        noremark_save_state = old_noremark;
        force_load_state = old_force;
    }
private:
    bool old_use, old_noremark, old_force;
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

    remove(path.c_str());
    return ok;
}

/* Server side: push the snapshot to one joining client. Emulation is frozen for
 * the duration - the state must not move on before the client has it. */
static bool INPUTNET_SendStateTo(InputNetPeer &p,const std::vector<uint8_t> &state) {
    const uint32_t total = (uint32_t)state.size();
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
            INPUTNET_BeginPacket(w,INPKT_STATE);
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
static bool INPUTNET_ReceiveState(std::vector<uint8_t> &state,uint32_t total) {
    if (total == 0) return true;

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
            const uint8_t type = r.u8();
            r.u8(); /* peer id */
            r.u8(); /* reserved */
            if (r.bad() || type != INPKT_STATE) continue;

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
            INPUTNET_SendRaw(inputnet_server_addr,buf,w.size());
            lastack = now;
        }

        if (have >= total) break;

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

    uint8_t welcome[128];
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

        while (!p->ready) {
            INPUTNET_PollSocket();
            if (p->ready) break;

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
            INPUTNET_ServerAcceptClient(from,name);
            break;
        }

        case INPKT_STATEACK: {
            if (!inputnet_is_server) return;
            InputNetPeer *p = INPUTNET_PeerByAddr(from);
            if (p == NULL) return;
            const uint32_t ack = r.u32();
            if (r.bad()) return;
            if (!p->ready && ack > p->confirmed) p->confirmed = ack;
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

    LOG_MSG("INPUTNET: waiting for participants on UDP port %u",(unsigned)port);
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

    /* Say hello until the server answers. */
    uint8_t buf[64];
    InputNetWriter w(buf,sizeof(buf));
    INPUTNET_BeginPacket(w,INPKT_HELLO);
    const char *me = "client";
    w.u8((uint8_t)strlen(me));
    w.bytes(me,strlen(me));

    uint32_t total = 0;
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
                        if (!r.bad() && myid < INPUTNET_MAX_PEERS) {
                            /* Clamp: these drive our ring buffer and packet sizes. */
                            inputnet_selfid = myid;
                            inputnet_frame_ms = (fms >= 1 && fms <= 100) ? fms : 10u;
                            inputnet_delay = (dly >= 1 && dly <= 64) ? dly : 3u;
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

    if (total != 0) {
        LOG_MSG("INPUTNET: receiving %u bytes of emulator state from the server",(unsigned)total);
        std::vector<uint8_t> state;
        if (!INPUTNET_ReceiveState(state,total)) {
            errmsg = "the emulator state transfer failed";
            INPUTNET_CloseLink(NULL,true);
            return false;
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
/* Public status                                                              */
/* ------------------------------------------------------------------------- */

bool INPUTNET_IsServer(void) { return inputnet_is_server && inputnet_link_up; }
bool INPUTNET_IsConnected(void) { return inputnet_link_up; }

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

        v = section->Get_int("delay");
        inputnet_delay = (unsigned int)((v >= 1 && v <= 64) ? v : 3);

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

        if (!inputnet_tick_installed) {
            TIMER_AddTickHandler(&INPUTNET_TickHandler);
            inputnet_tick_installed = true;
        }

        addinputnet = true;

        LOG(LOG_MISC,LOG_DEBUG)("INPUTNET: shared input emulation available (default UDP port %u)",
            (unsigned)inputnet_default_port);
    }

    ~INPUTNETModule() {
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
}

#else /* !C_INPUTNET */

/* Networking support was not compiled in: the hooks stay inert. */

bool INPUTNET_InterceptKey(KBD_KEYS,bool) { return false; }
bool INPUTNET_InterceptMouseMotion(float,float,float,float,bool) { return false; }
bool INPUTNET_InterceptMouseButton(uint8_t,bool) { return false; }
bool INPUTNET_InterceptMouseWheel(int32_t) { return false; }

bool INPUTNET_IsServer(void) { return false; }
bool INPUTNET_IsConnected(void) { return false; }

void INPUTNET_ShutDown(Section *) { }
void INPUTNET_Setup(Section *) { }
void INPUTNET_Init(void) { }

void INPUTNET_ProgramStart(Program * *) { }

#endif /* C_INPUTNET */
