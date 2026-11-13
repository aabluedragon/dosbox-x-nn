/*
 *  Inputnet remote play.
 *
 *  Only the host runs the emulated machine. It sends every rendered frame
 *  (compressed against the previous one) and its sound to the viewers, and
 *  applies the keyboard and mouse input the viewers send back as if it was
 *  typed on the host. A viewer is DOSBox-X started with -inputnet-connect while
 *  "mode = remote": it does not boot a machine, mount drives or need the game,
 *  so its configuration (sound card, game version, ...) cannot make the two
 *  sides differ.
 *
 *  All traffic is UDP. Packets start with "NNRP", a version byte and a type.
 *  Video frames are split into fragments; a viewer that misses one asks for a
 *  key frame. Input events are numbered and repeated until the host
 *  acknowledges them, so a lost datagram never leaves a key stuck.
 *
 *  Viewers that announce RP_FEAT_REFS in their HELLO report the newest frame
 *  they decoded and how many video fragments arrived (RP_FRAMEACK). The host
 *  then encodes each frame against a frame every viewer has (not simply the
 *  previous one), so a lost frame costs only itself, and it paces video to
 *  the rate the link takes (less on loss, more while there is none), skipping
 *  frames instead of queueing them. SVGA modes (800x600 and up) send several
 *  hundred KB per frame and depend on this over the internet. Older viewers
 *  and hosts keep the previous behaviour.
 */

#include "dosbox.h"
#include "remoteplay.h"
#include "menu.h"

bool remoteplay_streaming = false;
bool remoteplay_screen_hidden = false;

#if C_INPUTNET

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include <zlib.h>

#include "SDL.h"
#include "bios.h"
#include "control.h"
#include "cpu.h"
#include "dos_inc.h"
#include "keyboard.h"
#include "mem.h"
#include "logging.h"
#include "mouse.h"
#include "programs.h"
#include "render.h"   /* defines USE_TTF */
#include "setup.h"

#if defined(C_SDL2_NET) && C_SDL2_NET
# include <SDL2/SDL_net.h>
#else
# include "SDL_net.h"
#endif

#ifndef SDLNet_GetError
# define SDLNet_GetError SDL_GetError
#endif

extern bool SDLNetInited;
#if C_MODEM
void NetWrapper_WidenUdpBuffers(UDPsocket sock, int bytes);    /* src/hardware/serialport/misc_util.cpp */
#endif
extern bool dos_kernel_disabled;
extern unsigned int mouse_notify_mode;                      /* src/ints/bios.cpp: not 0 when the guest's pointer driver asks where the pointer is */
extern bool nn_remote_cursor, nn_remote_cursor_locked;      /* src/ints/bios.cpp: a viewer's pointer for such a guest */
extern uint16_t nn_remote_cursor_x, nn_remote_cursor_y;
extern uint16_t shell_psp;
bool PROGRAMS_IsRunningInternalPSP(uint16_t psp);
bool TTF_using(void);
#if defined(USE_TTF)
void ttf_switch_off(bool ss);
#endif

namespace {

/* ------------------------------------------------------------------------- */
/* Protocol                                                                   */
/* ------------------------------------------------------------------------- */

const uint8_t RP_VERSION = 1;

enum {
    RP_HELLO    = 1,    /* viewer -> host: join request and keepalive (name, session id) */
    RP_WELCOME  = 2,    /* host -> viewer: accepted */
    RP_REFUSE   = 3,    /* host -> viewer: not accepted (reason) */
    RP_VIDEO    = 4,    /* host -> viewer: frame fragment */
    RP_AUDIO    = 5,    /* host -> viewer: sound */
    RP_INPUT    = 6,    /* viewer -> host: session id, numbered input events */
    RP_INPUTACK = 7,    /* host -> viewer: highest input event applied */
    RP_KEYREQ   = 8,    /* viewer -> host: please send a key frame */
    RP_BYE      = 9,    /* either way: leaving (reason) */
    RP_FRAMEACK = 10,   /* viewer -> host: session, u8 flags (1: has a frame), newest decoded frame,
                           video fragments expected and received (cumulative) */
    RP_FRAMEINFO = 11   /* host -> viewer (RP_FEAT_TIMING): frame id, newest input event applied before the frame was
                           taken, microseconds from applying it to taking the frame, microseconds encoding the frame */
};

/* Viewer features (u32 after the session id in RP_HELLO; older viewers send none) */
const uint32_t RP_FEAT_REFS = 1;        /* decodes against the frame a frame names, sends RP_FRAMEACK */
const uint32_t RP_FEAT_TIMING = 2;      /* reads RP_FRAMEINFO to measure how long input takes to reach its screen */
const uint32_t RP_FEAT_PIECES = 4;      /* reads the piece table of a frame compressed in pieces and inflates them at once */
const uint32_t RP_FEAT_SHIFT = 8;       /* reads the shift table of a frame predicted from a moved base (scrolling), see RP_ShiftXor */

enum {
    RPEV_KEY     = 1,   /* u16 KBD_KEYS, u8 pressed */
    RPEV_MBUTTON = 2,   /* u8 button, u8 pressed */
    RPEV_MMOVE   = 3,   /* f32 xrel, f32 yrel, f32 x, f32 y, u8 emulate */
    RPEV_WHEEL   = 4    /* s16 */
};

const size_t RP_PACKET_MAX = 1400;      /* receive/send buffer size */
const size_t RP_FRAG = 1180;            /* video payload per datagram */
const unsigned RP_MAX_VIEWERS = 7;
const unsigned RP_AUDIO_FRAMES = 256;   /* stereo samples per sound packet */
const size_t RP_FRAME_HDR = 12;         /* u16 w, u16 h, u8 fmt, u8 flags, u16 back, u32 rawsize */
                                        /* flags: 1 key frame; 2 (only when every viewer has RP_FEAT_PIECES): a piece table
                                           comes before the zlib stream: u8 n, then n times u32, each piece's compressed size;
                                           4: the guest takes the pointer's place on the screen (a Windows guest with a
                                           pointer integration driver): the viewer sends where its pointer is in the
                                           picture and does not capture its mouse (older viewers do not look at this);
                                           8 (only when every viewer has RP_FEAT_SHIFT, never on a key frame): a shift table
                                           comes right after the header, before the piece table: u8 k (1...3 distances),
                                           k times s8 dx, s8 dy, then u8 log2 of the block size, u16 n and n bytes with
                                           two bits per block: 0 the base as it is, 1...k that distance (see RP_ShiftXor) */
const unsigned RP_SHIFT_BLOCK = 3;      /* scroll prediction works on blocks of 8x8 pixels... */
const int RP_SHIFT_RANGE = 24;          /* ...looked for up to this many pixels each way from one frame to the next... */
const int RP_SHIFT_LIMIT = 127;         /* ...and named up to this far when the base is several frames old (an s8)... */
const unsigned RP_SHIFT_MAX = 3;        /* ...by one of up to three distances a frame (layers that scroll at different speeds) */
struct ShiftVec { int dx, dy; };        /* pixel (x,y) of the frame comes from the base's pixel (x-dx,y-dy) */
const unsigned RP_PIECES_MAX = 8;
                                        /* back: frames between this one and its base (0: the previous frame) */
const unsigned RP_HISTORY = 16;         /* decoded frames both sides keep as delta bases */
const size_t RP_HISTORY_BYTES = (size_t)48 << 20;
const double RP_RATE_MAX = 28.0e6;      /* video bytes/s: the fixed rate for older viewers */
const double RP_RATE_START = 1.5e6;     /* pacing, once loss turns it on, starts at least here... */
const double RP_RATE_MIN = 96.0e3;      /* ...and never goes below this */
const unsigned RP_MAX_EVENTS = 48;      /* input events per datagram */

struct Writer {
    uint8_t buf[RP_PACKET_MAX];
    size_t len = 0;
    bool ok = true;

    explicit Writer(uint8_t type) {
        bytes("NNRP",4); u8(RP_VERSION); u8(type);
    }
    void bytes(const void *p,size_t n) {
        if (!ok || len + n > sizeof(buf)) { ok = false; return; }
        memcpy(buf + len,p,n); len += n;
    }
    void u8(uint8_t v) { bytes(&v,1); }
    void u16(uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; bytes(b,2); }
    void u32(uint32_t v) {
        uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
        bytes(b,4);
    }
    void f32(float f) { uint32_t u; memcpy(&u,&f,4); u32(u); }
    void str(const std::string &s) {
        size_t n = std::min(s.size(),(size_t)200);
        u8((uint8_t)n); bytes(s.data(),n);
    }
};

struct Reader {
    const uint8_t *p;
    size_t len, pos = 0;
    bool ok = true;

    Reader(const uint8_t *data,size_t n) : p(data), len(n) {}
    bool need(size_t n) { if (!ok || pos + n > len) { ok = false; return false; } return true; }
    uint8_t u8() { if (!need(1)) return 0; return p[pos++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint16_t)(p[pos] | (p[pos+1] << 8)); pos += 2; return v; }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t)p[pos] | ((uint32_t)p[pos+1] << 8) | ((uint32_t)p[pos+2] << 16) | ((uint32_t)p[pos+3] << 24);
        pos += 4; return v;
    }
    float f32() { uint32_t u = u32(); float f; memcpy(&f,&u,4); return f; }
    std::string str() {
        size_t n = u8();
        if (!need(n)) return std::string();
        std::string s((const char*)p + pos,n); pos += n; return s;
    }
    const uint8_t *rest(size_t &n) { n = ok ? len - pos : 0; return p + pos; }
};

/* Returns the packet type, or 0 if this is not one of ours. */
uint8_t RP_Header(Reader &r) {
    if (r.len < 6 || memcmp(r.p,"NNRP",4) != 0 || r.p[4] != RP_VERSION) return 0;
    r.pos = 6;
    return r.p[5];
}

bool RP_SameAddr(const IPaddress &a,const IPaddress &b) {
    return a.host == b.host && a.port == b.port;
}

std::string RP_AddrString(const IPaddress &a) {
    const uint8_t *h = (const uint8_t*)&a.host;
    char tmp[32];
    snprintf(tmp,sizeof(tmp),"%u.%u.%u.%u",h[0],h[1],h[2],h[3]);
    return tmp;
}

unsigned RP_BytesPerPixel(unsigned fmt) {
    switch (fmt) {
        case 8: return 1;
        case 15: case 16: return 2;
        case 32: return 4;
        default: return 0;
    }
}

/* Microseconds on a clock that only goes forward (SDL 1.2 has no high resolution counter). */
uint64_t RP_Micros(void) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

unsigned RP_CpuCount(void) {
#if SDL_VERSION_ATLEAST(2,0,0)
    const int n = SDL_GetCPUCount();
#else
    const char *e = getenv("NUMBER_OF_PROCESSORS");     /* Windows sets it; elsewhere SDL 1.2 hosts encode on one thread */
    const int n = e ? atoi(e) : 1;
#endif
    return n > 0 ? (unsigned)n : 1u;
}

uint32_t RP_ClampU32(uint64_t v) { return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v; }

/* Room for a whole burst of video (a big frame, or several) while the other end is busy. */
void RP_WidenSocket(UDPsocket sock) {
#if C_MODEM
    NetWrapper_WidenUdpBuffers(sock,4 << 20);
#else
    (void)sock;
#endif
}

