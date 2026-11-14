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
#include <stdarg.h>
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

#if defined(WIN32)
# ifndef NOMINMAX
#  define NOMINMAX
# endif
# include <windows.h>
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
bool Mouse_NNPointer(float &x,float &y,float &gx1,float &gy1,float &gxs,float &gys);   /* src/ints/mouse.cpp */
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
                           taken, microseconds from applying it to taking the frame, microseconds encoding the frame,
                           and (hosts with view prediction) when it was taken: the host's clock in microseconds, its
                           low 32 bits; then u8 1 when the DOS mouse driver's pointer follows: u16 x, u16 y (its
                           place as parts of its range, 65535 = the far end) and four f32: how far one count of
                           mouse motion moves it, x and y, for a motion of one count and for a larger one */
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

const uint32_t RP_LOOPBACK = 0x7F000001u;   /* 127.0.0.1: where NukemNet listens and talks from */

/* The keys by their keymapper names: [inputnet] block keys, the input reports and NukemNet's control channel name
 * keys this way. */
const struct { const char *name; KBD_KEYS key; } rp_key_names[] = {
    {"1",KBD_1},{"2",KBD_2},{"3",KBD_3},{"4",KBD_4},{"5",KBD_5},{"6",KBD_6},{"7",KBD_7},{"8",KBD_8},{"9",KBD_9},{"0",KBD_0},
    {"q",KBD_q},{"w",KBD_w},{"e",KBD_e},{"r",KBD_r},{"t",KBD_t},{"y",KBD_y},{"u",KBD_u},{"i",KBD_i},{"o",KBD_o},{"p",KBD_p},
    {"a",KBD_a},{"s",KBD_s},{"d",KBD_d},{"f",KBD_f},{"g",KBD_g},{"h",KBD_h},{"j",KBD_j},{"k",KBD_k},{"l",KBD_l},{"z",KBD_z},
    {"x",KBD_x},{"c",KBD_c},{"v",KBD_v},{"b",KBD_b},{"n",KBD_n},{"m",KBD_m},
    {"f1",KBD_f1},{"f2",KBD_f2},{"f3",KBD_f3},{"f4",KBD_f4},{"f5",KBD_f5},{"f6",KBD_f6},{"f7",KBD_f7},{"f8",KBD_f8},
    {"f9",KBD_f9},{"f10",KBD_f10},{"f11",KBD_f11},{"f12",KBD_f12},
    {"esc",KBD_esc},{"tab",KBD_tab},{"bspace",KBD_backspace},{"enter",KBD_enter},{"space",KBD_space},
    {"lalt",KBD_leftalt},{"ralt",KBD_rightalt},{"lctrl",KBD_leftctrl},{"rctrl",KBD_rightctrl},
    {"lshift",KBD_leftshift},{"rshift",KBD_rightshift},{"capslock",KBD_capslock},{"scrolllock",KBD_scrolllock},{"numlock",KBD_numlock},
    {"grave",KBD_grave},{"minus",KBD_minus},{"equals",KBD_equals},{"backslash",KBD_backslash},
    {"lbracket",KBD_leftbracket},{"rbracket",KBD_rightbracket},{"semicolon",KBD_semicolon},{"quote",KBD_quote},
    {"period",KBD_period},{"comma",KBD_comma},{"slash",KBD_slash},{"lessthan",KBD_extra_lt_gt},
    {"pause",KBD_pause},{"insert",KBD_insert},{"home",KBD_home},{"pageup",KBD_pageup},
    {"delete",KBD_delete},{"end",KBD_end},{"pagedown",KBD_pagedown},
    {"left",KBD_left},{"up",KBD_up},{"down",KBD_down},{"right",KBD_right},
    {"kp_1",KBD_kp1},{"kp_2",KBD_kp2},{"kp_3",KBD_kp3},{"kp_4",KBD_kp4},{"kp_5",KBD_kp5},{"kp_6",KBD_kp6},{"kp_7",KBD_kp7},
    {"kp_8",KBD_kp8},{"kp_9",KBD_kp9},{"kp_0",KBD_kp0},{"kp_divide",KBD_kpdivide},{"kp_multiply",KBD_kpmultiply},
    {"kp_minus",KBD_kpminus},{"kp_plus",KBD_kpplus},{"kp_enter",KBD_kpenter},{"kp_period",KBD_kpperiod},
    {"lwindows",KBD_lwindows},{"rwindows",KBD_rwindows},{"rwinmenu",KBD_rwinmenu},
};

const char *RP_KeyName(unsigned key) {
    for (const auto &e : rp_key_names) if ((unsigned)e.key == key) return e.name;
    return "unknown";
}

bool RP_KeyByName(const std::string &name,KBD_KEYS &key) {
    for (const auto &e : rp_key_names) if (name == e.name) { key = e.key; return true; }
    return false;
}

