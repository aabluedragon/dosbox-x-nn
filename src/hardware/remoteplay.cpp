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
extern bool dos_kernel_disabled;
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
    RP_FRAMEACK = 10    /* viewer -> host: session, u8 flags (1: has a frame), newest decoded frame,
                           video fragments expected and received (cumulative) */
};

/* Viewer features (u32 after the session id in RP_HELLO; older viewers send none) */
const uint32_t RP_FEAT_REFS = 1;        /* decodes against the frame a frame names, sends RP_FRAMEACK */

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
const size_t RP_FRAME_HDR = 12;         /* u16 w, u16 h, u8 fmt, u8 key, u16 back, u32 rawsize */
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
    bool keydown[KBD_LAST];
    uint32_t buttons = 0;
    uint32_t features = 0;              /* RP_FEAT_* from its HELLO */
    bool hasack = false;
    uint32_t acked = 0;                 /* newest frame it decoded */
    uint32_t loss_exp = 0, loss_got = 0, loss_ms = 0;  /* fragment counters at the last rate decision */
};

UDPsocket host_sock = NULL;
UDPpacket *host_out = NULL, *host_in = NULL;
bool host_running = false;

/* Keepalive while the emulator stands still. A menu (Windows' own menu bar, or the SDL drawn one), dragging or
 * resizing the window on Windows, and message boxes stop the main loop, so REMOTEPLAY_Poll stops: the viewers heard
 * nothing and gave up after the timeout ("The connection to the host was lost"), and hiding the screen from the Video
 * menu broke the stream. While the main loop has not polled for a while, this thread answers the viewers that are
 * already in, so they stay connected (the picture waits); their input is left for the main loop (viewers send it
 * again until it is acknowledged). host_lock guards the socket, its packets and the viewer list. */
SDL_mutex *host_lock = NULL;
SDL_Thread *host_keepalive = NULL;
volatile int host_keepalive_stop = 0;
volatile uint32_t host_lastpoll = 0;
uint16_t host_port = 0;
Viewer viewers[RP_MAX_VIEWERS];
unsigned cfg_timeout = 10000;
unsigned cfg_maxfps = 0;

/* Video encoder state */
struct EncFrame { uint32_t id = 0; std::vector<uint8_t> raw; };
std::vector<uint8_t> enc_raw, enc_work, enc_comp;
std::vector<uint8_t> enc_lastin;                /* the last 32-bit picture offered, as given */
unsigned enc_inw = 0, enc_inh = 0;
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
double rate_delivered = 0;                      /* the most video bytes/s a viewer has received lately */
bool rate_adaptive = false;
bool rate_limited = false;                      /* a viewer reported loss: video is paced from then on */

/* 32-bit frames of 256-colour modes: DOSBox-X draws SVGA 8-bit modes (Shadow Warrior's 800x600, for one) as 32-bit
 * pixels, a quarter of which would do. Such frames go out as palette + indices; a colour keeps its index from frame
 * to frame (deltas stay small) until more than 256 have been seen, and then the palette starts over. */