bool RP_OpenNet(void) {
    if (!SDLNetInited) {
        if (SDLNet_Init() == -1) {
            LOG_MSG("INPUTNET: SDLNet_Init failed: %s",SDLNet_GetError());
            return false;
        }
        SDLNetInited = true;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Host                                                                       */
/* ------------------------------------------------------------------------- */

struct Viewer {
    bool active = false;
    IPaddress addr = { 0, 0 };
    std::string name;
    uint32_t session = 0;               /* random per viewer start: a restart from the same address is a new player */
    uint32_t lastheard = 0;
    uint32_t applied = 0;               /* highest input event applied */
    uint64_t applied_us = 0;            /* when it was applied (RP_Micros) */
    bool keydown[KBD_LAST];
    uint32_t buttons = 0;
    uint32_t features = 0;              /* RP_FEAT_* from its HELLO */
    bool hasack = false;
    uint32_t acked = 0;                 /* newest frame it decoded */
    uint32_t loss_exp = 0, loss_got = 0, loss_ms = 0;  /* fragment counters at the last rate decision */
    /* How long a frame takes from leaving here to this viewer saying it has decoded it (see Host_DelayFeedback) */
    double delay_now = 0;               /* lately, ms (smoothed) */
    uint32_t delay_base = 0, delay_base_prev = 0, delay_bucket_ms = 0;  /* the shortest of this and the previous 10 seconds */
    bool delay_late = false, delay_raised = false;      /* well above the shortest; somewhat above it */
};

UDPsocket host_sock = NULL;
UDPpacket *host_out = NULL, *host_in = NULL;
bool host_running = false;

/* Keepalive while the emulator stands still. A menu (Windows' own menu bar, or the SDL drawn one), dragging or
 * resizing the window on Windows, and message boxes stop the main loop, so REMOTEPLAY_Poll stops: the viewers heard
 * nothing and gave up after the timeout ("The connection to the host was lost"), and hiding the screen from the Video
 * menu broke the stream. While the main loop has not polled for a while, this thread answers the viewers that are
 * already in, so they stay connected (the picture waits); their input is left for the main loop (viewers send it
 * again until it is acknowledged). host_lock guards the socket's packets and the viewer list, and what the video
 * encoder thread shares with the main loop (see Capture below). */
SDL_mutex *host_lock = NULL;
inline void Host_Lock(void) { if (host_lock) SDL_LockMutex(host_lock); }
inline void Host_Unlock(void) { if (host_lock) SDL_UnlockMutex(host_lock); }
SDL_mutex *send_lock = NULL;        /* one thread at a time sends queued video, so it leaves in the order queued */
SDL_Thread *host_keepalive = NULL;
volatile int host_keepalive_stop = 0;
volatile uint32_t host_lastpoll = 0;
uint16_t host_port = 0;
Viewer viewers[RP_MAX_VIEWERS];
unsigned cfg_timeout = 10000;
unsigned cfg_maxfps = 0;
int cfg_present = 2;            /* [inputnet] present: 0 scanout, 1 retrace, 2 flip (vga_draw.cpp, NN_EarlyFrameOK) */

/* Video encoder state */
struct EncFrame {
    uint32_t id = 0;
    std::vector<uint8_t> raw;
    ShiftVec step[RP_SHIFT_MAX];    /* how far its parts had moved since the frame encoded before it (scroll prediction) */
    unsigned nstep = 0;
};
std::vector<uint8_t> enc_raw, enc_work, enc_comp, enc_alt;
std::vector<uint8_t> enc_shiftbits, enc_shiftcheck;     /* scroll prediction: one bit per block; the check under NN_RP_SHIFTCHECK */
ShiftVec enc_shift_vec[RP_SHIFT_MAX];                   /* the distances this frame's parts have moved since its base */
unsigned enc_shift_n = 0;
ShiftVec enc_step_vec[RP_SHIFT_MAX];                    /* ...and since the frame encoded before it */
unsigned enc_step_n = 0;
std::vector<uint32_t> enc_pieces, enc_altpieces;
unsigned enc_plain_streak = 0;                  /* frames in a row that were smaller compressed alone than as a difference */
std::deque<EncFrame> enc_hist;                  /* the newest frames sent, oldest first */
EncFrame enc_key;                               /* the last key frame (kept even when older than enc_hist) */
unsigned enc_w = 0, enc_h = 0, enc_fmt = 0;
bool enc_haveprev = false, enc_needkey = true, enc_haskey = false, enc_keyreq = false;
uint32_t enc_frameid = 0, enc_lastkey = 0, enc_lastsend = 0;
std::deque<std::vector<uint8_t> > send_queue;     /* paced video fragments */
size_t send_queued = 0;                         /* bytes in send_queue */
uint32_t send_lastms = 0, send_backlog_ms = 0, rate_cut_ms = 0, rate_log_ms = 0;
double send_budget = 0;                         /* bytes that may go out now */
double send_rate = RP_RATE_MAX;                 /* video bytes/s */
uint32_t frame_sent_id[64], frame_sent_ms[64];  /* when the last fragment of each of the newest frames left (SDL ticks) */
double rate_delivered = 0;                      /* the most video bytes/s a viewer has received lately */
bool rate_adaptive = false;
bool rate_limited = false;                      /* a viewer reported loss: video is paced from then on */

/* The video encoder thread. REMOTEPLAY_AddFrame runs on the emulation thread, in the middle of the game's own frame:
 * indexing, comparing and compressing the picture there took the game 3 to 6 ms a frame at 800x600, a tenth to a
 * quarter of its time. It now only copies the picture into a Capture and hands it over; the encoder thread encodes
 * and sends it. One capture can wait: a newer picture replaces one the encoder has not started on, so viewers get the
 * newest. Three buffers are enough (one being encoded, one waiting, one being filled); only the emulation thread
 * writes them and the encoder reads the one it took.
 * host_lock guards the capture slots, the requests (enc_needkey, enc_haveprev, enc_keyreq), enc_frameid and the times
 * beside it, the send queue and its rate, and the statistics. The frames kept as bases (enc_hist, enc_key) and the
 * work buffers belong to the encoder alone. With one processor there is no encoder thread and the emulation thread
 * encodes as before. */
struct Capture {
    std::vector<uint8_t> px;            /* the rows, without padding */
    uint8_t pal[1024];
    unsigned w = 0, h = 0, bpp = 0;
    uint64_t taken_us = 0;
    uint32_t session[RP_MAX_VIEWERS];   /* per viewer: who it was, and its newest input applied when this was taken */
    uint32_t applied[RP_MAX_VIEWERS];
    uint64_t applied_us[RP_MAX_VIEWERS];
};
Capture caps[3];
int cap_last = -1;                              /* the newest picture handed over (the next one is compared with it) */
int cap_waiting = -1, cap_encoding = -1;
SDL_Thread *enc_thread = NULL;
SDL_sem *enc_go = NULL;
volatile int enc_stop = 0;
void Enc_Start(void);
void Enc_Stop(void);

/* 32-bit frames of 256-colour modes: DOSBox-X draws SVGA 8-bit modes (Shadow Warrior's 800x600, for one) as 32-bit
 * pixels, a quarter of which would do. Such frames go out as palette + indices; a colour keeps its index from frame
 * to frame (deltas stay small) until more than 256 have been seen, and then the palette starts over. */
struct PaletteMap {
    uint32_t colors[256];
    unsigned count = 0;
    uint32_t keys[1024];
    uint16_t vals[1024];                /* index + 1; 0: free */
    void clear(void) { count = 0; memset(vals,0,sizeof(vals)); }
    int find(uint32_t c) const {        /* a known colour's index, or -1: changes nothing, so threads can share it */
        unsigned h = (unsigned)((c * 2654435761u) >> 22);
        while (vals[h] != 0) {
            if (keys[h] == c) return vals[h] - 1;
            h = (h + 1) & 1023u;
        }
        return -1;
    }
    int lookup(uint32_t c) {
        unsigned h = (unsigned)((c * 2654435761u) >> 22);
        while (vals[h] != 0) {
            if (keys[h] == c) return vals[h] - 1;
            h = (h + 1) & 1023u;
        }
        if (count >= 256) return -1;
        keys[h] = c;
        vals[h] = (uint16_t)(count + 1);
        colors[count] = c;
        return (int)count++;
    }
};
PaletteMap enc_palmap;
bool enc_palmap_init = false;

/* ------------------------------------------------------------------------- */
/* Scrolling                                                                  */
/* ------------------------------------------------------------------------- */

/* A scrolling picture differs from the one before in nearly every pixel, so its difference compresses no better than
 * the frame alone (a 640x480 game sent about 130 KB a frame while it scrolled). When parts of the picture have moved
 * as a whole (the playfield of a 2D game, often in layers at different speeds; its status bar has not), the frame
 * goes out as the difference against a prediction: for every block of 8x8 pixels two bits say whether the block is
 * predicted from the base as it is (0) or from the base moved by one of up to three distances (1...3), that is pixel
 * (x,y) from the base's pixel (x-dx,y-dy). Pixels whose moved source would lie outside the picture are predicted from
 * the base as it is. Nothing is lost: the viewer makes the same prediction from the same base. The palette in front
 * of an 8-bit frame is never moved. */
struct ShiftGeom {
    unsigned w, h, bpp;     /* pixels, bytes per pixel */
    size_t pal;             /* bytes in front of the pixels */
};

inline unsigned RP_ShiftBitmapBytes(const ShiftGeom &g) {
    const unsigned bs = 1u << RP_SHIFT_BLOCK;
    return (((g.w + bs - 1) >> RP_SHIFT_BLOCK) * ((g.h + bs - 1) >> RP_SHIFT_BLOCK) + 3u) / 4u;
}

inline unsigned RP_ShiftChoice(const uint8_t *bits,unsigned idx) {
    return (bits[idx >> 2] >> ((idx & 3u) * 2u)) & 3u;
}

/* The pixels x0...x1 of row y of a moved block that have a source in the base: a...b (empty when a >= b). */
inline void RP_ShiftSpan(const ShiftGeom &g,unsigned y,unsigned x0,unsigned x1,int dx,int dy,long &a,long &b) {
    const long sy = (long)y - dy;
    if (sy < 0 || sy >= (long)g.h) { a = b = (long)x0; return; }
    a = std::max<long>((long)x0,(long)dx);
    b = std::min<long>((long)x1,(long)g.w + dx);
    if (a >= b) a = b = (long)x0;
}

/* out = in ^ prediction, for the whole frame (frame ^ prediction is what is sent; that ^ prediction is the frame).
 * A choice above `nvec` counts as 0. */
void RP_ShiftXor(uint8_t *out,const uint8_t *in,const uint8_t *base,const ShiftGeom &g,const ShiftVec *vec,unsigned nvec,const uint8_t *bits) {
    const unsigned bs = 1u << RP_SHIFT_BLOCK;
    const unsigned bw = (g.w + bs - 1) >> RP_SHIFT_BLOCK;
    const size_t row = (size_t)g.w * g.bpp;
    for (size_t i=0;i < g.pal;i++) out[i] = in[i] ^ base[i];
    for (unsigned y=0;y < g.h;y++) {
        const uint8_t *ip = in + g.pal + (size_t)y * row, *bp = base + g.pal + (size_t)y * row;
        uint8_t *op = out + g.pal + (size_t)y * row;
        const unsigned by = y >> RP_SHIFT_BLOCK;
        for (unsigned bx=0;bx < bw;bx++) {
            const unsigned x0 = bx << RP_SHIFT_BLOCK, x1 = std::min(g.w,x0 + bs);
            const unsigned c = RP_ShiftChoice(bits,by * bw + bx);
            long a = x0, b = x0;
            const uint8_t *mp = bp;
            if (c != 0 && c <= nvec) {
                const ShiftVec &v = vec[c - 1];
                RP_ShiftSpan(g,y,x0,x1,v.dx,v.dy,a,b);
                if (a < b) mp = base + g.pal + (size_t)((long)y - v.dy) * row - (ptrdiff_t)v.dx * (ptrdiff_t)g.bpp;  /* only read inside a...b */
            }
            size_t i = (size_t)x0 * g.bpp;
            const size_t ia = (size_t)a * g.bpp, ib = (size_t)b * g.bpp, ie = (size_t)x1 * g.bpp;
            for (;i < ia;i++) op[i] = ip[i] ^ bp[i];
            for (;i < ib;i++) op[i] = ip[i] ^ mp[i];
            for (;i < ie;i++) op[i] = ip[i] ^ bp[i];
        }
    }
}

/* Whether the pixels x0...x1 of row y of the frame are what the prediction says (`v` NULL: the base as it is). */
inline bool RP_ShiftRowSame(const uint8_t *cur,const uint8_t *base,const ShiftGeom &g,unsigned y,unsigned x0,unsigned x1,const ShiftVec *v) {
    const size_t row = (size_t)g.w * g.bpp;
    const uint8_t *cp = cur + g.pal + (size_t)y * row, *bp = base + g.pal + (size_t)y * row;
    long a = x0, b = x0;
    if (v != NULL) RP_ShiftSpan(g,y,x0,x1,v->dx,v->dy,a,b);
    if (a < b) {
        const uint8_t *mp = base + g.pal + (size_t)((long)y - v->dy) * row - (ptrdiff_t)v->dx * (ptrdiff_t)g.bpp;
        if (memcmp(cp + (size_t)a * g.bpp,mp + (size_t)a * g.bpp,(size_t)(b - a) * g.bpp) != 0) return false;
        if ((long)x0 < a && memcmp(cp + (size_t)x0 * g.bpp,bp + (size_t)x0 * g.bpp,(size_t)(a - (long)x0) * g.bpp) != 0) return false;
        if (b < (long)x1 && memcmp(cp + (size_t)b * g.bpp,bp + (size_t)b * g.bpp,(size_t)((long)x1 - b) * g.bpp) != 0) return false;
        return true;
    }
    return memcmp(cp + (size_t)x0 * g.bpp,bp + (size_t)x0 * g.bpp,(size_t)(x1 - x0) * g.bpp) == 0;
}

/* Blocks of 16x16 pixels spread over the frame, to be looked for in the base. Blocks that have not changed since the
 * base and plain ones say nothing about movement and are left out. */
struct ShiftProbes {
    enum { P = 16, NX = 10, NY = 8 };
    const uint8_t *cp, *bp;
    ShiftGeom g;
    size_t row, pb;
    unsigned n;
    struct { unsigned x, y; } p[NX * NY];

    ShiftProbes(const uint8_t *cur,const uint8_t *base,const ShiftGeom &geom) : cp(cur + geom.pal), bp(base + geom.pal), g(geom),
        row((size_t)geom.w * geom.bpp), pb((size_t)P * geom.bpp), n(0) {
        if (g.w < 4u * P + 2u * (unsigned)RP_SHIFT_RANGE || g.h < 4u * P + 2u * (unsigned)RP_SHIFT_RANGE) return;
        for (unsigned j=0;j < NY;j++) {
            for (unsigned i=0;i < NX;i++) {
                const unsigned x = (g.w - P) * (2 * i + 1) / (2 * NX), y = (g.h - P) * (2 * j + 1) / (2 * NY);
                const uint8_t *c = cp + (size_t)y * row + (size_t)x * g.bpp, *b = bp + (size_t)y * row + (size_t)x * g.bpp;
                bool same = true, plain = true;
                for (unsigned r=0;r < P;r++) {
                    if (same && memcmp(c + r * row,b + r * row,pb) != 0) same = false;
                    if (plain) {
                        for (size_t k=g.bpp;k < pb;k++) if (c[r * row + k] != c[k % g.bpp]) { plain = false; break; }
                    }
                }
                if (same || plain) continue;
                p[n].x = x; p[n].y = y; n++;
            }
        }
    }
    /* Whether block k of the frame is in the base, (vx,vy) back */
    bool found(unsigned k,int vx,int vy) const {
        const long sx = (long)p[k].x - vx, sy = (long)p[k].y - vy;
        if (sx < 0 || sy < 0 || sx + (long)P > (long)g.w || sy + (long)P > (long)g.h) return false;
        const uint8_t *c = cp + (size_t)p[k].y * row + (size_t)p[k].x * g.bpp, *b = bp + (size_t)sy * row + (size_t)sx * g.bpp;
        for (unsigned r=0;r < P;r++) if (memcmp(c + r * row,b + r * row,pb) != 0) return false;
        return true;
    }
    unsigned votes(int vx,int vy) const {
        unsigned v = 0;
        for (unsigned k=0;k < n;k++) if (found(k,vx,vy)) v++;
        return v;
    }
};

/* How far the parts of the picture have moved since the base, one frame earlier: each block votes for the distances
 * at which it is found (up to 24 pixels each way), and the up to three distances with the most votes win (at least
 * three blocks each). Blocks found in too many places say nothing. `vec` comes in with the distances of the frame
 * before (`nvec` of them), which are tried first: a block found at one of them is not looked for elsewhere. Returns
 * how many distances it found. */
unsigned RP_FindShift(const uint8_t *cur,const uint8_t *base,const ShiftGeom &g,ShiftVec *vec,unsigned nvec) {
    const int R = RP_SHIFT_RANGE;
    const ShiftProbes probes(cur,base,g);
    const unsigned n = probes.n;
    if (n < 3) return 0;
    const unsigned span = 2 * (unsigned)R + 1;
    static thread_local std::vector<uint16_t> votes;
    votes.assign((size_t)span * span,0);
    for (unsigned k=0;k < n;k++) {
        /* the distances of the frame before, while the picture keeps scrolling the same way */
        bool known = false;
        for (unsigned h=0;h < nvec;h++) {
            if (vec[h].dx < -R || vec[h].dx > R || vec[h].dy < -R || vec[h].dy > R || (vec[h].dx == 0 && vec[h].dy == 0)) continue;
            if (probes.found(k,vec[h].dx,vec[h].dy)) {
                votes[(size_t)(vec[h].dy + R) * span + (size_t)(vec[h].dx + R)]++;
                known = true;
            }
        }
        if (known) continue;
        int hits[8][2];
        unsigned nh = 0;
        bool many = false;
        for (int vy=-R;vy <= R && !many;vy++) {
            for (int vx=-R;vx <= R;vx++) {
                if (vx == 0 && vy == 0) continue;
                if (!probes.found(k,vx,vy)) continue;
                if (nh == 8) { many = true; break; }
                hits[nh][0] = vx; hits[nh][1] = vy; nh++;
            }
        }
        if (many) continue;
        for (unsigned h=0;h < nh;h++) votes[(size_t)(hits[h][1] + R) * span + (size_t)(hits[h][0] + R)]++;
    }
    unsigned out = 0;
    while (out < RP_SHIFT_MAX) {
        unsigned best = 0;
        size_t at = 0;
        for (size_t i=0;i < votes.size();i++) if (votes[i] > best) { best = votes[i]; at = i; }
        if (best < 3) break;
        votes[at] = 0;
        vec[out].dx = (int)(at % span) - R;
        vec[out].dy = (int)(at / span) - R;
        out++;
    }
    return out;
}

/* The same against a base several frames old (over a real link the newest frame a viewer is known to have is two to
 * four frames back): the picture has moved too far to look everywhere. `guess` holds where each part should be by
 * now (the distances it moved from frame to frame, added up); around each guess the place with the most blocks is
 * looked for, a few pixels each way. Returns how many distances stood (at least three blocks each), in `vec`. */
unsigned RP_FollowShift(const uint8_t *cur,const uint8_t *base,const ShiftGeom &g,const ShiftVec *guess,unsigned nguess,ShiftVec *vec) {
    const ShiftProbes probes(cur,base,g);
    if (probes.n < 3) return 0;
    unsigned out = 0;
    for (unsigned q=0;q < nguess && out < RP_SHIFT_MAX;q++) {
        unsigned best = 0;
        int bx = 0, by = 0;
        for (int oy=-3;oy <= 3;oy++) {
            for (int ox=-3;ox <= 3;ox++) {
                const int vx = guess[q].dx + ox, vy = guess[q].dy + oy;
                if (vx < -RP_SHIFT_LIMIT || vx > RP_SHIFT_LIMIT || vy < -RP_SHIFT_LIMIT || vy > RP_SHIFT_LIMIT || (vx == 0 && vy == 0)) continue;
                const unsigned v = probes.votes(vx,vy);
                if (v > best || (v == best && v != 0 && abs(ox) + abs(oy) < abs(bx - guess[q].dx) + abs(by - guess[q].dy))) { best = v; bx = vx; by = vy; }
            }
        }
        if (best < 3) continue;
        bool twice = false;
        for (unsigned k=0;k < out;k++) if (vec[k].dx == bx && vec[k].dy == by) twice = true;
        if (twice) continue;
        vec[out].dx = bx; vec[out].dy = by; out++;
    }
    return out;
}

/* For every block the prediction with the fewest rows that differ (the base as it is on a tie). False when no block
 * is better off moved. */
bool RP_ShiftBlocks(const uint8_t *cur,const uint8_t *base,const ShiftGeom &g,const ShiftVec *vec,unsigned nvec,std::vector<uint8_t> &bits) {
    const unsigned bs = 1u << RP_SHIFT_BLOCK;
    const unsigned bw = (g.w + bs - 1) >> RP_SHIFT_BLOCK, bh = (g.h + bs - 1) >> RP_SHIFT_BLOCK;
    bits.assign(RP_ShiftBitmapBytes(g),0);
    unsigned moved = 0;
    for (unsigned by=0;by < bh;by++) {
        const unsigned y0 = by << RP_SHIFT_BLOCK, y1 = std::min(g.h,y0 + bs);
        for (unsigned bx=0;bx < bw;bx++) {
            const unsigned x0 = bx << RP_SHIFT_BLOCK, x1 = std::min(g.w,x0 + bs);
            unsigned best = 0, choice = 0;
            for (unsigned y=y0;y < y1;y++) if (!RP_ShiftRowSame(cur,base,g,y,x0,x1,NULL)) best++;
            for (unsigned k=0;k < nvec && best != 0;k++) {
                unsigned differ = 0;
                for (unsigned y=y0;y < y1 && differ < best;y++) if (!RP_ShiftRowSame(cur,base,g,y,x0,x1,&vec[k])) differ++;
                if (differ < best) { best = differ; choice = k + 1; }
            }
            if (choice != 0) {
                const unsigned idx = by * bw + bx;
                bits[idx >> 2] |= (uint8_t)(choice << ((idx & 3u) * 2u));
                moved++;
            }
        }
    }
    return moved != 0;
}

bool RP_IndexParallel(const uint8_t *px,size_t pixels,uint8_t *idx);

/* Writes palette + indices for a 32-bit frame into out, or returns false when it has more than 256 colours. */
bool RP_IndexFrame(unsigned width,unsigned height,unsigned pitch,const uint8_t *data,std::vector<uint8_t> &out) {
    if (!enc_palmap_init) { enc_palmap.clear(); enc_palmap_init = true; }
    out.resize(768 + (size_t)width * height);
    /* Nearly every frame has only colours seen before: those are looked up on all the encoder's threads at once. */
    bool done = (pitch == width * 4) && RP_IndexParallel(data,(size_t)width * height,out.data() + 768);
    for (unsigned attempt=0;attempt < 2 && !done;attempt++) {
        uint8_t *o = out.data() + 768;
        bool ok = true;
        uint32_t lastc = 0xFFFFFFFFu;
        int lasti = 0;
        for (unsigned y=0;y < height && ok;y++) {
            const uint8_t *row = data + (size_t)y * pitch;
            for (unsigned x=0;x < width;x++) {
                uint32_t c;
                memcpy(&c,row + x * 4,4);
                c &= 0xFFFFFFu;
                if (c != lastc) {
                    lasti = enc_palmap.lookup(c);
                    if (lasti < 0) { ok = false; break; }
                    lastc = c;
                }
                *o++ = (uint8_t)lasti;
            }
        }
        if (ok) done = true;
        else enc_palmap.clear();    /* new colours (a fade, another screen): start the palette over */
    }
    if (!done) return false;
    uint8_t *pal = out.data();
    for (unsigned i=0;i < 256;i++) {
        const uint32_t c = i < enc_palmap.count ? enc_palmap.colors[i] : 0;
        pal[i*3+0] = (uint8_t)(c >> 16); pal[i*3+1] = (uint8_t)(c >> 8); pal[i*3+2] = (uint8_t)c;
    }
    return true;
}

/* Parallel zlib, as pigz does it: a big frame is cut into pieces deflated at the same time (worker threads take all but
 * the last, which this thread does). Each piece ends on a byte boundary (Z_SYNC_FLUSH) and the last one ends the stream,
 * so the pieces joined after a zlib header, with the combined Adler-32 after them, are one ordinary zlib stream: every
 * viewer, older ones too, inflates it as before. Encoding a frame took the emulation several milliseconds at 800x600,
 * which was latency for the players and frames the game did not draw. */
struct ZJob {
    int kind = 0;                       /* 0: deflate. 1: palette indices for len pixels at src into idx.
                                           2: inflate len bytes at src into want bytes at idx (a viewer's frame piece) */
    uint8_t *idx = NULL;
    size_t want = 0;
    bool iinit = false;
    z_stream zi;
    const uint8_t *src = NULL;
    size_t len = 0, outlen = 0;
    bool last = false, ok = false, zinit = false;
    uLong adler = 1;
    std::vector<uint8_t> out;
    z_stream zs;
};
const unsigned RP_ZWORKERS_MAX = 3;
const size_t RP_ZPARALLEL_MIN = (size_t)128 << 10;   /* smaller frames (320x200 ones) are not worth splitting */
ZJob zjobs[RP_ZWORKERS_MAX + 1];
SDL_Thread *zthreads[RP_ZWORKERS_MAX];
SDL_sem *zgo[RP_ZWORKERS_MAX], *zdone[RP_ZWORKERS_MAX];
unsigned zworkers = 0;
bool zstarted = false;
volatile int zstop = 0;

void RP_ZRun(ZJob &j) {
    j.ok = false;
    if (j.kind == 2) {
        if (!j.iinit) {
            memset(&j.zi,0,sizeof(j.zi));
            if (inflateInit2(&j.zi,-15) != Z_OK) return;
            j.iinit = true;
        }
        else if (inflateReset(&j.zi) != Z_OK) return;
        j.zi.next_in = (Bytef*)j.src;
        j.zi.avail_in = (uInt)j.len;
        j.zi.next_out = j.idx;
        j.zi.avail_out = (uInt)j.want;
        const int r = inflate(&j.zi,Z_SYNC_FLUSH);
        /* all but the last piece end at a flush point, not at the end of a stream */
        j.ok = j.zi.avail_in == 0 && j.zi.total_out == j.want && (j.last ? r == Z_STREAM_END : (r == Z_OK || r == Z_BUF_ERROR));
        if (j.ok) j.adler = adler32(adler32(0L,Z_NULL,0),j.idx,(uInt)j.want);
        return;
    }
    if (j.kind == 1) {
        uint32_t lastc = 0xFFFFFFFFu;
        int lasti = 0;
        for (size_t i=0;i < j.len;i++) {
            uint32_t c;
            memcpy(&c,j.src + i * 4,4);
            c &= 0xFFFFFFu;
            if (c != lastc) {
                lasti = enc_palmap.find(c);
                if (lasti < 0) return;      /* a new colour: the caller indexes the frame alone, adding colours */
                lastc = c;
            }
            j.idx[i] = (uint8_t)lasti;
        }
        j.ok = true;
        return;
    }
    if (!j.zinit) {
        memset(&j.zs,0,sizeof(j.zs));
        if (deflateInit2(&j.zs,1,Z_DEFLATED,-15,8,Z_DEFAULT_STRATEGY) != Z_OK) return;
        j.zinit = true;
    }
    else if (deflateReset(&j.zs) != Z_OK) return;
    const size_t bound = (size_t)deflateBound(&j.zs,(uLong)j.len) + 16;
    if (j.out.size() < bound) j.out.resize(bound);
    j.zs.next_in = (Bytef*)j.src;
    j.zs.avail_in = (uInt)j.len;
    j.zs.next_out = j.out.data();
    j.zs.avail_out = (uInt)j.out.size();
    const int r = deflate(&j.zs,j.last ? Z_FINISH : Z_SYNC_FLUSH);
    j.ok = j.zs.avail_in == 0 && (j.last ? r == Z_STREAM_END : r == Z_OK);
    j.outlen = j.out.size() - j.zs.avail_out;
    j.adler = adler32(adler32(0L,Z_NULL,0),j.src,(uInt)j.len);
}

int RP_ZThread(void *arg) {
    const unsigned i = (unsigned)(uintptr_t)arg;
    for (;;) {
        SDL_SemWait(zgo[i]);
        if (zstop) break;
        RP_ZRun(zjobs[i]);
        SDL_SemPost(zdone[i]);
    }
    return 0;
}

void RP_ZStart(void) {
    if (zstarted) return;
    zstarted = true;
    zstop = 0;
    /* The emulation keeps a processor to itself, and so does the encoder thread, which takes a piece too. */
    const unsigned cpus = RP_CpuCount(), busy = (enc_thread != NULL) ? 2u : 1u;
    const unsigned want = std::min(cpus > busy ? cpus - busy : 0u,RP_ZWORKERS_MAX);
    zworkers = 0;
    for (unsigned i=0;i < want;i++) {
        zgo[i] = SDL_CreateSemaphore(0);
        zdone[i] = SDL_CreateSemaphore(0);
        if (zgo[i] == NULL || zdone[i] == NULL) break;
#if SDL_VERSION_ATLEAST(2,0,0)
        zthreads[i] = SDL_CreateThread(RP_ZThread,"INPUTNET encoder",(void*)(uintptr_t)i);
#else
        zthreads[i] = SDL_CreateThread(RP_ZThread,(void*)(uintptr_t)i);
#endif
        if (zthreads[i] == NULL) break;
        zworkers++;
    }
}

void RP_ZStop(void) {
    if (!zstarted) return;
    zstop = 1;
    for (unsigned i=0;i < zworkers;i++) SDL_SemPost(zgo[i]);
    for (unsigned i=0;i < zworkers;i++) SDL_WaitThread(zthreads[i],NULL);
    for (unsigned i=0;i < zworkers;i++) { SDL_DestroySemaphore(zgo[i]); SDL_DestroySemaphore(zdone[i]); }
    for (unsigned i=0;i <= RP_ZWORKERS_MAX;i++) {
        if (zjobs[i].zinit) { deflateEnd(&zjobs[i].zs); zjobs[i].zinit = false; }
        if (zjobs[i].iinit) { inflateEnd(&zjobs[i].zi); zjobs[i].iinit = false; }
    }
    zworkers = 0;
    zstarted = false;
}

/* Compresses len bytes at src into out from offset at (out grows as needed); clen gets the compressed size, and pieces
 * each piece's (none when the frame went in one). */
bool RP_Compress(const uint8_t *src,size_t len,std::vector<uint8_t> &out,size_t at,size_t &clen,std::vector<uint32_t> &pieces) {
    RP_ZStart();
    pieces.clear();
    const unsigned parts = (len >= RP_ZPARALLEL_MIN) ? zworkers + 1 : 1;
    if (parts == 1) {
        uLongf n = compressBound((uLong)len);
        out.resize(at + n);
        if (compress2(out.data() + at,&n,src,(uLong)len,1) != Z_OK) return false;
        clen = n;
        return true;
    }
    const size_t piece = len / parts;
    for (unsigned k=0;k < parts;k++) {
        ZJob &j = zjobs[k];
        j.kind = 0;
        j.src = src + (size_t)k * piece;
        j.len = (k + 1 == parts) ? len - (size_t)k * piece : piece;
        j.last = (k + 1 == parts);
    }
    for (unsigned k=0;k + 1 < parts;k++) SDL_SemPost(zgo[k]);
    RP_ZRun(zjobs[parts - 1]);
    for (unsigned k=0;k + 1 < parts;k++) SDL_SemWait(zdone[k]);
    size_t total = 2 + 4;
    for (unsigned k=0;k < parts;k++) {
        if (!zjobs[k].ok) return false;
        total += zjobs[k].outlen;
    }
    out.resize(at + total);
    uint8_t *o = out.data() + at;
    *o++ = 0x78; *o++ = 0x01;   /* zlib header: deflate, 32K window, fastest */
    uLong adler = zjobs[0].adler;
    for (unsigned k=0;k < parts;k++) {
        memcpy(o,zjobs[k].out.data(),zjobs[k].outlen);
        o += zjobs[k].outlen;
        pieces.push_back((uint32_t)zjobs[k].outlen);
        if (k) adler = adler32_combine(adler,zjobs[k].adler,(z_off_t)zjobs[k].len);
    }
    *o++ = (uint8_t)(adler >> 24); *o++ = (uint8_t)(adler >> 16); *o++ = (uint8_t)(adler >> 8); *o++ = (uint8_t)adler;
    clen = total;
    return true;
}

bool RP_IndexParallel(const uint8_t *px,size_t pixels,uint8_t *idx) {
    RP_ZStart();
    const unsigned parts = zworkers + 1;
    if (parts == 1 || pixels * 4 < RP_ZPARALLEL_MIN || enc_palmap.count == 0) return false;
    const size_t piece = pixels / parts;
    for (unsigned k=0;k < parts;k++) {
        ZJob &j = zjobs[k];
        j.kind = 1;
        j.src = px + (size_t)k * piece * 4;
        j.idx = idx + (size_t)k * piece;
        j.len = (k + 1 == parts) ? pixels - (size_t)k * piece : piece;
    }
    for (unsigned k=0;k + 1 < parts;k++) SDL_SemPost(zgo[k]);
    RP_ZRun(zjobs[parts - 1]);
    for (unsigned k=0;k + 1 < parts;k++) SDL_SemWait(zdone[k]);
    bool ok = true;
    for (unsigned k=0;k < parts;k++) ok = ok && zjobs[k].ok;
    return ok;
}

/* A viewer's side of a frame compressed in pieces: stream is the whole zlib stream (2 byte header, the pieces, the
 * Adler-32), lens each piece's size in it. The pieces are inflated at the same time, straight into out. False when
 * anything is off; the caller then inflates the stream the ordinary way. */
bool RP_InflatePieces(const uint8_t *stream,size_t slen,const uint32_t *lens,unsigned n,uint8_t *out,size_t rawsize) {
    if (n < 2 || n > RP_PIECES_MAX || slen < 6) return false;
    size_t sum = 0;
    for (unsigned k=0;k < n;k++) sum += lens[k];
    if (sum + 6 != slen) return false;
    RP_ZStart();
    const unsigned wave = zworkers + 1;
    const size_t piece = rawsize / n;
    uLong adlers[RP_PIECES_MAX];
    size_t in_at = 2;
    for (unsigned base=0;base < n;base += wave) {
        const unsigned m = std::min(wave,n - base);
        for (unsigned i=0;i < m;i++) {
            const unsigned k = base + i;
            ZJob &j = zjobs[i];
            j.kind = 2;
            j.src = stream + in_at;
            j.len = lens[k];
            j.idx = out + (size_t)k * piece;
            j.want = (k + 1 == n) ? rawsize - (size_t)k * piece : piece;
            j.last = (k + 1 == n);
            in_at += lens[k];
        }
        for (unsigned i=0;i + 1 < m;i++) SDL_SemPost(zgo[i]);
        RP_ZRun(zjobs[m - 1]);
        for (unsigned i=0;i + 1 < m;i++) SDL_SemWait(zdone[i]);
        for (unsigned i=0;i < m;i++) {
            if (!zjobs[i].ok) return false;
            adlers[base + i] = zjobs[i].adler;
        }
    }
    uLong adler = adlers[0];
    for (unsigned k=1;k < n;k++)
        adler = adler32_combine(adler,adlers[k],(z_off_t)((k + 1 == n) ? rawsize - (size_t)k * piece : piece));
    const uint8_t *t = stream + slen - 4;
    return adler == (((uLong)t[0] << 24) | ((uLong)t[1] << 16) | ((uLong)t[2] << 8) | (uLong)t[3]);
}

/* Sound */
std::vector<int16_t> aud_buf;
uint32_t aud_seq = 0;
unsigned aud_rate = 0;

/* Statistics */
uint64_t stat_bytes = 0, stat_enc_us = 0, stat_index_us = 0, stat_cap_us = 0;
uint32_t stat_frames = 0, stat_keys = 0, stat_offered = 0, stat_skipped = 0, stat_replaced = 0;
uint32_t stat_shifted = 0;                      /* frames sent with scroll prediction */

/* Remote input guard. Viewers must never be able to type DOS commands on the
 * host (DEL, FORMAT, MOUNT, ... on host folders mounted as drives), so their
 * keyboard and mouse only reach the machine while a DOS program other than a
 * command shell runs, or once a guest system was booted. */
bool guard_guest_os = false;    /* BOOT started a guest system: no DOS kernel, no shell */
int guard_state = -1;           /* -1 not checked yet, 0 allowed, 1 blocked */
bool guard_typed = false;       /* a viewer pressed keys since remote input was last blocked */

std::string Guard_ProcessName(uint16_t psp) {
    std::string name = DOS_MCB((uint16_t)(psp - 1)).GetFileName();
    while (!name.empty() && name.back() == ' ') name.pop_back();
    for (size_t i=0;i < name.size();i++) name[i] = (char)toupper((unsigned char)name[i]);
    return name;
}

/* True while the current DOS process is DOSBox-X's shell (the first one or a
 * nested COMMAND.COM), one of its internal programs (MOUNT, CONFIG, ...), a
 * command shell loaded from disk, or while DOS is not up yet. This also covers
 * the moments between a game's exit and the batch file's next line, Ctrl+C or
 * Ctrl+Break back to the prompt and "Terminate batch job (Y/N)?". */
bool Guard_Blocked(void) {
    if (dos_kernel_disabled) return !guard_guest_os;
    const uint16_t psp = dos.psp();
    if (psp < 0x40 || psp == shell_psp || PROGRAMS_IsRunningInternalPSP(psp)) return true;
    const std::string name = Guard_ProcessName(psp);
    return name == "COMMAND" || name == "4DOS" || name == "NDOS";
}

/* Throws away keystrokes that are waiting in the keyboard controller and in
 * the BIOS keyboard buffer (where a game that read them through the BIOS may
 * have left them), so the DOS prompt never reads what a viewer typed. */
void Guard_FlushKeyboard(void) {
    KEYBOARD_ClrBuffer();
    if (!IS_PC98_ARCH) {
        mem_writeb(BIOS_KEYBOARD_TOKEN,0);  /* Alt+numpad code being typed */
        mem_writew(BIOS_KEYBOARD_BUFFER_TAIL,mem_readw(BIOS_KEYBOARD_BUFFER_HEAD));
    }
}

void Host_ReleaseInput(Viewer &v);

void Guard_Update(void) {
    const bool blocked = Guard_Blocked();
    if (guard_state == (blocked ? 1 : 0)) return;
    guard_state = blocked ? 1 : 0;
    if (blocked) {
        if (guard_typed) Guard_FlushKeyboard();
        guard_typed = false;
        for (unsigned i=0;i < RP_MAX_VIEWERS;i++)
            if (viewers[i].active) Host_ReleaseInput(viewers[i]);
        LOG_MSG("INPUTNET: remote input blocked: the DOS prompt or a DOSBox-X command is in the foreground");
    }
    else if (dos_kernel_disabled) {
        LOG_MSG("INPUTNET: remote input allowed: a guest system is running");
    }
    else {
        LOG_MSG("INPUTNET: remote input allowed: %s is running",Guard_ProcessName(dos.psp()).c_str());
    }
}

bool Guard_Allows(void) {
    Guard_Update();
    return guard_state == 0;
}

void Host_SendTo(const IPaddress &addr,const uint8_t *data,size_t len) {
    if (host_lock) SDL_LockMutex(host_lock);
    if (host_sock != NULL && host_out != NULL && len <= (size_t)host_out->maxlen) {
        memcpy(host_out->data,data,len);
        host_out->len = (int)len;
        host_out->address = addr;
        SDLNet_UDP_Send(host_sock,-1,host_out);
        stat_bytes += len;
    }
    if (host_lock) SDL_UnlockMutex(host_lock);
}

void Host_SendAll(const uint8_t *data,size_t len) {
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++)
        if (viewers[i].active) Host_SendTo(viewers[i].addr,data,len);
}

/* A datagram sent from the caller's own memory, without host_lock: sending on a socket another thread receives or
 * sends on is fine, and SDL_net only reads the packet structure it is given. */
void Host_SendDirect(const IPaddress &addr,const uint8_t *data,size_t len) {
    if (host_sock == NULL) return;
    UDPpacket p;
    memset(&p,0,sizeof(p));
    p.channel = -1;
    p.data = (Uint8*)data;
    p.len = p.maxlen = (int)len;
    p.address = addr;
    SDLNet_UDP_Send(host_sock,-1,&p);
}

/* Every viewer decodes against named frames and reports what arrives: frames go against a frame they all have and the
 * video rate follows the link. One older viewer keeps everyone on the previous behaviour. */
bool Host_RefsMode(void) {
    bool any = false;
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
        if (!viewers[i].active) continue;
        if (!(viewers[i].features & RP_FEAT_REFS)) return false;
        any = true;
    }
    return any;
}

