/*
 *  Inputnet remote play: the host runs the game and streams its picture and
 *  sound; viewers only send keyboard and mouse input back.
 */

#ifndef DOSBOX_REMOTEPLAY_H
#define DOSBOX_REMOTEPLAY_H

#include "inputnet.h"

#include <stdint.h>
#include <string>

class Section_prop;
class Program;

/* Set while at least one viewer is connected; the render and mixer hooks check
 * it before doing any work. */
extern bool remoteplay_streaming;

/* Set while a remote play host does not show its picture on this machine
 * ([inputnet] hostscreen=false, -inputnet-hidescreen, or the "Show screen
 * (remote play)" menu item). The render code then only keeps the frame the
 * viewers are sent, the window shows a small placeholder, and the mixer
 * silences the local sound output (the viewers still get the sound). */
extern bool remoteplay_screen_hidden;

/* Host side */
void REMOTEPLAY_Init(void);
void REMOTEPLAY_Configure(Section_prop *section);
bool REMOTEPLAY_StartServer(uint16_t port);
void REMOTEPLAY_StopServer(const char *reason);
bool REMOTEPLAY_ServerRunning(void);
int REMOTEPLAY_PresentMode(void);   /* how a host's frames are timed: 0 scanout, 1 retrace, 2 flip; 0 without a server */
void REMOTEPLAY_Status(Program *prog);
void REMOTEPLAY_Poll(void);
void REMOTEPLAY_AddFrame(unsigned width,unsigned height,unsigned bpp,unsigned pitch,
    const uint8_t *data,const uint8_t *pal);
void REMOTEPLAY_AddAudio(unsigned rate,unsigned frames,const int16_t *samples);

/* True when the configuration or command line makes this DOSBox-X a remote
 * play host, and whether it starts with the local screen hidden. */
bool REMOTEPLAY_HostRequested(void);
bool REMOTEPLAY_HideScreenRequested(void);

/* Shows or hides the local picture of a remote play host (sdlmain.cpp). */
void GFX_RemotePlaySetScreenHidden(bool hidden);
void GFX_RemotePlayDrawHidden(void);
void REMOTEPLAY_UpdateMenu(void);

/* Remote input guard: called by DOS_Terminate once the parent process is the
 * current one again, so keys a viewer typed into a game that has just ended
 * cannot reach the DOS prompt. */
void REMOTEPLAY_ProcessEnded(void);

/* Viewer side. Returns true (and the address) when DOSBox-X was started to
 * join a remote play session, in which case main() runs the viewer instead of
 * the emulator. */
bool REMOTEPLAY_ViewerRequested(std::string &address);
int REMOTEPLAY_RunViewer(const std::string &address);

#endif