struct PaletteMap {
    uint32_t colors[256];
    unsigned count = 0;
    uint32_t keys[1024];
    uint16_t vals[1024];                /* index + 1; 0: free */
    void clear(void) { count = 0; memset(vals,0,sizeof(vals)); }
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

/* Writes palette + indices for a 32-bit frame into out, or returns false when it has more than 256 colours. */
bool RP_IndexFrame(unsigned width,unsigned height,unsigned pitch,const uint8_t *data,std::vector<uint8_t> &out) {
    if (!enc_palmap_init) { enc_palmap.clear(); enc_palmap_init = true; }
    out.resize(768 + (size_t)width * height);
    for (unsigned attempt=0;attempt < 2;attempt++) {
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
        if (ok) {
            uint8_t *pal = out.data();
            for (unsigned i=0;i < 256;i++) {
                const uint32_t c = i < enc_palmap.count ? enc_palmap.colors[i] : 0;
                pal[i*3+0] = (uint8_t)(c >> 16); pal[i*3+1] = (uint8_t)(c >> 8); pal[i*3+2] = (uint8_t)c;
            }
            return true;
        }
        enc_palmap.clear();     /* new colours (a fade, another screen): start the palette over */
    }
    return false;
}

/* Sound */
std::vector<int16_t> aud_buf;
uint32_t aud_seq = 0;
unsigned aud_rate = 0;

/* Statistics */
uint64_t stat_bytes = 0;
uint32_t stat_frames = 0, stat_keys = 0, stat_offered = 0, stat_skipped = 0;

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
    else if (rate_limited && loss < 0.01 && now - rate_cut_ms >= 400 && now - send_backlog_ms < 300) {
        send_rate = std::min(send_rate * 1.2,RP_RATE_MAX);
    }
    if (rate_limited) send_rate = std::min(send_rate,std::max(RP_RATE_START,rate_delivered * 2.0));
    if (now - rate_log_ms >= 5000) {
        static uint32_t last_frames = 0;
        static uint64_t last_bytes = 0;
        const double secs = rate_log_ms ? (now - rate_log_ms) / 1000.0 : 5.0;
        const uint32_t frames = stat_frames - last_frames;
        static uint32_t last_offered = 0, last_skipped = 0;
        LOG_MSG("INPUTNET: video at most %u KB/s, sent %u KB/s in %.1f frames/s (of %.1f, %.1f skipped for the link), %.1f%% of fragments lost; CPU %ld cycles",
            (unsigned)(send_rate / 1024.0),(unsigned)((stat_bytes - last_bytes) / 1024.0 / secs),frames / secs,
            (stat_offered - last_offered) / secs,(stat_skipped - last_skipped) / secs,loss * 100.0,(long)CPU_CycleMax);
        last_frames = stat_frames; last_bytes = stat_bytes; last_offered = stat_offered; last_skipped = stat_skipped;
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
                    if (r.ok) v->applied = seq;
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

void Host_FlushQueue(void) {
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
    while (!send_queue.empty() && send_budget >= (double)send_queue.front().size()) {
        const std::vector<uint8_t> &p = send_queue.front();
        Host_SendAll(p.data(),p.size());
        send_budget -= (double)p.size();
        send_queued -= std::min(send_queued,p.size());
        send_queue.pop_front();
    }
    /* Video waits for the rate, not the other way round: only then is a higher rate worth trying. */
    if (!send_queue.empty()) send_backlog_ms = now;
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
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) viewers[i].active = false;
    host_running = true;
    host_port = port;
    /* The window may have lost focus already, which stops cycles=max from guessing: a host keeps guessing without it. */
    CPU_Disable_SkipAutoAdjust();
    if (host_lock == NULL) host_lock = SDL_CreateMutex();
    host_lastpoll = SDL_GetTicks();
    host_keepalive_stop = 0;
    if (host_lock != NULL && host_keepalive == NULL)
#if SDL_VERSION_ATLEAST(2,0,0)
        host_keepalive = SDL_CreateThread(Host_KeepaliveThread,"INPUTNET keepalive",NULL);
#else
        host_keepalive = SDL_CreateThread(Host_KeepaliveThread,NULL);
#endif
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
    for (unsigned i=0;i < RP_MAX_VIEWERS;i++) Host_DropViewer(viewers[i],reason ? reason : "the host stopped",true);
    SDLNet_UDP_Close(host_sock);
    host_sock = NULL;
    host_running = false;
    remoteplay_streaming = false;
    send_queue.clear();
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
    prog->WriteOut("Sent %u frames (%u key frames), %u KB.\n",(unsigned)stat_frames,(unsigned)stat_keys,
        (unsigned)(stat_bytes / 1024u));
}

void REMOTEPLAY_Poll(void) {
    if (!host_running) return;

    /* GFX_Events runs very often; the socket only needs a look once a
     * millisecond, unless paced video is waiting to go out. */
    static uint32_t lastpoll = 0;
    const uint32_t now = SDL_GetTicks();
    if (now == lastpoll && (send_queue.empty() || send_budget < (double)send_queue.front().size())) return;
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

    Host_FlushQueue();
}

void REMOTEPLAY_AddFrame(unsigned width,unsigned height,unsigned bpp,unsigned pitch,
    const uint8_t *data,const uint8_t *pal) {
    if (!host_running || !remoteplay_streaming || data == NULL || TTF_using()) return;

    const unsigned bytespp = RP_BytesPerPixel(bpp);
    if (bytespp == 0 || width == 0 || height == 0 || width > 4096 || height > 4096) return;
    const size_t rowbytes = (size_t)width * bytespp;
    if (rowbytes > pitch) return;

    const uint32_t now = SDL_GetTicks();
    if (cfg_maxfps != 0 && !enc_needkey && (now - enc_lastsend) * cfg_maxfps < 1000u) return;
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
        Host_FlushQueue();
        return;
    }