/* Every viewer reads a frame's piece table. */
bool Host_PiecesMode(void) {
    bool any = false;
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
        if (!viewers[i].active) continue;
        if (!(viewers[i].features & RP_FEAT_PIECES)) return false;
        any = true;
    }
    return any;
}

/* Scroll prediction only when every viewer undoes it. */
bool Host_ShiftMode(void) {
    bool any = false;
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
        if (!viewers[i].active) continue;
        if (!(viewers[i].features & RP_FEAT_SHIFT)) return false;
        any = true;
    }
    return any;
}

void Host_UpdateRateMode(void) {
    const bool adaptive = Host_RefsMode();
    if (adaptive == rate_adaptive) return;
    rate_adaptive = adaptive;
    send_rate = RP_RATE_MAX;
    rate_delivered = 0;
    rate_limited = false;
    rate_cut_ms = SDL_GetTicks();
}

/* A viewer's fragment counters (cumulative, from RP_FRAMEACK): less on loss, more while the link takes everything and
 * video is waiting for it. */
/* The time from a frame leaving here to its viewer saying it has decoded it (viewers say so at once): the way there
 * and back, plus whatever waits in a queue on the way. Loss is not the only sign of sending more than the way takes:
 * the buffer of a slow link, or a program that forwards the packets and cannot keep up, loses nothing for a long time
 * and makes the picture seconds late instead (three 640x480 streams through one NukemNet tunnel did, with pings above
 * a second). The shortest time of the last 10 to 20 seconds is taken for the way itself; a time well above it means
 * a queue is growing, and the rate is cut as it is on loss (Host_RateFeedback). */
void Host_DelayFeedback(Viewer &v,uint32_t id,uint32_t now) {
    if (!rate_adaptive || frame_sent_id[id & 63u] != id) return;
    const uint32_t d = now - frame_sent_ms[id & 63u];
    if (d > 20000u) return;
    if (v.delay_bucket_ms == 0 || now - v.delay_bucket_ms >= 10000u) {
        v.delay_base_prev = v.delay_bucket_ms ? v.delay_base : d;
        v.delay_base = d;
        v.delay_bucket_ms = now ? now : 1u;
    }
    else if (d < v.delay_base) v.delay_base = d;
    const double base = (double)std::min(v.delay_base,v.delay_base_prev);
    v.delay_now = (v.delay_now <= 0) ? (double)d : v.delay_now * 0.5 + (double)d * 0.5;
    v.delay_late = v.delay_now > base + std::max(60.0,base);
    v.delay_raised = v.delay_now > base + std::max(30.0,base * 0.5);
}