/* The words of a text, in lower case. */
std::vector<std::string> RP_Words(const std::string &text,const char *separators) {
    std::vector<std::string> words;
    size_t pos = 0;
    while ((pos = text.find_first_not_of(separators,pos)) != std::string::npos) {
        const size_t end = std::min(text.find_first_of(separators,pos),text.size());
        words.push_back(text.substr(pos,end - pos));
        for (char &c : words.back()) c = (char)tolower((unsigned char)c);
        pos = end;
    }
    return words;
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
    float report_dx = 0, report_dy = 0; /* [inputnet] report input: mouse motion not reported yet */
    uint32_t report_ms = 0;
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
bool cfg_blocked[KBD_LAST];     /* [inputnet] block keys: dropped from every remote player (see RP_SetBlockedKeys) */
IPaddress cfg_report_to = { 0, 0 }; /* [inputnet] report input: 127.0.0.1:<port> hears every player's input (port 0: off) */
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
float enc_same = -1.0f;                         /* the part of the frame (in groups of 8 bytes) that is as in its base; -1: not known */
unsigned enc_noscroll = 0;                      /* frames looked at in a row in which nothing had scrolled */
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
    bool ptr = false;                   /* where the mouse driver had its pointer when the picture was taken... */
    float ptr_v[6];                     /* ...and how far a count moves it (Mouse_NNPointer) */
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
const size_t RP_ZPARALLEL_MIN = (size_t)48 << 10;    /* smaller frames are not worth splitting (a 320x200 picture is 64 KB,
                                                         or 128 KB with every line twice) */
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
/* where the encoder's time goes: waiting for its thread to start on the picture, looking for a scroll, the difference,
 * compressing it, compressing the frame alone as well */
uint64_t stat_part_us[5] = { 0, 0, 0, 0, 0 };
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
        {
            static uint64_t last_part[5] = { 0, 0, 0, 0, 0 };
            if (frames)
                LOG_MSG("INPUTNET: encoding a frame: %.2f ms until the encoder starts on it, %.2f looking for a scroll, %.2f for the difference, %.2f compressing it, %.2f compressing the frame alone too",
                    (stat_part_us[0] - last_part[0]) / 1000.0 / frames,(stat_part_us[1] - last_part[1]) / 1000.0 / frames,
                    (stat_part_us[2] - last_part[2]) / 1000.0 / frames,(stat_part_us[3] - last_part[3]) / 1000.0 / frames,
                    (stat_part_us[4] - last_part[4]) / 1000.0 / frames);
            for (unsigned k=0;k < 5;k++) last_part[k] = stat_part_us[k];
        }
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

/* [inputnet] block keys: key names separated by spaces or commas. Unknown names are logged and skipped. */
void RP_SetBlockedKeys(const std::string &list) {
    std::fill(cfg_blocked,cfg_blocked + KBD_LAST,false);
    std::string shown;
    for (const std::string &name : RP_Words(list," ,\t")) {
        KBD_KEYS key;
        if (!RP_KeyByName(name,key)) { LOG_MSG("INPUTNET: block keys: unknown key name \"%s\"",name.c_str()); continue; }
        cfg_blocked[key] = true;
        shown += (shown.empty() ? "" : " ") + name;
    }
    if (!shown.empty()) LOG_MSG("INPUTNET: keys blocked for remote players: %s",shown.c_str());
}

/* [inputnet] report input: one datagram per input event of a remote player to 127.0.0.1:<port>, the fields given
 * here after the player's name: {"p":"<player>",<fields>} (NukemNet's server automation hears them as inputnetInput
 * events). Nothing is formatted while the setting is off. */
void RP_ReportInput(Viewer &v,const char *fmt,...) GCC_ATTRIBUTE(__format__(__printf__,2,3));
void RP_ReportInput(Viewer &v,const char *fmt,...) {
    if (!cfg_report_to.port || !host_sock) return;
    std::string name;
    for (char c : v.name) if (c != '"' && c != '\\' && (unsigned char)c >= 32) name += c;
    char buf[256];
    int n = snprintf(buf,sizeof(buf),"{\"p\":\"%s\",",name.c_str());
    if (n < 0 || n >= (int)sizeof(buf)) return;
    va_list ap;
    va_start(ap,fmt);
    const int fields = vsnprintf(buf + n,sizeof(buf) - (size_t)n,fmt,ap);
    va_end(ap);
    if (fields < 0 || (n += fields) >= (int)sizeof(buf) - 1) return;    /* no room left for the closing brace */
    buf[n++] = '}';
    Host_SendTo(cfg_report_to,(const uint8_t*)buf,(size_t)n);
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
            /* Keys the host blocked for remote players ([inputnet] block keys): a release still goes through for a key
             * that was down before (none can be: the list is read before anyone joins), the rest is dropped. */
            const bool blocked = cfg_blocked[key] && !(v.keydown[key] && !pressed);
            RP_ReportInput(v,"\"t\":\"key\",\"k\":\"%s\",\"d\":%d,\"b\":%d",RP_KeyName(key),pressed ? 1 : 0,blocked ? 1 : 0);
            if (blocked) return;
            v.keydown[key] = pressed;
            if (pressed) guard_typed = true;
            KEYBOARD_AddKeyRaw((KBD_KEYS)key,pressed);
            break;
        }
        case RPEV_MBUTTON: {
            const uint8_t button = r.u8();
            const bool pressed = r.u8() != 0;
            if (!r.ok || button >= 32 || !Guard_Allows()) return;
            RP_ReportInput(v,"\"t\":\"button\",\"n\":%u,\"d\":%d",(unsigned)button,pressed ? 1 : 0);
            if (pressed) { v.buttons |= 1u << button; Mouse_ButtonPressedRaw(button); }
            else { v.buttons &= ~(1u << button); Mouse_ButtonReleasedRaw(button); }
            break;
        }
        case RPEV_MMOVE: {
            const float xrel = r.f32(), yrel = r.f32(), x = r.f32(), y = r.f32();
            const bool emulate = r.u8() != 0;
            if (!r.ok || !Guard_Allows()) return;
            if (cfg_report_to.port) {
                /* motion is gathered and reported at most ten times a second per player */
                v.report_dx += xrel; v.report_dy += yrel;
                const uint32_t now = SDL_GetTicks();
                if (now - v.report_ms >= 100 && (v.report_dx != 0 || v.report_dy != 0)) {
                    RP_ReportInput(v,"\"t\":\"move\",\"dx\":%.1f,\"dy\":%.1f",v.report_dx,v.report_dy);
                    v.report_dx = v.report_dy = 0; v.report_ms = now;
                }
            }
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
            v->report_dx = v->report_dy = 0;    /* (nothing of the slot's last player) */
            v->report_ms = 0;
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

/* ------------------------------------------------------------------------- */
/* NukemNet's control channel                                                 */
/* ------------------------------------------------------------------------- */

/* Env NN_DOSBOX_CONTROL_PORT and NN_DOSBOX_CONTROL_TOKEN (16+ characters) open a UDP port in any DOSBox-X-NN, remote
 * play or not. A datagram from 127.0.0.1 is "<token> <commands>": one command, or several separated by newlines. They
 * are queued in the order given, behind what is still waiting:
 *   type <text>             types the text (Shift where a character needs it)
 *   keys <name> [name...]   presses and releases each key (the keymapper's names: enter esc f1 kp_5 ...)
 *   key <name> down|up      one key step
 *   wait <ms>               nothing for that long (up to 10000) before what follows
 *   sync <id>               answers "done <id>" to the sender, from this port, once everything before it has gone
 *                           out (the id: up to 32 characters, sent back as they came)
 * Key steps go out one every 40 ms, so a game that polls the keyboard sees each of them. Anything else is ignored. */
struct ControlStep {
    enum Kind { KEY, WAIT, SYNC } kind;
    KBD_KEYS key; bool down;        /* KEY */
    uint32_t ms;                    /* WAIT */
    IPaddress to; char id[33];      /* SYNC: "done <id>" goes to this address */
};

UDPsocket control_sock = NULL;
UDPpacket *control_pkt = NULL;      /* what arrives, and the answers */
std::string control_token;          /* with the space that follows it in a datagram */
std::deque<ControlStep> control_queue;
uint32_t control_last = 0, control_pause = 0;   /* when the last step was taken, and how long the next one waits after it */

ControlStep &Control_Add(ControlStep::Kind kind) {
    control_queue.push_back(ControlStep());
    control_queue.back().kind = kind;
    return control_queue.back();
}

void Control_AddKeyStep(KBD_KEYS key,bool down) {
    ControlStep &step = Control_Add(ControlStep::KEY);
    step.key = key; step.down = down;
}

/* A key pressed and released, in Shift if asked. */
void Control_QueueKey(KBD_KEYS key,bool shift) {
    if (shift) Control_AddKeyStep(KBD_leftshift,true);
    Control_AddKeyStep(key,true);
    Control_AddKeyStep(key,false);
    if (shift) Control_AddKeyStep(KBD_leftshift,false);
}

void Control_QueueChar(char c) {
    static const char *plain = "`1234567890-=[]\\;',./";
    static const char *shifted = "~!@#$%^&*()_+{}|:\"<>?";
    static const KBD_KEYS keys[] = { KBD_grave,KBD_1,KBD_2,KBD_3,KBD_4,KBD_5,KBD_6,KBD_7,KBD_8,KBD_9,KBD_0,KBD_minus,KBD_equals,
        KBD_leftbracket,KBD_rightbracket,KBD_backslash,KBD_semicolon,KBD_quote,KBD_comma,KBD_period,KBD_slash };
    const bool upper = c >= 'A' && c <= 'Z';
    const char letter = upper ? (char)(c - 'A' + 'a') : c;
    KBD_KEYS k;
    if (letter >= 'a' && letter <= 'z') { if (RP_KeyByName(std::string(1,letter),k)) Control_QueueKey(k,upper); return; }
    if (c == ' ') { Control_QueueKey(KBD_space,false); return; }
    for (unsigned i=0;plain[i];i++) {
        if (plain[i] == c) { Control_QueueKey(keys[i],false); return; }
        if (shifted[i] == c) { Control_QueueKey(keys[i],true); return; }
    }
}

/* One command of a datagram that came from this address. */
void Control_Command(const std::string &line,const IPaddress &from) {
    const size_t space = line.find(' ');
    if (space == std::string::npos) return;
    const std::string cmd = line.substr(0,space), arg = line.substr(space + 1);
    KBD_KEYS k;
    if (cmd == "type") {
        for (char c : arg) Control_QueueChar(c);
    } else if (cmd == "keys") {
        for (const std::string &name : RP_Words(arg," ")) if (RP_KeyByName(name,k)) Control_QueueKey(k,false);
    } else if (cmd == "key") {
        const std::vector<std::string> words = RP_Words(arg," ");
        if (words.size() == 2 && RP_KeyByName(words[0],k)) Control_AddKeyStep(k,words[1] == "down");
    } else if (cmd == "wait") {
        Control_Add(ControlStep::WAIT).ms = (uint32_t)std::min(std::max(strtol(arg.c_str(),NULL,10),0L),10000L);
    } else if (cmd == "block") {
        /* [inputnet] block keys from now on, for the remote players of the running game ("none": no key) */
        if (arg == "none") LOG_MSG("INPUTNET: no keys blocked for remote players any more");
        RP_SetBlockedKeys(arg == "none" ? std::string() : arg);
    } else if (cmd == "sync" && !arg.empty() && arg.size() < sizeof(ControlStep::id)) {
        ControlStep &step = Control_Add(ControlStep::SYNC);
        step.to = from;
        memcpy(step.id,arg.c_str(),arg.size() + 1);
    }
}

/* Opens the channel the first time, when the environment names a port and a token. True while there is one. */
bool Control_Open(void) {
    static bool tried = false;
    if (!tried) {
        tried = true;
        const char *port = getenv("NN_DOSBOX_CONTROL_PORT"), *token = getenv("NN_DOSBOX_CONTROL_TOKEN");
        const int p = port ? atoi(port) : 0;
        /* (RP_OpenNet: the socket library may not be up yet when nothing else of this DOSBox-X uses the network) */
        if (p > 0 && p < 65536 && token && strlen(token) >= 16 && RP_OpenNet()) {
            control_token = std::string(token) + ' ';
            control_pkt = SDLNet_AllocPacket(1024);
            control_sock = control_pkt ? SDLNet_UDP_Open((uint16_t)p) : NULL;
            if (control_sock) LOG_MSG("NUKEMNET: control channel on UDP port %d",p);
            else LOG_MSG("NUKEMNET: control channel: UDP port %d is not free",p);
        }
    }
    return control_sock != NULL;
}

/* Queues what arrived and takes the queue's next step when the one before it has had its time. */
void Control_Poll(uint32_t now) {
    while (SDLNet_UDP_Recv(control_sock,control_pkt) > 0) {
        if (SDLNet_Read32(&control_pkt->address.host) != RP_LOOPBACK) continue;
        const std::string msg((const char*)control_pkt->data,(size_t)control_pkt->len);
        if (msg.size() <= control_token.size() || msg.compare(0,control_token.size(),control_token) != 0) continue;
        for (size_t pos = control_token.size();pos < msg.size();) {
            const size_t end = std::min(msg.find('\n',pos),msg.size());
            Control_Command(msg.substr(pos,end - pos),control_pkt->address);
            pos = end + 1;
        }
    }
    while (!control_queue.empty() && now - control_last >= control_pause) {
        const ControlStep step = control_queue.front();
        control_queue.pop_front();
        control_last = now;
        control_pause = 0;
        switch (step.kind) {
            case ControlStep::KEY:
                KEYBOARD_AddKeyRaw(step.key,step.down);
                control_pause = 40;
                break;
            case ControlStep::WAIT:
                control_pause = step.ms;
                break;
            case ControlStep::SYNC:
                control_pkt->len = snprintf((char*)control_pkt->data,(size_t)control_pkt->maxlen,"done %s",step.id);
                control_pkt->address = step.to;
                SDLNet_UDP_Send(control_sock,-1,control_pkt);
                break;
        }
    }
}

} /* namespace */

void REMOTEPLAY_Configure(Section_prop *section) {
    RP_SetBlockedKeys(section->Get_string("block keys"));
    const int report = section->Get_int("report input");
    SDLNet_Write32(RP_LOOPBACK,&cfg_report_to.host);
    SDLNet_Write16((uint16_t)((report > 0 && report < 65536) ? report : 0),&cfg_report_to.port);
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

#if defined(WIN32)
/* Windows 11 slows down a process whose window is minimized or hidden ("efficiency mode": its threads are put on
 * the slow cores and its timers lose their one-millisecond steps). That is exactly what a copy that plays for a
 * remote player is, and it showed: the same frame took the encoder more than twice as long on a Windows host as on
 * a Linux one. A remote play host therefore asks to be left out of both (SetProcessInformation with
 * ProcessPowerThrottling; looked up at run time, older Windows has no such call or only the first of the two). */
static void Host_NoThrottle(void) {
    typedef BOOL (WINAPI *SetInfo)(HANDLE,int,LPVOID,DWORD);
    const HMODULE k = GetModuleHandleA("kernel32.dll");
    const SetInfo set = k ? (SetInfo)(void*)GetProcAddress(k,"SetProcessInformation") : NULL;
    if (set == NULL) return;
    struct { ULONG Version, ControlMask, StateMask; } st = { 1, 0x1 | 0x4, 0 };    /* execution speed, timer resolution: never throttled */
    bool ok = set(GetCurrentProcess(),4 /* ProcessPowerThrottling */,&st,sizeof(st)) != 0;
    if (!ok) {
        st.ControlMask = 0x1;
        ok = set(GetCurrentProcess(),4,&st,sizeof(st)) != 0;
    }
    LOG_MSG("INPUTNET: remote play host %s of Windows' slowing down of windows that are not in front",ok ? "is left out" : "could not be left out");
}
#endif

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
#if defined(WIN32)
    Host_NoThrottle();
#endif
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
    /* Neither a control channel nor a host: nothing to look at, not even the clock (GFX_Events runs very often, and
     * under Emscripten reading the clock is what costs). */
    const bool listening = Control_Open();
    if (!listening && !host_running) return;

    /* GFX_Events runs very often; the sockets only need a look once a
     * millisecond (the video rate allows more once a millisecond too, and
     * whoever queues video sends what may go at once). */
    static uint32_t lastpoll = 0;
    const uint32_t now = SDL_GetTicks();
    if (now == lastpoll) return;
    lastpoll = now;

    if (listening) Control_Poll(now);
    if (!host_running) return;

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
    uint64_t part[5] = { 0, 0, 0, 0, 0 }, mark = RP_Micros();
    part[0] = mark - std::min(mark,c.taken_us);
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
    mark = RP_Micros();
    /* Looking for a scroll costs more than everything else together (2 of 5 ms a frame at 320x200), and a view that
     * turns in perspective never has one: after four frames without, only every eighth frame is looked at, after
     * thirty-two every sixteenth, until one is found again. */
    const bool look = enc_noscroll < 4 || (id & (enc_noscroll < 32 ? 7u : 15u)) == 0;
    if (!key && shiftmode && look && !enc_hist.empty() && enc_hist.back().raw.size() == rawsize) {
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
        if (shifted) enc_noscroll = 0;
        else if (enc_noscroll < 1000) enc_noscroll++;
    }
    { const uint64_t t = RP_Micros(); part[1] = t - mark; mark = t; }

    const uint8_t *src = enc_raw.data();
    enc_same = -1.0f;
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
            size_t i = 0, same = 0;
            for (;i + 8 <= rawsize;i += 8) {
                uint64_t x, y;
                memcpy(&x,a + i,8); memcpy(&y,b + i,8);
                x ^= y;
                same += x == 0;
                memcpy(d + i,&x,8);
            }
            for (;i < rawsize;i++) d[i] = a[i] ^ b[i];
            enc_same = rawsize >= 64 ? (float)(same * 8) / (float)rawsize : 1.0f;
        }
        src = enc_work.data();
        /* Much motion (turning in a 3D game): the frame alone compresses better than its difference, and goes out
         * as a key frame, which every viewer decodes whatever it has. Which of the two is smaller follows from how
         * much of the frame is as in its base: measured over Doom and Duke Nukem 3D streams, with three tenths or
         * more the difference was the smaller one every time, with less the frame alone nearly every time (by a
         * tenth). So only one of them is compressed, not both to compare; a link that limits the video still gets
         * both tried (below), there every byte counts more than a millisecond. */
        if (!shifted && !limited && enc_same >= 0 && enc_same < 0.3f) {
            key = turned_key = true;
            src = enc_raw.data();
        }
    }
    { const uint64_t t = RP_Micros(); part[2] = t - mark; mark = t; }

    size_t clen = 0;
    bool ok = RP_Compress(src,rawsize,enc_comp,RP_FRAME_HDR,clen,enc_pieces);
    { const uint64_t t = RP_Micros(); part[3] = t - mark; mark = t; }
    /* Much motion (turning in a 3D game): the frame alone often compresses better than its difference. Such a frame
     * goes out as a key frame, which every viewer decodes, whatever it has. */
    if (ok && !key) {
        bool alone = false;
        const size_t dlen = clen;
        if (clen > rawsize / 16 && (limited || shifted)) {
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
        static const bool encstats = getenv("NN_RP_ENCSTATS") != NULL && getenv("NN_RP_ENCSTATS")[0] != 0;
        if (encstats && enc_same >= 0) {
            size_t ilen = 0;
            std::vector<uint8_t> t1; std::vector<uint32_t> t2;
            RP_Compress(enc_work.size() == rawsize && alone ? enc_raw.data() : enc_raw.data(),rawsize,t1,0,ilen,t2);
            LOG_MSG("INPUTNET: encstats same %.3f difference %u alone %u",(double)enc_same,(unsigned)dlen,(unsigned)ilen);
        }
        (void)dlen;
    }
    { const uint64_t t = RP_Micros(); part[4] = t - mark; mark = t; }
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
    for (unsigned k=0;k < 5;k++) stat_part_us[k] += part[k];
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
        w.u32((uint32_t)c.taken_us);    /* for view prediction: older viewers stop reading before it */
        /* ...and the mouse driver's pointer, for the viewer's pointer dot */
        w.u8(c.ptr ? 1 : 0);
        if (c.ptr) {
            w.u16((uint16_t)(std::max(0.0f,std::min(1.0f,c.ptr_v[0])) * 65535.0f + 0.5f));
            w.u16((uint16_t)(std::max(0.0f,std::min(1.0f,c.ptr_v[1])) * 65535.0f + 0.5f));
            for (unsigned k=2;k < 6;k++) w.f32(c.ptr_v[k]);
        }
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
    /* (a Windows guest's pointer goes another way: see RPEV_MMOVE) */
    c.ptr = mouse_notify_mode == 0 && Mouse_NNPointer(c.ptr_v[0],c.ptr_v[1],c.ptr_v[2],c.ptr_v[3],c.ptr_v[4],c.ptr_v[5]);

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

/* ------------------------------------------------------------------------- */
/* View prediction (viewer)                                                   */
/* ------------------------------------------------------------------------- */

/* Moving the picture is OFF unless the player asks for it ([inputnet] predict view = true, or Ctrl+F12 in the viewer):
 * a moved first-person view shows. What stays on the screen (a weapon, a crosshair, a message) either moves with the
 * view and returns with the next frame, or is held in place, and then the world it covered has to be made up; both
 * were built, and a player saw each (see the end of this note). By default the viewer shows every frame exactly as
 * the host sent it, and only the pointer dot (below) is predicted. The rest of this note describes what happens when
 * it is turned on; it is learned in the background either way, so the switch works at once.
 *
 * Most of what a player feels as lag in a streamed game is the view: the mouse moves, and the picture turns one round
 * trip and the game's own reaction later. A turn is a simple change of the picture: it moves the whole view sideways
 * by an amount that depends on the column only, never on how far away things are (looking up and down moves it
 * vertically). So the viewer shows the newest frame already moved by the input that frame does not show yet; the
 * frames that follow arrive where the picture already is. Between frames the picture is drawn again whenever the
 * prediction has changed, so the view also follows the hand while the link stalls for a moment.
 *
 * Nothing is known about a game beforehand. All of it is learned from the stream while it is played:
 *  - how far the picture moved from one frame to the next (VP_Measure: the place where the pixels that changed
 *    fit the frame before, for the middle of the view and for 8 strips of it);
 *  - which input a frame shows (RP_FRAMEINFO: the newest input applied when the host took it, and when);
 *  - how far the picture moves per mouse count, and how long the game itself takes to show it (a regression of the
 *    picture's movement on the mouse's, for every candidate delay);
 *  - the lens: how much more the sides move than the middle (a view in perspective), or all alike (a 2D view);
 *  - what a held key does to the view, by how long it has been held (turn keys), per combination of Shift, Ctrl
 *    and Alt;
 *  - the view itself: the rows and columns whose pixels its movement explains (not a status bar, a border, a side
 *    panel), and inside it the pixels that stay (a weapon held still, a crosshair), which are left out of measuring;
 *  - layers (VP_Bands, learnLayers): bands of the view that the engine scrolls evenly at a rate of their own instead
 *    of turning them with the lens (Build's sky: 1.24 times the ground's movement in the middle of the view, the
 *    same in every column). Each of 6 bands is tried both ways against a frame a few frames back; a band that an
 *    even scroll explains clearly better is moved at its own rate and left out of the view's own measurement, and
 *    while layers are in view the lens is not learned (taken to be 90 degrees wide if nothing else has taught it).
 * A prediction is shown only while the frames that arrive agree with what had been predicted for them (the gate):
 * in a menu, a pause or a cutscene the picture does not follow the mouse, and prediction stops after two stretches
 * of frames that say so. Escape, Tab and the function keys close the gates at once, since they open menus.
 * Ctrl+F12 turns moving the picture on and off; [inputnet] predict = false turns all prediction off.
 * Nothing is drawn that the frame does not have: the view is shown slightly larger and moved as one piece, by whole
 * pixels, within that margin (see ViewPredict::warp), with everything in it, the weapon, the crosshair and messages
 * included; they return to their places with the next frame, which a player sees while turning (the reason this is
 * off by default). Mirroring the edge, pictures kept by direction, a smear, a stretch, and holding what stays in
 * place with guesses for what it hid were each tried and taken out after a player saw them (Duke Nukem 3D, November
 * 2026). Things that move by themselves keep their own lag.
 * A game that draws a pointer of its own where the DOS mouse driver has it (strategy games, adventures, menus) gets
 * a dot instead of a moved picture: the host reports the driver's pointer and how far a count moves it with every
 * frame, the viewer adds the motion the host had not applied and draws the dot at once (pointerAt, from the newest
 * report whichever frame is on the screen), after seeing in the picture that something is drawn there and moves with
 * it (judgePointer).
 * Everything happens in the viewer; a host that says when it took each frame makes the timing exact, an older one
 * is followed as well as its frame information allows (and gets no pointer dot).
 *
 * For tests (environment): NN_RP_PREDICT=0|1, NN_RP_PREDICTVIEW=0|1 (moving the picture), NN_RP_PREDICTLOG=1|2 (what
 * was learned; every frame),
 * NN_RP_RECORD=<folder> (frames and input, to learn from again offline), NN_RP_PREDICTSHOT=<folder> (pictures moved
 * far, before and after), NN_RP_TESTINPUT=mouse|look|keys|mix|walk|walkkeys|none with NN_RP_TESTSCALE,
 * NN_RP_TESTSCALEY, NN_RP_TESTSTART and NN_RP_TESTKEYS (a mouse and keys that move by themselves; "t:key:secs", the
 * key "click" is the left mouse button). Host: NN_RP_ENCSTATS=1 logs what each frame's compression chose. */

const unsigned VP_STRIPS = 8;           /* the picture is measured in its middle and in 8 strips side by side */
const unsigned VP_LAGS = 26;            /* the game's own delay is looked for among 0, 8 ... 200 ms */
const uint64_t VP_LAG_STEP = 8000;      /* microseconds */
const uint8_t VP_STILL = 4;             /* a pixel that stayed this often while the view moved does not move with it */
const unsigned VP_KEYBINS = 16;         /* what a key does to the view is learned for 11 stretches of the time it has been
                                           held and 5 of the time since it was let go */
const unsigned VP_KEYCTX = 8;           /* ...separately with and without Shift, Ctrl and Alt held */
const unsigned VP_KEYHIST = 6;          /* presses and releases remembered per key */
const float VP_MARGIN = 0.025f;         /* the view is shown this much larger at each side, and moved at most that much */

/* The DOS mouse driver's pointer as the host reports it with a frame: its place (0 ... 1 of its range each way)
 * and how far one count of mouse motion moves it (a single count; a larger motion, per count). */
struct VPPointer {
    bool told = false;      /* the host's information for the frame arrived (a datagram of its own: it can get lost) */
    bool has = false;       /* ...and says that the mouse driver has a pointer, and where */
    float x = 0, y = 0, gx1 = 0, gy1 = 0, gxs = 0, gys = 0;
};

struct VPLens {
    bool known = false;     /* decided: enough turns have been seen */
    bool assumed = false;   /* ...or taken to be 90 degrees, until frames that can tell say otherwise */
    bool persp = false;     /* a view in perspective: the sides move more than the middle; false: all alike */
    float f = 0, cx = 0;    /* focal length and the middle column, in pixels */
};

/* The column the content shown at column xo comes from, when the middle of the picture has moved p pixels right. */
inline float VP_SourceX(const VPLens &l,float xo,float p) {
    if (!l.persp) return xo - p;
    const float u = xo - l.cx;
    const float den = 1.0f + u * p / (l.f * l.f);
    return l.cx + (u - p) / (den > 0.25f ? den : 0.25f);
}

/* ...and how much less tall it is where it comes from, at column xs (a turn in perspective stretches the side that
 * leaves the view): its distance from the middle row there, over the distance here. */
inline float VP_RowScale(const VPLens &l,float xs,float p) {
    if (!l.persp) return 1.0f;
    const float k = (l.f * l.f - (xs - l.cx) * p) / (l.f * sqrtf(l.f * l.f + p * p));
    return k < 0.25f ? 0.25f : (k > 4.0f ? 4.0f : k);
}

/* How far the middle of the picture moved, when the content now at column xo moved d pixels right. */
inline bool VP_CentreShift(const VPLens &l,float xo,float d,float &p) {
    if (!l.persp) { p = d; return true; }
    const float u = xo - l.cx;
    const float den = 1.0f + u * (u - d) / (l.f * l.f);
    if (den < 0.25f) return false;
    p = d / den;
    return true;
}

/* What is kept of a frame to compare the next one with: its brightness, and the same halved in size up to three times
 * (the smallest about 80 pixels wide: a movement of a quarter of the picture is found there in a few steps). */
const unsigned VP_LEVELS = 4;
struct VPPlane {
    unsigned w = 0, h = 0, levels = 0;
    unsigned ystep = 1;                 /* 2: every row of the picture comes twice (320x200 sent as 320x400); only one of each is kept here */
    uint32_t id = 0;
    bool valid = false, hasc = false;
    uint64_t c = 0;                     /* the moment (this computer's clock) up to which it shows the input */
    uint32_t seq = 0xFFFFFFFFu;         /* ...and the newest input the host had applied when it took it, when known */
    VPPointer ptr;                      /* the DOS mouse driver's pointer when the host took it */
    double sumx = 0, sumy = 0;          /* all the view's movement up to this frame, while it could be followed... */
    unsigned chain = 0;                 /* ...since it last could not (frames of one chain can be compared) */
    std::vector<uint8_t> y[VP_LEVELS];
    unsigned lw[VP_LEVELS], lh[VP_LEVELS];
};

void VP_BuildPlane(VPPlane &pl,const uint8_t *raw,unsigned w,unsigned h,unsigned fmt) {
    const size_t bpp = RP_BytesPerPixel(fmt), pal = (fmt == 8) ? 768 : 0;
    /* A 200-line mode arrives with every line twice: measuring up and down between two equal lines says nothing,
     * and half the lines are half the work. */
    unsigned ystep = 1;
    if (h >= 2 && (h & 1) == 0) {
        ystep = 2;
        for (unsigned y=0;y + 1 < h && ystep == 2;y += 2 * std::max(1u,h / 64))
            if (memcmp(raw + pal + (size_t)y * w * bpp,raw + pal + (size_t)(y + 1) * w * bpp,(size_t)w * bpp) != 0) ystep = 1;
    }
    pl.ystep = ystep;
    const unsigned ph = h / ystep;
    pl.w = w; pl.h = ph;
    pl.lw[0] = w; pl.lh[0] = ph;
    pl.y[0].resize((size_t)w * ph);
    uint8_t lut[256];
    if (fmt == 8)
        for (unsigned i=0;i < 256;i++) lut[i] = (uint8_t)((raw[i*3] * 77u + raw[i*3+1] * 150u + raw[i*3+2] * 29u) >> 8);
    for (unsigned y=0;y < ph;y++) {
        uint8_t *o = pl.y[0].data() + (size_t)y * w;
        const uint8_t *s = raw + pal + (size_t)(y * ystep) * w * bpp;
        if (fmt == 8) {
            for (unsigned x=0;x < w;x++) o[x] = lut[s[x]];
        }
        else if (fmt == 15 || fmt == 16) {
            for (unsigned x=0;x < w;x++) {
                const unsigned p = s[x*2] | ((unsigned)s[x*2+1] << 8);
                unsigned r, g;
                if (fmt == 15) { r = (p >> 10) & 31; g = ((p >> 5) & 31) << 1; }
                else { r = (p >> 11) & 31; g = (p >> 5) & 63; }
                o[x] = (uint8_t)((r * 77u * 8u + g * 150u * 4u + (p & 31) * 29u * 8u) >> 8);
            }
        }
        else {
            for (unsigned x=0;x < w;x++) o[x] = (uint8_t)((s[x*4+2] * 77u + s[x*4+1] * 150u + s[x*4] * 29u) >> 8);
        }
    }
    pl.levels = 1;
    while (pl.levels < VP_LEVELS && pl.lw[pl.levels-1] > 100 && pl.lh[pl.levels-1] >= 64) {
        const unsigned l = pl.levels, pw = pl.lw[l-1], nw = pw / 2, nh = pl.lh[l-1] / 2;
        pl.lw[l] = nw; pl.lh[l] = nh;
        pl.y[l].resize((size_t)nw * nh);
        for (unsigned y=0;y < nh;y++) {
            const uint8_t *a = pl.y[l-1].data() + (size_t)(y * 2) * pw, *b = a + pw;
            uint8_t *o = pl.y[l].data() + (size_t)y * nw;
            for (unsigned x=0;x < nw;x++) o[x] = (uint8_t)((a[x*2] + a[x*2+1] + b[x*2] + b[x*2+1] + 2u) >> 2);
        }
        pl.levels++;
    }
    pl.valid = true;
}

struct VPScratch {
    std::vector<uint8_t> mask[VP_LEVELS], maskc, stillnow;
    std::vector<float> vals, sorted;
    std::vector<int32_t> srcx, rowk, colw;
};

/* How unlike the picture cur is to the earlier one moved by (d,e), in the window of columns [x0,x1) and rows [y0,y1),
 * every ystep-th row: the mean difference in brightness over the pixels marked in mask (those that changed in
 * place: what stays, a status bar, a border, a plain wall, says nothing about a movement). -1 when too little is
 * left to compare. */
float VP_Unlike(const uint8_t *cur,const uint8_t *prev,const uint8_t *mask,int w,int h,int x0,int x1,int y0,int y1,int ystep,int d,int e) {
    const int xa = std::max(x0,d), xb = std::min(x1,w + d);
    if ((xb - xa) * 10 < (x1 - x0) * 6) return -1.0f;
    int ya = std::max(y0,e);
    const int yb = std::min(y1,h + e);
    if ((yb - ya) * 10 < (y1 - y0) * 6) return -1.0f;
    ya = y0 + ((ya - y0 + ystep - 1) / ystep) * ystep;
    uint32_t sum = 0, n = 0, rows = 0;
    for (int y=ya;y < yb;y += ystep) {
        const uint8_t *c = cur + (size_t)y * w, *p = prev + (size_t)(y - e) * w - d, *m = mask + (size_t)y * w;
        uint32_t rs = 0, rn = 0;
        for (int x=xa;x < xb;x++) {
            const int df = (int)c[x] - (int)p[x];
            rs += (uint32_t)(m[x] * (df < 0 ? -df : df));
            rn += m[x];
        }
        sum += rs; n += rn;
        rows++;
    }
    /* a few pixels that changed (a blinking light, a counter) fit many places: one in fifty of the window at least */
    if (n < 24 || n * 50 < rows * (uint32_t)(xb - xa)) return -1.0f;
    return (float)sum / (float)n;
}

/* Where in a run of three the least lies: -0.5 ... 0.5 from the middle one. */
inline float VP_Vertex(float l,float m,float r) {
    const float den = l - 2.0f * m + r;
    if (l < 0 || r < 0 || den <= 1e-6f) return 0;
    return std::max(-0.5f,std::min(0.5f,0.5f * (l - r) / den));
}

/* Where between whole pixels the movement lies: the places d - 1, d and d + 1 (sideways; or e - 1, e, e + 1 up and
 * down when vertical is set) compared over the pixels where the earlier picture has something to tell them apart
 * by. The pixels that changed in place would not do here: they are picked for not fitting "no movement", which
 * pulls the answer away from it. Returns -0.5 ... 0.5 to add to d (or e). */
float VP_Fine(const uint8_t *cur,const uint8_t *prev,int w,int h,int x0,int x1,int y0,int y1,int ystep,int d,int e,bool vertical) {
    const int mx = vertical ? 0 : 1, my = vertical ? 1 : 0;
    const int xa = std::max(x0,d + mx), xb = std::min(x1,w + d - mx);
    int ya = std::max(y0,e + my);
    const int yb = std::min(y1,h + e - my);
    if (xb - xa < 8 || yb - ya < 4) return 0;
    ya = y0 + ((ya - y0 + ystep - 1) / ystep) * ystep;
    const std::ptrdiff_t step = vertical ? (std::ptrdiff_t)w : 1;
    uint32_t sl = 0, sm = 0, sr = 0, n = 0;
    for (int y=ya;y < yb;y += ystep) {
        const uint8_t *c = cur + (size_t)y * w, *p = prev + (size_t)(y - e) * w - d;
        for (int x=xa;x < xb;x++) {
            const int before = p[x + step], at = p[x], after = p[x - step];    /* the earlier picture moved one less, as found, one more */
            const int g = before - after;
            if (g <= 4 && g >= -4) continue;
            const int v = c[x];
            sl += (uint32_t)abs(v - before); sm += (uint32_t)abs(v - at); sr += (uint32_t)abs(v - after);
            n++;
        }
    }
    if (n < 24) return 0;
    return VP_Vertex((float)sl,(float)sm,(float)sr);
}

const unsigned VP_CELLROWS = 6;         /* the view is also looked at in 6 bands of 8 cells, for what scrolls at a rate of its own */

struct VPView { int x0, x1, y0, y1; };  /* the view: columns [x0,x1), rows [y0,y1) of the picture */

struct VPMotion {
    bool same = false;          /* the two pictures are the same */
    bool okx = false, oky = false;
    float sx = 0, sy = 0;       /* how far the middle of the picture moved right, and the picture down */
    float whole = 0, wholey = 0;        /* ...as measured in the middle alone */
    float fit = 0;              /* how well that fits: the difference left, against what other places leave */
    bool layered = false;       /* part of the view scrolls at a rate of its own and was left out (set by the caller) */
    bool masked = false;        /* the pixels that changed between the two frames are in the scratch space */
    float sd[VP_STRIPS];        /* how far each of the 8 strips of the picture moved right */
    uint8_t sk[VP_STRIPS];      /* 0 not known, 1 measured, 2 it stayed as it was */
    int bandvote[VP_CELLROWS];  /* for each band of the view: 1 it scrolls evenly, -1 it moves as the lens says, 0 no telling */
    float bandrate[VP_CELLROWS];        /* ...and at what rate, against the view's middle (see VP_Bands) */
    bool bands = false;         /* the bands were asked for this frame */
    unsigned measured = 0, stayed = 0, agreed = 0;
    bool oneway = false;        /* the strips all moved the way the middle did, whatever the lens: good to learn the lens from */
    float changed = 0;          /* the part of the picture that changed */
    int why = 0;                /* 0 measured, 1 the middle fits nowhere, 2 nothing to tell by, 3 the strips disagree */
    VPMotion() { memset(sd,0,sizeof(sd)); memset(sk,0,sizeof(sk)); memset(bandvote,0,sizeof(bandvote)); memset(bandrate,0,sizeof(bandrate)); }
};

float VP_Median(float *v,unsigned n) {
    std::nth_element(v,v + n / 2,v + n);
    return v[n / 2];
}

/* How far the middle of rows [y0,y1) of the view moved (there a turn moves everything alike, whatever the lens):
 * looked for in the smallest copy of the picture, sideways first and up and down too when sideways alone fits badly,
 * then followed down to the full picture two pixels around, level by level. 0: found; 1: it fits nowhere (the view
 * changed, and not by moving as one); 2: nothing to tell by. */
int VP_Middle(const VPPlane &cur,const VPPlane &prev,const VPView &v,int y0,int y1,VPScratch &tmp,float &whole,float &wholey,float &fit) {
    const unsigned top = cur.levels - 1;
    const int tw = (int)cur.lw[top], th = (int)cur.lh[top];
    const int vw = v.x1 - v.x0, cx2 = v.x0 + v.x1;         /* twice the middle column */
    const int tx0 = (cx2 - vw * 4 / 10) >> (top + 1), tx1 = (cx2 + vw * 4 / 10) >> (top + 1);
    const int ty0 = y0 >> top, ty1 = std::min(th,(y1 + (1 << top) - 1) >> top);
    const int D = (vw >> top) / 4, E = std::max(2,(ty1 - ty0) / 6);
    if (tx1 - tx0 < 6 || ty1 - ty0 < 4 || D < 2) return 2;
    int bd = 0, be = 0;
    float bestv = -1.0f;
    tmp.vals.clear();
    for (int d=-D;d <= D;d++) {
        const float val = VP_Unlike(cur.y[top].data(),prev.y[top].data(),tmp.mask[top].data(),tw,th,tx0,tx1,ty0,ty1,1,d,0);
        if (val < 0) continue;
        tmp.vals.push_back(val);
        if (bestv < 0 || val < bestv) { bestv = val; bd = d; }
    }
    if (tmp.vals.size() < 8) return 2;
    tmp.sorted = tmp.vals;
    float med = VP_Median(tmp.sorted.data(),(unsigned)tmp.sorted.size());
    if (bestv > 0.35f * med) {
        for (int e=-E;e <= E;e++) {
            if (e == 0) continue;
            for (int d=-D;d <= D;d++) {
                const float val = VP_Unlike(cur.y[top].data(),prev.y[top].data(),tmp.mask[top].data(),tw,th,tx0,tx1,ty0,ty1,1,d,e);
                if (val < 0) continue;
                tmp.vals.push_back(val);
                if (val < bestv) { bestv = val; bd = d; be = e; }
            }
        }
        tmp.sorted = tmp.vals;
        med = VP_Median(tmp.sorted.data(),(unsigned)tmp.sorted.size());
    }
    fit = med > 0 ? bestv / med : 1.0f;
    if (med <= 0 || bestv > 0.6f * med) return 1;
    if (bd <= -D || bd >= D) return 1;      /* at the end of where it was looked for: it may lie beyond */
    for (unsigned l=top;l-- > 0;) {
        const int lw = (int)cur.lw[l], lh = (int)cur.lh[l];
        const int x0 = (cx2 - vw / 4) >> (l + 1), x1 = (cx2 + vw / 4) >> (l + 1);
        const int ly0 = y0 >> l, ly1 = std::min(lh,(y1 + (1 << l) - 1) >> l), ystep = std::max(1,(ly1 - ly0) / 60);
        const int cd = bd * 2, ce = be * 2;
        float bv = -1.0f;
        for (int e=-2;e <= 2;e++) for (int d=-2;d <= 2;d++) {
            const float val = VP_Unlike(cur.y[l].data(),prev.y[l].data(),tmp.mask[l].data(),lw,lh,x0,x1,ly0,ly1,ystep,cd + d,ce + e);
            if (val >= 0 && (bv < 0 || val < bv)) { bv = val; bd = cd + d; be = ce + e; }
        }
        if (bv < 0) return 2;
        if (l == 0) {
            whole = (float)bd + VP_Fine(cur.y[0].data(),prev.y[0].data(),lw,lh,x0,x1,ly0,ly1,ystep,bd,be,false);
            wholey = (float)be + VP_Fine(cur.y[0].data(),prev.y[0].data(),lw,lh,x0,x1,ly0,ly1,ystep,bd,be,true);
        }
    }
    if (top == 0) { whole = (float)bd; wholey = (float)be; }
    return 0;
}

/* How the picture moved from prev to cur. The middle of the view first (there a turn moves everything alike,
 * whatever the lens). Then each of 8 strips near where the lens puts it; the movement counts as the view's when
 * most strips agree. A picture whose strips mostly stayed did not move, whatever a monster or a door did in it.
 * pview: the view's place in the picture as far as it is known; row0, row1: the rows of it to measure (the caller
 * leaves out bands known to scroll at a rate of their own); strict: the strips have to fit the lens. */
void VP_Measure(const VPPlane &cur,const VPPlane &prev,const VPLens &lens,const VPView &pview,int row0,int row1,bool strict,const uint8_t *still,VPMotion &mo,VPScratch &tmp) {
    const int w = (int)cur.w, h = (int)cur.h;
    const float unitx = (float)w / 320.0f;
    if (cur.levels == 0 || cur.levels != prev.levels || cur.ystep != prev.ystep || cur.h != prev.h) { mo.why = 2; return; }
    VPView v;       /* in rows of the plane (every other row of a picture whose rows come twice) */
    v.x0 = std::max(0,std::min(w - 8,pview.x0)); v.x1 = std::max(v.x0 + 8,std::min(w,pview.x1));
    v.y0 = std::max(0,std::min(h - 8,pview.y0 / (int)cur.ystep)); v.y1 = std::max(v.y0 + 8,std::min(h,(pview.y1 + (int)cur.ystep - 1) / (int)cur.ystep));
    const int r0 = std::max(v.y0,std::min(v.y1 - 8,row0 / (int)cur.ystep)), r1 = std::max(r0 + 8,std::min(v.y1,(row1 + (int)cur.ystep - 1) / (int)cur.ystep));

    /* The pixels that changed, in every size of the picture. */
    for (unsigned l=0;l < cur.levels;l++) {
        const size_t n = (size_t)cur.lw[l] * cur.lh[l];
        tmp.mask[l].resize(n);
        const uint8_t *a = cur.y[l].data(), *b = prev.y[l].data();
        uint8_t *m = tmp.mask[l].data();
        const int tau = l ? 3 : 4;
        if (l == 0 && still != NULL) {
            /* (the pixels known to stay are kept for the whole picture, the plane may hold every other row of it) */
            for (int y=0;y < h;y++) {
                const uint8_t *st = still + (size_t)(y * (int)cur.ystep) * w;
                const size_t at = (size_t)y * w;
                for (int x=0;x < w;x++) {
                    const int df = (int)a[at + x] - (int)b[at + x];
                    m[at + x] = (uint8_t)(((df > tau) | (df < -tau)) & (st[x] < VP_STILL));
                }
            }
        }
        else for (size_t i=0;i < n;i++) { const int df = (int)a[i] - (int)b[i]; m[i] = (uint8_t)((df > tau) | (df < -tau)); }
    }
    mo.masked = true;
    {
        /* the part of the view that changed: with less than a twentieth the view stayed, whatever moved in it */
        size_t on = 0;
        const uint8_t *m0 = tmp.mask[0].data();
        for (int y=v.y0;y < v.y1;y++) for (int x=v.x0;x < v.x1;x++) on += m0[(size_t)y * w + x];
        mo.changed = (float)on / (float)((size_t)(v.x1 - v.x0) * (size_t)(v.y1 - v.y0));
    }
    if (mo.changed < 0.05f) { mo.okx = mo.oky = true; return; }

    const int rm = VP_Middle(cur,prev,v,r0,r1,tmp,mo.whole,mo.wholey,mo.fit);
    if (rm != 0) { mo.why = rm; return; }

    /* The strips, in the full picture or (a wide one) the half-size copy. */
    const unsigned sl = (w >= 512 && cur.levels > 1) ? 1 : 0;
    const int sw = (int)cur.lw[sl], sh = (int)cur.lh[sl], sc = 1 << sl;
    const int sx0 = v.x0 >> sl, sx1 = v.x1 >> sl, sy0 = r0 >> sl, sy1 = std::min(sh,(r1 + sc - 1) >> sl), systep = std::max(1,(sy1 - sy0) / 60);
    const int se = (int)lroundf(mo.wholey / (float)sc);
    const uint8_t *m = tmp.mask[sl].data();
    for (unsigned s=0;s < VP_STRIPS;s++) {
        const int x0 = sx0 + (int)(s * (unsigned)(sx1 - sx0) / VP_STRIPS), x1 = sx0 + (int)((s + 1) * (unsigned)(sx1 - sx0) / VP_STRIPS);
        unsigned on = 0, all = 0;
        for (int y=sy0;y < sy1;y += systep) for (int x=x0;x < x1;x++) { on += m[(size_t)y * sw + x]; all++; }
        if (on * 50 < all) { mo.sk[s] = 2; mo.stayed++; continue; }
        const float xc = (float)(x0 + x1) * 0.5f * (float)sc - 0.5f;
        const float want = (lens.persp ? xc - VP_SourceX(lens,xc,mo.whole) : mo.whole) / (float)sc;
        float lo, hi;
        if (lens.known) { lo = want - 6.0f; hi = want + 6.0f; }
        else { lo = std::min(0.8f * want,2.4f * want) - 6.0f; hi = std::max(0.8f * want,2.4f * want) + 6.0f; }
        const int ilo = std::max(-sw / 2,(int)floorf(lo)), ihi = std::min(sw / 2,(int)ceilf(hi));
        if (ihi - ilo < 6) continue;
        tmp.vals.assign((size_t)(ihi - ilo + 1),-1.0f);
        tmp.sorted.resize(tmp.vals.size());
        int sb = ilo;
        float sv = -1.0f;
        unsigned nv = 0;
        for (int d=ilo;d <= ihi;d++) {
            const float val = VP_Unlike(cur.y[sl].data(),prev.y[sl].data(),m,sw,sh,x0,x1,sy0,sy1,systep,d,se);
            tmp.vals[(size_t)(d - ilo)] = val;
            if (val < 0) continue;
            tmp.sorted[nv++] = val;
            if (sv < 0 || val < sv) { sv = val; sb = d; }
        }
        if (nv < 7) continue;
        const float smed = VP_Median(tmp.sorted.data(),nv);
        if (smed <= 0 || sv > 0.7f * smed) continue;
        const float off = VP_Fine(cur.y[sl].data(),prev.y[sl].data(),sw,sh,x0,x1,sy0,sy1,systep,sb,se,false);
        mo.sd[s] = ((float)sb + off) * (float)sc;
        mo.sk[s] = 1;
        mo.measured++;
    }
    if (mo.stayed >= 5) { mo.okx = mo.oky = true; mo.whole = mo.wholey = 0; return; }        /* the view stayed; something in it moved */
    if (mo.measured < 3) { mo.why = 3; return; }
    double sum = 0;
    unsigned same = 0;
    for (unsigned s=0;s < VP_STRIPS;s++) {
        if (mo.sk[s] != 1) continue;
        const float xc = (float)v.x0 + ((float)s + 0.5f) * (float)(v.x1 - v.x0) / (float)VP_STRIPS - 0.5f;
        /* whatever the lens: the same way as the middle, between a little less and three times as far */
        const bool way = fabsf(mo.sd[s] - mo.whole) <= 1.5f * unitx
            || (mo.sd[s] * mo.whole > 0 && fabsf(mo.sd[s]) >= 0.6f * fabsf(mo.whole) && fabsf(mo.sd[s]) <= 3.0f * fabsf(mo.whole));
        if (way) same++;
        /* (low in the view a turn also moves things up and down, more the further to the side, and a floor's lines
         * then seem to move less far sideways than they do: where only the ground is left to measure, below a sky,
         * the strips only have to go the same way, and the middle, where none of that happens, is the answer) */
        float p = mo.whole;
        const bool fits = (lens.known && strict) ? (VP_CentreShift(lens,xc,mo.sd[s],p) && fabsf(p - mo.whole) <= std::max(unitx,0.2f * fabsf(mo.whole))) : way;
        if (fits) { mo.agreed++; sum += p; }
    }
    mo.oneway = same * 10 >= mo.measured * 8;
    if (mo.agreed * 10 < mo.measured * 6) { mo.why = 3; return; }
    mo.okx = mo.oky = true;
    mo.sx = (lens.persp && strict) ? 0.5f * (mo.whole + (float)(sum / mo.agreed)) : mo.whole;
    mo.sy = mo.wholey * (float)cur.ystep;      /* in rows of the picture */

}

/* Which bands of the view (6, one above the other) scroll evenly: a sky or a backdrop that an engine moves by the
 * same amount in every column, at a rate of its own, where a turn moves the world as the lens says (further at the
 * sides, and there also up or down). For each band both are tried against an earlier frame, some frames back, in
 * which the view's middle moved (dx,dy): the lens (lens, or 90 degrees while none is known), and an even scroll at
 * the rate that fits best between half and twice the middle's. The one that leaves clearly less difference in the
 * band's 8 cells gets the band's vote; cells in the middle, where the two say the same, count for little by
 * themselves. vote: 1 even (rate: its rate), -1 lens, 0 no telling. still: the pixels that stay, or NULL. */
void VP_Bands(const VPPlane &cur,const VPPlane &old,const VPLens &lens,const VPView &pview,float dx,float dy,const uint8_t *still,int *vote,float *rate,VPScratch &tmp) {
    for (unsigned b=0;b < VP_CELLROWS;b++) { vote[b] = 0; rate[b] = 1; }
    const int w = (int)cur.w, h = (int)cur.h;
    if (cur.levels == 0 || cur.levels != old.levels || cur.ystep != old.ystep || cur.h != old.h || cur.w != old.w) return;
    VPView v;
    v.x0 = std::max(0,std::min(w - 8,pview.x0)); v.x1 = std::max(v.x0 + 8,std::min(w,pview.x1));
    v.y0 = std::max(0,std::min(h - 8,pview.y0 / (int)cur.ystep)); v.y1 = std::max(v.y0 + 8,std::min(h,(pview.y1 + (int)cur.ystep - 1) / (int)cur.ystep));
    VPLens ln = lens;
    if (!ln.known) { ln.known = ln.persp = true; ln.f = (float)(v.x1 - v.x0) * 0.5f; ln.cx = (float)(v.x0 + v.x1 - 1) * 0.5f; }
    if (!ln.persp) return;      /* a view that moves as one piece has no layers to tell apart this way */
    const unsigned sl = (w >= 512 && cur.levels > 1) ? 1 : 0;
    const int sw = (int)cur.lw[sl], sh = (int)cur.lh[sl], sc = 1 << sl;
    const int sx0 = v.x0 >> sl, sx1 = v.x1 >> sl;
    const int ch = (v.y1 - v.y0) / (int)VP_CELLROWS;
    if (ch < 6) return;
    /* the pixels that changed between the two */
    tmp.maskc.resize((size_t)sw * sh);
    {
        const uint8_t *a = cur.y[sl].data(), *b = old.y[sl].data();
        uint8_t *m = tmp.maskc.data();
        for (int y=0;y < sh;y++) {
            const uint8_t *st = still ? still + (size_t)(y * sc * (int)cur.ystep) * w : NULL;
            const size_t at = (size_t)y * sw;
            for (int x=0;x < sw;x++) {
                const int df = (int)a[at + x] - (int)b[at + x];
                m[at + x] = (uint8_t)(((df > 4) | (df < -4)) & (st == NULL || st[x * sc] < VP_STILL));
            }
        }
    }
    const uint8_t *m = tmp.maskc.data();
    const float cy = (float)(v.y0 + v.y1 - 1) * 0.5f;      /* rows of the plane */
    const float ey = dy / (float)cur.ystep;                 /* the view's own movement down, in rows of the plane */
    const int RATES = 31;                                   /* 0.5, 0.55 ... 2.0 */
    for (unsigned b=0;b < VP_CELLROWS;b++) {
        const int by0 = v.y0 + (int)b * ch, by1 = v.y0 + (int)(b + 1) * ch;
        const int cy0 = by0 >> sl, cy1 = std::min(sh,by1 >> sl), cstep = std::max(1,(cy1 - cy0) / 24);
        const float yc = (float)(by0 + by1 - 1) * 0.5f;
        float lensleft[VP_STRIPS], evenleft[VP_STRIPS][31];
        bool have[VP_STRIPS];
        unsigned n = 0, outer = 0;
        for (unsigned s=0;s < VP_STRIPS;s++) {
            const int x0 = sx0 + (int)(s * (unsigned)(sx1 - sx0) / VP_STRIPS), x1 = sx0 + (int)((s + 1) * (unsigned)(sx1 - sx0) / VP_STRIPS);
            const float xc = (float)(x0 + x1) * 0.5f * (float)sc - 0.5f;
            /* as the lens has it: sideways by the column, and up or down by how far the cell is from the middle row */
            const float xs = VP_SourceX(ln,xc,dx);
            const float k = VP_RowScale(ln,xs,dx);
            const float ld = (xc - xs) / (float)sc, le = (yc - (cy + (yc - ey - cy) * k)) / (float)sc;
            lensleft[s] = VP_Unlike(cur.y[sl].data(),old.y[sl].data(),m,sw,sh,x0,x1,cy0,cy1,cstep,(int)lroundf(ld),(int)lroundf(le));
            have[s] = lensleft[s] >= 0;
            for (int r=0;r < RATES && have[s];r++) {
                const float ed = (0.5f + 0.05f * (float)r) * dx / (float)sc;
                evenleft[s][r] = VP_Unlike(cur.y[sl].data(),old.y[sl].data(),m,sw,sh,x0,x1,cy0,cy1,cstep,(int)lroundf(ed),(int)lroundf(ey / (float)sc));
                if (evenleft[s][r] < 0) have[s] = false;
            }
            if (!have[s]) continue;
            n++;
            if (s < 2 || s >= VP_STRIPS - 2) outer++;
        }
        if (n < 4 || outer < 2) continue;      /* (in the middle columns the two say the same) */
        double byLens = 0, byEven = -1;
        int bestr = 0;
        for (unsigned s=0;s < VP_STRIPS;s++) if (have[s]) byLens += lensleft[s];
        for (int r=0;r < RATES;r++) {
            double sum = 0;
            for (unsigned s=0;s < VP_STRIPS;s++) if (have[s]) sum += evenleft[s][r];
            if (byEven < 0 || sum < byEven) { byEven = sum; bestr = r; }
        }
        rate[b] = 0.5f + 0.05f * (float)bestr;
        /* (an even scroll at the middle's own rate is what a dark ceiling with little in it looks like too: only a
         * rate that is clearly another one counts) */
        if (byEven <= 0.7 * byLens && fabsf(rate[b] - 1.0f) >= 0.1f) vote[b] = 1;
        else if (byLens <= 0.7 * byEven) vote[b] = -1;
    }
}

/* One direction (sideways, or up and down): what the mouse does to the picture, learned and checked. */
struct VPAxis {
    double a[VP_LAGS], b[VP_LAGS], c;   /* fading sums per candidate delay: movement x counts, counts squared; movement squared */
    unsigned n, rejected;
    bool known;                 /* the gain and the delay have been learned */
    double gain;                /* pixels of picture per mouse count */
    double delay;               /* the game's own delay, microseconds */
    unsigned lag;               /* ...and its candidate */
    float q;                    /* the gate: up with every stretch of frames that agreed with the prediction... */
    bool open;                  /* ...and open while that is the rule */
    float strength;             /* how much of the prediction is shown (it fades in and out) */
    double st_err, st_base;     /* for the log: how far off the prediction was, how far the picture moved */
    unsigned st_n, st_open;

    VPAxis() { reset(); }
    void reset(void) {
        memset(a,0,sizeof(a)); memset(b,0,sizeof(b));
        c = 0; n = rejected = 0; known = false; gain = delay = 0; lag = 0; q = 0; open = false; strength = 0;
        st_err = st_base = 0; st_n = st_open = 0;
    }
    void gate(bool ok) {
        q = ok ? std::min(1.0f,q + 0.34f) : std::max(0.0f,q - 0.5f);
        if (q >= 0.66f) open = true;
        else if (q < 0.33f) open = false;
    }
    void close(void) { q = 0; open = false; }
};

struct VPSample {               /* a frame against the one analysed before it */
    uint64_t c0, c1;            /* the moments up to which the two show the input */
    uint32_t q0, q1;            /* ...and the newest input each had */
    bool okx, oky;
    float sx, sy;
};

struct VPMouseEv { uint64_t t; uint32_t seq; double x, y; };    /* all the mouse counts sent up to and including this input event */

/* What one key does to the view (in one combination of Shift, Ctrl and Alt): pixels of picture per millisecond, by
 * how long the key has been held (the first 11) or let go (the last 5). A turn key gives a steady value once it is
 * held, often less at first (games turn slowly, then faster); most keys give nothing. The game's own delay is part
 * of the curve: nothing in the first stretches, and still something after the key was let go. */
struct VPKeyModel {
    float mx, my;               /* while held, whatever for how long: learned within a few frames... */
    float vx[VP_KEYBINS], vy[VP_KEYBINS];      /* ...and what each stretch adds to that (the first, the ones after) */
    unsigned n;                 /* frames learned from */
    float q;                    /* the gate, as for the mouse */
    bool open;
};
struct VPKeyHist {              /* the last presses and releases of a key, oldest first */
    uint64_t t[VP_KEYHIST];
    bool down[VP_KEYHIST];
    unsigned n;
};
const uint32_t VP_KEYEDGE[VP_KEYBINS + 1] = {   /* milliseconds */
    0, 20, 40, 60, 90, 130, 180, 250, 350, 500, 750, 0xFFFFFFFFu,       /* held */
    20, 40, 60, 90, 130                                                 /* let go: 0-20, ... 90-130 */
};

class ViewPredict {
public:
    bool enabled = true;        /* the player's switch for all of it ([inputnet] predict) */
    bool moveView = false;      /* ...and for moving the picture ([inputnet] predict view, Ctrl+F12). Off unless asked
                                 * for: a first-person view cannot be moved ahead of the host without showing it.
                                 * Whatever stays on the screen (a weapon, a crosshair, a message) either moves with
                                 * the view and jumps back with the next frame, or is held in place, and then what it
                                 * covered has to be made up. A player saw both (Duke Nukem 3D and Doom, November 2026).
                                 * Without it the picture is shown exactly as the host sent it. */
    int debug = 0;              /* NN_RP_PREDICTLOG: 1 what was learned every five seconds, 2 every frame */
    unsigned dbg_pairs = 0, dbg_same = 0, dbg_okx = 0, dbg_oky = 0, dbg_moved = 0, dbg_regx = 0, dbg_regy = 0;
    unsigned w = 0, h = 0, fmt = 0;
    VPLens lens;
    VPAxis ax, ay;

    void resize(unsigned fw,unsigned fh,unsigned ffmt) {
        const bool samesize = fw == w && fh == h;
        w = fw; h = fh; fmt = ffmt;
        unitx = (float)w / 320.0f; unity = (float)h / 200.0f;
        for (int i=0;i < VP_KEPT;i++) planes[i].valid = false;
        samples.clear();
        if (samesize) return;   /* only the colours changed: what was learned still holds */
        still.assign((size_t)w * h,0);
        still_at.assign((size_t)w * h,0);
        world_at.assign((size_t)w * h,0);
        still_dirty = true;
        rowch.clear(); colch.clear(); rowcg.clear(); colcg.clear(); view_frames = 0;
        cand_x0 = cand_x1 = cand_y0 = cand_y1 = -1; cand_n = 0; zoom_on = false; box_known = false;
        vx0 = vy0 = 0; vx1 = (int)w; vy1 = (int)h;
        memset(band_score,0,sizeof(band_score));
        lens_clean = cells_done = 0;
        layers_at = 0;
        ptr_q = 0;
        lens = VPLens();
        memset(lens_mean,0,sizeof(lens_mean)); memset(lens_var,0,sizeof(lens_var)); memset(lens_n,0,sizeof(lens_n));
        lens_frames = 0;
        ax.reset(); ay.reset();
    }

    uint32_t prevId(bool &valid) const { valid = planes[cur].valid; return planes[cur].id; }
    void viewRect(int &x0,int &x1,int &y0,int &y1) const { x0 = vx0; x1 = vx1; y0 = vy0; y1 = vy1; }     /* (for tests) */
    bool stays(size_t i) const { return i < stay.size() && stay[i] != 0; }
    bool hasSample(void) const { return !samples.empty(); }
    const VPSample &lastSample(void) const { return samples.back(); }

    /* Mouse counts sent to the host at t, as input event seq. */
    void mouse(uint64_t t,uint32_t seq,float dx,float dy) {
        totx += dx; toty += dy;
        VPMouseEv e = { t, seq, totx, toty };
        mlog.push_back(e);
        while (mlog.size() > 2 && t - mlog.front().t > 3000000) {
            basex = mlog.front().x; basey = mlog.front().y;
            mlog_cut = true; mlog_cut_seq = mlog.front().seq;
            mlog.pop_front();
        }
        logEvent(t,seq);
    }

    /* A key pressed or let go at t, as input event seq. Keys that open menus close the gates: what the mouse and the
     * keys do there is not what they did in the game. */
    void key(unsigned k,bool down,uint64_t t,uint32_t seq) {
        logEvent(t,seq);
        if (k == 0 || k >= (unsigned)KBD_LAST) return;
        if (khist.empty()) {
            khist.resize((size_t)KBD_LAST);
            for (size_t i=0;i < khist.size();i++) khist[i].n = 0;
        }
        VPKeyHist &hst = khist[k];
        if (hst.n == VP_KEYHIST) {
            for (unsigned i=1;i < VP_KEYHIST;i++) { hst.t[i-1] = hst.t[i]; hst.down[i-1] = hst.down[i]; }
            hst.n--;
        }
        hst.t[hst.n] = t; hst.down[hst.n] = down; hst.n++;
        if (!down) return;
        if (k == (unsigned)KBD_esc || k == (unsigned)KBD_tab || k == (unsigned)KBD_pause || (k >= (unsigned)KBD_f1 && k <= (unsigned)KBD_f12)) {
            ax.close(); ay.close();
            for (size_t i=0;i < kmodel.size();i++) { kmodel[i].q = 0; kmodel[i].open = false; }
        }
    }

    /* The frame now on the screen: up to when it shows the input (if known), the newest input the host had applied
     * when it took the frame (0xFFFFFFFF: not known), and when it arrived. */
    void shown(bool hasc,uint64_t c,uint32_t seq,uint64_t arrival) {
        shown_seq = seq;
        if (shown_arrival != 0 && arrival > shown_arrival) {
            const uint64_t d = std::min<uint64_t>(arrival - shown_arrival,100000);
            ival = ival ? (ival * 7 + d) / 8 : d;
        }
        shown_hasc = hasc; shown_c = c; shown_arrival = arrival;
    }

    /* A decoded frame, and the frame analysed before it when that is still at hand. */
    void frame(uint32_t id,const uint8_t *raw,size_t rawsize,unsigned fw,unsigned fh,unsigned ffmt,const uint8_t *prevraw,bool hasc,uint64_t c,uint32_t seq,const VPPointer &ptr) {
        const uint64_t t0 = RP_Micros();
        if (fw != w || fh != h || ffmt != fmt) { resize(fw,fh,ffmt); prevraw = NULL; }
        VPPlane &pl = planes[(cur + 1) % VP_KEPT];
        const VPPlane &prev = planes[cur];
        VP_BuildPlane(pl,raw,w,h,fmt);
        pl.id = id; pl.hasc = hasc; pl.c = c; pl.seq = seq;
        pl.ptr = ptr;
        pl.sumx = pl.sumy = 0; pl.chain = 0;
        if (prev.valid && prevraw != NULL) {
            VPMotion mo;
            mo.same = memcmp(raw,prevraw,rawsize) == 0;
            if (mo.same) { mo.okx = mo.oky = true; pl.sumx = prev.sumx; pl.sumy = prev.sumy; pl.chain = prev.chain; }
            else {
                const VPView pv = { vx0, vx1, vy0, vy1 };
                int row0, row1;
                mo.layered = layerState(t0,row0,row1) == 2;
                VP_Measure(pl,prev,lens,pv,row0,row1,lens.known && !mo.layered,still.empty() ? NULL : still.data(),mo,tmp);
                /* The cells, a few times a second: against the frame furthest back that the view could be followed
                 * from, as long as it has not moved more than a tenth of its width since. */
                if (!mo.okx) chain++;
                pl.sumx = prev.sumx + (mo.okx ? mo.sx : 0.0f); pl.sumy = prev.sumy + (mo.okx ? mo.sy : 0.0f);
                pl.chain = chain;
                if (mo.okx && prev.chain == chain && t0 > cells_at + 120000) {
                    const VPPlane *old = NULL;
                    float dx = 0, dy = 0;
                    for (int k=1;k < VP_KEPT;k++) {
                        const VPPlane &q = planes[(cur + 1 + VP_KEPT - k) % VP_KEPT];
                        if (!q.valid || q.chain != chain || q.h != pl.h || q.w != pl.w) break;
                        const float qx = (float)(pl.sumx - q.sumx);
                        if (fabsf(qx) > (float)w / 10.0f) break;
                        if (fabsf(qx) >= 6.0f * unitx && fabsf(qx) > fabsf(dx)) { old = &q; dx = qx; dy = (float)(pl.sumy - q.sumy); }
                    }
                    if (old != NULL) {
                        VP_Bands(pl,*old,lens,pv,dx,dy,still.empty() ? NULL : still.data(),mo.bandvote,mo.bandrate,tmp);
                        mo.bands = true;
                        cells_at = t0;
                        if (cells_done < 1000) cells_done++;
                    }
                }
                markStill(raw,prevraw,mo);
            }
            if (debug >= 2 && mo.bands)
                LOG_MSG("INPUTNET: vp bands %u: votes %d %d %d %d %d %d rates %.2f %.2f %.2f %.2f %.2f %.2f scores %.0f %.0f %.0f %.0f %.0f %.0f",id,
                    mo.bandvote[0],mo.bandvote[1],mo.bandvote[2],mo.bandvote[3],mo.bandvote[4],mo.bandvote[5],
                    mo.bandrate[0],mo.bandrate[1],mo.bandrate[2],mo.bandrate[3],mo.bandrate[4],mo.bandrate[5],
                    band_score[0],band_score[1],band_score[2],band_score[3],band_score[4],band_score[5]);
            judgePointer(pl,prev,mo);
            if (mo.same) dbg_same++;
            if (mo.okx) dbg_okx++;
            if (mo.oky) dbg_oky++;
            if (mo.okx && fabsf(mo.sx) >= 1.5f * unitx) dbg_moved++;
            dbg_regx += mo.measured; dbg_regy += mo.stayed;
            dbg_pairs++;
            if (debug >= 2 && !mo.same)
                LOG_MSG("INPUTNET: vp frame %u: sx %s%.2f sy %s%.2f c %s%.1f ms after the last; middle %.2f %.2f (lower %s%.2f upper %s%.2f%s) fit %.2f changed %.0f%%; strips %u measured %u stayed %u agreed why %d: %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f",
                    id,mo.okx ? "" : "?",mo.sx,mo.oky ? "" : "?",mo.sy,
                    (prev.hasc && hasc) ? "" : "?",(prev.hasc && hasc) ? (double)(int64_t)(c - prev.c) / 1000.0 : 0.0,
                    mo.whole,mo.wholey,"",0.0,"",0.0,mo.layered ? ", layers" : "",
                    mo.fit,mo.changed * 100.0f,mo.measured,mo.stayed,mo.agreed,mo.why,
                    mo.sd[0],mo.sd[1],mo.sd[2],mo.sd[3],mo.sd[4],mo.sd[5],mo.sd[6],mo.sd[7]);
            if (prev.hasc && hasc && c > prev.c && c - prev.c < 400000) {
                VPSample s;
                s.c0 = prev.c; s.c1 = c;
                s.q0 = prev.seq; s.q1 = seq;
                /* a stretch without frames has been judged already (see idle) */
                if (mo.same && idle_c > s.c0 && idle_c < s.c1) s.c0 = idle_c;
                if (!samples.empty() && samples.back().c1 != s.c0) samples.clear();
                s.okx = mo.okx; s.oky = mo.oky; s.sx = mo.sx; s.sy = mo.sy;
                samples.push_back(s);
                if (samples.size() > 16) samples.pop_front();
                learn();
                learnKeys(s);
                /* The lens: only from movement the player's input made (a screen that shakes, a camera that pans,
                 * a level that scrolls by itself say nothing about how a turn moves the picture). */
                if (!mo.same && inputMoved(s)) { learnLens(mo); learnLayers(mo,RP_Micros()); }
            }
            else samples.clear();
        }
        else samples.clear();
        cur = (cur + 1) % VP_KEPT;
        st_frames++;
        st_analyse_us += RP_Micros() - t0;
    }

    /* No frame for a while: the host sends none while its picture stays as it is. If the mouse moved meanwhile and
     * the picture should have followed, the game is not listening to it (a menu, a pause). */
    void idle(uint64_t now) {
        if (!shown_hasc || (!ax.known && !ay.known)) return;
        const uint64_t margin = std::max<uint64_t>(100000,4 * ival);
        if (now < shown_arrival + margin + 100000) return;
        const uint64_t c1 = shown_c + (now - shown_arrival - margin);
        const uint64_t c0 = std::max(shown_c,idle_c);
        if (c1 < c0 + 100000) return;
        VPAxis *axes[2] = { &ax, &ay };
        for (unsigned k=0;k < 2;k++) {
            VPAxis &a = *axes[k];
            if (!a.known) continue;
            double x1, y1, x0, y0;
            const uint64_t d = (uint64_t)a.delay;
            cum(c1 - d,x1,y1); cum(c0 - d,x0,y0);
            if (fabs(a.gain * (k ? y1 - y0 : x1 - x0)) >= 4.0 * (k ? unity : unitx)) a.gate(false);
        }
        idle_c = c1;
    }

    /* ---- The pointer of a game that draws its own (a strategy game, an adventure, a menu) ----
     * Such a pointer lags like everything else in the picture, and a pointer that lags is hard to aim. The host
     * reports with every frame where the DOS mouse driver had its pointer and how far a count of motion moves it;
     * the viewer adds the motion the host had not applied yet and draws a small dot there at once, as it does over
     * a Windows guest. Whether the game shows a pointer at the driver's place at all is found out by looking: when
     * the place moves, the picture has to change around the old and the new place and hardly anywhere else. A game
     * that turns a view with the mouse, or hides its pointer, never passes, and gets no dot.
     * The dot starts from the newest report the host has sent, whichever frame is on the screen: where the pointer
     * is going does not depend on the picture. A report that gets lost on the way (it is a datagram of its own)
     * then only makes the one before it serve a little longer; tied to the frame on the screen, each loss switched
     * the dot off until three more moves had been seen (found between two computers; with 3% of the host's
     * datagrams dropped on one PC the dot was off at 3 of 8 checks that way). */
    void pointerTold(uint32_t id,uint32_t seq,const VPPointer &p) {
        /* (one that comes out of order is left out; ids that start over are a new session of the host) */
        if (told_any && (int32_t)(id - told_id) <= 0 && (int32_t)(told_id - id) < 64) return;
        /* for the log: where the report before this one and the motion sent since said the pointer would be now,
         * against where it is: how far off the dot is once the game's pointer has caught up with it */
        if (told_any && told_ptr.has && p.has && (int32_t)(seq - told_seq) > 0 && !(mlog_cut && (int32_t)(mlog_cut_seq - told_seq) > 0)) {
            double x, y;
            if (pointerFrom(seq,false,x,y)) {
                const double e = sqrt((x - p.x) * (x - p.x) * 320.0 * 320.0 + (y - p.y) * (y - p.y) * 200.0 * 200.0);
                st_ptr_err += e; st_ptr_errn++;
                st_ptr_errmax = std::max(st_ptr_errmax,e);
            }
        }
        told_any = true; told_id = id; told_seq = seq; told_ptr = p;
        if (!p.has) ptr_q = 0;
    }

    /* Where the driver's pointer is after the motion the host had not applied when it made its newest report (what
     * it had applied is in the reported place): up to input event upto, or all of it. Step by step as the driver
     * takes it: it keeps its pointer inside its range at every step, so a pointer pushed on against an edge leaves
     * the edge at once when the hand turns back. False: no motion in between. */
    bool pointerFrom(uint32_t upto,bool all,double &x,double &y) const {
        x = told_ptr.x; y = told_ptr.y;
        size_t first = mlog.size();
        while (first > 0 && (int32_t)(mlog[first-1].seq - told_seq) > 0) first--;
        bool any = false;
        for (size_t i=first;i < mlog.size();i++) {
            if (!all && (int32_t)(mlog[i].seq - upto) > 0) break;
            const double dx = mlog[i].x - (i ? mlog[i-1].x : basex), dy = mlog[i].y - (i ? mlog[i-1].y : basey);
            x = std::max(0.0,std::min(1.0,x + dx * (fabs(dx) > 1.0 ? told_ptr.gxs : told_ptr.gx1)));
            y = std::max(0.0,std::min(1.0,y + dy * (fabs(dy) > 1.0 ? told_ptr.gys : told_ptr.gy1)));
            any = true;
        }
        return any;
    }

    /* Where the dot goes now (parts of the picture's width and height); false: no dot. accx, accy: counts not even sent. */
    bool pointerAt(float accx,float accy,float &nx,float &ny) {
        const bool on = pointerWhere(accx,accy,nx,ny);
        /* for the log: how much of the time a pointer the host reports had its dot */
        const uint64_t now = RP_Micros();
        if (dot_t != 0 && told_any && told_ptr.has) {
            const uint64_t dt = std::min<uint64_t>(now - dot_t,100000);
            st_dot_us += dt;
            if (dot_was) st_dot_on_us += dt;
        }
        dot_t = now; dot_was = on;
        return on;
    }

    bool pointerWhere(float accx,float accy,float &nx,float &ny) const {
        if (!enabled || !told_any || !told_ptr.has || ptr_q < 0.7f) return false;
        if (mlog_cut && (int32_t)(mlog_cut_seq - told_seq) > 0) return false;     /* (motion since the report is no longer remembered) */
        double x, y;
        pointerFrom(0,true,x,y);
        x += accx * (fabsf(accx) > 1.0f ? told_ptr.gxs : told_ptr.gx1);
        y += accy * (fabsf(accy) > 1.0f ? told_ptr.gys : told_ptr.gy1);
        nx = (float)std::max(0.0,std::min(1.0,x));
        ny = (float)std::max(0.0,std::min(1.0,y));
        return true;
    }

    /* How far the picture has to be moved now for the input its frame does not show yet (accx, accy: counts not
     * even sent). False when there is nothing to move. */
    bool offset(uint64_t now,float accx,float accy,float &px,float &py) {
        px = py = 0;
        const float step = (float)std::min<uint64_t>(now > str_t ? now - str_t : 0,1000000) / 60000.0f;
        str_t = now;
        const bool live = enabled && shown_hasc && lens.known && now < shown_arrival + 600000;
        VPAxis *axes[2] = { &ax, &ay };
        for (unsigned k=0;k < 2;k++) {
            VPAxis &a = *axes[k];
            const float want = (live && a.known && a.open) ? 1.0f : 0.0f;
            a.strength += std::max(-step,std::min(step,want - a.strength));
        }
        if (!shown_hasc) return false;
        if (ax.strength > 0 && ax.known) {
            double x, y;
            cum(bound(shown_c - (uint64_t)ax.delay,shown_seq),x,y);
            px = (float)(ax.gain * (totx - x + accx)) * ax.strength;
        }
        if (ay.strength > 0 && ay.known) {
            double x, y;
            cum(bound(shown_c - (uint64_t)ay.delay,shown_seq),x,y);
            py = (float)(ay.gain * (toty - y + accy)) * ay.strength;
        }
        kstrength += std::max(-step,std::min(step,(live ? 1.0f : 0.0f) - kstrength));
        if (kstrength > 0) {
            float kx, ky;
            keyOffset(now,kx,ky);
            px += kx * kstrength; py += ky * kstrength;
        }
        /* no further than the margin the view is shown larger by: see warp */
        int mx, my;
        margins(mx,my);
        px = std::max(-(float)mx,std::min((float)mx,px));
        py = std::max(-(float)my,std::min((float)my,py));
        return fabsf(px) >= 0.5f || fabsf(py) >= 0.5f;
    }

    /* Whether the view is shown larger (see warp), and its rectangle: once the view is known to turn in perspective
     * (a first-person game), from then on, so that it does not change size when prediction comes and goes. */
    bool boxKnown(void) const { return box_known; }
    bool zoomed(void) const {
        /* Once on it stays on for the video mode: a lens measured again the other way, or a moment without one, would
         * otherwise switch the enlargement off and on, which is a jump of the whole picture. */
        if (enabled && moveView && box_known && lens.known && lens.persp) zoom_on = true;
        return enabled && moveView && zoom_on && box_known && vx1 - vx0 >= (int)w / 4 && vy1 - vy0 >= (int)h / 4;
    }
    void viewBox(int &x0,int &y0,int &x1,int &y1) const { x0 = vx0; y0 = vy0; x1 = vx1; y1 = vy1; }
    void margins(int &mx,int &my) const {
        mx = std::max(1,(int)floorf(VP_MARGIN * (float)(vx1 - vx0)));
        my = std::max(1,(int)floorf(VP_MARGIN * (float)(vy1 - vy0)));
    }

    /* The picture src (w x h pixels) moved by (px,py), rounded to whole pixels, into dst. Nothing is drawn that the
     * frame does not have, and nothing is resampled: the view is moved as one piece, by whole pixels as games move it,
     * and the viewer shows it a little larger (VP_MARGIN at each side, see ViewerSession::present), so a movement of
     * up to that much uncovers nothing on the screen. (Filling an uncovered edge with the view mirrored, with pictures
     * kept by direction or with a smear, and moving the sides less than the middle, were each tried and taken out
     * after a player saw them; a turn in perspective moves the sides a little further than the middle, which this
     * leaves to the next frame.) Everything inside the view moves with it, the crosshair, the letters of a message
     * and the weapon too: holding what stays in place uncovered what it had hidden and drew its copy in the moved
     * picture, and the pixels guessed for both made marks between the letters of a message (Duke Nukem 3D,
     * November 2026). So every pixel shown is a pixel of the frame, moved as one piece with all the others. */
    void warp(const uint32_t *src,uint8_t *dst,int pitch,float px,float py) {
        const uint64_t t0 = RP_Micros();
        const int iw = (int)w, ih = (int)h;
        if (!box_known || !moveView) px = py = 0;    /* (predicted() says no first: see box_known, moveView) */
        int mx, my;
        margins(mx,my);
        const int sx = std::max(-mx,std::min(mx,(int)lroundf(px))), sy = std::max(-my,std::min(my,(int)lroundf(py)));
        for (int y=0;y < ih;y++) {
            uint32_t *o = (uint32_t*)(dst + (size_t)y * (size_t)pitch);
            const uint32_t *in = src + (size_t)y * w;
            if (y < vy0 || y >= vy1 || (sx == 0 && sy == 0)) { memcpy(o,in,(size_t)w * 4); continue; }
            if (vx0 > 0) memcpy(o,in,(size_t)vx0 * 4);
            if (vx1 < iw) memcpy(o + vx1,in + vx1,(size_t)(iw - vx1) * 4);
            /* (the clamping reaches only the margin, which the viewer leaves off the screen: see zoomCrop) */
            const uint32_t *srow = src + (size_t)std::max(vy0,std::min(vy1 - 1,y - sy)) * w;
            for (int x=vx0;x < vx1;x++) o[x] = srow[std::max(vx0,std::min(vx1 - 1,x - sx))];
        }
        st_warps++;
        st_warp_us += RP_Micros() - t0;
    }

    /* Every five seconds, for the log. */
    void report(void) {
        if (!enabled || st_frames == 0) return;
        if (debug) {
            double best = 0; unsigned bj = 0;
            for (unsigned j=0;j < VP_LAGS;j++) if (ax.b[j] > 1e-9) { const double e = ax.a[j] * ax.a[j] / ax.b[j]; if (e > best) { best = e; bj = j; } }
            LOG_MSG("INPUTNET: vp debug: %u frames, %u pairs (%u same, %u with x, %u with y, %u moved; strips measured %.1f stayed %.1f a pair); lens frames %u fit a %.3f b %.3f c %.3f rms %.3f spread %.3f means %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f; "
                "x: n %u c %.1f best lag %u explains %.2f gain %.4f q %.2f rejected %u; y: n %u c %.1f; mouse total %.0f %.0f",
                st_frames,dbg_pairs,dbg_same,dbg_okx,dbg_oky,dbg_moved,dbg_pairs ? (double)dbg_regx / dbg_pairs : 0.0,dbg_pairs ? (double)dbg_regy / dbg_pairs : 0.0,
                lens_frames,lens_fit_a,lens_fit_b,lens_fit_c,lens_fit_rms,lens_fit_spread,
                lens_mean[0],lens_mean[1],lens_mean[2],lens_mean[3],lens_mean[4],lens_mean[5],lens_mean[6],lens_mean[7],
                ax.n,ax.c,bj,ax.c > 0 ? best / ax.c : 0.0,ax.b[bj] > 1e-9 ? ax.a[bj] / ax.b[bj] : 0.0,(double)ax.q,ax.rejected,ay.n,ay.c,totx,toty);
            dbg_pairs = dbg_same = dbg_okx = dbg_oky = dbg_moved = dbg_regx = dbg_regy = 0;
        }
        const bool was = said_ready;
        if (lens.known && (ax.known || kst_open) && !said_ready) said_ready = true;
        if (st_ptr_hit + st_ptr_miss) {
            char shownText[48] = "", errText[144] = "";
            if (st_dot_us >= 500000) snprintf(shownText,sizeof(shownText)," (shown %u%% of the time)",(unsigned)(st_dot_on_us * 100u / st_dot_us));
            if (st_ptr_errn) snprintf(errText,sizeof(errText),"; the pointer came to be %.2f pixels (of 320x200) from the dot on average, at most %.2f (%u reports)",
                st_ptr_err / st_ptr_errn,st_ptr_errmax,st_ptr_errn);
            LOG_MSG("INPUTNET: pointer dot %s%s: the game's pointer was where the mouse driver has it in %u of %u moves%s",
                ptr_q >= 0.7f ? "on" : "off",shownText,st_ptr_hit,st_ptr_hit + st_ptr_miss,errText);
        }
        st_ptr_hit = st_ptr_miss = 0;
        st_dot_us = st_dot_on_us = 0;
        st_ptr_err = st_ptr_errmax = 0; st_ptr_errn = 0;
        if (!lens.known && !ax.known && !ay.known && kst_n == 0) { clearStats(); return; }
        if (!moveView && !debug) { clearStats(); return; }     /* (learned all the same, so Ctrl+F12 works at once) */
        char lensText[64];
        if (!lens.known) snprintf(lensText,sizeof(lensText),"not known yet");
        else if (lens.persp) snprintf(lensText,sizeof(lensText),"%.0f degrees wide%s",2.0 * atan((double)(vx1 - vx0) * 0.5 / lens.f) * 180.0 / 3.14159265358979,lens.assumed ? " (taken to be)" : "");
        else snprintf(lensText,sizeof(lensText),"flat");
        char layers[80];
        layers[0] = 0;
        {
            unsigned nb = 0; float rsum = 0;
            const uint64_t nowus = RP_Micros();
            for (unsigned b=0;b < VP_CELLROWS;b++) if (evenBand(b,nowus)) { nb++; rsum += band_rate[b]; }
            if (nb && anyLayer(nowus)) snprintf(layers,sizeof(layers),", %u of 6 bands scroll evenly (at %.2f of the ground's rate)",nb,rsum / nb);
        }
        if (still_dirty || stay.size() != still.size()) cleanStill();
        unsigned stillpx = 0;
        for (size_t i=0;i < stay.size();i++) stillpx += stay[i];
        char turn[160], look[160], keys[200];
        describe(ax,turn,sizeof(turn));
        describe(ay,look,sizeof(look));
        unsigned nkeys = 0;
        for (size_t i=0;i < kmodel.size();i++) if (kmodel[i].open && kmodel[i].n >= 20) {
            const float most = std::max(fabsf(kmodel[i].mx + kmodel[i].vx[10]),fabsf(kmodel[i].my + kmodel[i].vy[10]));
            if (most * 1000.0f >= 10.0f * unitx) nkeys++;      /* moves the view by 10 pixels a second or more */
        }
        if (kst_n) snprintf(keys,sizeof(keys),"%u keys that move the view (%u stretches, %u%% shown): off by %.1f px where the picture moved %.1f",
            nkeys,kst_n,kst_open * 100u / kst_n,kst_err / kst_n,kst_base / kst_n);
        else snprintf(keys,sizeof(keys),"%u keys that move the view",nkeys);
        LOG_MSG("INPUTNET: view prediction%s%s: mouse turning %s; looking up and down %s; %s; lens %s%s; view %dx%d at %d,%d, %u%% of the picture stays in place; "
            "%.2f ms a frame to learn, %.2f ms to move the picture (%u times)",
            moveView ? "" : " (off, the picture is not moved)",was ? "" : (said_ready ? " ready" : " learning"),turn,look,keys,lensText,layers,vx1 - vx0,vy1 - vy0,vx0,vy0,
            still.empty() ? 0u : (unsigned)((uint64_t)stillpx * 100u / still.size()),
            st_analyse_us / 1000.0 / st_frames,st_warps ? st_warp_us / 1000.0 / st_warps : 0.0,st_warps);
        clearStats();
    }

private:
    static const int VP_KEPT = 6;
    VPPlane planes[VP_KEPT];
    int cur = 0;                        /* planes[cur]: the frame analysed last; the ones before it follow backwards */
    unsigned chain = 1;
    VPScratch tmp;
    std::vector<uint8_t> still;         /* per pixel: how often it stayed the same while the view moved */
    std::vector<uint32_t> still_at;     /* ...when it last counted as staying (ms, 0: not lately), see markStill */
    std::vector<uint32_t> world_at;     /* ...when the world was last seen moving through it (ms, 0: not lately) */
    std::vector<uint8_t> stay;          /* ...and whether it counts as staying (see cleanStill) */
    bool still_dirty = true;
    VPPointer told_ptr;                 /* the newest report of the driver's pointer the host has sent... */
    uint32_t told_id = 0, told_seq = 0; /* ...its frame, and the newest input the host had applied then */
    bool told_any = false;
    bool mlog_cut = false;              /* the newest input event the log of motion no longer has */
    uint32_t mlog_cut_seq = 0;
    uint64_t dot_t = 0, st_dot_us = 0, st_dot_on_us = 0;
    bool dot_was = false;
    double st_ptr_err = 0, st_ptr_errmax = 0;   /* for the log: how far the pointer came to be from where the dot was */
    unsigned st_ptr_errn = 0;
    float ptr_q = 0;                    /* how sure it is that the game draws its pointer there (the dot shows from 0.7) */
    unsigned st_ptr_hit = 0, st_ptr_miss = 0;
    float band_score[VP_CELLROWS] = {}; /* see learnLayers */
    float band_rate[VP_CELLROWS] = {};
    uint64_t band_t[VP_CELLROWS] = {};
    uint64_t cells_at = 0;              /* when the cells were measured last */
    unsigned lens_clean = 0;            /* frames the lens was learned from (none with layers in view) */
    unsigned cells_done = 0;            /* times the cells were measured */
    uint64_t layers_at = 0;             /* when layers were last in view */
    std::vector<float> rowch, colch;    /* how much of each row and column the view's movement explains (a fading mean) */
    std::vector<float> rowcg, colcg;    /* ...and how much of it changes */
    unsigned view_frames = 0;
    int vx0 = 0, vx1 = 0, vy0 = 0, vy1 = 0;    /* the view: columns [vx0,vx1), rows [vy0,vy1) of the picture */
    int cand_x0 = -1, cand_x1 = -1, cand_y0 = -1, cand_y1 = -1;  /* ...one measured differently (see markStill) */
    unsigned cand_n = 0;                        /* ...and how many times in a row */
    mutable bool zoom_on = false;               /* the view is shown larger (see zoomed) */
    bool box_known = false;                     /* the view's rectangle has been measured in this video mode: until
                                                 * then it is the whole picture, status bar and all, so nothing is
                                                 * moved or shown larger (a viewer that predicted from the whole
                                                 * picture moved the numbers of Duke Nukem 3D's status bar) */
    float unitx = 1, unity = 1;         /* a pixel of a 320x200 picture, in this picture's pixels */
    std::deque<VPSample> samples;
    uint64_t block_end[2] = { 0, 0 };   /* where the last stretch of frames learned from ended, per direction */
    std::deque<VPMouseEv> mlog;
    std::deque<std::pair<uint32_t,uint64_t> > elog;     /* input events: number, time */
    double totx = 0, toty = 0, basex = 0, basey = 0;
    std::vector<VPKeyHist> khist;       /* per key */
    std::vector<VPKeyModel> kmodel;     /* per key and combination of Shift, Ctrl, Alt */
    struct KeyFrame { float resx, predx, resy, predy; };
    struct KeyAct { unsigned k; float ms[VP_KEYBINS]; float held; float px; };
    std::vector<KeyAct> kact;           /* the keys that count for the frame being learned from */
    std::deque<KeyFrame> kframes;       /* the last frames: what the keys were to explain, and what they did */
    float kstrength = 0;                /* how much of the keys' prediction is shown */
    double kst_err = 0, kst_base = 0;   /* for the log */
    unsigned kst_n = 0, kst_open = 0;
    double lens_mean[VP_STRIPS], lens_var[VP_STRIPS];
    unsigned lens_n[VP_STRIPS], lens_frames = 0;
    bool shown_hasc = false;
    uint32_t shown_seq = 0xFFFFFFFFu;
    uint64_t shown_c = 0, shown_arrival = 0, idle_c = 0, ival = 0, str_t = 0;
    bool said_ready = false;
    unsigned st_frames = 0, st_warps = 0;
    uint64_t st_analyse_us = 0, st_warp_us = 0;

    void clearStats(void) {
        st_frames = st_warps = 0;
        st_analyse_us = st_warp_us = 0;
        ax.st_err = ax.st_base = ay.st_err = ay.st_base = 0;
        ax.st_n = ax.st_open = ay.st_n = ay.st_open = 0;
        kst_err = kst_base = 0;
        kst_n = kst_open = 0;
    }

    void describe(const VPAxis &a,char *out,size_t n) const {
        if (!a.known) { snprintf(out,n,"not learned (%u samples)",a.n); return; }
        if (a.st_n == 0) { snprintf(out,n,"%.3f px a count after %.0f ms, %s",a.gain,a.delay / 1000.0,a.open ? "on" : "off"); return; }
        snprintf(out,n,"%.3f px a count after %.0f ms, %s (%u stretches, %u%% shown): off by %.1f px where the picture moved %.1f",
            a.gain,a.delay / 1000.0,a.open ? "on" : "off",a.st_n,a.st_open * 100u / a.st_n,a.st_err / a.st_n,a.st_base / a.st_n);
    }

    /* Every input event's number and time, to tell what a frame shows. */
    void logEvent(uint64_t t,uint32_t seq) {
        elog.push_back(std::make_pair(seq,t));
        while (elog.size() > 2 && t - elog.front().second > 4000000) elog.pop_front();
    }

    /* Up to which moment a frame shows the input: the moment c its clock gives, but nothing after the newest input
     * event (seq) the host had applied when it took the frame, however long ago the next one was sent (a datagram
     * that was late or lost is not in the picture). */
    uint64_t bound(uint64_t c,uint32_t seq) const {
        if (seq == 0xFFFFFFFFu || elog.empty()) return c;
        size_t lo = 0, hi = elog.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if ((int32_t)(elog[mid].first - seq) <= 0) lo = mid + 1; else hi = mid;
        }
        if (lo >= elog.size()) return c;
        return std::min(c,elog[lo].second - 1);
    }

    /* All the mouse counts sent up to t. */
    void cum(uint64_t t,double &x,double &y) const {
        size_t lo = 0, hi = mlog.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (mlog[mid].t <= t) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) { x = basex; y = basey; }
        else { x = mlog[lo-1].x; y = mlog[lo-1].y; }
    }

    /* Whether the mouse moved, or a key that turns the view was held, for what frame s shows. */
    bool inputMoved(const VPSample &s) const {
        const uint64_t b0 = bound(s.c0,s.q0), b1 = bound(s.c1,s.q1);
        if (b1 <= b0) return false;
        double x1, y1, x0, y0;
        cum(b1,x1,y1);
        cum(b0 > 120000 ? b0 - 120000 : 0,x0,y0);     /* (the game's own delay is not known at first) */
        if (x1 != x0) return true;
        if (khist.empty() || kmodel.empty()) return false;
        const unsigned ctx = contextAt(b1);
        float ms[VP_KEYBINS];
        for (unsigned k=1;k < (unsigned)KBD_LAST;k++) {
            if (!keyNear(k,b0)) continue;
            const VPKeyModel &m = kmodel[(size_t)k * VP_KEYCTX + ctx];
            if (m.n < 8 || fabsf(m.mx + m.vx[10]) * 1000.0f < 10.0f * unitx || !keyTime(k,b0,b1,ms)) continue;
            return true;
        }
        return false;
    }

    /* Whether the game draws a pointer where the driver has it (see pointerTold): judged whenever that place moved
     * between two frames. */
    void judgePointer(const VPPlane &pl,const VPPlane &prev,const VPMotion &mo) {
        if (!pl.ptr.told || !prev.ptr.told) return;     /* (lost on the way: nothing to judge by) */
        if (!pl.ptr.has || !prev.ptr.has) { ptr_q = 0; return; }
        if (mo.same || !mo.masked || pl.w != prev.w || pl.h != prev.h) return;
        const int pw = (int)pl.w, ph = (int)pl.h;
        const float ux = (float)pw / 320.0f, uy = (float)ph / 200.0f;
        const float ox = prev.ptr.x * (float)(pw - 1), oy = prev.ptr.y * (float)(ph - 1);
        const float nx = pl.ptr.x * (float)(pw - 1), ny = pl.ptr.y * (float)(ph - 1);
        if (fabsf(nx - ox) < 3.0f * ux && fabsf(ny - oy) < 3.0f * uy) return;
        /* around each place: a pointer's picture reaches right and down from it, some are centred on it */
        int bx0[2], bx1[2], by0[2], by1[2];
        const float px[2] = { ox, nx }, py[2] = { oy, ny };
        for (int k=0;k < 2;k++) {
            bx0[k] = std::max(0,(int)(px[k] - 12.0f * ux)); bx1[k] = std::min(pw,(int)(px[k] + 28.0f * ux));
            by0[k] = std::max(0,(int)(py[k] - 12.0f * uy)); by1[k] = std::min(ph,(int)(py[k] + 28.0f * uy));
        }
        const uint8_t *m = tmp.mask[0].data();
        size_t total = 0, inside = 0, room = 0;
        for (int y=0;y < ph;y++) {
            const bool r0 = y >= by0[0] && y < by1[0], r1 = y >= by0[1] && y < by1[1];
            const uint8_t *row = m + (size_t)y * pw;
            for (int x=0;x < pw;x++) {
                total += row[x];
                if ((r0 && x >= bx0[0] && x < bx1[0]) || (r1 && x >= bx0[1] && x < bx1[1])) { inside += row[x]; room++; }
            }
        }
        const size_t all = (size_t)pw * ph;
        /* at least a few pixels there, and four times as dense as in the rest of the picture */
        const bool hit = inside >= 6 && room < all && inside * (all - room) >= 4 * (total - inside) * room;
        ptr_q = std::max(0.0f,std::min(1.0f,ptr_q + (hit ? 0.25f : -0.34f)));
        if (hit) st_ptr_hit++; else st_ptr_miss++;
    }

    /* How far the view moved between what two frames show, as far as the mouse and the keys are known to move it.
     * False: not known (the mouse moved and its effect is not learned, or a key is held whose effect is in doubt). */
    bool between(uint64_t c0,uint32_t q0,uint64_t c1,uint32_t q1,float &mx,float &my) const {
        mx = my = 0;
        double x1, y1, x0, y0;
        cum(bound(c1 - (uint64_t)ax.delay,q1),x1,y1); cum(bound(c0 - (uint64_t)ax.delay,q0),x0,y0);
        if (x1 != x0) {
            if (!ax.known || !ax.open) return false;
            mx += (float)(ax.gain * (x1 - x0));
        }
        if (ay.known && ay.open) {
            cum(bound(c1 - (uint64_t)ay.delay,q1),x1,y1); cum(bound(c0 - (uint64_t)ay.delay,q0),x0,y0);
            my += (float)(ay.gain * (y1 - y0));
        }
        if (khist.empty() || kmodel.empty()) return true;
        const uint64_t b0 = bound(c0,q0), b1 = bound(c1,q1);
        if (b1 <= b0) return true;
        const unsigned ctx = contextAt(b1);
        float ms[VP_KEYBINS];
        for (unsigned k=1;k < (unsigned)KBD_LAST;k++) {
            if (!keyNear(k,b0) || !keyTime(k,b0,b1,ms)) continue;
            const VPKeyModel &m = kmodel[(size_t)k * VP_KEYCTX + ctx];
            float kx = 0, ky = 0;
            for (unsigned b=0;b < VP_KEYBINS;b++) {
                kx += (m.vx[b] + (b < 11 ? m.mx : 0.0f)) * ms[b];
                ky += (m.vy[b] + (b < 11 ? m.my : 0.0f)) * ms[b];
            }
            if (m.n < 20) return false;                                    /* a key not known yet */
            if (!m.open && (fabsf(kx) >= unitx || fabsf(ky) >= unity)) return false;   /* ...or in doubt, and said to move the view */
            if (m.open) { mx += kx; my += ky; }
        }
        return true;
    }

    static bool modifier(unsigned k) {
        return k == (unsigned)KBD_leftshift || k == (unsigned)KBD_rightshift || k == (unsigned)KBD_leftctrl
            || k == (unsigned)KBD_rightctrl || k == (unsigned)KBD_leftalt || k == (unsigned)KBD_rightalt;
    }

    /* Whether key k was held at t, and since when it was (or was not; 0: longer than is remembered). */
    bool heldAt(unsigned k,uint64_t t,uint64_t &since) const {
        const VPKeyHist &h = khist[k];
        for (unsigned i=h.n;i-- > 0;) if (h.t[i] <= t) { since = h.t[i]; return h.down[i]; }
        since = 0;
        return h.n > 0 && !h.down[0];
    }

    /* Which of Shift, Ctrl and Alt were held at t: they change what other keys do (run, strafe). */
    unsigned contextAt(uint64_t t) const {
        uint64_t x;
        unsigned c = 0;
        if (heldAt((unsigned)KBD_leftshift,t,x) || heldAt((unsigned)KBD_rightshift,t,x)) c |= 1;
        if (heldAt((unsigned)KBD_leftctrl,t,x) || heldAt((unsigned)KBD_rightctrl,t,x)) c |= 2;
        if (heldAt((unsigned)KBD_leftalt,t,x) || heldAt((unsigned)KBD_rightalt,t,x)) c |= 4;
        return c;
    }

    /* For how many milliseconds of (b0,b1] key k was in each of the 16 stretches of having been held or let go. */
    bool keyTime(unsigned k,uint64_t b0,uint64_t b1,float *ms) const {
        const VPKeyHist &h = khist[k];
        if (h.n == 0) return false;
        memset(ms,0,sizeof(float) * VP_KEYBINS);
        bool any = false;
        uint64_t t = b0;
        while (t < b1) {
            uint64_t since;
            const bool down = heldAt(k,t,since);
            uint64_t next = b1;
            for (unsigned i=0;i < h.n;i++) if (h.t[i] > t) { next = std::min(next,h.t[i]); break; }
            if (since == 0 && !down) { t = next; continue; }
            const double a0 = since ? (double)(t - since) / 1000.0 : 1e9, a1 = a0 + (double)(next - t) / 1000.0;
            if (down) {
                for (unsigned b=0;b < 11;b++) {
                    const double lo = VP_KEYEDGE[b], hi = (b == 10) ? 1e18 : (double)VP_KEYEDGE[b+1];
                    const double o = std::min(a1,hi) - std::max(a0,lo);
                    if (o > 0) { ms[b] += (float)o; any = true; }
                }
            }
            else {
                double lo = 0;
                for (unsigned b=0;b < 5;b++) {
                    const double hi = VP_KEYEDGE[12 + b];
                    const double o = std::min(a1,hi) - std::max(a0,lo);
                    if (o > 0) { ms[11 + b] += (float)o; any = true; }
                    lo = hi;
                }
            }
            t = next;
        }
        return any;
    }

    /* A key that could matter for (b0,b1]: held, or let go less than a fifth of a second before. */
    bool keyNear(unsigned k,uint64_t b0) const {
        const VPKeyHist &h = khist[k];
        if (h.n == 0 || modifier(k)) return false;
        return h.down[h.n-1] || h.t[h.n-1] + 200000 > b0;
    }

    /* What the keys do to the view, learned from one frame: the movement the mouse does not explain is shared out
     * among the keys held (or just let go) in proportion to their time, a little at a time, so that keys that are
     * always held together are told apart as soon as one is held alone. */
    void learnKeys(const VPSample &s) {
        if (khist.empty()) return;
        if (kmodel.empty()) {
            kmodel.resize((size_t)KBD_LAST * VP_KEYCTX);
            memset(kmodel.data(),0,kmodel.size() * sizeof(VPKeyModel));
        }
        const uint64_t b0 = bound(s.c0,s.q0), b1 = bound(s.c1,s.q1);
        if (b1 <= b0 || b1 - b0 > 400000) { kframes.clear(); return; }
        double resx = s.sx, resy = s.sy;
        bool usex = s.okx, usey = s.oky;
        double x1, y1, x0, y0;
        cum(bound(s.c1 - (uint64_t)ax.delay,s.q1),x1,y1); cum(bound(s.c0 - (uint64_t)ax.delay,s.q0),x0,y0);
        if (x1 != x0) { if (ax.known) resx -= ax.gain * (x1 - x0); else usex = false; }
        cum(bound(s.c1 - (uint64_t)ay.delay,s.q1),x1,y1); cum(bound(s.c0 - (uint64_t)ay.delay,s.q0),x0,y0);
        if (y1 != y0) { if (ay.known) resy -= ay.gain * (y1 - y0); else usey = false; }
        if (!usex) { kframes.clear(); return; }
        const unsigned ctx = contextAt(b1);
        kact.clear();
        double norm = 0, predx = 0, predy = 0;
        KeyAct a;
        for (unsigned k=1;k < (unsigned)KBD_LAST;k++) {
            if (!keyNear(k,b0) || !keyTime(k,b0,b1,a.ms)) continue;
            a.k = k;
            const VPKeyModel &m = kmodel[(size_t)k * VP_KEYCTX + ctx];
            a.px = 0;
            a.held = 0;
            for (unsigned b=0;b < VP_KEYBINS;b++) {
                if (b < 11) a.held += a.ms[b];
                norm += (double)a.ms[b] * a.ms[b];
                a.px += m.vx[b] * a.ms[b];
                predy += (double)m.vy[b] * a.ms[b];
            }
            norm += (double)a.held * a.held;
            a.px += m.mx * a.held;
            predy += (double)m.my * a.held;
            predx += a.px;
            kact.push_back(a);
        }
        if (kact.empty() || norm < 1.0) { kframes.clear(); return; }
        const double errx = resx - predx, erry = resy - predy;
        if (debug >= 2) {
            char list[400]; size_t at = 0; list[0] = 0;
            for (size_t i=0;i < kact.size() && at + 120 < sizeof(list);i++) {
                float held = 0, gone = 0;
                for (unsigned b=0;b < 11;b++) held += kact[i].ms[b];
                for (unsigned b=11;b < VP_KEYBINS;b++) gone += kact[i].ms[b];
                at += (size_t)snprintf(list + at,sizeof(list) - at," key %u held %.0f gone %.0f says %.1f [",kact[i].k,held,gone,kact[i].px);
                for (unsigned b=0;b < VP_KEYBINS && at + 24 < sizeof(list);b++) if (kact[i].ms[b] > 0)
                    at += (size_t)snprintf(list + at,sizeof(list) - at,"%u:%.0fms*%.3f ",b,kact[i].ms[b],kmodel[(size_t)kact[i].k * VP_KEYCTX + ctx].vx[b] + (b < 11 ? kmodel[(size_t)kact[i].k * VP_KEYCTX + ctx].mx : 0.0f));
                if (at + 3 < sizeof(list)) at += (size_t)snprintf(list + at,sizeof(list) - at,"];");
            }
            LOG_MSG("INPUTNET: vp keys: %.1f ms, the picture moved %.2f (keys: %.2f), predicted %.2f, ctx %u:%s",(b1 - b0) / 1000.0,s.sx,resx,predx,ctx,list);
        }

        /* How well the keys were predicted lately (before this frame is learned from): four frames together. */
        KeyFrame kf = { (float)resx, (float)predx, (float)resy, (float)predy };
        kframes.push_back(kf);
        if (kframes.size() >= 4) {
            double R = 0, P = 0;
            for (size_t i=0;i < kframes.size();i++) { R += kframes[i].resx; P += kframes[i].predx; }
            kframes.clear();    /* four frames together, each frame judged once */
            if (fabs(R) >= 2.0 * unitx || fabs(P) >= 2.0 * unitx) {
                const bool ok = fabs(R - P) <= std::max(3.0 * (double)unitx,0.35 * std::max(fabs(R),fabs(P)));
                bool shown = false;
                for (size_t i=0;i < kact.size();i++) {
                    VPKeyModel &m = kmodel[(size_t)kact[i].k * VP_KEYCTX + ctx];
                    if (m.open && m.n >= 20) shown = true;
                    /* a key is judged by what it was said to do, or by what happened when it said nothing yet */
                    if (fabs(kact[i].px) >= 0.3 * fabs(predx) || fabs(predx) < 1.0 * unitx) {
                        m.q = ok ? std::min(1.0f,m.q + 0.34f) : std::max(0.0f,m.q - 0.34f);
                        if (m.q >= 0.66f) m.open = true; else if (m.q < 0.33f) m.open = false;
                    }
                }
                kst_err += fabs(R - P); kst_base += fabs(R); kst_n++;
                if (shown) kst_open++;
            }
        }

        /* A frame in which a key was held for a moment only says little about it, however far the picture moved:
         * the step is measured against a frame of 20 ms, and against no more than a dozen pixels of surprise. */
        const double room = norm + 400.0, most = 12.0;
        const double stepx = std::max(-most * unitx,std::min(most * (double)unitx,errx));
        const double stepy = std::max(-most * unity,std::min(most * (double)unity,erry));
        for (size_t i=0;i < kact.size();i++) {
            VPKeyModel &m = kmodel[(size_t)kact[i].k * VP_KEYCTX + ctx];
            const double mu = m.n < 30 ? 0.5 : 0.2;        /* quick while a key is new, steady once it is known */
            m.mx += (float)(mu * stepx * kact[i].held / room);
            if (usey) m.my += (float)(mu * stepy * kact[i].held / room);
            for (unsigned b=0;b < VP_KEYBINS;b++) {
                if (kact[i].ms[b] <= 0) continue;
                m.vx[b] += (float)(mu * stepx * kact[i].ms[b] / room);
                if (usey) m.vy[b] += (float)(mu * stepy * kact[i].ms[b] / room);
            }
            if (m.n < 1000000) m.n++;
        }
    }

    /* The game's own delay as a key's curve has it: the time its start falls short of what the key does once held
     * (nothing at first, then all of it: that long). Microseconds. */
    static uint64_t keyDelay(const VPKeyModel &m) {
        const float fullx = m.mx + m.vx[10], fully = m.my + m.vy[10];
        const bool tilt = fabsf(fully) > fabsf(fullx);
        const float full = tilt ? fully : fullx;
        if (fabsf(full) < 1e-4f) return 0;
        /* (a game that turns slowly at first, as Doom does for six of its steps, is not late: only stretches with
         * next to nothing in them count, and the part missing of the one after) */
        double miss = 0;
        for (unsigned b=0;b < 6;b++) {      /* the first 180 ms */
            const double part = (double)((tilt ? m.my + m.vy[b] : m.mx + m.vx[b]) / full);
            const double width = (double)(VP_KEYEDGE[b+1] - VP_KEYEDGE[b]);
            if (part < 0.25) { miss += width; continue; }
            if (b > 0 && part < 1.0) miss += width * (1.0 - part);
            break;
        }
        return (uint64_t)(std::min(150.0,miss) * 1000.0);
    }

    /* How far the keys have moved the view since the frame on the screen was taken, for the player's input up to
     * now: what the game has yet to show of it is part of that (it turns on for its own delay after a key is let
     * go, and has not begun to when one was just pressed; the curve of each key says how). */
    void keyOffset(uint64_t now,float &kx,float &ky) const {
        kx = ky = 0;
        if (khist.empty() || kmodel.empty() || !shown_hasc) return;
        const uint64_t b0 = bound(shown_c,shown_seq);
        if (now <= b0 || now - b0 > 1000000) return;
        const unsigned ctx = contextAt(now);
        float ms[VP_KEYBINS];
        for (unsigned k=1;k < (unsigned)KBD_LAST;k++) {
            if (!keyNear(k,b0)) continue;
            const VPKeyModel &m = kmodel[(size_t)k * VP_KEYCTX + ctx];
            if (!m.open || m.n < 20) continue;
            const uint64_t ahead = ax.known ? (uint64_t)ax.delay : keyDelay(m);
            if (!keyTime(k,b0,now + ahead,ms)) continue;
            for (unsigned b=0;b < VP_KEYBINS;b++) {
                kx += (m.vx[b] + (b < 11 ? m.mx : 0.0f)) * ms[b];
                ky += (m.vy[b] + (b < 11 ? m.my : 0.0f)) * ms[b];
            }
        }
    }

    /* The newest frames together, about a tenth of a second of them: a game that turns in steps of its own (30 a
     * second, say) spreads a movement over frames differently each time, a stretch of them evens that out. */
    void learn(void) {
        for (unsigned axis=0;axis < 2;axis++) {
            double S = 0;
            size_t i = samples.size();
            const uint64_t c1 = samples.back().c1;
            const uint32_t q1 = samples.back().q1;
            uint64_t c0 = c1;
            uint32_t q0 = q1;
            bool full = false;
            while (i > 0) {
                const VPSample &s = samples[i-1];
                if (!(axis ? s.oky : s.okx) || s.c1 != c0) break;
                S += axis ? s.sy : s.sx;
                c0 = s.c0; q0 = s.q0;
                i--;
                if (c1 - c0 >= 80000) { full = true; break; }
            }
            if (!full) continue;
            /* stretches that do not share frames: one frame that went wrong (the host stalled, a frame was late) is
             * then judged once, not with every stretch it would be part of */
            if (c0 < block_end[axis] && block_end[axis] <= c1) continue;
            block_end[axis] = c1;
            double M[VP_LAGS];
            for (unsigned j=0;j < VP_LAGS;j++) {
                double x1, y1, x0, y0;
                cum(bound(c1 - j * VP_LAG_STEP,q1),x1,y1);
                cum(bound(c0 - j * VP_LAG_STEP,q0),x0,y0);
                M[j] = axis ? y1 - y0 : x1 - x0;
            }
            learnAxis(axis ? ay : ax,S,M,axis ? unity : unitx);
        }
    }

    /* S: how far the picture moved over a stretch of frames; M: the mouse counts that stretch shows, for each
     * candidate delay of the game. */
    void learnAxis(VPAxis &a,double S,const double *M,double unit) {
        bool any = false;
        for (unsigned j=0;j < VP_LAGS;j++) if (M[j] != 0) any = true;
        if (!any) return;
        const double pred = a.known ? a.gain * M[a.lag] : 0;
        if (fabs(S) < 0.5 * unit) {
            /* the mouse moved and the picture stayed: the game is not listening to it now */
            if (a.known && fabs(pred) >= 4.0 * unit) {
                a.gate(false);
                a.st_err += fabs(pred); a.st_n++;
            }
            return;
        }
        if (a.known) {
            if (fabs(pred) < 1.5 * unit) return;    /* the picture moved for another reason */
            const double err = fabs(S - pred);
            const bool ok = err <= std::max(2.5 * unit,0.3 * std::max(fabs(S),fabs(pred)));
            a.gate(ok);
            a.st_err += err; a.st_base += fabs(S); a.st_n++;
            if (a.open) a.st_open++;
            if (!ok) {
                /* a dozen in a row: the game answers the mouse differently now (its sensitivity setting, a zoom) */
                if (++a.rejected >= 12) a.reset();
                return;
            }
            a.rejected = 0;
        }
        const double keep = 0.99;
        for (unsigned j=0;j < VP_LAGS;j++) {
            a.a[j] = a.a[j] * keep + S * M[j];
            a.b[j] = a.b[j] * keep + M[j] * M[j];
        }
        a.c = a.c * keep + S * S;
        a.n++;
        double best = 0;
        unsigned bj = 0;
        for (unsigned j=0;j < VP_LAGS;j++) {
            if (a.b[j] <= 1e-9) continue;
            const double e = a.a[j] * a.a[j] / a.b[j];
            if (e > best) { best = e; bj = j; }
        }
        if (best <= 0) return;
        const bool first = !a.known;
        if (first) {
            /* enough movement seen, and the mouse explains most of it */
            if (a.n < 12 || a.c < 100.0 * unit * unit || best < 0.7 * a.c) return;
            a.known = true;
            LOG_MSG("INPUTNET: view prediction: the mouse %s the view by %.3f pixels a count (%u stretches of frames, the mouse explains %.0f%% of the movement)",
                &a == &ax ? "turns" : "tilts",a.a[bj] / a.b[bj],a.n,100.0 * best / a.c);
        }
        a.lag = bj;
        a.gain = a.a[bj] / a.b[bj];
        double d = (double)bj;
        if (bj > 0 && bj + 1 < VP_LAGS && a.b[bj-1] > 1e-9 && a.b[bj+1] > 1e-9) {
            const double l = a.a[bj-1] * a.a[bj-1] / a.b[bj-1], r = a.a[bj+1] * a.a[bj+1] / a.b[bj+1];
            const double den = l - 2.0 * best + r;
            if (den < -1e-12) d += std::max(-0.5,std::min(0.5,0.5 * (l - r) / den));
        }
        const double target = d * (double)VP_LAG_STEP;
        a.delay = first ? target : a.delay + 0.2 * (target - a.delay);
    }

    /* What scrolls evenly: a band of the view (a sixth of its height) that an even scroll explains clearly better
     * than the lens does (VP_Bands). That is a sky or a backdrop an engine scrolls at a rate of its own (Build's sky:
     * about a fifth faster than the ground in the middle of the view, and the same in every column). Such a band is
     * moved by its own rate; every other band by the lens. A band has to say so three times more often than not, and
     * is asked again every few seconds: what is in it changes as the player looks around. */
    void learnLayers(const VPMotion &mo,uint64_t now) {
        if (!mo.okx || !mo.bands) return;
        for (unsigned b=0;b < VP_CELLROWS;b++) {
            if (now > band_t[b] + 3000000) band_score[b] = 0;
            if (mo.bandvote[b] == 0) continue;
            band_t[b] = now;
            if (mo.bandvote[b] > 0) {
                band_rate[b] = (band_score[b] > 0 && band_rate[b] > 0) ? band_rate[b] + 0.25f * (mo.bandrate[b] - band_rate[b]) : mo.bandrate[b];
                band_score[b] = std::min(6.0f,band_score[b] + 1.0f);
            }
            else band_score[b] = std::max(-6.0f,band_score[b] - 2.0f);
        }
    }

    bool evenBand(unsigned b,uint64_t now) const { return band_score[b] >= 3.0f && now <= band_t[b] + 4000000; }

    /* What the bands say. 0: none scrolls evenly (or nothing is known); 1: all do, at one rate (a 2D view, no
     * layers); 2: some do: layers. row0, row1: the rows in which the view's own movement is measured (in 2 the
     * longest run of bands that do not scroll evenly). */
    int layerState(uint64_t now,int &row0,int &row1) const {
        row0 = vy0; row1 = vy1;
        unsigned even = 0, other = 0;
        float lo = 0, hi = 0;
        for (unsigned b=0;b < VP_CELLROWS;b++) {
            if (evenBand(b,now)) {
                if (even == 0) lo = hi = band_rate[b];
                lo = std::min(lo,band_rate[b]); hi = std::max(hi,band_rate[b]);
                even++;
            }
            else other++;
        }
        if (even == 0) return 0;
        if (other == 0 || (even >= 5 && hi <= lo * 1.08f)) return (hi <= lo * 1.08f) ? 1 : 2;
        int best0 = -1, bestn = 0, run0 = -1;
        for (unsigned b=0;b <= VP_CELLROWS;b++) {
            const bool geo = b < VP_CELLROWS && !evenBand(b,now);
            if (geo && run0 < 0) run0 = (int)b;
            if (!geo && run0 >= 0) {
                if ((int)b - run0 > bestn) { bestn = (int)b - run0; best0 = run0; }
                run0 = -1;
            }
        }
        if (best0 >= 0) {
            const int bh = (vy1 - vy0) / (int)VP_CELLROWS;
            row0 = vy0 + best0 * bh;
            row1 = (best0 + bestn == (int)VP_CELLROWS) ? vy1 : vy0 + (best0 + bestn) * bh;
        }
        return 2;
    }

    /* For a row of the picture: how much of it moves evenly (0: all by the lens, 1: all evenly), and at what rate. */
    void rowLayer(int y,uint64_t now,float &even,float &rate) const {
        even = 0; rate = 1;
        if (vy1 - vy0 < (int)VP_CELLROWS * 2) return;
        const float fy = ((float)(y - vy0) + 0.5f) / ((float)(vy1 - vy0) / (float)VP_CELLROWS) - 0.5f;
        const int b0 = std::max(0,std::min((int)VP_CELLROWS - 2,(int)floorf(fy)));
        const float t = std::max(0.0f,std::min(1.0f,fy - (float)b0));
        const bool e0 = evenBand((unsigned)b0,now), e1 = evenBand((unsigned)b0 + 1,now);
        if (!e0 && !e1) return;
        /* between two bands of which one scrolls evenly the change comes over a third of a band around their border */
        const float edge = std::max(0.0f,std::min(1.0f,(t - 0.33f) * 3.0f));
        even = (e0 ? 1.0f - edge : 0.0f) + (e1 ? edge : 0.0f);
        rate = (e0 && e1) ? band_rate[b0] + (band_rate[b0+1] - band_rate[b0]) * t : (e0 ? band_rate[b0] : band_rate[b0+1]);
    }

    /* Whether some of the view is moved by a rate of its own now. */
    bool anyLayer(uint64_t now) const {
        int a, b;
        return lens.persp && layerState(now,a,b) == 2;
    }

    /* The lens: over many frames with a clear sideways movement, how far each of the 8 strips of the picture moves
     * against its middle. A turn in perspective gives a parabola (more at the sides), a 2D view the same everywhere;
     * other movement (walking, strafing) gives something else each time, and then nothing is decided. */
    void learnLens(const VPMotion &mo) {
        /* (not only frames that fit the lens as it is known: a lens learned wrong could never be put right) */
        if (!mo.oneway || mo.measured < 5) return;
        if (fabsf(mo.whole) < 2.0f * unitx || fabsf(mo.whole) > (float)w / 12.0f) return;
        if (mo.layered) {
            layers_at = RP_Micros();
            /* The view moves in layers (a sky above the ground): what is left to measure is low in the view, where
             * the strips are not to be trusted (see VP_Measure), so such frames do not teach the lens. When there
             * are no others, the lens is taken to be what nearly every game of the kind has: 90 degrees across the
             * view, its middle in the view's middle. */
            if (!lens.assumed && (!lens.known || lens_clean < 30)) {
                /* (a lens decided from a few frames before the layers showed was decided from the sky too) */
                lens.known = lens.persp = lens.assumed = true;
                lens.f = (float)(vx1 - vx0) * 0.5f;
                lens.cx = (float)(vx0 + vx1 - 1) * 0.5f;
                memset(lens_n,0,sizeof(lens_n));
                lens_frames = lens_clean = 0;
                LOG_MSG("INPUTNET: view prediction: the view moves in layers (a sky, a backdrop); its lens is taken to be 90 degrees wide");
            }
            return;
        }
        /* the bands have not had their say yet, or said "layers" a moment ago (they are only asked while the view
         * moves far enough, and a sky does not go away while it does not) */
        if (cells_done < 3 || (layers_at != 0 && RP_Micros() < layers_at + 20000000)) return;
        lens_clean++;
        for (unsigned s=0;s < VP_STRIPS;s++) {
            if (mo.sk[s] != 1) continue;
            const double q = mo.sd[s] / mo.whole;
            if (q < 0.4 || q > 3.2) continue;
            const double al = std::max(0.07,1.0 / (double)(lens_n[s] + 1));
            const double dlt = q - lens_mean[s];
            lens_mean[s] += al * dlt;
            lens_var[s] = (1.0 - al) * (lens_var[s] + al * dlt * dlt);
            lens_n[s]++;
        }
        lens_frames++;
        if (lens_frames >= 12 && lens_frames % 4 == 0) fitLens();
    }

    void fitLens(void) {
        double s0 = 0, s1 = 0, s2 = 0, s3 = 0, s4 = 0, q0 = 0, q1 = 0, q2 = 0, spread = 0;
        unsigned used = 0;
        for (unsigned s=1;s + 1 < VP_STRIPS;s++) {      /* not the outer strips: a border, and what comes into view, pull them */
            if (lens_n[s] < 8) continue;
            /* a strip that says something else every time (a weapon that sways in it) counts for little */
            const double sd = sqrt(lens_var[s]);
            if (sd > 0.3) continue;
            const double u = (((double)s + 0.5) / VP_STRIPS) * 2.0 - 1.0, q = lens_mean[s], wt = 1.0 / (0.01 + lens_var[s]);
            s0 += wt; s1 += wt * u; s2 += wt * u * u; s3 += wt * u * u * u; s4 += wt * u * u * u * u;
            q0 += wt * q; q1 += wt * q * u; q2 += wt * q * u * u;
            spread += sd;
            used++;
        }
        if (used < 5) return;
        spread /= used;
        /* a + b u + c u^2 through the strips (u: -1 at the view's left edge, 1 at its right) */
        const double det = s0 * (s2 * s4 - s3 * s3) - s1 * (s1 * s4 - s3 * s2) + s2 * (s1 * s3 - s2 * s2);
        if (fabs(det) < 1e-9) return;
        const double a = (q0 * (s2 * s4 - s3 * s3) - s1 * (q1 * s4 - s3 * q2) + s2 * (q1 * s3 - s2 * q2)) / det;
        const double b = (s0 * (q1 * s4 - s3 * q2) - q0 * (s1 * s4 - s3 * s2) + s2 * (s1 * q2 - q1 * s2)) / det;
        const double c = (s0 * (s2 * q2 - q1 * s3) - s1 * (s1 * q2 - q1 * s2) + q0 * (s1 * s3 - s2 * s2)) / det;
        double rms = 0;
        for (unsigned s=1;s + 1 < VP_STRIPS;s++) {
            if (lens_n[s] < 8 || sqrt(lens_var[s]) > 0.3) continue;
            const double u = (((double)s + 0.5) / VP_STRIPS) * 2.0 - 1.0, e = lens_mean[s] - (a + b * u + c * u * u);
            rms += e * e;
        }
        rms = sqrt(rms / used);
        lens_fit_a = a; lens_fit_b = b; lens_fit_c = c; lens_fit_rms = rms; lens_fit_spread = spread;
        /* turns mixed with other movement: not decided (and from few frames only when they agree well) */
        if (spread > 0.22 || rms > 0.1 || (lens_frames < 30 && (spread > 0.16 || rms > 0.06))) return;
        VPLens nl;
        nl.known = true;
        if (c >= 0.12) {
            const double u0 = -b / (2.0 * c), a0 = a - b * b / (4.0 * c);
            if (fabs(u0) > 0.6 || a0 < 0.2) return;
            const double fu = 1.0 / sqrt(c / a0);   /* in half widths of the picture */
            if (fu < 0.35 || fu > 5.0) return;
            nl.persp = true;
            nl.f = (float)(fu * (double)(vx1 - vx0) * 0.5);
            nl.cx = (float)((double)vx0 + (u0 + 1.0) * (double)(vx1 - vx0) * 0.5 - 0.5);
        }
        else if (fabs(c) > 0.06 || fabs(b) > 0.08) return;
        const bool other = !lens.known || nl.persp != lens.persp || lens.assumed;
        if (!other && nl.persp) {
            /* the same kind of lens, measured again: follow it gently */
            lens.f += 0.3f * (nl.f - lens.f);
            lens.cx += 0.3f * (nl.cx - lens.cx);
            return;
        }
        if (!other) return;
        lens = nl;
        if (lens.persp)
            LOG_MSG("INPUTNET: view prediction: the view is in perspective, %.0f degrees wide, its middle at column %.0f of %u",
                2.0 * atan((double)(vx1 - vx0) * 0.5 / lens.f) * 180.0 / 3.14159265358979,lens.cx,w);
        else LOG_MSG("INPUTNET: view prediction: the view moves as one piece (no perspective)");
    }

    template <typename T> static inline bool same(const uint8_t *a,const uint8_t *b) {
        T x, y;
        memcpy(&x,a,sizeof(T)); memcpy(&y,b,sizeof(T));
        return x == y;
    }

    /* The pixels that stay while the view moves: the same as a frame ago in their place, and not what the view's
     * movement would have brought there (nor its neighbours: the movement is only known to about a pixel). A pixel
     * that changes in place starts over only when what changed it is the world moving through it; otherwise one
     * that counted as staying lately keeps counting for two seconds: a weapon that bobs while the player walks, or
     * fires, and a crosshair that changes its shade, change their pixels all the time and are still no part of the
     * world (moved with it, they came apart). */
    void markStill(const uint8_t *raw,const uint8_t *prevraw,const VPMotion &mo) {
        if (!mo.okx) return;
        const bool movedy = mo.oky && fabsf(mo.sy) >= 1.5f * unity;
        if (fabsf(mo.sx) < 1.5f * unitx && !movedy) return;
        const size_t bpp = RP_BytesPerPixel(fmt), pal = (fmt == 8) ? 768 : 0;
        const int iw = (int)w, ih = (int)h, dy = mo.oky ? (int)lroundf(mo.sy) : 0;
        const uint64_t nowus = RP_Micros();
        const uint32_t nowms = (uint32_t)(nowus / 1000u) | 1u;
        const bool layered = anyLayer(nowus);
        if (still_at.size() != still.size()) still_at.assign(still.size(),0);
        if (world_at.size() != still.size()) world_at.assign(still.size(),0);
        tmp.srcx.resize(w);
        tmp.rowk.assign(w,0);       /* per column: the pixels that changed */
        tmp.colw.assign(w,0);       /* ...and of them, those the view's movement explains (the world) */
        if (rowch.size() != h || colch.size() != w) { rowch.assign(h,0.0f); colch.assign(w,0.0f); rowcg.assign(h,0.0f); colcg.assign(w,0.0f); view_frames = 0; }
        for (int x=0;x < iw;x++) tmp.srcx[(size_t)x] = (int32_t)lroundf(VP_SourceX(lens,(float)x,mo.sx));
        for (int y=0;y < ih;y++) {
            const int ys = y - dy;
            const bool rowok = ys >= 0 && ys < ih;
            const uint8_t *c = raw + pal + (size_t)y * w * bpp, *p = prevraw + pal + (size_t)y * w * bpp;
            const uint8_t *ps = prevraw + pal + (size_t)(rowok ? ys : 0) * w * bpp;
            uint8_t *st = still.data() + (size_t)y * w;
            uint32_t *sa = still_at.data() + (size_t)y * w;
            uint32_t *wa = world_at.data() + (size_t)y * w;
            int changed = 0, worldn = 0;
            float even = 0, rate = 1;
            if (layered) rowLayer(y,nowus,even,rate);
            const int flat = (int)lroundf(rate * mo.sx);
            for (int x=0;x < iw;x++) {
                const uint8_t *cp = c + (size_t)x * bpp;
                const int xs = even > 0.5f ? x - flat : tmp.srcx[(size_t)x];
                const bool known = rowok && xs >= 1 && xs < iw - 1;
                const uint8_t *sp = ps + (size_t)(known ? xs : 0) * bpp;
                /* what the view's movement brings there. Only a pixel of the world says anything: compared with a
                 * pixel that stays (or did lately), one arm of a crosshair, moved by about its size, looked as if
                 * the world had brought it the other, and a dark pixel next to a crosshair always differed from it
                 * and was held in place as a dark patch beside it. (Inside a weapon, moved by less than its width,
                 * the weapon is compared with itself and learns nothing either: see the holes in cleanStill.) */
                const size_t si = (size_t)(rowok ? ys : 0) * w + (size_t)(known ? xs : 1);
                auto moves = [&](size_t i) { return still[i] < VP_STILL && (still_at[i] == 0 || nowms - still_at[i] > 2000u); };
                const bool told = known && moves(si) && moves(si - 1) && moves(si + 1);
                const bool world = told && (samePixel(cp,sp,bpp) || samePixel(cp,sp - bpp,bpp) || samePixel(cp,sp + bpp,bpp));
                if (!samePixel(cp,p + (size_t)x * bpp,bpp)) {
                    /* changed in place: by the world moving through it (it is world), or otherwise (a crosshair
                     * that changes its shade, a weapon that bobs or fires: halved, and against a known world
                     * still not the world, so whether it stays is kept fresh) */
                    changed++; tmp.rowk[(size_t)x]++;
                    if (world) { st[x] = 0; sa[x] = 0; wa[x] = nowms; worldn++; tmp.colw[(size_t)x]++; }
                    else {
                        st[x] /= 2;
                        if (sa[x] != 0 && nowms - sa[x] <= 2000u) sa[x] = nowms;
                    }
                    continue;
                }
                if (world) continue;
                /* the same in place while the view moved: one that counts as staying goes on counting, also when
                 * the view moved by less than its size and it could only be compared with itself (small turns
                 * wore the crosshair's mark out, and an arm of it was moved with the world) */
                if (!told) { if (sa[x] != 0 && nowms - sa[x] <= 2000u) sa[x] = nowms; continue; }
                if (st[x] < 255) st[x]++;
                if (st[x] >= VP_STILL) sa[x] = nowms;
            }
            rowch[(size_t)y] += 0.1f * ((float)worldn / (float)iw - rowch[(size_t)y]);
            rowcg[(size_t)y] += 0.1f * ((float)changed / (float)iw - rowcg[(size_t)y]);
        }
        for (int x=0;x < iw;x++) {
            colch[(size_t)x] += 0.1f * ((float)tmp.colw[(size_t)x] / (float)ih - colch[(size_t)x]);
            colcg[(size_t)x] += 0.1f * ((float)tmp.rowk[(size_t)x] / (float)ih - colcg[(size_t)x]);
        }
        still_dirty = true;
        /* The view: the rows and columns that change when the view moves, its movement explaining some of it (the
         * world brought there from beside), and where most pixels do not stay. A status bar whose numbers change
         * while the player turns (health, ammunition), or that a flash of colour changes all over, counted as view
         * and was moved with it, its numbers sliding across their panels (Duke Nukem 3D, November 2026); its panel
         * stays, so it is outside. The world alone is not enough at the edges: where a turn, or looking up, brings
         * new picture in, nothing explains it, and a long turn one way wore the edge columns out of the view. A
         * status bar below it, a border around it, a panel beside it are outside, and nothing there is moved or
         * moved in from. */
        if (++view_frames >= 6) {
            const float ramp = std::min(1.0f,(float)view_frames / 20.0f + 0.3f);
            const float wbar = 0.02f * ramp, cbar = 0.06f * ramp;
            const uint8_t *stl = still.data();
            const uint32_t *sat = still_at.data();
            auto stays = [&](size_t i) { return stl[i] >= VP_STILL || (sat[i] != 0 && nowms - sat[i] <= 2000u); };
            auto rowIn = [&](int y) {
                if (rowch[(size_t)y] < wbar && rowcg[(size_t)y] < cbar) return false;
                int n = 0;
                for (int x=0;x < iw;x++) n += stays((size_t)y * w + (size_t)x);
                return n * 2 < iw;
            };
            auto colIn = [&](int x,int a,int b) {
                if (colch[(size_t)x] < wbar && colcg[(size_t)x] < cbar) return false;
                int n = 0;
                for (int y=a;y < b;y++) n += stays((size_t)y * w + (size_t)x);
                return n * 2 < b - a;
            };
            int a = 0, b = ih, c0 = 0, c1 = iw;
            while (a < ih && !rowIn(a)) a++;
            while (b > a && !rowIn(b - 1)) b--;
            while (c0 < iw && !colIn(c0,a,b)) c0++;
            while (c1 > c0 && !colIn(c1 - 1,a,b)) c1--;
            /* A new rectangle is taken when it is measured the same (to a pixel) several times in a row; once the view
             * is shown larger, every change of it moves and scales the picture, so then only one that stays measured
             * for about three seconds of moving (the game's view made larger or smaller). */
            if (b - a >= ih / 4 && c1 - c0 >= iw / 4) {
                if (abs(a - cand_y0) <= 1 && abs(b - cand_y1) <= 1 && abs(c0 - cand_x0) <= 1 && abs(c1 - cand_x1) <= 1) cand_n++;
                else { cand_y0 = a; cand_y1 = b; cand_x0 = c0; cand_x1 = c1; cand_n = 1; }
                const bool differs = abs(cand_y0 - vy0) > 1 || abs(cand_y1 - vy1) > 1 || abs(cand_x0 - vx0) > 1 || abs(cand_x1 - vx1) > 1;
                if ((differs || !box_known) && cand_n >= (zoom_on ? 120u : 3u)) {
                    vy0 = cand_y0; vy1 = cand_y1; vx0 = cand_x0; vx1 = cand_x1;
                    box_known = true;
                }
            }
        }
    }

    static inline bool samePixel(const uint8_t *a,const uint8_t *b,size_t bpp) {
        return bpp == 1 ? *a == *b : (bpp == 2 ? same<uint16_t>(a,b) : same<uint32_t>(a,b));
    }

    /* What the picture is moved by. A pixel counts as staying when it stays (or did in the last two seconds, see
     * markStill) and one of the 8 around it does too: a single pixel of the world that happened to pass makes no
     * speck, and the 1-pixel strokes of a crosshair or of text on the screen stay whole to their ends (a majority of
     * the 3 x 3 broke them, and their pieces were moved with the world). A pixel with 6 of its 8 around it staying
     * counts as staying too (no holes). */
    void cleanStill(void) {
        still_dirty = false;
        const int iw = (int)w, ih = (int)h;
        const uint32_t nowms = (uint32_t)(RP_Micros() / 1000u) | 1u;
        if (still_at.size() != still.size()) still_at.assign(still.size(),0);
        tmp.stillnow.resize((size_t)w * h);
        uint8_t *is = tmp.stillnow.data();
        for (size_t i=0;i < still.size();i++)
            is[i] = (uint8_t)(still[i] >= VP_STILL || (still_at[i] != 0 && nowms - still_at[i] <= 2000u));
        stay.assign((size_t)w * h,0);
        tmp.rowk.assign((size_t)w * 3,0);      /* three rows of sums of three */
        int32_t *rows[3] = { tmp.rowk.data(), tmp.rowk.data() + w, tmp.rowk.data() + 2 * (size_t)w };
        auto sumrow = [&](int y,int32_t *out) {
            if (y < 0 || y >= ih) { for (int x=0;x < iw;x++) out[x] = 0; return; }
            const uint8_t *r = is + (size_t)y * w;
            for (int x=0;x < iw;x++) out[x] = (x > 0 && r[x-1]) + r[x] + (x + 1 < iw && r[x+1]);
        };
        sumrow(-1,rows[0]); sumrow(0,rows[1]);
        for (int y=0;y < ih;y++) {
            sumrow(y + 1,rows[2]);
            uint8_t *o = stay.data() + (size_t)y * w;
            const uint8_t *r = is + (size_t)y * w;
            for (int x=0;x < iw;x++) {
                const int n = rows[0][x] + rows[1][x] + rows[2][x];     /* (itself included) */
                o[x] = (uint8_t)(r[x] ? n >= 2 : n >= 6);
            }
            int32_t *first = rows[0];
            rows[0] = rows[1]; rows[1] = rows[2]; rows[2] = first;
        }
        /* Holes inside something that stays (plain parts of a weapon or a hand, which compared with the rest of it
         * prove nothing either way, see markStill): a pixel with staying pixels within a few columns to its left
         * and right and a few rows above and below it stays too, unless the world was seen moving through it
         * lately (the middle of a crosshair). */
        if (world_at.size() != still.size()) world_at.assign(still.size(),0);
        const int reachx = std::max(2,(int)lroundf(6.0f * unitx)), reachy = std::max(2,(int)lroundf(6.0f * unity));
        tmp.stillnow.assign(stay.begin(),stay.end());
        const uint8_t *sv = tmp.stillnow.data();
        for (int y=1;y + 1 < ih;y++) {
            for (int x=1;x + 1 < iw;x++) {
                const size_t i = (size_t)y * w + (size_t)x;
                if (sv[i] || (world_at[i] != 0 && nowms - world_at[i] <= 2000u)) continue;
                bool l = false, r = false, u = false, d = false;
                for (int k=1;k <= reachx && !l && x - k >= 0;k++) l = sv[i - (size_t)k] != 0;
                if (!l) continue;
                for (int k=1;k <= reachx && !r && x + k < iw;k++) r = sv[i + (size_t)k] != 0;
                if (!r) continue;
                for (int k=1;k <= reachy && !u && y - k >= 0;k++) u = sv[i - (size_t)k * w] != 0;
                if (!u) continue;
                for (int k=1;k <= reachy && !d && y + k < ih;k++) d = sv[i + (size_t)k * w] != 0;
                if (d) stay[i] = 1;
            }
        }
    }

