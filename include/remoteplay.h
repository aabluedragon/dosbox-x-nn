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

/* Host side */
void REMOTEPLAY_Configure(Section_prop *section);
bool REMOTEPLAY_StartServer(uint16_t port);
void REMOTEPLAY_StopServer(const char *reason);
bool REMOTEPLAY_ServerRunning(void);
void REMOTEPLAY_Status(Program *prog);
void REMOTEPLAY_Poll(void);
void REMOTEPLAY_AddFrame(unsigned width,unsigned height,unsigned bpp,unsigned pitch,
    const uint8_t *data,const uint8_t *pal);
void REMOTEPLAY_AddAudio(unsigned rate,unsigned frames,const int16_t *samples);

/* Viewer side. Returns true (and the address) when DOSBox-X was started to
 * join a remote play session, in which case main() runs the viewer instead of
 * the emulator. */
bool REMOTEPLAY_ViewerRequested(std::string &address);
int REMOTEPLAY_RunViewer(const std::string &address);

#endif