void Host_RateFeedback(Viewer &v,uint32_t exp,uint32_t got) {
    if (!rate_adaptive) return;
    const uint32_t now = SDL_GetTicks();
    if (v.loss_ms == 0) { v.loss_exp = exp; v.loss_got = got; v.loss_ms = now; return; }
    if (now - v.loss_ms < 200) return;
    const uint32_t de = exp - v.loss_exp, dg = got - v.loss_got;
    if (de < 24 && now - v.loss_ms < 1000) return;  /* too few to judge yet */
    const double secs = (now - v.loss_ms) / 1000.0;
    v.loss_exp = exp; v.loss_got = got; v.loss_ms = now;
    if (de == 0 || de > 0x7fffffffu) return;
    const double loss = dg < de ? (double)(de - dg) / (double)de : 0.0;

    /* Video goes out as fast as it comes (no pacing, no added latency) until a viewer reports loss: a 320x200 game,
     * or any game on a link that takes it all, is never paced. Loss turns pacing on, at what actually got through, and
     * from then on the rate follows the link: down on loss, up while video waits for it and none is lost, and never
     * above twice what viewers have received lately (remembered for a while, halving in about half a minute), so a
     * still picture does not let it creep up and the next burst of motion is not sent far faster than the link takes. */
    const double delivered_now = (double)dg * RP_FRAG / secs;
    rate_delivered = std::max(rate_delivered * pow(0.98,secs),delivered_now);
    if (loss > 0.03) {
        /* Loss reports lag a round trip behind: cut again only once the last cut could show. */
        if (now - rate_cut_ms >= 400) {
            if (!rate_limited) send_rate = std::min(send_rate,std::max(RP_RATE_START,delivered_now));
            else send_rate = std::max(RP_RATE_MIN,send_rate * 0.75);
            rate_limited = true;
            rate_cut_ms = now;
        }
    }
    else if (v.delay_late) {
        /* Frames arrive late: a queue stands somewhere on the way (Host_DelayFeedback). While it does, what arrives
         * is what the way takes: a little below that, so the queue empties. The rate is never cut step after step
         * while the queue empties (that took it down to almost nothing on a link that carried 700 KB/s). */
        if (now - rate_cut_ms >= 400) {
            send_rate = std::min(send_rate,std::max(4.0 * RP_RATE_MIN,delivered_now * 0.85));
            rate_limited = true;
            rate_cut_ms = now;
        }
    }
    else if (rate_limited && loss < 0.01 && !v.delay_raised && now - rate_cut_ms >= 400 && now - send_backlog_ms < 300) {
        send_rate = std::min(send_rate * 1.2,RP_RATE_MAX);
    }
    if (rate_limited) send_rate = std::min(send_rate,std::max(RP_RATE_START,rate_delivered * 2.0));
    if (now - rate_log_ms >= 5000) {
        static uint32_t last_frames = 0;
        static uint64_t last_bytes = 0;
        const double secs = rate_log_ms ? (now - rate_log_ms) / 1000.0 : 5.0;
        const uint32_t frames = stat_frames - last_frames;
        static uint32_t last_offered = 0, last_skipped = 0;
        static uint32_t last_replaced = 0, last_shifted = 0;
        static uint64_t last_enc_us = 0, last_index_us = 0, last_cap_us = 0;
        LOG_MSG("INPUTNET: video at most %u KB/s, sent %u KB/s in %.1f frames/s (of %.1f, %.1f skipped for the link, %.1f for the encoder), %.1f%% of fragments lost; encoding %.1f ms a frame (%.1f of it palette, %u threads), %.2f ms of the game's own thread%s; scroll prediction in %u%% of the frames; frames acknowledged after %.0f ms (%u at best)%s; CPU %ld cycles",
            (unsigned)(send_rate / 1024.0),(unsigned)((stat_bytes - last_bytes) / 1024.0 / secs),frames / secs,
            (stat_offered - last_offered) / secs,(stat_skipped - last_skipped) / secs,
            (stat_replaced - last_replaced) / secs,loss * 100.0,
            frames ? (stat_enc_us - last_enc_us) / 1000.0 / frames : 0.0,
            frames ? (stat_index_us - last_index_us) / 1000.0 / frames : 0.0,zworkers + 1,
            frames ? (stat_cap_us - last_cap_us) / 1000.0 / frames : 0.0,
            enc_thread != NULL ? "" : " (no encoder thread)",
            frames ? (unsigned)((stat_shifted - last_shifted) * 100u / frames) : 0u,
            v.delay_now,(unsigned)std::min(v.delay_base,v.delay_base_prev),v.delay_late ? ", late" : "",(long)CPU_CycleMax);
        last_shifted = stat_shifted;
        last_frames = stat_frames; last_bytes = stat_bytes; last_offered = stat_offered; last_skipped = stat_skipped;
        last_replaced = stat_replaced;
        last_enc_us = stat_enc_us; last_index_us = stat_index_us; last_cap_us = stat_cap_us;
        rate_log_ms = now;
    }
}

void Host_UpdateStreaming(void) {
    bool any = false;
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) any = any || viewers[i].active;
    if (any && !remoteplay_streaming) {
        enc_needkey = true;
        enc_haveprev = false;
    }
    remoteplay_streaming = any;
    if (!any) {
        send_queue.clear();
        send_queued = 0;
        aud_buf.clear();
    }
    Host_UpdateRateMode();
}

void Host_ReleaseInput(Viewer &v) {
    for (unsigned k=0;k < (unsigned)KBD_LAST;k++) {
        if (v.keydown[k]) {
            v.keydown[k] = false;
            KEYBOARD_AddKeyRaw((KBD_KEYS)k,false);
        }
    }
    for (unsigned b=0;b < 32;b++) {
        if (v.buttons & (1u << b)) Mouse_ButtonReleasedRaw((uint8_t)b);
    }
    v.buttons = 0;
    nn_remote_cursor_locked = false;    /* a viewer that left with its mouse captured must not leave the guest's pointer driver in relative mode */
}

void Host_DropViewer(Viewer &v,const char *reason,bool notify) {
    if (!v.active) return;
    if (notify) {
        Writer w(RP_BYE);
        w.str(reason ? reason : "");
        Host_SendTo(v.addr,w.buf,w.len);
    }
    Host_ReleaseInput(v);
    LOG_MSG("INPUTNET: remote player \"%s\" left (%s)",v.name.c_str(),reason ? reason : "");
    v.active = false;
    Host_UpdateStreaming();
}

/* Viewer input goes straight to the emulated keyboard and mouse (below the
 * mapper), so it can never trigger DOSBox-X's own hotkeys or menu functions.
 * It is dropped while the remote input guard blocks it. */
void Host_ApplyEvent(Viewer &v,Reader &r) {
    const uint8_t type = r.u8();
    switch (type) {
        case RPEV_KEY: {
            const uint16_t key = r.u16();
            const bool pressed = r.u8() != 0;
            if (!r.ok || key == 0 || key >= (uint16_t)KBD_LAST) return;
            /* Print Screen makes the BIOS print the screen on the host's printer emulation. */
            if (key == (uint16_t)KBD_printscreen || !Guard_Allows()) return;
            v.keydown[key] = pressed;
            if (pressed) guard_typed = true;
            KEYBOARD_AddKeyRaw((KBD_KEYS)key,pressed);
            break;
        }
        case RPEV_MBUTTON: {
            const uint8_t button = r.u8();
            const bool pressed = r.u8() != 0;
            if (!r.ok || button >= 32 || !Guard_Allows()) return;
            if (pressed) { v.buttons |= 1u << button; Mouse_ButtonPressedRaw(button); }
            else { v.buttons &= ~(1u << button); Mouse_ButtonReleasedRaw(button); }
            break;
        }
        case RPEV_MMOVE: {
            const float xrel = r.f32(), yrel = r.f32(), x = r.f32(), y = r.f32();
            const bool emulate = r.u8() != 0;
            if (!r.ok || !Guard_Allows()) return;
            if (mouse_notify_mode != 0) {
                /* The guest's pointer driver asks the integration device where the pointer is (bios.cpp): for this
                 * player that is the place in the viewer's picture. A viewer that captured its mouse gives relative
                 * motion, as a captured host does; otherwise the motion packet only tells the driver to look. */
                const float cx = (x >= 0.0f) ? std::min(x,1.0f) : 0.0f, cy = (y >= 0.0f) ? std::min(y,1.0f) : 0.0f;
                nn_remote_cursor_x = (uint16_t)(cx * 65535.0f + 0.5f);
                nn_remote_cursor_y = (uint16_t)(cy * 65535.0f + 0.5f);
                nn_remote_cursor_locked = emulate;
                nn_remote_cursor = true;
                if (emulate) Mouse_CursorMovedRaw(xrel,yrel,x,y,true);
                else Mouse_CursorMovedRaw(0.0f,0.0f,0.0f,0.0f,false);
                break;
            }
            /* the viewer captured its mouse: relative motion, also for the INT 33h motion counters */
            mouse_motion_is_relative = emulate;
            Mouse_CursorMovedRaw(xrel,yrel,x,y,emulate);
            mouse_motion_is_relative = false;
            break;
        }
        case RPEV_WHEEL: {
            const int16_t amount = (int16_t)r.u16();
            if (!r.ok || !Guard_Allows()) return;
            Mouse_WheelMovedRaw(amount);
            break;
        }
        default:
            r.ok = false;
            break;
    }
}

void Host_HandlePacket(const IPaddress &from,const uint8_t *data,size_t len) {
    Reader r(data,len);
    const uint8_t type = RP_Header(r);
    if (type == 0) return;

    Viewer *v = NULL;
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
        if (viewers[i].active && RP_SameAddr(viewers[i].addr,from)) { v = &viewers[i]; break; }
    }

    if (type == RP_HELLO) {
        std::string name = r.str();
        const uint32_t session = r.u32();
        if (!r.ok) return;
        const uint32_t features = (r.pos + 4 <= r.len) ? r.u32() : 0;
        if (name.empty()) name = "player";
        if (v != NULL && v->session != session) {
            /* Same address, new DOSBox-X: forget the old one's input. */
            Host_DropViewer(*v,"rejoined",false);
            v = NULL;
        }
        if (v == NULL) {
            for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
                if (!viewers[i].active) { v = &viewers[i]; break; }
            }
            if (v == NULL) {
                Writer w(RP_REFUSE);
                w.str("the host is full");
                Host_SendTo(from,w.buf,w.len);
                return;
            }
            v->active = true;
            v->addr = from;
            v->name = name;
            v->session = session;
            v->applied = 0;
            v->applied_us = 0;
            v->buttons = 0;
            v->features = features;
            v->hasack = false;
            v->acked = 0;
            v->loss_ms = 0;
            memset(v->keydown,0,sizeof(v->keydown));
            LOG_MSG("INPUTNET: remote player \"%s\" joined from %s",name.c_str(),RP_AddrString(from).c_str());
            Host_UpdateStreaming();
            enc_needkey = true;
        }
        v->lastheard = SDL_GetTicks();
        Writer w(RP_WELCOME);
        w.u8((uint8_t)(v - viewers));
        w.u32(aud_rate);
        Host_SendTo(from,w.buf,w.len);
        return;
    }

    if (v == NULL) return;
    v->lastheard = SDL_GetTicks();

    switch (type) {
        case RP_INPUT: {
            if (r.u32() != v->session) break;
            const uint32_t first = r.u32();
            const unsigned count = r.u8();
            for (unsigned i=0;i < count && r.ok;i++) {
                const uint32_t seq = first + i;
                if (seq == v->applied + 1) {
                    Host_ApplyEvent(*v,r);
                    if (r.ok) { v->applied = seq; v->applied_us = RP_Micros(); }
                }
                else {
                    /* Already applied (a repeat): parse it to skip over it. */
                    const size_t before = r.pos;
                    const uint8_t t = r.u8();
                    switch (t) {
                        case RPEV_KEY: r.pos += 3; break;
                        case RPEV_MBUTTON: r.pos += 2; break;
                        case RPEV_MMOVE: r.pos += 17; break;
                        case RPEV_WHEEL: r.pos += 2; break;
                        default: r.ok = false; break;
                    }
                    if (r.pos > r.len) r.ok = false;
                    if (seq > v->applied + 1) { r.pos = before; break; }
                }
            }
            Writer w(RP_INPUTACK);
            w.u32(v->applied);
            Host_SendTo(from,w.buf,w.len);
            break;
        }
        case RP_KEYREQ:
            /* With named bases a viewer asks when it lacks one; one key frame per round trip is enough. */
            if (rate_adaptive) enc_keyreq = true;
            else enc_needkey = true;
            break;
        case RP_FRAMEACK: {
            if (r.u32() != v->session) break;
            const uint8_t flags = r.u8();
            const uint32_t id = r.u32();
            const uint32_t exp = r.u32();
            const uint32_t got = r.u32();
            if (!r.ok) break;
            if ((flags & 1) && (!v->hasack || (int32_t)(id - v->acked) > 0) && (int32_t)(enc_frameid - id) >= 0) {
                v->acked = id;
                v->hasack = true;
                Host_DelayFeedback(*v,id,SDL_GetTicks());
            }
            Host_RateFeedback(*v,exp,got);
            break;
        }
        case RP_BYE:
            Host_DropViewer(*v,"left the session",false);
            break;
        default:
            break;
    }
}

/* Sends the video the rate allows now. The encoder thread and the main loop both come here: what may go is taken off
 * the queue under host_lock and sent after it, so input and sound are not held up while a whole frame is sent.
 * send_lock keeps the two from sending at once (a frame's fragments overtaken by the next frame's would be thrown
 * away by the viewer). The main loop does not wait for it (wait false): the encoder is sending already, and what the
 * rate holds back goes with the next poll. */
void Host_FlushQueue(bool wait) {
    std::vector<std::vector<uint8_t> > out;
    IPaddress to[RP_MAX_VIEWERS];
    unsigned nto = 0;

    if (send_lock != NULL) {
#if SDL_VERSION_ATLEAST(2,0,0)
        if (!wait) {
            if (SDL_TryLockMutex(send_lock) != 0) return;
        }
        else
#else
        (void)wait;
#endif
        SDL_LockMutex(send_lock);
    }
    Host_Lock();
    const uint32_t now = SDL_GetTicks();
    if (now != send_lastms) {
        /* send_rate spread over each millisecond, so a big key frame does not overflow the receiver's socket buffer or
         * a router's queue in one burst. Up to 32 KB (or 4 ms worth) may still go at once: an ordinary frame of a
         * 320x200 game fits, and goes out without waiting for the pacing. */
        const uint32_t elapsed = now - send_lastms;
        const double burst = std::max(send_rate * 0.004,32.0 * 1024.0);
        send_budget = std::min(send_budget + send_rate * (double)elapsed / 1000.0,burst);
        send_lastms = now;
    }
    size_t bytes = 0;
    while (!send_queue.empty() && send_budget >= (double)send_queue.front().size()) {
        const size_t n = send_queue.front().size();
        {
            /* "NNRP", version, type, u32 frame, u16 fragment, u16 fragments: the frame's last fragment leaves now */
            const std::vector<uint8_t> &f = send_queue.front();
            if (n >= 14 && f[5] == RP_VIDEO && ((unsigned)f[10] | ((unsigned)f[11] << 8)) + 1u == ((unsigned)f[12] | ((unsigned)f[13] << 8))) {
                const uint32_t fid = (uint32_t)f[6] | ((uint32_t)f[7] << 8) | ((uint32_t)f[8] << 16) | ((uint32_t)f[9] << 24);
                frame_sent_id[fid & 63u] = fid;
                frame_sent_ms[fid & 63u] = now;
            }
        }
        send_budget -= (double)n;
        send_queued -= std::min(send_queued,n);
        bytes += n;
        out.push_back(std::move(send_queue.front()));
        send_queue.pop_front();
    }
    /* Video waits for the rate, not the other way round: only then is a higher rate worth trying. */
    if (!send_queue.empty()) send_backlog_ms = now;
    if (!out.empty()) {
        for (unsigned i=0;i < RP_MAX_VIEWERS;i++)
            if (viewers[i].active) to[nto++] = viewers[i].addr;
        stat_bytes += (uint64_t)bytes * nto;
    }
    Host_Unlock();

    for (size_t k=0;k < out.size();k++)
        for (unsigned i=0;i < nto;i++) Host_SendDirect(to[i],out[k].data(),out[k].size());
    if (send_lock != NULL) SDL_UnlockMutex(send_lock);
}

int Host_KeepaliveThread(void *) {
    while (host_keepalive_stop == 0) {
        SDL_Delay(100);
        if (SDL_GetTicks() - host_lastpoll < 500) continue;
        SDL_LockMutex(host_lock);
        while (host_sock != NULL && SDLNet_UDP_Recv(host_sock,host_in) > 0) {
            Reader r(host_in->data,(size_t)host_in->len);
            const uint8_t type = RP_Header(r);
            if (type == 0) continue;
            Viewer *v = NULL;
            for (unsigned i=0;i < RP_MAX_VIEWERS;i++)
                if (viewers[i].active && RP_SameAddr(viewers[i].addr,host_in->address)) { v = &viewers[i]; break; }
            if (v == NULL) continue;    /* someone new: their HELLO comes again, the main loop lets them in */
            if (type == RP_HELLO) {
                (void)r.str();
                if (r.u32() != v->session || !r.ok) continue;
                Writer w(RP_WELCOME);
                w.u8((uint8_t)(v - viewers));
                w.u32(aud_rate);
                Host_SendTo(host_in->address,w.buf,w.len);
            }
            if (type == RP_BYE) continue;  /* the main loop sees it through the timeout */
            v->lastheard = SDL_GetTicks();
        }
        SDL_UnlockMutex(host_lock);
    }
    return 0;
}

} /* namespace */

void REMOTEPLAY_Configure(Section_prop *section) {
    int v = section->Get_int("timeout");
    cfg_timeout = (unsigned)((v >= 250 && v <= 600000) ? v : 10000);
    v = section->Get_int("stream fps");
    cfg_maxfps = (unsigned)((v >= 0 && v <= 240) ? v : 0);
    const std::string present = section->Get_string("present");
    cfg_present = (present == "scanout") ? 0 : (present == "retrace") ? 1 : 2;
}

int REMOTEPLAY_PresentMode(void) {
    return host_running ? cfg_present : 0;
}

bool REMOTEPLAY_StartServer(uint16_t port) {
    if (host_running) return false;
    if (!RP_OpenNet()) return false;
    if (host_out == NULL) host_out = SDLNet_AllocPacket((int)RP_PACKET_MAX);
    if (host_in == NULL) host_in = SDLNet_AllocPacket((int)RP_PACKET_MAX);
    if (host_out == NULL || host_in == NULL) return false;

    host_sock = SDLNet_UDP_Open(port);
    if (host_sock == NULL) {
        LOG_MSG("INPUTNET: unable to open UDP port %u: %s",(unsigned)port,SDLNet_GetError());
        return false;
    }
    RP_WidenSocket(host_sock);
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) viewers[i].active = false;
    host_running = true;
    host_port = port;
    /* The window may have lost focus already, which stops cycles=max from guessing: a host keeps guessing without it. */
    CPU_Disable_SkipAutoAdjust();
    if (host_lock == NULL) host_lock = SDL_CreateMutex();
    if (send_lock == NULL) send_lock = SDL_CreateMutex();
    host_lastpoll = SDL_GetTicks();
    host_keepalive_stop = 0;
    if (host_lock != NULL && host_keepalive == NULL)