public:
    double lens_fit_a = 0, lens_fit_b = 0, lens_fit_c = 0, lens_fit_rms = 0, lens_fit_spread = 0;   /* the last fit, for tests */
};

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
struct FrameInfo {
    uint32_t id = 0, seq = 0, wait_us = 0, enc_us = 0;
    uint32_t taken = 0;     /* when the host took the frame: its clock, microseconds, the low 32 bits */
    bool hastaken = false;  /* (hosts before view prediction do not say) */
    VPPointer ptr;          /* the DOS mouse driver's pointer when the frame was taken */
    uint64_t at = 0;
};
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
    float aud_fast = 1.0f;          /* below 1: played that much faster, to shorten what waits (see handleAudio) */
    std::vector<uint8_t> aud_out;
    uint64_t aud_wait_sum = 0;      /* for the log: how long sound waited to be played, how often it was sped up */
    unsigned aud_wait_n = 0, aud_fast_n = 0;

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

    /* view prediction (see ViewPredict) */
    ViewPredict vp;
    std::vector<uint32_t> pix;      /* the frame as the texture's pixels, converted once a frame */
    bool pix_dirty = true;
    bool vp_due = false;            /* a decoded frame waits to be analysed (after it has been shown) */
    bool frame_hasc = false;        /* the frame on the screen: up to which moment it shows the input */
    uint64_t frame_c = 0;
    uint32_t frame_seq = 0xFFFFFFFFu;       /* ...and the newest input the host had applied when it took it (if it said) */
    VPPointer frame_ptr;            /* ...and where the DOS mouse driver had its pointer */
    SDL_Texture *zoom_tex = NULL;   /* the view enlarged by whole pixels (see present) */
    int zoom_w = 0, zoom_h = 0;
    bool dot_on = false;            /* the pointer dot is on the screen, at dot_px, dot_py of the window */
    int dot_px = 0, dot_py = 0;
    std::string shot_dir;           /* NN_RP_PREDICTSHOT=<folder>: the first 24 pictures moved 2 to 8 pixels (of 320), before and after, as PPM */
    unsigned shots = 0;
    uint64_t arrive_est = 0;        /* how long after that moment a frame usually arrives, microseconds */
    uint32_t off_cur = 0, off_prev = 0;     /* the host's clock against this one: the least of the last seconds */
    bool off_hascur = false, off_hasprev = false;
    uint64_t off_bucket = 0;
    uint64_t last_present_us = 0;
    float shown_px = 0, shown_py = 0;       /* the prediction in the picture on the screen */
    uint64_t unshown_us = 0;        /* the oldest mouse movement the screen does not show yet */
    double m2s_sum = 0;             /* mouse to screen with prediction, for the log */
    unsigned m2s_n = 0;
    /* test input (NN_RP_TESTINPUT): the mouse and keys move by themselves, the same way every time */
    std::string test_mode;
    FILE *rec_ev = NULL, *rec_fr = NULL;    /* NN_RP_RECORD=<folder>: the frames and the input, to learn from again offline */
    double test_scale = 1.0, test_scaley = 1.0, test_start = 6.0, test_frac = 0, test_fracy = 0;
    uint64_t test_t0 = 0, test_last = 0;
    int test_keystate = 0;

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
        const uint64_t t = RP_Micros();
        vp.key((unsigned)k,pressed,t,nextseq - 1);
        if (rec_ev) fprintf(rec_ev,"K %llu %u %d %u\n",(unsigned long long)t,(unsigned)k,pressed ? 1 : 0,nextseq - 1);
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
        if (captured && (acc_xrel != 0 || acc_yrel != 0)) {
            const uint64_t t = RP_Micros();
            vp.mouse(t,nextseq - 1,acc_xrel,acc_yrel);
            if (rec_ev) fprintf(rec_ev,"M %llu %g %g %u\n",(unsigned long long)t,(double)acc_xrel,(double)acc_yrel,nextseq - 1);
        }
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
                if (e.key.keysym.scancode == SDL_SCANCODE_F12 && (mod & KMOD_CTRL)) {
                    vp.moveView = !vp.moveView;
                    LOG_MSG("INPUTNET: view prediction %s",vp.moveView ? "on: the picture is moved by the input the host's frames do not show yet"
                        : "off: the picture is shown as the host sends it");
                    redraw = true;
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
                    if (unshown_us == 0) unshown_us = RP_Micros();
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
        pix_dirty = true;
        frame_hasc = frameClock(id,frame_c,frame_seq);
        vp.shown(frame_hasc,frame_c,frame_seq,frame_done);
        frame_ptr = (infos[id & 31u].id == id) ? infos[id & 31u].ptr : VPPointer();
        vp_due = true;
        if (rec_ev && rec_fr) {
            fprintf(rec_ev,"F %u %llu %d %llu %u %u %u %llu %u\n",id,(unsigned long long)frame_done,frame_hasc ? 1 : 0,(unsigned long long)frame_c,
                frame_w,frame_h,frame_fmt,(unsigned long long)frame.size(),frame_seq);
            fwrite(frame.data(),1,frame.size(),rec_fr);
        }
    }

    /* Up to which moment of this computer's clock a frame shows the input: input sent before it had been applied when
     * the host took the frame. From the host's own clock when it says when it took the frame (the least difference
     * between the two clocks over the last seconds is the way there at its quickest), otherwise from the newest
     * input the frame had and how long that had been applied; without either, from when frames usually arrive. */
    bool frameClock(uint32_t id,uint64_t &c,uint32_t &seq) {
        const FrameInfo &f = infos[id & 31u];
        const uint64_t now = RP_Micros();
        seq = 0xFFFFFFFFu;
        if (f.id == id && f.seq != 0 && f.wait_us != 0xFFFFFFFFu) {
            const InputTime &t = times[f.seq & 1023u];
            seq = f.seq;
            if (t.seq == f.seq && t.sent != 0) {
                c = t.sent + f.wait_us;
                if (f.hastaken) {
                    const uint32_t off = f.taken - f.wait_us - (uint32_t)t.sent;
                    if (now - off_bucket > 6000000) {
                        off_prev = off_cur; off_hasprev = off_hascur;
                        off_hascur = false;
                        off_bucket = now;
                    }
                    if (!off_hascur || (int32_t)(off - off_cur) < 0) { off_cur = off; off_hascur = true; }
                    const uint32_t least = (off_hasprev && (int32_t)(off_prev - off_cur) < 0) ? off_prev : off_cur;
                    c = now - (uint32_t)((uint32_t)now - (f.taken - least));
                }
                if (c <= now && now - c < 5000000 && frame_done >= c) {
                    const uint64_t d = frame_done - c;
                    arrive_est = arrive_est ? (arrive_est * 7 + d) / 8 : d;
                    return true;
                }
            }
        }
        if (arrive_est != 0 && frame_done > arrive_est) { c = frame_done - arrive_est; return true; }
        return false;
    }

    /* Learns from the frame just shown (ViewPredict::frame), against the frame learned from before it. */
    void analyse(void) {
        vp_due = false;
        if (rec_ev && haveframe) {
            fprintf(rec_ev,"A %u %llu\n",lastid,(unsigned long long)RP_Micros());
            fflush(rec_ev);
            if (rec_fr) fflush(rec_fr);
        }
        if (!haveframe || !vp.enabled) return;
        bool valid = false;
        const uint32_t pid = vp.prevId(valid);
        const uint8_t *prev = NULL;
        if (valid && pid != lastid)
            for (size_t i=hist.size();i-- > 0 && prev == NULL;)
                if (hist[i].first == pid && hist[i].second.size() == frame.size()) prev = hist[i].second.data();
        vp.frame(lastid,frame.data(),frame.size(),frame_w,frame_h,frame_fmt,prev,frame_hasc,frame_c,frame_seq,frame_ptr);
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
        if (aud_wait_n) {
            LOG_MSG("INPUTNET: sound waits %u ms on average to be played%s",(unsigned)(aud_wait_sum / aud_wait_n),
                aud_fast_n ? (std::string(", played faster for ") + std::to_string(aud_fast_n * 100u / aud_wait_n) + "% of the time to catch up").c_str() : "");
            aud_wait_sum = 0; aud_wait_n = aud_fast_n = 0;
        }
        vp.report();
        if (m2s_n) LOG_MSG("INPUTNET: with view prediction the picture followed the mouse after %.1f ms on average (%u movements)",m2s_sum / m2s_n,m2s_n);
        m2s_sum = 0; m2s_n = 0;
        if (show_latency) updateTitle();
    }

    /* The frame as the texture's pixels (once a frame: the picture may be drawn several times, moved differently). */
    void convertFrame(void) {
        pix.resize((size_t)frame_w * frame_h);
        const uint8_t *p = frame.data();
        uint32_t lut[256];
        if (frame_fmt == 8)
            for (unsigned i=0;i < 256;i++)
                lut[i] = 0xFF000000u | ((uint32_t)p[i*3] << 16) | ((uint32_t)p[i*3+1] << 8) | p[i*3+2];
        for (unsigned y=0;y < frame_h;y++) {
            uint32_t *o = pix.data() + (size_t)y * frame_w;
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
        pix_dirty = false;
    }

    /* Where in the window the pointer dot goes at this moment; false: there is none. */
    bool pointerDot(const SDL_Rect &r,int &x,int &y) {
        float nx, ny;
        if (!captured || abs_pointer || !haveframe || !vp.pointerAt(acc_xrel,acc_yrel,nx,ny)) return false;
        float fx = nx * (float)(frame_w - 1), fy = ny * (float)(frame_h - 1);
        zoomPoint(fx,fy);
        x = r.x + (int)lroundf(fx * (float)r.w / (float)frame_w);
        y = r.y + (int)lroundf(fy * (float)r.h / (float)frame_h);
        return true;
    }

    /* The view shown larger (see ViewPredict::warp): what of the frame is drawn where the view is. False: shown as is. */
    bool zoomCrop(SDL_Rect &view,SDL_Rect &crop) const {
        if (!vp.zoomed() || vp.w != frame_w || vp.h != frame_h) return false;
        int x0, y0, x1, y1, mx, my;
        vp.viewBox(x0,y0,x1,y1);
        vp.margins(mx,my);
        if (x1 - x0 <= 4 * mx || y1 - y0 <= 4 * my) return false;
        view.x = x0; view.y = y0; view.w = x1 - x0; view.h = y1 - y0;
        crop.x = x0 + mx; crop.y = y0 + my; crop.w = view.w - 2 * mx; crop.h = view.h - 2 * my;
        return true;
    }

    /* Where a point of the frame (in its pixels) is on the screen, in frame pixels, with the view shown larger. */
    void zoomPoint(float &fx,float &fy) const {
        SDL_Rect v, c;
        if (!zoomCrop(v,c) || fx < (float)v.x || fx >= (float)(v.x + v.w) || fy < (float)v.y || fy >= (float)(v.y + v.h)) return;
        fx = (float)v.x + (fx - (float)c.x) * (float)v.w / (float)c.w;
        fy = (float)v.y + (fy - (float)c.y) * (float)v.h / (float)c.h;
    }

    /* How far view prediction moves the picture at this moment; false: not at all. */
    bool predicted(uint64_t now_us,float &px,float &py) {
        px = py = 0;
        if (!vp.enabled || !vp.moveView || !haveframe || vp.w != frame_w || vp.h != frame_h || !vp.boxKnown()) return false;
        if (vp.offset(now_us,captured ? acc_xrel : 0.0f,captured ? acc_yrel : 0.0f,px,py)) return true;
        px = py = 0;
        return false;
    }

    void present(void) {
        const uint64_t now_us = RP_Micros();
        float px = 0, py = 0;
        if (haveframe) {
            if (texture == NULL || tex_w != frame_w || tex_h != frame_h) {
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,(int)frame_w,(int)frame_h);
                tex_w = frame_w; tex_h = frame_h;
            }
            if (pix_dirty || pix.size() != (size_t)frame_w * frame_h) convertFrame();
            const bool moved = predicted(now_us,px,py) && (lroundf(px) != 0 || lroundf(py) != 0);
            /* Straight into the texture, or through argb when it cannot be locked. */
            void *dst = NULL;
            int dpitch = 0;
            const bool locked = texture != NULL && SDL_LockTexture(texture,NULL,&dst,&dpitch) == 0;
            if (!locked) {
                argb.resize((size_t)frame_w * frame_h);
                dst = argb.data();
                dpitch = (int)(frame_w * 4);
            }
            if (moved) vp.warp(pix.data(),(uint8_t*)dst,dpitch,px,py);
            else for (unsigned y=0;y < frame_h;y++)
                memcpy((uint8_t*)dst + (size_t)y * (size_t)dpitch,pix.data() + (size_t)y * frame_w,(size_t)frame_w * 4);
            if (moved && !shot_dir.empty() && shots < 24 && fabsf(px) >= (float)(2 + 2 * (shots % 4)) * (float)frame_w / 320.0f) {
                for (int k=0;k < 2;k++) {
                    char name[64];
                    snprintf(name,sizeof(name),"/shot%02u-%s.ppm",shots,k ? "moved" : "frame");
                    FILE *fp = fopen((shot_dir + name).c_str(),"wb");
                    if (fp == NULL) continue;
                    fprintf(fp,"P6\n%u %u\n255\n",frame_w,frame_h);
                    for (unsigned y=0;y < frame_h;y++) {
                        const uint32_t *row = k ? (const uint32_t*)((const uint8_t*)dst + (size_t)y * (size_t)dpitch) : pix.data() + (size_t)y * frame_w;
                        for (unsigned x=0;x < frame_w;x++) { const uint8_t rgb[3] = { (uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8), (uint8_t)row[x] }; fwrite(rgb,1,3,fp); }
                    }
                    fclose(fp);
                }
                LOG_MSG("INPUTNET: view prediction test picture %u: moved by %.1f, %.1f",shots,px,py);
                shots++;
            }
            if (locked) SDL_UnlockTexture(texture);
            else if (texture) SDL_UpdateTexture(texture,NULL,argb.data(),(int)(frame_w * 4));
        }
        SDL_SetRenderDrawColor(renderer,0,0,0,255);
        SDL_RenderClear(renderer);
        dot_on = false;
        if (texture && haveframe) {
            const SDL_Rect r = displayRect();
            SDL_RenderCopy(renderer,texture,NULL,&r);
            /* the view shown larger, over itself (see ViewPredict::warp); the status bar and the rest as they are */
            SDL_Rect view, crop;
            if (zoomCrop(view,crop)) {
                const float sxw = (float)r.w / (float)frame_w, syh = (float)r.h / (float)frame_h;
#if SDL_VERSION_ATLEAST(2,0,12)
                /* Scaled by a little more than the rest, "nearest" would make some of its pixels a screen pixel wider
                 * than others, and what moves would crawl through that pattern: enlarged by whole pixels first, then
                 * to the window with linear filtering, every pixel is as wide as the next (DOSBox's "sharp" output). */
                const SDL_FRect d = { (float)r.x + (float)view.x * sxw, (float)r.y + (float)view.y * syh, (float)view.w * sxw, (float)view.h * syh };
                const int kx = std::max(1,(int)ceilf(d.w / (float)crop.w)), ky = std::max(1,(int)ceilf(d.h / (float)crop.h));
                bool sharp = SDL_RenderTargetSupported(renderer) == SDL_TRUE;
                if (sharp && (zoom_tex == NULL || zoom_w != crop.w * kx || zoom_h != crop.h * ky)) {
                    if (zoom_tex) SDL_DestroyTexture(zoom_tex);
                    zoom_w = crop.w * kx; zoom_h = crop.h * ky;
                    zoom_tex = SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_TARGET,zoom_w,zoom_h);
                    if (zoom_tex) SDL_SetTextureScaleMode(zoom_tex,SDL_ScaleModeLinear);
                }
                if (sharp && zoom_tex && SDL_SetRenderTarget(renderer,zoom_tex) == 0) {
                    SDL_RenderCopy(renderer,texture,&crop,NULL);
                    SDL_SetRenderTarget(renderer,NULL);
                    SDL_RenderCopyF(renderer,zoom_tex,NULL,&d);
                }
                else SDL_RenderCopyF(renderer,texture,&crop,&d);
#else
                const SDL_Rect d = { r.x + (int)lroundf((float)view.x * sxw), r.y + (int)lroundf((float)view.y * syh), (int)lroundf((float)view.w * sxw), (int)lroundf((float)view.h * syh) };
                SDL_RenderCopy(renderer,texture,&crop,&d);
#endif
            }
            /* the pointer dot (see ViewPredict::pointerTold): a white square in a black ring, as over a Windows guest */
            if (pointerDot(r,dot_px,dot_py)) {
                dot_on = true;
                const SDL_Rect ring = { dot_px - 3, dot_py - 3, 7, 7 }, core = { dot_px - 1, dot_py - 1, 3, 3 };
                SDL_SetRenderDrawColor(renderer,0,0,0,255);
                SDL_RenderFillRect(renderer,&ring);
                SDL_SetRenderDrawColor(renderer,255,255,255,255);
                SDL_RenderFillRect(renderer,&core);
            }
        }
        SDL_RenderPresent(renderer);
        redraw = false;
        shown_px = px; shown_py = py;
        last_present_us = now_us;
        if (unshown_us != 0) {
            /* mouse movement the screen shows now, ahead of the host's own frames */
            if (px != 0 || py != 0) { m2s_sum += (double)(RP_Micros() - unshown_us) / 1000.0; m2s_n++; }
            unshown_us = 0;
        }
    }

    /* Test input (NN_RP_TESTINPUT=mouse|look|keys|mix|walk|walkkeys|none, NN_RP_TESTSCALE, NN_RP_TESTSTART seconds):
     * a mouse and keys that move by themselves, the same way every time, to measure view prediction without a hand
     * on them ("none": only what NN_RP_TESTKEYS says). */
    void testInput(void) {
        if (test_mode.empty() || !welcomed) return;
        const uint64_t now = RP_Micros();
        if (test_t0 == 0) {
            test_t0 = now + (uint64_t)(test_start * 1e6);
            test_last = test_t0;
            captured = true;    /* relative motion, without taking the system's pointer */
            return;
        }
        if (now <= test_last) return;
        const double t = (double)(now - test_t0) / 1e6, dt = (double)(now - test_last) / 1e6;
        test_last = now;
        const bool mix = test_mode == "mix";
        const double ph = fmod(t,6.0);
        const bool second = fmod(t,12.0) >= 6.0;
        double vx = 0, vy = 0;      /* mouse counts a second */
        int keys = 0;               /* 1 left, 2 right, 4 up */
        if (test_mode == "mouse" || test_mode == "look" || test_mode == "walk" || (mix && !second)) {
            if (ph < 2.0) vx = 500.0 * sin(2.0 * 3.14159265358979 * ph);
            else if (ph >= 2.4 && ph < 2.55) vx = 1600.0;
            else if (ph >= 3.2 && ph < 3.35) vx = -1600.0;
            else if (ph >= 4.0 && ph < 5.2) vx = 200.0;
            else if (ph >= 5.6 && ph < 5.9) vx = -800.0;
            if (test_mode == "look" && ph < 2.0) vy = 250.0 * sin(2.0 * 3.14159265358979 * 0.5 * ph);
        }
        if (test_mode == "keys" || test_mode == "walkkeys" || (mix && second)) {
            if (ph < 0.5) keys = 1;
            else if (ph >= 1.0 && ph < 1.5) keys = 2;
            else if (ph >= 2.0 && ph < 2.2) keys = 1;
            else if (ph >= 2.6 && ph < 2.8) keys = 2;
            else if (ph >= 3.2 && ph < 4.2) keys = 1;
            else if (ph >= 4.6 && ph < 5.6) keys = 2;
        }
        if (test_mode == "walk" || test_mode == "walkkeys") keys |= 4;
        test_frac += vx * test_scale * dt;
        test_fracy += vy * test_scaley * dt;
        const double wx = trunc(test_frac), wy = trunc(test_fracy);
        if (wx != 0 || wy != 0) {
            test_frac -= wx; test_fracy -= wy;
            if (acc_xrel == 0 && acc_yrel == 0 && motion_made == 0) motion_made = now;
            if (unshown_us == 0) unshown_us = now;
            acc_xrel += (float)wx;
            acc_yrel += (float)wy;
        }
        if (keys != test_keystate) {
            keyEvent(KBD_left,(keys & 1) != 0);
            keyEvent(KBD_right,(keys & 2) != 0);
            keyEvent(KBD_up,(keys & 4) != 0);
            test_keystate = keys;
        }
        /* NN_RP_TESTKEYS="12:esc:0.1,20.5:lalt:3": at 12 s Escape for 0.1 s, at 20.5 s left Alt for 3 s */
        for (size_t i=0;i < test_keys.size();i++) {
            TestKey &k = test_keys[i];
            const bool down = t >= k.at && t < k.at + k.hold;
            if (down != k.down) {
                k.down = down;
                if (k.key == KBD_NONE) { flushMotion(); buttonEvent(0,down); }  /* "click": the left mouse button */
                else keyEvent(k.key,down);
            }
        }
    }

    struct TestKey { double at, hold; KBD_KEYS key; bool down; };
    std::vector<TestKey> test_keys;
    void parseTestKeys(const char *spec) {
        /* the keymapper's names (RP_KeyByName), and these */
        static const struct { const char *name; KBD_KEYS key; } more[] = {
            { "pgup", KBD_pageup }, { "pgdn", KBD_pagedown }, { "ins", KBD_insert }, { "del", KBD_delete },
            { "kp2", KBD_kp2 }, { "kp4", KBD_kp4 }, { "kp6", KBD_kp6 }, { "kp8", KBD_kp8 }, { "click", KBD_NONE }
        };
        std::string text = spec;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find(',',pos);
            if (end == std::string::npos) end = text.size();
            const std::string item = text.substr(pos,end - pos);
            pos = end + 1;
            const size_t c1 = item.find(':'), c2 = item.find(':',c1 == std::string::npos ? 0 : c1 + 1);
            if (c1 == std::string::npos) continue;
            const std::string name = item.substr(c1 + 1,c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
            KBD_KEYS key = KBD_NONE;
            bool known = RP_KeyByName(name,key);
            for (const auto &m : more) if (!known && name == m.name) { key = m.key; known = true; }
            if (!known) continue;
            TestKey k = { atof(item.c_str()), c2 == std::string::npos ? 0.1 : atof(item.c_str() + c2 + 1), key, false };
            test_keys.push_back(k);
        }
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
        /* Sound that waits to be played is sound heard late. The queue grows whenever packets arrive in a burst (or
         * the host's clock runs a little faster than this sound card's) and never shrank again by itself: it sat
         * anywhere up to the quarter of a second at which it is thrown away. Above 70 ms it is now played 3% faster
         * (8% above 130 ms), a few samples left out of every packet by interpolation, until it is back at 45 ms:
         * no gap, no click, and a change of pitch too small and too short to hear. */
        const Uint32 bytes_ms = rate * 4u / 1000u;
        const Uint32 waiting = bytes_ms ? SDL_GetQueuedAudioSize(audio) / bytes_ms : 0;
        aud_wait_sum += waiting; aud_wait_n++;
        if (audio_playing && waiting > 130) aud_fast = 0.92f;
        else if (audio_playing && waiting > 70) aud_fast = std::min(aud_fast,0.97f);
        else if (waiting <= 45) aud_fast = 1.0f;
        if (aud_fast >= 1.0f || frames < 8) { SDL_QueueAudio(audio,pcm,(Uint32)n); return; }
        aud_fast_n++;
        const unsigned outn = std::max(1u,(unsigned)lroundf((float)frames * aud_fast));
        aud_out.resize((size_t)outn * 4);
        const double step = (double)(frames - 1) / (double)std::max(1u,outn - 1);
        for (unsigned i=0;i < outn;i++) {
            const double at = (double)i * step;
            const unsigned a = std::min(frames - 1,(unsigned)at), b = std::min(frames - 1,a + 1);
            const double f = at - (double)a;
            for (unsigned c=0;c < 2;c++) {
                const int16_t va = (int16_t)(pcm[(a*2+c)*2] | (pcm[(a*2+c)*2+1] << 8)), vb = (int16_t)(pcm[(b*2+c)*2] | (pcm[(b*2+c)*2+1] << 8));
                const int16_t v = (int16_t)lround((double)va + ((double)vb - (double)va) * f);
                aud_out[(i*2+c)*2] = (uint8_t)v; aud_out[(i*2+c)*2+1] = (uint8_t)((uint16_t)v >> 8);
            }
        }
        SDL_QueueAudio(audio,aud_out.data(),(Uint32)aud_out.size());
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
                if (r.pos + 4 <= r.len) { f.taken = r.u32(); f.hastaken = true; }
                if (r.pos + 1 <= r.len && (r.u8() & 1) && r.pos + 20 <= r.len) {
                    f.ptr.x = (float)r.u16() / 65535.0f; f.ptr.y = (float)r.u16() / 65535.0f;
                    f.ptr.gx1 = r.f32(); f.ptr.gy1 = r.f32(); f.ptr.gxs = r.f32(); f.ptr.gys = r.f32();
                    f.ptr.has = r.ok;
                }
                f.ptr.told = true;
                f.at = RP_Micros();
                infos[f.id & 31u] = f;
                vp.pointerTold(f.id,f.seq,f.ptr);
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
            vp.enabled = sec->Get_bool("predict");
            vp.moveView = sec->Get_bool("predict view");
        }
        if (const char *e = getenv("NN_RP_PREDICT")) vp.enabled = atoi(e) != 0;
        if (const char *e = getenv("NN_RP_PREDICTVIEW")) vp.moveView = atoi(e) != 0;
        if (const char *e = getenv("NN_RP_PREDICTLOG")) vp.debug = atoi(e);
        if (const char *e = getenv("NN_RP_PREDICTSHOT")) shot_dir = e;
        if (const char *e = getenv("NN_RP_RECORD")) {
            rec_ev = fopen((std::string(e) + "/events.txt").c_str(),"w");
            rec_fr = fopen((std::string(e) + "/frames.bin").c_str(),"wb");
        }
        if (const char *e = getenv("NN_RP_TESTINPUT")) {
            test_mode = e;
            if (const char *k = getenv("NN_RP_TESTSCALE")) test_scale = test_scaley = atof(k);
            if (const char *k = getenv("NN_RP_TESTSCALEY")) test_scaley = atof(k);
            if (const char *k = getenv("NN_RP_TESTSTART")) test_start = atof(k);
            if (const char *k = getenv("NN_RP_TESTKEYS")) parseTestKeys(k);
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
            if (vp_due) analyse();

            SDL_Event e;
            while (SDL_PollEvent(&e)) handleEvent(e);
            testInput();

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
            else if (vp.enabled && haveframe) {
                /* Between frames the picture follows the mouse too: drawn again when the prediction has changed. */
                const uint64_t now_us = RP_Micros();
                if (now_us - last_present_us >= 5000) {
                    vp.idle(now_us);
                    float px, py;
                    predicted(now_us,px,py);
                    int dx = 0, dy = 0;
                    const bool dot = pointerDot(displayRect(),dx,dy);
                    if (fabsf(px - shown_px) >= 0.5f || fabsf(py - shown_py) >= 0.5f || dot != dot_on || (dot && (dx != dot_px || dy != dot_py))) present();
                }
            }
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
        if (rec_ev) fclose(rec_ev);
        if (rec_fr) fclose(rec_fr);
        if (audio) SDL_CloseAudioDevice(audio);
        if (texture) SDL_DestroyTexture(texture);
        if (zoom_tex) SDL_DestroyTexture(zoom_tex);
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