    /* The same 32-bit picture as the last one: nothing to index or send. That is most refreshes while a game draws fewer
     * frames than the display shows (Shadow Warrior at 800x600), and indexing it every time costs the game emulation. */
    if (bpp == 32) {
        if (width == enc_inw && height == enc_inh && enc_haveprev && !enc_needkey && !enc_keyreq && (now - enc_lastsend) < 250) {
            bool same = true;
            for (unsigned y=0;y < height && same;y++)
                same = memcmp(enc_lastin.data() + (size_t)y * rowbytes,data + (size_t)y * pitch,rowbytes) == 0;
            if (same) return;
        }
        enc_lastin.resize(rowbytes * height);
        for (unsigned y=0;y < height;y++) memcpy(enc_lastin.data() + (size_t)y * rowbytes,data + (size_t)y * pitch,rowbytes);
        enc_inw = width; enc_inh = height;
    }

    /* 32-bit pixels of at most 256 colours go out as an 8-bit frame (any viewer decodes it). */
    const bool indexed = (bpp == 32) && RP_IndexFrame(width,height,pitch,data,enc_raw);
    const unsigned fmt = indexed ? 8 : bpp;
    const size_t palbytes = (fmt == 8) ? 768 : 0;
    const size_t rawsize = palbytes + (size_t)width * RP_BytesPerPixel(fmt) * height;
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

    if (!indexed) {
        enc_raw.resize(rawsize);
        uint8_t *o = enc_raw.data();
        if (palbytes) {
            for (unsigned i=0;i < 256;i++) {
                *o++ = pal[i*4+0];
                *o++ = pal[i*4+1];
                *o++ = pal[i*4+2];
            }
        }
        for (unsigned y=0;y < height;y++) {
            memcpy(o,data + (size_t)y * pitch,rowbytes);
            o += rowbytes;
        }
    }