#if SDL_VERSION_ATLEAST(2,0,0)
        host_keepalive = SDL_CreateThread(Host_KeepaliveThread,"INPUTNET keepalive",NULL);
#else
        host_keepalive = SDL_CreateThread(Host_KeepaliveThread,NULL);
#endif
    Enc_Start();
    RP_ZStart();
    enc_needkey = true;
    enc_haveprev = false;
    guard_state = -1;
    guard_typed = false;
    LOG_MSG("INPUTNET: remote play: waiting for players on UDP port %u",(unsigned)port);
    REMOTEPLAY_UpdateMenu();
    return true;
}

void REMOTEPLAY_StopServer(const char *reason) {
    if (!host_running) return;
    if (host_keepalive != NULL) {
        host_keepalive_stop = 1;
        SDL_WaitThread(host_keepalive,NULL);
        host_keepalive = NULL;
    }
    Enc_Stop();     /* before the socket goes: it sends on it */
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) Host_DropViewer(viewers[i],reason ? reason : "the host stopped",true);
    SDLNet_UDP_Close(host_sock);
    host_sock = NULL;
    host_running = false;
    remoteplay_streaming = false;
    send_queue.clear();
    send_queued = 0;
    RP_ZStop();
    LOG_MSG("INPUTNET: remote play server stopped");
    REMOTEPLAY_UpdateMenu();
}

bool REMOTEPLAY_ServerRunning(void) {
    return host_running;
}

void REMOTEPLAY_ProcessEnded(void) {
    if (host_running) Guard_Update();
}

static void REMOTEPLAY_GuestBoot(Section *) {
    guard_guest_os = true;
    /* A locked down copy (remoteplay.h): the guest now runs whatever the remote player makes it run. Secure mode
     * cannot be left again, so even a guest that resets the machine never reaches MOUNT, IMGMOUNT or BOOT. */
    if (NN_Lockdown() && !control->SecureMode()) {
        control->SwitchToSecureMode();
        LOG_MSG("INPUTNET: locked down copy: secure mode from here on");
    }
}

static void REMOTEPLAY_DOSBoot(Section *) {
    guard_guest_os = false;
}

void REMOTEPLAY_Init(void) {
    AddVMEventFunction(VM_EVENT_GUEST_OS_BOOT,AddVMEventFunctionFuncPair(REMOTEPLAY_GuestBoot));
    AddVMEventFunction(VM_EVENT_RESET,AddVMEventFunctionFuncPair(REMOTEPLAY_DOSBoot));
    AddVMEventFunction(VM_EVENT_DOS_BOOT,AddVMEventFunctionFuncPair(REMOTEPLAY_DOSBoot));
}

bool REMOTEPLAY_HostRequested(void) {
    Section_prop *sec = static_cast<Section_prop *>(control->GetSection("inputnet"));
    if (sec == NULL || std::string(sec->Get_string("mode")) == "lockstep") return false;
    if (control->opt_inputnet_server >= 0) return true;
    if (!control->opt_inputnet_connect.empty() || !sec->Get_bool("inputnet")) return false;
    return std::string(sec->Get_string("connect")).empty() && !std::string(sec->Get_string("startserver")).empty();
}

bool REMOTEPLAY_HideScreenRequested(void) {
    Section_prop *sec = static_cast<Section_prop *>(control->GetSection("inputnet"));
    if (sec == NULL) return false;
    return (control->opt_inputnet_hidescreen || !sec->Get_bool("hostscreen")) && REMOTEPLAY_HostRequested();
}

void REMOTEPLAY_Status(Program *prog) {
    if (!host_running) {
        prog->WriteOut("No remote play server is running.\n");
        return;
    }
    prog->WriteOut("Remote play server on UDP port %u.\n",(unsigned)host_port);
    unsigned n = 0;
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
        if (!viewers[i].active) continue;
        prog->WriteOut("  %s from %s\n",viewers[i].name.c_str(),RP_AddrString(viewers[i].addr).c_str());
        n++;
    }
    if (n == 0) prog->WriteOut("No players connected.\n");
    Host_Lock();
    const unsigned frames = (unsigned)stat_frames, keys = (unsigned)stat_keys, kb = (unsigned)(stat_bytes / 1024u);
    Host_Unlock();
    prog->WriteOut("Sent %u frames (%u key frames), %u KB.\n",frames,keys,kb);
}

void REMOTEPLAY_Poll(void) {
    if (!host_running) return;

    /* GFX_Events runs very often; the socket only needs a look once a
     * millisecond (the video rate allows more once a millisecond too, and
     * whoever queues video sends what may go at once). */
    static uint32_t lastpoll = 0;
    const uint32_t now = SDL_GetTicks();
    if (now == lastpoll) return;
    lastpoll = now;

    /* Releases what viewers hold as soon as a game has ended, even when no
     * input arrives. */
    Guard_Update();

    host_lastpoll = now;
    if (host_lock) SDL_LockMutex(host_lock);
    while (SDLNet_UDP_Recv(host_sock,host_in) > 0)
        Host_HandlePacket(host_in->address,host_in->data,(size_t)host_in->len);
    if (host_lock) SDL_UnlockMutex(host_lock);

#if defined(USE_TTF)
    /* TrueType text output draws the DOS prompt without filling the frame
     * the viewers are sent, so use the regular output while anyone watches
     * or the local screen is hidden (ttf_switch_on stays off meanwhile). */
    if ((remoteplay_streaming || remoteplay_screen_hidden) && TTF_using()) ttf_switch_off(false);
#endif

    static uint32_t lastcheck = 0;
    if (now - lastcheck >= 100) {
        lastcheck = now;
        if (host_lock) SDL_LockMutex(host_lock);
        for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
            if (viewers[i].active && (int32_t)(now - viewers[i].lastheard) > (int32_t)cfg_timeout)
                Host_DropViewer(viewers[i],"timed out",true);
        }
        if (host_lock) SDL_UnlockMutex(host_lock);
    }

    Host_FlushQueue(false);
}

namespace {

/* Encodes one picture and queues it for the viewers: on the encoder thread, or on the emulation thread when there is
 * none. */
void Enc_Frame(const Capture &c) {
    const unsigned width = c.w, height = c.h, bpp = c.bpp;
    const size_t rowbytes = (size_t)width * RP_BytesPerPixel(bpp);

    /* 32-bit pixels of at most 256 colours go out as an 8-bit frame (any viewer decodes it). */
    const uint64_t index_start = RP_Micros();
    const bool indexed = (bpp == 32) && RP_IndexFrame(width,height,(unsigned)rowbytes,c.px.data(),enc_raw);
    const uint64_t index_us = (bpp == 32) ? RP_Micros() - index_start : 0;
    const unsigned fmt = indexed ? 8 : bpp;
    const size_t palbytes = (fmt == 8) ? 768 : 0;
    const size_t rawsize = palbytes + (size_t)width * RP_BytesPerPixel(fmt) * height;
    if (!indexed) {
        enc_raw.resize(rawsize);
        uint8_t *o = enc_raw.data();
        if (palbytes) {
            for (unsigned i=0;i < 256;i++) {
                *o++ = c.pal[i*4+0];
                *o++ = c.pal[i*4+1];
                *o++ = c.pal[i*4+2];
            }
        }
        memcpy(o,c.px.data(),rowbytes * height);
    }
    /* Only this thread changes the frames kept as bases; when they are out of date a key frame is due and this is
     * not looked at. */
    const bool unchanged = !enc_hist.empty() && enc_hist.back().raw.size() == rawsize
        && memcmp(enc_raw.data(),enc_hist.back().raw.data(),rawsize) == 0;

    /* What to send. */
    Host_Lock();
    const uint32_t now = SDL_GetTicks();
    stat_index_us += index_us;
    if (width != enc_w || height != enc_h || fmt != enc_fmt) {
        enc_w = width; enc_h = height; enc_fmt = fmt;
        enc_haveprev = false;
        enc_needkey = true;
    }
    if (!enc_haveprev) {
        enc_hist.clear();
        enc_haskey = false;
        enc_key.raw.clear();
    }
    const bool refs = rate_adaptive;
    const bool forced = enc_needkey;
    bool key = forced || enc_hist.empty() || (now - enc_lastkey) >= (refs ? 10000u : 5000u)
        || (enc_keyreq && now - enc_lastkey >= 300);
    if (!key && unchanged && (now - enc_lastsend) < 250) {
        Host_Unlock();
        return; /* nothing changed; still send now and then so the viewer knows we are alive */
    }

    /* The base: the previous frame sent, or with named bases the oldest of each viewer's newest decoded frame (the last
     * key frame for a viewer that has decoded none we still have). */
    const EncFrame *base = NULL;
    if (!key) {
        if (!refs) base = &enc_hist.back();
        else {
            auto find = [](uint32_t id) -> const EncFrame* {
                for (size_t i=enc_hist.size();i-- > 0;) if (enc_hist[i].id == id) return &enc_hist[i];
                if (enc_haskey && enc_key.id == id) return &enc_key;
                return NULL;
            };
            for (unsigned i=0;i < RP_MAX_VIEWERS && !key;i++) {
                const Viewer &v = viewers[i];
                if (!v.active) continue;
                const EncFrame *f = v.hasack ? find(v.acked) : NULL;
                if (f == NULL && enc_haskey && (!v.hasack || (int32_t)(v.acked - enc_key.id) < 0)) f = &enc_key;
                if (f == NULL) key = true;
                else if (base == NULL || (int32_t)(f->id - base->id) < 0) base = f;
            }
            if (base != NULL && enc_frameid + 1 - base->id > 65535u) key = true;
        }
        if (base == NULL || base->raw.size() != rawsize) key = true;
    }
    /* With named bases a key frame goes out at most every 300 ms unless the picture itself changed shape: a viewer that
     * lost the last one asks again, and back to back key frames would only fill the link. */
    if (key && refs && !forced && enc_haskey && now - enc_lastkey < 300) {
        Host_Unlock();
        return;
    }
    /* The requests are taken here, before the slow part: one that comes while this frame is being encoded is for the
     * next frame. */
    const uint32_t id = enc_frameid + 1;
    enc_needkey = false;
    enc_haveprev = true;
    enc_lastsend = now;
    if (key) { enc_lastkey = now; enc_keyreq = false; }
    const bool table = Host_PiecesMode();
    const bool shiftmode = Host_ShiftMode();
    const bool limited = rate_limited;
    Host_Unlock();

    /* While the picture keeps moving a lot, the frame alone has been the smaller one every time: three frames in four
     * then go out alone without compressing the difference as well, which is half the work. Not on a link that
     * limits the video: there every byte counts more than a millisecond. */
    bool turned_key = false;
    /* A picture that scrolled is predicted from the base moved the same way (see RP_ShiftXor), which beats the frame
     * alone: such a frame is never one of those three. */
    ShiftGeom geom;
    geom.w = width; geom.h = height; geom.bpp = RP_BytesPerPixel(fmt); geom.pal = palbytes;
    bool shifted = false;
    enc_step_n = 0;
    if (!key && shiftmode && !enc_hist.empty() && enc_hist.back().raw.size() == rawsize) {
        /* How far the picture has moved since the frame before, starting from how far that one had moved. */
        const EncFrame &prev = enc_hist.back();
        for (unsigned k=0;k < prev.nstep;k++) enc_step_vec[k] = prev.step[k];
        enc_step_n = RP_FindShift(enc_raw.data(),prev.raw.data(),geom,enc_step_vec,prev.nstep);
        if (base == &prev) {
            enc_shift_n = enc_step_n;
            for (unsigned k=0;k < enc_step_n;k++) enc_shift_vec[k] = enc_step_vec[k];
        }
        else {
            /* An older base: each part's steps added up over the frames since it (in every frame the step nearest
             * to the part's last one is taken for the same part), then looked for near there. */
            ShiftVec guess[RP_SHIFT_MAX];
            for (unsigned q=0;q < enc_step_n;q++) {
                ShiftVec total = enc_step_vec[q], last = enc_step_vec[q];
                for (size_t i=enc_hist.size();i-- > 0;) {
                    const EncFrame &f = enc_hist[i];
                    if ((int32_t)(f.id - base->id) <= 0) break;
                    if (f.nstep == 0) continue;     /* stood still, or unknown */
                    unsigned nearest = 0;
                    int least = 0x7fffffff;     /* ("far" and "near" are macros in the Windows headers) */
                    for (unsigned k=0;k < f.nstep;k++) {
                        const int d = abs(f.step[k].dx - last.dx) + abs(f.step[k].dy - last.dy);
                        if (d < least) { least = d; nearest = k; }
                    }
                    last = f.step[nearest];
                    total.dx += last.dx; total.dy += last.dy;
                }
                guess[q] = total;
            }
            enc_shift_n = RP_FollowShift(enc_raw.data(),base->raw.data(),geom,guess,enc_step_n,enc_shift_vec);
            /* nothing there (or the picture stood still since the frame before): look near, as from one frame to the next */
            if (enc_shift_n == 0) enc_shift_n = RP_FindShift(enc_raw.data(),base->raw.data(),geom,enc_shift_vec,0);
        }
        shifted = enc_shift_n != 0 && RP_ShiftBlocks(enc_raw.data(),base->raw.data(),geom,enc_shift_vec,enc_shift_n,enc_shiftbits);
    }
    if (!key && !shifted && !limited && enc_plain_streak >= 3 && (id & 3u) != 0) key = turned_key = true;

    const uint8_t *src = enc_raw.data();
    if (!key) {
        enc_work.resize(rawsize);
        const uint8_t *a = enc_raw.data(), *b = base->raw.data();
        uint8_t *d = enc_work.data();
        if (shifted) {
            RP_ShiftXor(d,a,b,geom,enc_shift_vec,enc_shift_n,enc_shiftbits.data());
            static const bool check = getenv("NN_RP_SHIFTCHECK") != NULL;
            if (check) {
                enc_shiftcheck.resize(rawsize);
                RP_ShiftXor(enc_shiftcheck.data(),d,b,geom,enc_shift_vec,enc_shift_n,enc_shiftbits.data());
                if (memcmp(enc_shiftcheck.data(),a,rawsize) != 0) LOG_MSG("INPUTNET: scroll prediction does not give the frame back");
            }
        }
        else {
            size_t i = 0;
            for (;i + 8 <= rawsize;i += 8) {
                uint64_t x, y;
                memcpy(&x,a + i,8); memcpy(&y,b + i,8);
                x ^= y;
                memcpy(d + i,&x,8);
            }
            for (;i < rawsize;i++) d[i] = a[i] ^ b[i];
        }
        src = enc_work.data();
    }

    size_t clen = 0;
    bool ok = RP_Compress(src,rawsize,enc_comp,RP_FRAME_HDR,clen,enc_pieces);
    /* Much motion (turning in a 3D game): the frame alone often compresses better than its difference. Such a frame
     * goes out as a key frame, which every viewer decodes, whatever it has. */
    if (ok && !key) {
        bool alone = false;
        if (clen > rawsize / 16) {
            size_t ilen = 0;
            if (RP_Compress(enc_raw.data(),rawsize,enc_alt,0,ilen,enc_altpieces) && ilen < clen) {
                enc_comp.resize(RP_FRAME_HDR + ilen);
                memcpy(enc_comp.data() + RP_FRAME_HDR,enc_alt.data(),ilen);
                clen = ilen;
                enc_pieces.swap(enc_altpieces);
                key = turned_key = alone = true;
                shifted = false;
            }
        }
        enc_plain_streak = alone ? enc_plain_streak + 1 : 0;
    }
    /* In front of the compressed frame: the shift table of a frame with scroll prediction, then the piece table, for
     * viewers that inflate the pieces at the same time. */
    const size_t sbytes = (ok && shifted) ? 1 + 2 * enc_shift_n + 3 + enc_shiftbits.size() : 0;
    const bool pieces_table = ok && table && enc_pieces.size() >= 2 && enc_pieces.size() <= RP_PIECES_MAX;
    const size_t ptbytes = pieces_table ? 1 + 4 * enc_pieces.size() : 0;
    const size_t tbytes = sbytes + ptbytes;
    if (tbytes) {
        enc_comp.resize(RP_FRAME_HDR + tbytes + clen);
        uint8_t *t = enc_comp.data() + RP_FRAME_HDR;
        memmove(t + tbytes,t,clen);
        if (sbytes) {
            *t++ = (uint8_t)enc_shift_n;
            for (unsigned k=0;k < enc_shift_n;k++) {
                *t++ = (uint8_t)(int8_t)enc_shift_vec[k].dx;
                *t++ = (uint8_t)(int8_t)enc_shift_vec[k].dy;
            }
            *t++ = (uint8_t)RP_SHIFT_BLOCK;
            *t++ = (uint8_t)enc_shiftbits.size(); *t++ = (uint8_t)(enc_shiftbits.size() >> 8);
            memcpy(t,enc_shiftbits.data(),enc_shiftbits.size());
            t += enc_shiftbits.size();
        }
        if (ptbytes) {
            *t++ = (uint8_t)enc_pieces.size();
            for (size_t k=0;k < enc_pieces.size();k++) {
                const uint32_t v = enc_pieces[k];
                *t++ = (uint8_t)v; *t++ = (uint8_t)(v >> 8); *t++ = (uint8_t)(v >> 16); *t++ = (uint8_t)(v >> 24);
            }
        }
    }
    const size_t total = RP_FRAME_HDR + tbytes + clen;
    const size_t count = (total + RP_FRAG - 1) / RP_FRAG;
    if (!ok || count > 65535) {
        Host_Lock();
        enc_needkey = true;     /* nothing went out: start again from a key frame */
        Host_Unlock();
        return;
    }
    uint8_t *h = enc_comp.data();
    const unsigned back = (!key && refs) ? (unsigned)(id - base->id) : 0;
    h[0] = (uint8_t)width; h[1] = (uint8_t)(width >> 8);
    h[2] = (uint8_t)height; h[3] = (uint8_t)(height >> 8);
    h[4] = (uint8_t)fmt; h[5] = (uint8_t)((key ? 1 : 0) | (ptbytes ? 2 : 0) | (mouse_notify_mode != 0 ? 4 : 0) | (sbytes ? 8 : 0));
    h[6] = (uint8_t)back; h[7] = (uint8_t)(back >> 8);
    h[8] = (uint8_t)rawsize; h[9] = (uint8_t)(rawsize >> 8); h[10] = (uint8_t)(rawsize >> 16); h[11] = (uint8_t)(rawsize >> 24);

    std::vector<std::vector<uint8_t> > frags;
    frags.reserve(count);
    size_t fragbytes = 0;
    for (size_t i=0;i < count;i++) {
        const size_t off = i * RP_FRAG;
        const size_t n = std::min(RP_FRAG,total - off);
        Writer w(RP_VIDEO);
        w.u32(id);
        w.u16((uint16_t)i);
        w.u16((uint16_t)count);
        w.bytes(enc_comp.data() + off,n);
        frags.push_back(std::vector<uint8_t>(w.buf,w.buf + w.len));
        fragbytes += w.len;
    }

    /* Keep it as a base for later frames. */
    if (key) {
        enc_key.id = id;
        enc_key.raw = enc_raw;
        enc_haskey = true;
    }
    EncFrame f;
    f.id = id;
    f.raw.swap(enc_raw);
    f.nstep = enc_step_n;
    for (unsigned k=0;k < enc_step_n;k++) f.step[k] = enc_step_vec[k];
    enc_hist.push_back(std::move(f));
    while (enc_hist.size() > 1 && (enc_hist.size() > RP_HISTORY || enc_hist.size() * rawsize > RP_HISTORY_BYTES))
        enc_hist.pop_front();

    const uint64_t encoded_us = RP_Micros();
    Host_Lock();
    enc_frameid = id;
    for (size_t i=0;i < frags.size();i++) send_queue.push_back(std::move(frags[i]));
    send_queued += fragbytes;
    stat_frames++;
    if (key) stat_keys++;
    if (sbytes) stat_shifted++;
    if (turned_key) { enc_lastkey = SDL_GetTicks(); enc_keyreq = false; }
    stat_enc_us += encoded_us - std::min(encoded_us,c.taken_us);
    /* Ahead of the frame itself, so viewers that measure latency know which of their input it shows: what had been
     * applied when the picture was taken, not what has come in since. */
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
        const Viewer &v = viewers[i];
        if (!v.active || !(v.features & RP_FEAT_TIMING) || v.session != c.session[i] || c.applied[i] == 0) continue;
        Writer w(RP_FRAMEINFO);
        w.u32(id);
        w.u32(c.applied[i]);
        w.u32(RP_ClampU32(c.taken_us - std::min(c.taken_us,c.applied_us[i])));
        w.u32(RP_ClampU32(encoded_us - std::min(encoded_us,c.taken_us)));
        Host_SendTo(v.addr,w.buf,w.len);
    }
    Host_Unlock();

    Host_FlushQueue(true);
}

int Enc_Thread(void *) {
    for (;;) {
        SDL_SemWait(enc_go);
        if (enc_stop) break;
        Host_Lock();
        const int slot = cap_waiting;
        cap_waiting = -1;
        cap_encoding = slot;
        Host_Unlock();
        if (slot < 0) continue;     /* a newer picture replaced that one and was taken in its place */
        Enc_Frame(caps[slot]);
        Host_Lock();
        cap_encoding = -1;
        Host_Unlock();
    }
    return 0;
}

void Enc_Start(void) {
    cap_last = cap_waiting = cap_encoding = -1;
    if (enc_thread != NULL || host_lock == NULL || RP_CpuCount() < 2) return;
    enc_stop = 0;
    enc_go = SDL_CreateSemaphore(0);
    if (enc_go == NULL) return;
#if SDL_VERSION_ATLEAST(2,0,0)
    enc_thread = SDL_CreateThread(Enc_Thread,"INPUTNET video",NULL);
#else
    enc_thread = SDL_CreateThread(Enc_Thread,NULL);
#endif
    if (enc_thread == NULL) {
        SDL_DestroySemaphore(enc_go);
        enc_go = NULL;
    }
}

void Enc_Stop(void) {
    if (enc_thread != NULL) {
        enc_stop = 1;
        SDL_SemPost(enc_go);
        SDL_WaitThread(enc_thread,NULL);
        enc_thread = NULL;
        SDL_DestroySemaphore(enc_go);
        enc_go = NULL;
    }
    cap_last = cap_waiting = cap_encoding = -1;
}

} /* namespace */

void REMOTEPLAY_AddFrame(unsigned width,unsigned height,unsigned bpp,unsigned pitch,
    const uint8_t *data,const uint8_t *pal) {
    if (!host_running || !remoteplay_streaming || data == NULL || TTF_using()) return;

    const unsigned bytespp = RP_BytesPerPixel(bpp);
    if (bytespp == 0 || width == 0 || height == 0 || width > 4096 || height > 4096) return;
    const size_t rowbytes = (size_t)width * bytespp;
    if (rowbytes > pitch) return;

    const uint32_t now = SDL_GetTicks();
    const uint64_t taken_us = RP_Micros();  /* the frame is complete: input applied before now is in it */
    uint32_t v_session[RP_MAX_VIEWERS], v_applied[RP_MAX_VIEWERS];
    uint64_t v_applied_us[RP_MAX_VIEWERS];

    Host_Lock();
    if (cfg_maxfps != 0 && !enc_needkey && (now - enc_lastsend) * cfg_maxfps < 1000u) {
        Host_Unlock();
        return;
    }
    stat_offered++;

    /* Frames come faster than the link takes them: skip this one. With named bases the next frame is encoded against
     * what the viewers have; otherwise against the last frame sent, so skipping costs nothing either way. */
    if (!send_queue.empty() && (double)send_queued > std::max(send_rate * 0.06,64.0 * 1024.0)) {
        stat_skipped++;
        if (send_queued > ((size_t)16 << 20)) {     /* nobody takes anything: start over */
            send_queue.clear();
            send_queued = 0;
            enc_needkey = true;
        }
        Host_Unlock();
        Host_FlushQueue(false);
        return;
    }
    const bool maybe_same = enc_haveprev && !enc_needkey && !enc_keyreq && (now - enc_lastsend) < 250;
    const int last = cap_last;
    int slot = 0;
    while (slot == cap_waiting || slot == cap_encoding) slot++;
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
        const Viewer &v = viewers[i];
        v_session[i] = v.active ? v.session : 0;
        v_applied[i] = v.active ? v.applied : 0;
        v_applied_us[i] = v.applied_us;
    }
    Host_Unlock();

    /* The same 32-bit picture as the last one: nothing to index or send. That is most refreshes while a game draws fewer
     * frames than the display shows (Shadow Warrior at 800x600). */
    if (bpp == 32 && maybe_same && last >= 0 && caps[last].w == width && caps[last].h == height && caps[last].bpp == bpp) {
        const uint8_t *prev = caps[last].px.data();
        bool same = true;
        for (unsigned y=0;y < height && same;y++)
            same = memcmp(prev + (size_t)y * rowbytes,data + (size_t)y * pitch,rowbytes) == 0;
        if (same) {
            const uint64_t spent = RP_Micros() - taken_us;
            Host_Lock();
            stat_cap_us += spent;
            Host_Unlock();
            return;
        }
    }

    /* This thread only takes the picture; the rest is the encoder's. */
    Capture &c = caps[slot];
    c.w = width; c.h = height; c.bpp = bpp;
    c.taken_us = taken_us;
    c.px.resize(rowbytes * height);
    if (pitch == rowbytes) memcpy(c.px.data(),data,rowbytes * height);
    else for (unsigned y=0;y < height;y++) memcpy(c.px.data() + (size_t)y * rowbytes,data + (size_t)y * pitch,rowbytes);
    if (bpp == 8) {
        if (pal != NULL) memcpy(c.pal,pal,sizeof(c.pal));
        else memset(c.pal,0,sizeof(c.pal));
    }
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) {
        c.session[i] = v_session[i];
        c.applied[i] = v_applied[i];
        c.applied_us[i] = v_applied_us[i];
    }

    Host_Lock();
    cap_last = slot;
    const bool threaded = enc_thread != NULL;
    if (threaded) {
        if (cap_waiting >= 0) stat_replaced++;  /* the encoder had not started on the one before */
        cap_waiting = slot;
    }
    Host_Unlock();
    if (threaded) SDL_SemPost(enc_go);
    else Enc_Frame(c);

    const uint64_t spent = RP_Micros() - taken_us;
    Host_Lock();
    stat_cap_us += spent;
    Host_Unlock();
}

void REMOTEPLAY_AddAudio(unsigned rate,unsigned frames,const int16_t *samples) {
    if (!host_running || !remoteplay_streaming || frames == 0) return;
    if (rate != aud_rate) {
        aud_rate = rate;
        aud_buf.clear();
    }
    aud_buf.insert(aud_buf.end(),samples,samples + (size_t)frames * 2);
    size_t at = 0;
    while (aud_buf.size() - at >= (size_t)RP_AUDIO_FRAMES * 2) {
        Writer w(RP_AUDIO);
        w.u32(++aud_seq);
        w.u32(aud_rate);
        w.u16((uint16_t)RP_AUDIO_FRAMES);
        for (unsigned i=0;i < RP_AUDIO_FRAMES * 2;i++) w.u16((uint16_t)aud_buf[at + i]);
        Host_SendAll(w.buf,w.len);
        at += (size_t)RP_AUDIO_FRAMES * 2;
    }
    if (at) aud_buf.erase(aud_buf.begin(),aud_buf.begin() + (std::ptrdiff_t)at);
}

/* ------------------------------------------------------------------------- */
/* Viewer (needs the SDL2 build: its own window, renderer and audio queue)    */
/* ------------------------------------------------------------------------- */

#if defined(C_SDL2)
namespace {

KBD_KEYS RP_MapScancode(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_1: return KBD_1; case SDL_SCANCODE_2: return KBD_2; case SDL_SCANCODE_3: return KBD_3;
        case SDL_SCANCODE_4: return KBD_4; case SDL_SCANCODE_5: return KBD_5; case SDL_SCANCODE_6: return KBD_6;
        case SDL_SCANCODE_7: return KBD_7; case SDL_SCANCODE_8: return KBD_8; case SDL_SCANCODE_9: return KBD_9;
        case SDL_SCANCODE_0: return KBD_0;
        case SDL_SCANCODE_Q: return KBD_q; case SDL_SCANCODE_W: return KBD_w; case SDL_SCANCODE_E: return KBD_e;
        case SDL_SCANCODE_R: return KBD_r; case SDL_SCANCODE_T: return KBD_t; case SDL_SCANCODE_Y: return KBD_y;
        case SDL_SCANCODE_U: return KBD_u; case SDL_SCANCODE_I: return KBD_i; case SDL_SCANCODE_O: return KBD_o;
        case SDL_SCANCODE_P: return KBD_p;
        case SDL_SCANCODE_A: return KBD_a; case SDL_SCANCODE_S: return KBD_s; case SDL_SCANCODE_D: return KBD_d;
        case SDL_SCANCODE_F: return KBD_f; case SDL_SCANCODE_G: return KBD_g; case SDL_SCANCODE_H: return KBD_h;
        case SDL_SCANCODE_J: return KBD_j; case SDL_SCANCODE_K: return KBD_k; case SDL_SCANCODE_L: return KBD_l;
        case SDL_SCANCODE_Z: return KBD_z; case SDL_SCANCODE_X: return KBD_x; case SDL_SCANCODE_C: return KBD_c;
        case SDL_SCANCODE_V: return KBD_v; case SDL_SCANCODE_B: return KBD_b; case SDL_SCANCODE_N: return KBD_n;
        case SDL_SCANCODE_M: return KBD_m;
        case SDL_SCANCODE_F1: return KBD_f1; case SDL_SCANCODE_F2: return KBD_f2; case SDL_SCANCODE_F3: return KBD_f3;
        case SDL_SCANCODE_F4: return KBD_f4; case SDL_SCANCODE_F5: return KBD_f5; case SDL_SCANCODE_F6: return KBD_f6;
        case SDL_SCANCODE_F7: return KBD_f7; case SDL_SCANCODE_F8: return KBD_f8; case SDL_SCANCODE_F9: return KBD_f9;
        case SDL_SCANCODE_F10: return KBD_f10; case SDL_SCANCODE_F11: return KBD_f11; case SDL_SCANCODE_F12: return KBD_f12;
        case SDL_SCANCODE_ESCAPE: return KBD_esc; case SDL_SCANCODE_TAB: return KBD_tab;
        case SDL_SCANCODE_BACKSPACE: return KBD_backspace; case SDL_SCANCODE_RETURN: return KBD_enter;
        case SDL_SCANCODE_SPACE: return KBD_space;
        case SDL_SCANCODE_LALT: return KBD_leftalt; case SDL_SCANCODE_RALT: return KBD_rightalt;
        case SDL_SCANCODE_LCTRL: return KBD_leftctrl; case SDL_SCANCODE_RCTRL: return KBD_rightctrl;
        case SDL_SCANCODE_LSHIFT: return KBD_leftshift; case SDL_SCANCODE_RSHIFT: return KBD_rightshift;
        case SDL_SCANCODE_CAPSLOCK: return KBD_capslock; case SDL_SCANCODE_SCROLLLOCK: return KBD_scrolllock;
        case SDL_SCANCODE_NUMLOCKCLEAR: return KBD_numlock;
        case SDL_SCANCODE_GRAVE: return KBD_grave; case SDL_SCANCODE_MINUS: return KBD_minus;
        case SDL_SCANCODE_EQUALS: return KBD_equals; case SDL_SCANCODE_BACKSLASH: return KBD_backslash;
        case SDL_SCANCODE_LEFTBRACKET: return KBD_leftbracket; case SDL_SCANCODE_RIGHTBRACKET: return KBD_rightbracket;
        case SDL_SCANCODE_SEMICOLON: return KBD_semicolon; case SDL_SCANCODE_APOSTROPHE: return KBD_quote;
        case SDL_SCANCODE_PERIOD: return KBD_period; case SDL_SCANCODE_COMMA: return KBD_comma;
        case SDL_SCANCODE_SLASH: return KBD_slash; case SDL_SCANCODE_NONUSBACKSLASH: return KBD_extra_lt_gt;
        case SDL_SCANCODE_PRINTSCREEN: return KBD_printscreen; case SDL_SCANCODE_PAUSE: return KBD_pause;
        case SDL_SCANCODE_INSERT: return KBD_insert; case SDL_SCANCODE_HOME: return KBD_home;
        case SDL_SCANCODE_PAGEUP: return KBD_pageup; case SDL_SCANCODE_DELETE: return KBD_delete;
        case SDL_SCANCODE_END: return KBD_end; case SDL_SCANCODE_PAGEDOWN: return KBD_pagedown;
        case SDL_SCANCODE_LEFT: return KBD_left; case SDL_SCANCODE_UP: return KBD_up;
        case SDL_SCANCODE_DOWN: return KBD_down; case SDL_SCANCODE_RIGHT: return KBD_right;
        case SDL_SCANCODE_KP_1: return KBD_kp1; case SDL_SCANCODE_KP_2: return KBD_kp2; case SDL_SCANCODE_KP_3: return KBD_kp3;
        case SDL_SCANCODE_KP_4: return KBD_kp4; case SDL_SCANCODE_KP_5: return KBD_kp5; case SDL_SCANCODE_KP_6: return KBD_kp6;
        case SDL_SCANCODE_KP_7: return KBD_kp7; case SDL_SCANCODE_KP_8: return KBD_kp8; case SDL_SCANCODE_KP_9: return KBD_kp9;
        case SDL_SCANCODE_KP_0: return KBD_kp0;
        case SDL_SCANCODE_KP_DIVIDE: return KBD_kpdivide; case SDL_SCANCODE_KP_MULTIPLY: return KBD_kpmultiply;
        case SDL_SCANCODE_KP_MINUS: return KBD_kpminus; case SDL_SCANCODE_KP_PLUS: return KBD_kpplus;
        case SDL_SCANCODE_KP_ENTER: return KBD_kpenter; case SDL_SCANCODE_KP_PERIOD: return KBD_kpperiod;
        case SDL_SCANCODE_LGUI: return KBD_lwindows; case SDL_SCANCODE_RGUI: return KBD_rwindows;
        case SDL_SCANCODE_APPLICATION: return KBD_rwinmenu; case SDL_SCANCODE_KP_EQUALS: return KBD_kpequals;
        default: return KBD_NONE;
    }
}

struct Assembly {
    bool used = false;
    uint32_t id = 0;
    unsigned count = 0, got = 0;
    size_t total = 0;
    std::vector<uint8_t> data;
    std::vector<uint8_t> have;
};

struct PendingEvent {
    uint32_t seq;
    std::vector<uint8_t> bytes;
};

/* Latency measurement: when each input event was made and first sent, and what the host said about each frame. */
struct InputTime { uint32_t seq = 0; uint64_t made = 0, sent = 0; };
struct FrameInfo { uint32_t id = 0, seq = 0, wait_us = 0, enc_us = 0; uint64_t at = 0; };
struct LatencySums {
    unsigned n = 0, rtt_n = 0;
    double total = 0, most = 0, local = 0, net = 0, wait = 0, enc = 0, xfer = 0, draw = 0, rtt = 0;
};

class ViewerSession {
public:
    std::string target;
    IPaddress server = { 0, 0 };
    UDPsocket sock = NULL;
    UDPpacket *in = NULL, *out = NULL;
    SDLNet_SocketSet set = NULL;
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    unsigned tex_w = 0, tex_h = 0;
    std::vector<uint32_t> argb;

    bool running = true, welcomed = false, fullscreen = false, captured = false;
    std::string error;
    uint32_t lastheard = 0, lasthello = 0, started = 0;
    uint32_t session = 0;
    unsigned timeout = 10000;

    /* video */
    Assembly asm_slots[4];
    std::vector<uint8_t> frame, scratch;
    std::vector<uint8_t> shiftbase;     /* the base of a frame with scroll prediction, copied aside */
    uint32_t frames_decoded = 0, frames_shifted = 0, pic_log = 0;   /* since the last picture report */
    uint64_t bytes_decoded = 0;
    unsigned frame_w = 0, frame_h = 0, frame_fmt = 0;
    uint32_t lastid = 0;
    bool haveframe = false, needkey = true, redraw = true;
    uint32_t lastkeyreq = 0;
    std::deque<std::pair<uint32_t,std::vector<uint8_t> > > hist;  /* recent decoded frames: bases the host may name */
    uint32_t keyid = 0;
    std::vector<uint8_t> keyframe;      /* the last key frame */
    bool havekey = false;
    uint32_t frags_exp = 0, frags_got = 0, lastack = 0;