    const bool refs = rate_adaptive;
    bool key = enc_needkey || enc_hist.empty() || (now - enc_lastkey) >= (refs ? 10000u : 5000u)
        || (enc_keyreq && now - enc_lastkey >= 300);
    if (!key && memcmp(enc_raw.data(),enc_hist.back().raw.data(),rawsize) == 0 && (now - enc_lastsend) < 250)
        return; /* nothing changed; still send now and then so the viewer knows we are alive */

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
        if (base == NULL) key = true;
    }
    /* With named bases a key frame goes out at most every 300 ms unless the picture itself changed shape: a viewer that
     * lost the last one asks again, and back to back key frames would only fill the link. */
    if (key && refs && !enc_needkey && enc_haskey && now - enc_lastkey < 300) return;

    const uint8_t *src = enc_raw.data();
    if (!key) {
        enc_work.resize(rawsize);
        const uint8_t *a = enc_raw.data(), *b = base->raw.data();
        uint8_t *d = enc_work.data();
        size_t i = 0;
        for (;i + 8 <= rawsize;i += 8) {
            uint64_t x, y;
            memcpy(&x,a + i,8); memcpy(&y,b + i,8);
            x ^= y;
            memcpy(d + i,&x,8);
        }
        for (;i < rawsize;i++) d[i] = a[i] ^ b[i];
        src = enc_work.data();
    }

    const uint32_t id = enc_frameid + 1;
    uLongf clen = compressBound((uLong)rawsize);
    enc_comp.resize(RP_FRAME_HDR + clen);
    uint8_t *h = enc_comp.data();
    if (compress2(h + RP_FRAME_HDR,&clen,src,(uLong)rawsize,1) != Z_OK) return;
    /* Much motion (turning in a 3D game): the frame alone often compresses better than its difference. Such a frame
     * goes out as a key frame, which every viewer decodes, whatever it has. */
    if (!key && clen > rawsize / 16) {
        uLongf ilen = compressBound((uLong)rawsize);
        enc_work.resize(ilen);
        if (compress2(enc_work.data(),&ilen,enc_raw.data(),(uLong)rawsize,1) == Z_OK && ilen < clen) {
            memcpy(h + RP_FRAME_HDR,enc_work.data(),ilen);
            clen = ilen;
            key = true;
        }
    }
    const unsigned back = (!key && refs) ? (unsigned)(id - base->id) : 0;
    h[0] = (uint8_t)width; h[1] = (uint8_t)(width >> 8);
    h[2] = (uint8_t)height; h[3] = (uint8_t)(height >> 8);
    h[4] = (uint8_t)fmt; h[5] = key ? 1 : 0;
    h[6] = (uint8_t)back; h[7] = (uint8_t)(back >> 8);
    h[8] = (uint8_t)rawsize; h[9] = (uint8_t)(rawsize >> 8); h[10] = (uint8_t)(rawsize >> 16); h[11] = (uint8_t)(rawsize >> 24);

    const size_t total = RP_FRAME_HDR + clen;
    const size_t count = (total + RP_FRAG - 1) / RP_FRAG;
    if (count > 65535) return;

    enc_frameid = id;
    for (size_t i=0;i < count;i++) {
        const size_t off = i * RP_FRAG;
        const size_t n = std::min(RP_FRAG,total - off);
        Writer w(RP_VIDEO);
        w.u32(enc_frameid);
        w.u16((uint16_t)i);
        w.u16((uint16_t)count);
        w.bytes(enc_comp.data() + off,n);
        send_queue.push_back(std::vector<uint8_t>(w.buf,w.buf + w.len));
        send_queued += w.len;
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
    enc_hist.push_back(std::move(f));
    while (enc_hist.size() > 1 && (enc_hist.size() > RP_HISTORY || enc_hist.size() * rawsize > RP_HISTORY_BYTES))
        enc_hist.pop_front();

    enc_haveprev = true;
    enc_needkey = false;
    enc_lastsend = now;
    stat_frames++;
    if (key) { enc_lastkey = now; enc_keyreq = false; stat_keys++; }

    Host_FlushQueue();
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
    bool keydown[KBD_LAST];
    uint32_t buttons = 0;

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
        w.u32(RP_FEAT_REFS);
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
        SDL_SetWindowTitle(window,t.c_str());
    }

    /* ---- input ---- */

    void pushEvent(const uint8_t *bytes,size_t n) {
        PendingEvent ev;
        ev.seq = nextseq++;
        ev.bytes.assign(bytes,bytes + n);
        pending.push_back(ev);
        input_dirty = true;
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
        if (acc_xrel == 0 && acc_yrel == 0) return;
        Writer tmp(0);
        const size_t start = tmp.len;
        tmp.u8(RPEV_MMOVE);
        tmp.f32(acc_xrel); tmp.f32(acc_yrel); tmp.f32(last_x); tmp.f32(last_y);
        tmp.u8(captured ? 1 : 0);
        pushEvent(tmp.buf + start,tmp.len - start);
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
        lastinput = SDL_GetTicks();
        input_dirty = false;
    }

    void setCapture(bool on) {
        if (captured == on) return;
        if (!on) releaseAll();
        captured = on;
        SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
        updateTitle();
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
                    setCapture(false);
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
                    acc_xrel += (float)e.motion.xrel;
                    acc_yrel += (float)e.motion.yrel;
                    const SDL_Rect r = displayRect();
                    if (r.w > 1 && r.h > 1) {
                        last_x = std::min(1.0f,std::max(0.0f,(float)(e.motion.x - r.x) / (float)(r.w - 1)));
                        last_y = std::min(1.0f,std::max(0.0f,(float)(e.motion.y - r.y) / (float)(r.h - 1)));
                    }
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                const bool pressed = (e.type == SDL_MOUSEBUTTONDOWN);
                if (!captured) {
                    if (pressed && e.button.button == SDL_BUTTON_LEFT) setCapture(true);
                    break;
                }
                flushMotion();
                if (e.button.button == SDL_BUTTON_LEFT) buttonEvent(0,pressed);
                else if (e.button.button == SDL_BUTTON_RIGHT) buttonEvent(1,pressed);
                else if (e.button.button == SDL_BUTTON_MIDDLE) buttonEvent(2,pressed);
                break;
            }
            case SDL_MOUSEWHEEL:
                if (captured && e.wheel.y != 0) {
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
        const bool key = data[5] != 0;
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

        scratch.resize(rawsize);
        uLongf outlen = (uLongf)rawsize;
        if (uncompress(scratch.data(),&outlen,data + RP_FRAME_HDR,(uLong)(len - RP_FRAME_HDR)) != Z_OK || outlen != rawsize) {
            needkey = true;
            return;
        }

        if (key) {
            frame.swap(scratch);
            keyframe = frame;
            keyid = id;
            havekey = true;
        }
        else {
            if (base != &frame) frame = *base;
            uint8_t *d = frame.data();
            const uint8_t *s = scratch.data();
            for (size_t i=0;i < rawsize;i++) d[i] ^= s[i];
        }
        if (!hist.empty() && hist.back().second.size() != rawsize) hist.clear();
        hist.push_back(std::make_pair(id,frame));
        while (hist.size() > 1 && (hist.size() > RP_HISTORY || hist.size() * rawsize > RP_HISTORY_BYTES)) hist.pop_front();
        frame_w = w; frame_h = h; frame_fmt = fmt;
        lastid = id;
        haveframe = true;
        needkey = false;
        present();
    }

    void present(void) {
        if (haveframe) {
            if (texture == NULL || tex_w != frame_w || tex_h != frame_h) {
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,(int)frame_w,(int)frame_h);
                tex_w = frame_w; tex_h = frame_h;
            }
            const size_t pixels = (size_t)frame_w * frame_h;
            argb.resize(pixels);
            const uint8_t *p = frame.data();
            if (frame_fmt == 8) {
                uint32_t lut[256];
                for (unsigned i=0;i < 256;i++)
                    lut[i] = 0xFF000000u | ((uint32_t)p[i*3] << 16) | ((uint32_t)p[i*3+1] << 8) | p[i*3+2];
                const uint8_t *s = p + 768;
                for (size_t i=0;i < pixels;i++) argb[i] = lut[s[i]];
            }
            else if (frame_fmt == 15 || frame_fmt == 16) {
                for (size_t i=0;i < pixels;i++) {
                    const unsigned v = p[i*2] | (p[i*2+1] << 8);
                    unsigned r, g, b;
                    if (frame_fmt == 15) { r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31; r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); }
                    else { r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31; r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); }
                    b = (b << 3) | (b >> 2);
                    argb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
            else {
                for (size_t i=0;i < pixels;i++) {
                    uint32_t v;
                    memcpy(&v,p + i*4,4);
                    argb[i] = 0xFF000000u | (v & 0xFFFFFFu);
                }
            }
            if (texture) SDL_UpdateTexture(texture,NULL,argb.data(),(int)(frame_w * 4));
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
            case RP_INPUTACK: {
                const uint32_t ack = r.u32();
                if (!r.ok) break;
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
        SDLNet_UDP_AddSocket(set,sock);

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"nearest");
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
            SDLNet_CheckSockets(set,2);
            while (SDLNet_UDP_Recv(sock,in) > 0) {
                if (RP_SameAddr(in->address,server)) handlePacket(in->data,(size_t)in->len);
                if (!running) break;
            }

            SDL_Event e;
            while (SDL_PollEvent(&e)) handleEvent(e);

            const uint32_t now = SDL_GetTicks();
            if (now - lasthello >= (welcomed ? 1000u : 500u)) sendHello();
            if ((acc_xrel != 0 || acc_yrel != 0) && now - lastmove >= 4) flushMotion();
            if (!pending.empty() && (input_dirty || now - lastinput >= 30)) sendInput();
            if (welcomed && now - lastack >= 100) sendAck();   /* also reports loss when no frame completes */
            if (welcomed && needkey && now - lastkeyreq >= 150) {
                Writer w(RP_KEYREQ);
                send(w);
                lastkeyreq = now;
            }
            pumpAudio();
            if (redraw) present();

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