    /* sound */
    SDL_AudioDeviceID audio = 0;
    unsigned audio_rate = 0;
    uint32_t audio_seq = 0;
    bool audio_playing = false;

    /* input */
    std::deque<PendingEvent> pending;
    uint32_t nextseq = 1;
    uint32_t lastinput = 0, lastmove = 0;
    bool input_dirty = false;
    float acc_xrel = 0, acc_yrel = 0, last_x = 0, last_y = 0;
    bool abs_pointer = false;   /* the host's guest takes the pointer's place in the picture: no capture needed */
    bool motion_dirty = false;  /* the place changed (or has to be sent again) without relative motion */
    SDL_Cursor *dot_cursor = NULL;  /* this window's pointer over such a picture: a small dot, see pointerLook() */
    bool dot_shown = false;
    bool keydown[KBD_LAST];
    uint32_t buttons = 0;

    /* latency (Ctrl+F11 shows it in the title; the log gets it every 5 seconds) */
    InputTime times[1024];
    FrameInfo infos[32];
    uint64_t motion_made = 0, frame_done = 0;
    uint32_t lat_seq = 0, lat_ack = 0, lat_log = 0;
    bool newframe = false, show_latency = false;
    unsigned pieces_logged = 0;
    LatencySums lat;
    std::string latency_text;

    ViewerSession() { memset(keydown,0,sizeof(keydown)); }

    void send(const Writer &w) {
        if (!w.ok || sock == NULL || w.len > (size_t)out->maxlen) return;
        memcpy(out->data,w.buf,w.len);
        out->len = (int)w.len;
        out->address = server;
        SDLNet_UDP_Send(sock,-1,out);
    }

    void sendHello(void) {
        Writer w(RP_HELLO);
        const char *user = getenv("USERNAME");
        if (user == NULL) user = getenv("USER");
        w.str(user ? user : "player");
        w.u32(session);
        w.u32(RP_FEAT_REFS | RP_FEAT_TIMING | RP_FEAT_PIECES | RP_FEAT_SHIFT);
        send(w);
        lasthello = SDL_GetTicks();
    }

    /* The newest decoded frame and the fragment counts, so the host picks bases we have and paces to the link. */
    void sendAck(void) {
        Writer w(RP_FRAMEACK);
        w.u32(session);
        w.u8(haveframe ? 1 : 0);
        w.u32(lastid);
        w.u32(frags_exp);
        w.u32(frags_got);
        send(w);
        lastack = SDL_GetTicks();
    }

    void updateTitle(void) {
        std::string t = "DOSBox-X-NN remote play"; /* the host's address stays private */
        if (!welcomed) t += " (connecting...)";
        else if (captured) t += " - Ctrl+F10 releases the mouse";
        else if (abs_pointer) t += " - Ctrl+F10 captures the mouse";
        if (show_latency && welcomed) t += " - " + (latency_text.empty() ? std::string("measuring latency...") : latency_text);
        SDL_SetWindowTitle(window,t.c_str());
    }

    /* ---- input ---- */

    void pushEvent(const uint8_t *bytes,size_t n,uint64_t made = 0) {
        PendingEvent ev;
        ev.seq = nextseq++;
        ev.bytes.assign(bytes,bytes + n);
        pending.push_back(ev);
        input_dirty = true;
        InputTime &t = times[ev.seq & 1023u];
        t.seq = ev.seq;
        t.made = made ? made : RP_Micros();
        t.sent = 0;
    }

    void keyEvent(KBD_KEYS k,bool pressed) {
        if (k == KBD_NONE || keydown[k] == pressed) return;
        keydown[k] = pressed;
        uint8_t b[4] = { RPEV_KEY, (uint8_t)k, (uint8_t)((unsigned)k >> 8), (uint8_t)(pressed ? 1 : 0) };
        pushEvent(b,4);
    }

    void buttonEvent(uint8_t button,bool pressed) {
        const bool down = (buttons & (1u << button)) != 0;
        if (down == pressed) return;
        if (pressed) buttons |= 1u << button; else buttons &= ~(1u << button);
        uint8_t b[3] = { RPEV_MBUTTON, button, (uint8_t)(pressed ? 1 : 0) };
        pushEvent(b,3);
    }

    void flushMotion(void) {
        if (acc_xrel == 0 && acc_yrel == 0 && !motion_dirty) return;
        motion_dirty = false;
        Writer tmp(0);
        const size_t start = tmp.len;
        tmp.u8(RPEV_MMOVE);
        tmp.f32(acc_xrel); tmp.f32(acc_yrel); tmp.f32(last_x); tmp.f32(last_y);
        tmp.u8(captured ? 1 : 0);
        pushEvent(tmp.buf + start,tmp.len - start,motion_made);
        motion_made = 0;
        acc_xrel = acc_yrel = 0;
        lastmove = SDL_GetTicks();
    }

    void releaseAll(void) {
        flushMotion();
        for (unsigned k=1;k < (unsigned)KBD_LAST;k++)
            if (keydown[k]) keyEvent((KBD_KEYS)k,false);
        for (uint8_t b=0;b < 32;b++)
            if (buttons & (1u << b)) buttonEvent(b,false);
    }

    void sendInput(void) {
        if (pending.empty() || !welcomed) return;
        Writer w(RP_INPUT);
        w.u32(session);
        w.u32(pending.front().seq);
        const size_t countpos = w.len;
        w.u8(0);
        unsigned n = 0;
        for (size_t i=0;i < pending.size() && n < RP_MAX_EVENTS;i++) {
            if (w.len + pending[i].bytes.size() > RP_PACKET_MAX) break;
            w.bytes(pending[i].bytes.data(),pending[i].bytes.size());
            n++;
        }
        w.buf[countpos] = (uint8_t)n;
        send(w);
        const uint64_t sent = RP_Micros();
        for (unsigned i=0;i < n;i++) {
            InputTime &t = times[pending[i].seq & 1023u];
            if (t.seq == pending[i].seq && t.sent == 0) t.sent = sent;
        }
        lastinput = SDL_GetTicks();
        input_dirty = false;
    }

    void setCapture(bool on) {
        if (captured == on) return;
        if (!on) releaseAll();
        captured = on;
        SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
        if (!on) { dot_shown = true; pointerLook(false); }
        updateTitle();
    }

    /* Over the picture of a guest that takes the pointer's place, this window's pointer is a small dot: it shows at
     * once where the pointer is going, and the guest's own pointer (drawn in the picture, one round trip later) settles
     * under it. Anywhere else the system's pointer. */
    void pointerLook(bool dot) {
        if (dot == dot_shown) return;
        dot_shown = dot;
        if (dot && dot_cursor == NULL) {
            /* 8x8: a white 3x3 square in a black ring; SDL: mask 1 and data 1 is black, mask 1 and data 0 white */
            static const Uint8 data[8] = { 0x00, 0x38, 0x44, 0x44, 0x44, 0x38, 0x00, 0x00 };
            static const Uint8 mask[8] = { 0x00, 0x38, 0x7C, 0x7C, 0x7C, 0x38, 0x00, 0x00 };
            dot_cursor = SDL_CreateCursor(data,mask,8,8,3,3);
        }
        SDL_SetCursor((dot && dot_cursor != NULL) ? dot_cursor : SDL_GetDefaultCursor());
        SDL_ShowCursor(SDL_ENABLE);
    }

    /* The host says whether its guest takes the pointer's place (frame flag 4). Then the pointer works without
     * capturing the mouse; Ctrl+F10 still captures it for games that turn with the mouse. */
    void setAbsPointer(bool on) {
        if (abs_pointer == on) return;
        abs_pointer = on;
        if (on) setCapture(false);
        else pointerLook(false);
        LOG_MSG("INPUTNET: the pointer %s",on ? "follows this window's pointer (no capture needed)" : "needs the mouse captured again");
        updateTitle();
    }

    bool inPicture(int x,int y) {
        const SDL_Rect r = displayRect();
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    }

    void pointerAt(int x,int y) {
        const SDL_Rect r = displayRect();
        if (r.w > 1 && r.h > 1) {
            last_x = std::min(1.0f,std::max(0.0f,(float)(x - r.x) / (float)(r.w - 1)));
            last_y = std::min(1.0f,std::max(0.0f,(float)(y - r.y) / (float)(r.h - 1)));
        }
    }

    SDL_Rect displayRect(void) {
        int ww = 0, wh = 0;
        SDL_GetRendererOutputSize(renderer,&ww,&wh);
        SDL_Rect r;
        /* DOS games are drawn for a 4:3 screen whatever their resolution. */
        if ((long)ww * 3 > (long)wh * 4) { r.h = wh; r.w = wh * 4 / 3; }
        else { r.w = ww; r.h = ww * 3 / 4; }
        r.x = (ww - r.w) / 2;
        r.y = (wh - r.h) / 2;
        return r;
    }

    void handleEvent(const SDL_Event &e) {
        switch (e.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    releaseAll();
                    setCapture(false);
                }
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST || e.window.event == SDL_WINDOWEVENT_LEAVE)
                    pointerLook(false);
                redraw = true;
                break;
            case SDL_KEYDOWN: {
                if (e.key.repeat) break;
                const SDL_Keymod mod = SDL_GetModState();
                if (e.key.keysym.scancode == SDL_SCANCODE_RETURN && (mod & KMOD_ALT)) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(window,fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    redraw = true;
                    break;
                }
                if (e.key.keysym.scancode == SDL_SCANCODE_F10 && (mod & KMOD_CTRL)) {
                    setCapture(abs_pointer ? !captured : false);
                    break;
                }
                if (e.key.keysym.scancode == SDL_SCANCODE_F11 && (mod & KMOD_CTRL)) {
                    show_latency = !show_latency;
                    LOG_MSG("INPUTNET: latency in the window title %s",show_latency ? "on" : "off");
                    updateTitle();
                    break;
                }
                keyEvent(RP_MapScancode(e.key.keysym.scancode),true);
                break;
            }
            case SDL_KEYUP:
                keyEvent(RP_MapScancode(e.key.keysym.scancode),false);
                break;
            case SDL_MOUSEMOTION:
                if (captured) {
                    if (acc_xrel == 0 && acc_yrel == 0 && motion_made == 0) motion_made = RP_Micros();
                    acc_xrel += (float)e.motion.xrel;
                    acc_yrel += (float)e.motion.yrel;
                    pointerAt(e.motion.x,e.motion.y);
                }
                else if (abs_pointer) {
                    /* The guest draws its pointer where this window's pointer is (see pointerLook). While a button
                     * is held the pointer keeps counting outside the picture (dragging to an edge). */
                    const bool inside = inPicture(e.motion.x,e.motion.y);
                    pointerLook(inside);
                    if (inside || buttons != 0) {
                        if (!motion_dirty && motion_made == 0) motion_made = RP_Micros();
                        pointerAt(e.motion.x,e.motion.y);
                        motion_dirty = true;
                    }
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                const bool pressed = (e.type == SDL_MOUSEBUTTONDOWN);
                if (!captured) {
                    if (!abs_pointer) {
                        if (pressed && e.button.button == SDL_BUTTON_LEFT) setCapture(true);
                        break;
                    }
                    /* a press counts in the picture only; every button event carries the pointer's place */
                    if (pressed && !inPicture(e.button.x,e.button.y)) break;
                    pointerAt(e.button.x,e.button.y);
                    motion_dirty = true;
                }
                flushMotion();
                if (e.button.button == SDL_BUTTON_LEFT) buttonEvent(0,pressed);
                else if (e.button.button == SDL_BUTTON_RIGHT) buttonEvent(1,pressed);
                else if (e.button.button == SDL_BUTTON_MIDDLE) buttonEvent(2,pressed);
                break;
            }
            case SDL_MOUSEWHEEL:
                if ((captured || abs_pointer) && e.wheel.y != 0) {
                    const int amount = (e.wheel.direction == SDL_MOUSEWHEEL_NORMAL) ? -e.wheel.y : e.wheel.y;
                    const int16_t a = (int16_t)std::max(-32767,std::min(32767,amount));
                    uint8_t b[3] = { RPEV_WHEEL, (uint8_t)a, (uint8_t)((uint16_t)a >> 8) };
                    pushEvent(b,3);
                }
                break;
            default:
                break;
        }
    }

    /* ---- video ---- */

    void decodeFrame(uint32_t id,const uint8_t *data,size_t len) {
        if (len < RP_FRAME_HDR) return;
        const unsigned w = data[0] | (data[1] << 8);
        const unsigned h = data[2] | (data[3] << 8);
        const unsigned fmt = data[4];
        const bool key = (data[5] & 1) != 0;
        setAbsPointer((data[5] & 4) != 0);
        const size_t rawsize = (size_t)data[8] | ((size_t)data[9] << 8) | ((size_t)data[10] << 16) | ((size_t)data[11] << 24);
        const unsigned bytespp = RP_BytesPerPixel(fmt);
        if (bytespp == 0 || w == 0 || h == 0 || w > 4096 || h > 4096) return;
        if (rawsize != (fmt == 8 ? 768u : 0u) + (size_t)w * h * bytespp) return;

        /* The base: the frame "back" frames earlier (an older host always means the previous one). */
        const unsigned back = data[6] | (data[7] << 8);
        const uint32_t baseid = id - (back ? back : 1);
        const std::vector<uint8_t> *base = NULL;
        if (!key) {
            if (haveframe && baseid == lastid) base = &frame;
            else {
                for (size_t i=hist.size();i-- > 0 && base == NULL;) if (hist[i].first == baseid) base = &hist[i].second;
                if (base == NULL && havekey && keyid == baseid) base = &keyframe;
            }
            if (base == NULL || base->size() != rawsize) {
                needkey = true;
                return;
            }
        }

        /* The zlib stream, after the piece table when there is one. */
        const uint8_t *stream = data + RP_FRAME_HDR;
        size_t slen = len - RP_FRAME_HDR;
        /* The shift table of a frame with scroll prediction (see RP_ShiftXor). */
        const uint8_t *shiftbits = NULL;
        ShiftVec shiftvec[RP_SHIFT_MAX];
        unsigned shiftn = 0;
        ShiftGeom geom;
        geom.w = w; geom.h = h; geom.bpp = bytespp; geom.pal = (fmt == 8) ? 768u : 0u;
        if (data[5] & 8) {
            if (key || slen < 1) return;
            shiftn = stream[0];
            if (shiftn < 1 || shiftn > RP_SHIFT_MAX || slen < 1 + 2 * (size_t)shiftn + 3) { needkey = true; return; }
            bool fine = true;
            for (unsigned k=0;k < shiftn;k++) {
                shiftvec[k].dx = (int8_t)stream[1 + 2 * k];
                shiftvec[k].dy = (int8_t)stream[2 + 2 * k];
                if (shiftvec[k].dx < -RP_SHIFT_LIMIT || shiftvec[k].dx > RP_SHIFT_LIMIT || shiftvec[k].dy < -RP_SHIFT_LIMIT || shiftvec[k].dy > RP_SHIFT_LIMIT) fine = false;
            }
            const uint8_t *q = stream + 1 + 2 * (size_t)shiftn;
            const size_t n = (size_t)q[1] | ((size_t)q[2] << 8);
            const size_t used = 1 + 2 * (size_t)shiftn + 3 + n;
            if (!fine || q[0] != RP_SHIFT_BLOCK || n != RP_ShiftBitmapBytes(geom) || slen < used) { needkey = true; return; }
            shiftbits = q + 3;
            stream += used;
            slen -= used;
        }
        uint32_t lens[RP_PIECES_MAX];
        unsigned pieces = 0;
        if (data[5] & 2) {
            if (slen < 1) return;
            pieces = stream[0];
            if (pieces < 2 || pieces > RP_PIECES_MAX || slen < 1 + 4 * (size_t)pieces) return;
            for (unsigned k=0;k < pieces;k++) {
                const uint8_t *q = stream + 1 + 4 * (size_t)k;
                lens[k] = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
            }
            stream += 1 + 4 * (size_t)pieces;
            slen -= 1 + 4 * (size_t)pieces;
        }
        scratch.resize(rawsize);
        const bool together = pieces != 0 && RP_InflatePieces(stream,slen,lens,pieces,scratch.data(),rawsize);
        if (together && pieces != pieces_logged) {
            pieces_logged = pieces;
            LOG_MSG("INPUTNET: frames come compressed in %u pieces, inflated on %u threads",pieces,std::min(pieces,zworkers + 1));
        }
        if (!together) {
            uLongf outlen = (uLongf)rawsize;
            if (uncompress(scratch.data(),&outlen,stream,(uLong)slen) != Z_OK || outlen != rawsize) {
                needkey = true;
                return;
            }
        }

        if (key) {
            frame.swap(scratch);
            keyframe = frame;
            keyid = id;
            havekey = true;
        }
        else if (shiftbits != NULL) {
            shiftbase = *base;      /* the base may be the very frame that is replaced */
            frame.resize(rawsize);
            RP_ShiftXor(frame.data(),scratch.data(),shiftbase.data(),geom,shiftvec,shiftn,shiftbits);
            frames_shifted++;
        }
        else {
            if (base != &frame) frame = *base;
            uint8_t *d = frame.data();
            const uint8_t *s = scratch.data();
            for (size_t i=0;i < rawsize;i++) d[i] ^= s[i];
        }
        frames_decoded++;
        bytes_decoded += len;
        if (!hist.empty() && hist.back().second.size() != rawsize) hist.clear();
        hist.push_back(std::make_pair(id,frame));
        while (hist.size() > 1 && (hist.size() > RP_HISTORY || hist.size() * rawsize > RP_HISTORY_BYTES)) hist.pop_front();
        frame_w = w; frame_h = h; frame_fmt = fmt;
        lastid = id;
        haveframe = true;
        needkey = false;
        newframe = true;    /* drawn as soon as the packets that arrived together are handled */
    }

    /* Input to screen for the newest input event this frame is the first to show (the host says which), split into
     * where the time went. It does not include the game's own reaction (the frames a game takes to draw the result of
     * what it read), only what remote play adds. */
    void measureShown(void) {
        const uint64_t shown = RP_Micros();
        const FrameInfo &f = infos[lastid & 31u];
        if (f.id != lastid || f.seq == 0 || (int32_t)(f.seq - lat_seq) <= 0) return;
        lat_seq = f.seq;
        const InputTime &t = times[f.seq & 1023u];
        if (t.seq != f.seq || t.sent == 0 || shown < t.made) return;
        const double total = (shown - t.made) / 1000.0;
        if (total > 5000.0) return;
        const double local = (t.sent - t.made) / 1000.0;
        const double wait = f.wait_us / 1000.0, enc = f.enc_us / 1000.0;
        const double xfer = frame_done > f.at ? (frame_done - f.at) / 1000.0 : 0.0;
        const double draw = shown > frame_done ? (shown - frame_done) / 1000.0 : 0.0;
        lat.n++;
        lat.total += total;
        lat.most = std::max(lat.most,total);
        lat.local += local;
        lat.wait += wait;
        lat.enc += enc;
        lat.xfer += xfer;
        lat.draw += draw;
        lat.net += std::max(0.0,total - local - wait - enc - xfer - draw);
    }

    void reportLatency(void) {
        const uint32_t now = SDL_GetTicks();
        if (now - lat_log < 5000) return;
        if (frames_decoded != 0) {
            const double secs = pic_log ? (now - pic_log) / 1000.0 : 5.0;
            LOG_MSG("INPUTNET: picture %.1f frames/s, %u KB/s (%u KB a frame), %u%% of the frames with scroll prediction",
                frames_decoded / secs,(unsigned)(bytes_decoded / 1024.0 / secs),(unsigned)(bytes_decoded / 1024u / frames_decoded),
                (unsigned)(frames_shifted * 100u / frames_decoded));
        }
        pic_log = now;
        frames_decoded = frames_shifted = 0;
        bytes_decoded = 0;
        lat_log = now;
        const double rtt = lat.rtt_n ? lat.rtt / lat.rtt_n : -1.0;
        if (lat.n) {
            const double n = lat.n;
            LOG_MSG("INPUTNET: input to screen %.1f ms on average (at most %.1f) over %u inputs: viewer %.1f, network %.1f (ping %.1f), "
                "host waiting for the next frame %.1f, encoding %.1f, rest of the frame arriving %.1f, decoding and drawing %.1f; "
                "the game's own reaction comes on top",
                lat.total / n,lat.most,lat.n,lat.local / n,lat.net / n,rtt,lat.wait / n,lat.enc / n,lat.xfer / n,lat.draw / n);
            char tmp[96];
            snprintf(tmp,sizeof(tmp),"%.0f ms input to screen, ping %.0f ms",lat.total / n,rtt);
            latency_text = tmp;
        }
        else if (rtt >= 0) {
            char tmp[64];
            snprintf(tmp,sizeof(tmp),"ping %.0f ms",rtt);
            latency_text = tmp;
        }
        lat = LatencySums();
        if (show_latency) updateTitle();
    }

    void present(void) {
        if (haveframe) {
            if (texture == NULL || tex_w != frame_w || tex_h != frame_h) {
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,(int)frame_w,(int)frame_h);
                tex_w = frame_w; tex_h = frame_h;
            }
            /* Converted straight into the texture (one copy less), or through argb when it cannot be locked. */
            void *dst = NULL;
            int dpitch = 0;
            const bool locked = texture != NULL && SDL_LockTexture(texture,NULL,&dst,&dpitch) == 0;
            if (!locked) {
                argb.resize((size_t)frame_w * frame_h);
                dst = argb.data();
                dpitch = (int)(frame_w * 4);
            }
            const uint8_t *p = frame.data();
            uint32_t lut[256];
            if (frame_fmt == 8)
                for (unsigned i=0;i < 256;i++)
                    lut[i] = 0xFF000000u | ((uint32_t)p[i*3] << 16) | ((uint32_t)p[i*3+1] << 8) | p[i*3+2];
            for (unsigned y=0;y < frame_h;y++) {
                uint32_t *o = (uint32_t*)((uint8_t*)dst + (size_t)y * (size_t)dpitch);
                if (frame_fmt == 8) {
                    const uint8_t *s = p + 768 + (size_t)y * frame_w;
                    for (unsigned x=0;x < frame_w;x++) o[x] = lut[s[x]];
                }
                else if (frame_fmt == 15 || frame_fmt == 16) {
                    const uint8_t *s = p + (size_t)y * frame_w * 2;
                    for (unsigned x=0;x < frame_w;x++) {
                        const unsigned v = s[x*2] | (s[x*2+1] << 8);
                        unsigned r, g, b;
                        if (frame_fmt == 15) { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); }
                        else { r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); }
                        b = (b << 3) | (b >> 2);
                        o[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                }
                else {
                    const uint8_t *s = p + (size_t)y * frame_w * 4;
                    for (unsigned x=0;x < frame_w;x++) {
                        uint32_t v;
                        memcpy(&v,s + x*4,4);
                        o[x] = 0xFF000000u | (v & 0xFFFFFFu);
                    }
                }
            }
            if (locked) SDL_UnlockTexture(texture);
            else if (texture) SDL_UpdateTexture(texture,NULL,argb.data(),(int)(frame_w * 4));
        }
        SDL_SetRenderDrawColor(renderer,0,0,0,255);
        SDL_RenderClear(renderer);
        if (texture && haveframe) {
            const SDL_Rect r = displayRect();
            SDL_RenderCopy(renderer,texture,NULL,&r);
        }
        SDL_RenderPresent(renderer);
        redraw = false;
    }

    void handleVideo(Reader &r) {
        const uint32_t id = r.u32();
        const unsigned idx = r.u16();
        const unsigned count = r.u16();
        size_t n = 0;
        const uint8_t *payload = r.rest(n);
        if (!r.ok || count == 0 || idx >= count || n == 0 || n > RP_FRAG) return;
        if (haveframe && (int32_t)(id - lastid) <= 0) return;   /* older than what is shown */

        Assembly *a = NULL;
        for (unsigned i=0;i < 4;i++) if (asm_slots[i].used && asm_slots[i].id == id) a = &asm_slots[i];
        if (a == NULL) {
            /* Reuse a free slot, or the oldest one. */
            for (unsigned i=0;i < 4 && a == NULL;i++) if (!asm_slots[i].used) a = &asm_slots[i];
            if (a == NULL) {
                a = &asm_slots[0];
                for (unsigned i=1;i < 4;i++) if ((int32_t)(asm_slots[i].id - a->id) < 0) a = &asm_slots[i];
                frags_exp += a->count; frags_got += a->got;    /* given up: count what it lacked */
            }
            a->used = true;
            a->id = id;
            a->count = count;
            a->got = 0;
            a->total = 0;
            a->data.assign((size_t)count * RP_FRAG,0);
            a->have.assign(count,0);
        }
        if (a->count != count || a->have[idx]) return;
        memcpy(a->data.data() + (size_t)idx * RP_FRAG,payload,n);
        a->have[idx] = 1;
        a->got++;
        if (idx == count - 1) a->total = (size_t)idx * RP_FRAG + n;
        if (a->got < a->count) return;

        /* Complete. Older unfinished frames will never be usable now. */
        for (unsigned i=0;i < 4;i++)
            if (asm_slots[i].used && (int32_t)(asm_slots[i].id - id) <= 0 && &asm_slots[i] != a) {
                asm_slots[i].used = false;
                frags_exp += asm_slots[i].count; frags_got += asm_slots[i].got;
            }
        a->used = false;
        frags_exp += a->count; frags_got += a->got;
        frame_done = RP_Micros();
        const uint32_t before = lastid;
        const bool had = haveframe;
        decodeFrame(id,a->data.data(),a->total);
        if (haveframe && (!had || lastid != before)) sendAck();
    }

    /* ---- sound ---- */

    void handleAudio(Reader &r) {
        const uint32_t seq = r.u32();
        const unsigned rate = r.u32();
        const unsigned frames = r.u16();
        size_t n = 0;
        const uint8_t *pcm = r.rest(n);
        if (!r.ok || rate < 8000 || rate > 192000 || n != (size_t)frames * 4) return;
        if (audio_seq != 0 && (int32_t)(seq - audio_seq) <= 0) return;
        audio_seq = seq;

        if (audio == 0 || rate != audio_rate) {
            if (audio) SDL_CloseAudioDevice(audio);
            SDL_AudioSpec want, have;
            memset(&want,0,sizeof(want));
            want.freq = (int)rate;
            want.format = AUDIO_S16LSB;
            want.channels = 2;
            want.samples = 512;
            audio = SDL_OpenAudioDevice(NULL,0,&want,&have,0);
            audio_rate = rate;
            audio_playing = false;
            if (audio == 0) return;
        }
        SDL_QueueAudio(audio,pcm,(Uint32)n);
    }

    void pumpAudio(void) {
        if (audio == 0) return;
        const Uint32 queued = SDL_GetQueuedAudioSize(audio);
        const Uint32 bytes_ms = audio_rate * 4u / 1000u;
        if (queued > bytes_ms * 250u) {
            /* Too far behind the host: drop the backlog instead of lagging. */
            SDL_ClearQueuedAudio(audio);
            SDL_PauseAudioDevice(audio,1);
            audio_playing = false;
        }
        else if (!audio_playing && queued >= bytes_ms * 50u) {
            SDL_PauseAudioDevice(audio,0);
            audio_playing = true;
        }
        else if (audio_playing && queued == 0) {
            SDL_PauseAudioDevice(audio,1);
            audio_playing = false;
        }
    }

    /* ---- network ---- */

    void handlePacket(const uint8_t *data,size_t len) {
        Reader r(data,len);
        const uint8_t type = RP_Header(r);
        if (type == 0) return;
        lastheard = SDL_GetTicks();
        switch (type) {
            case RP_WELCOME:
                if (!welcomed) {
                    welcomed = true;
                    updateTitle();
                    sendInput();
                }
                break;
            case RP_REFUSE:
                error = "The host refused to let you join: " + r.str();
                running = false;
                break;
            case RP_BYE:
                error = "The host ended the session: " + r.str();
                running = false;
                break;
            case RP_VIDEO:
                handleVideo(r);
                break;
            case RP_AUDIO:
                handleAudio(r);
                break;
            case RP_FRAMEINFO: {
                FrameInfo f;
                f.id = r.u32();
                f.seq = r.u32();
                f.wait_us = r.u32();
                f.enc_us = r.u32();
                if (!r.ok) break;
                f.at = RP_Micros();
                infos[f.id & 31u] = f;
                break;
            }
            case RP_INPUTACK: {
                const uint32_t ack = r.u32();
                if (!r.ok) break;
                if ((int32_t)(ack - lat_ack) > 0) {
                    const InputTime &t = times[ack & 1023u];
                    const uint64_t now_us = RP_Micros();
                    if (t.seq == ack && t.sent != 0 && now_us - t.sent < 2000000u) {
                        lat.rtt += (now_us - t.sent) / 1000.0;
                        lat.rtt_n++;
                    }
                    lat_ack = ack;
                }
                while (!pending.empty() && (int32_t)(pending.front().seq - ack) <= 0) pending.pop_front();
                break;
            }
            default:
                break;
        }
    }

    bool open(void) {
        std::string host = target;
        unsigned port = 2130;
        Section_prop *sec = static_cast<Section_prop*>(control->GetSection("inputnet"));
        if (sec != NULL) {
            const int p = sec->Get_int("port");
            if (p > 0 && p < 65536) port = (unsigned)p;
            const int t = sec->Get_int("timeout");
            if (t >= 250 && t <= 600000) timeout = (unsigned)t;
        }
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos && host.find(':') == colon) {
            port = (unsigned)atoi(host.c_str() + colon + 1);
            host = host.substr(0,colon);
        }
        if (host.empty() || port == 0 || port > 65535) { error = "Invalid address: " + target; return false; }

        if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER) < 0) { error = std::string("SDL: ") + SDL_GetError(); return false; }
        SDL_InitSubSystem(SDL_INIT_AUDIO);  /* sound is optional */
        if (!RP_OpenNet()) { error = "Networking is not available"; return false; }
        if (SDLNet_ResolveHost(&server,host.c_str(),(Uint16)port) != 0) { error = "Unable to resolve " + host; return false; }
        sock = SDLNet_UDP_Open(0);
        in = SDLNet_AllocPacket((int)RP_PACKET_MAX);
        out = SDLNet_AllocPacket((int)RP_PACKET_MAX);
        set = SDLNet_AllocSocketSet(1);
        if (sock == NULL || in == NULL || out == NULL || set == NULL) { error = "Unable to open a UDP socket"; return false; }
        RP_WidenSocket(sock);
        SDLNet_UDP_AddSocket(set,sock);

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"nearest");
        SDL_SetHint(SDL_HINT_RENDER_VSYNC,"0");     /* each frame shows the moment it arrives, never waiting for the display */
        window = SDL_CreateWindow("DOSBox-X-NN remote play",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
            960,720,SDL_WINDOW_RESIZABLE);
        if (window == NULL) { error = std::string("Unable to open a window: ") + SDL_GetError(); return false; }
        renderer = SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED);
        if (renderer == NULL) renderer = SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE);
        if (renderer == NULL) { error = std::string("Unable to draw: ") + SDL_GetError(); return false; }
        updateTitle();
        present();
        return true;
    }

    void loop(void) {
        started = lastheard = SDL_GetTicks();
        session = (uint32_t)rand() ^ ((uint32_t)rand() << 16) ^ (uint32_t)SDL_GetPerformanceCounter() ^ (uint32_t)(uintptr_t)this;
        if (session == 0) session = 1;
        sendHello();
        while (running) {
            SDLNet_CheckSockets(set,1);
            while (SDLNet_UDP_Recv(sock,in) > 0) {
                if (RP_SameAddr(in->address,server)) handlePacket(in->data,(size_t)in->len);
                if (!running) break;
            }
            /* Only the newest of the frames that arrived together is drawn, right away. */
            if (newframe) {
                newframe = false;
                present();
                measureShown();
            }

            SDL_Event e;
            while (SDL_PollEvent(&e)) handleEvent(e);

            const uint32_t now = SDL_GetTicks();
            if (now - lasthello >= (welcomed ? 1000u : 500u)) sendHello();
            if ((acc_xrel != 0 || acc_yrel != 0 || motion_dirty) && now - lastmove >= 4) flushMotion();
            if (!pending.empty() && (input_dirty || now - lastinput >= 30)) sendInput();
            if (welcomed && now - lastack >= 100) sendAck();   /* also reports loss when no frame completes */
            if (welcomed && needkey && now - lastkeyreq >= 150) {
                Writer w(RP_KEYREQ);
                send(w);
                lastkeyreq = now;
            }
            pumpAudio();
            if (redraw) present();
            reportLatency();

            /* Until the host answers, keep trying: its DOSBox-X may still be starting.
             * The window title says "connecting..." and closing it gives up. */
            if (welcomed && now - lastheard > timeout) {
                error = "The connection to the host was lost.";
                running = false;
            }
        }
    }

    void close(void) {
        if (sock != NULL) {
            Writer w(RP_BYE);
            w.str("closed");
            send(w);
            send(w);
        }
        if (captured) SDL_SetRelativeMouseMode(SDL_FALSE);
        if (audio) SDL_CloseAudioDevice(audio);
        if (texture) SDL_DestroyTexture(texture);
        if (dot_cursor) { SDL_SetCursor(SDL_GetDefaultCursor()); SDL_FreeCursor(dot_cursor); dot_cursor = NULL; }
        if (renderer) SDL_DestroyRenderer(renderer);
        /* The viewer window goes first: on some window managers (labwc, for one) a message box for it opened behind
         * it, and the player only saw the last frame (or black) and a viewer that no longer reacted. */
        if (window && !error.empty()) {
            SDL_HideWindow(window);
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION,"DOSBox-X-NN remote play",error.c_str(),NULL);
        }
        if (window) SDL_DestroyWindow(window);
        if (set) SDLNet_FreeSocketSet(set);
        if (in) SDLNet_FreePacket(in);
        if (out) SDLNet_FreePacket(out);
        if (sock) SDLNet_UDP_Close(sock);
    }
};

} /* namespace */

int REMOTEPLAY_RunViewer(const std::string &address) {
    ViewerSession s;
    s.target = address;
    if (s.open()) s.loop();
    s.close();
    RP_ZStop();
    if (!s.error.empty()) {
        LOG_MSG("INPUTNET: %s",s.error.c_str());
        fprintf(stderr,"%s\n",s.error.c_str());
        if (s.window == NULL)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"DOSBox-X-NN remote play",s.error.c_str(),NULL);
        return 1;
    }
    return 0;
}
#else
int REMOTEPLAY_RunViewer(const std::string &) {
    LOG_MSG("INPUTNET: joining a remote play session needs the SDL2 build of DOSBox-X");
    fprintf(stderr,"Joining a remote play session needs the SDL2 build of DOSBox-X.\n");
    return 1;
}
#endif /* C_SDL2 */

bool REMOTEPLAY_ViewerRequested(std::string &address) {
    Section_prop *sec = static_cast<Section_prop*>(control->GetSection("inputnet"));
    if (sec == NULL) return false;
    if (std::string(sec->Get_string("mode")) == "lockstep") return false;
    address = control->opt_inputnet_connect;
    if (address.empty() && control->opt_inputnet_server < 0 && sec->Get_bool("inputnet"))
        address = sec->Get_string("connect");
    return !address.empty();
}


#else /* !C_INPUTNET */

void REMOTEPLAY_Configure(Section_prop *) { }
int REMOTEPLAY_PresentMode(void) { return 0; }
void REMOTEPLAY_Init(void) { }
void REMOTEPLAY_ProcessEnded(void) { }
bool REMOTEPLAY_HostRequested(void) { return false; }
bool REMOTEPLAY_HideScreenRequested(void) { return false; }
bool REMOTEPLAY_StartServer(uint16_t) { return false; }
void REMOTEPLAY_StopServer(const char *) { }
bool REMOTEPLAY_ServerRunning(void) { return false; }
void REMOTEPLAY_Status(Program *) { }
void REMOTEPLAY_Poll(void) { }
void REMOTEPLAY_AddFrame(unsigned,unsigned,unsigned,unsigned,const uint8_t *,const uint8_t *) { }
void REMOTEPLAY_AddAudio(unsigned,unsigned,const int16_t *) { }
bool REMOTEPLAY_ViewerRequested(std::string &) { return false; }
int REMOTEPLAY_RunViewer(const std::string &) { return 1; }

#endif

/* The "Show screen (remote play)" menu item only appears on remote play hosts. */
void REMOTEPLAY_UpdateMenu(void) {
    if (!mainMenu.item_exists("mapper_rpscreen")) return;
    const bool host = remoteplay_screen_hidden || REMOTEPLAY_ServerRunning() || REMOTEPLAY_HostRequested();
    mainMenu.get_item("mapper_rpscreen").hide(!host).check(!remoteplay_screen_hidden).refresh_item(mainMenu);
}

/* See remoteplay.h. */
bool NN_Lockdown(void) {
    return control != NULL && control->opt_inputnet_lockdown;
}
