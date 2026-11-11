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

/* Shared input ("input net") emulation.
 *
 * Many DOS games only ever supported two players sharing one keyboard on one
 * PC. This module lets two (or more) DOSBox-X instances on different machines
 * play those games together: the emulated machines run the same program in
 * lockstep and every instance receives the keyboard/mouse input of every
 * participant, so each player can use their own keyboard while the game still
 * thinks both players sit at the same PC.
 *
 * Nothing about the video output is transmitted - only emulator state (an
 * optional save state hand-off when a client joins) and input events. */

#ifndef DOSBOX_INPUTNET_H
#define DOSBOX_INPUTNET_H

#include "dosbox.h"
#include "keyboard.h"

#include <stdint.h>

class Section;
class Program;

#if defined(C_SDL_NET) || defined(C_SDL2_NET)
# define C_INPUTNET 1
#endif

/* Fast path flag. While an input sharing session is running local input events
 * must be routed through the network layer (so that every instance applies them
 * at the same emulated time) instead of being applied right away. Checked
 * inline by the input entry points so that the normal case costs nothing. */
extern bool inputnet_capture_active;

/* Interception hooks used by the keyboard/mouse entry points. Each returns true
 * when the event was consumed (queued for synchronized delivery) and therefore
 * must not be handed to the emulated hardware by the caller. */
bool INPUTNET_InterceptKey(KBD_KEYS keytype,bool pressed);
bool INPUTNET_InterceptMouseMotion(float xrel,float yrel,float x,float y,bool emulate);
bool INPUTNET_InterceptMouseButton(uint8_t button,bool pressed);
bool INPUTNET_InterceptMouseWheel(int32_t scroll);

/* "Raw" input entry points that bypass the interception hooks. These are what
 * the input sharing layer calls once an event is due, and what the emulator
 * itself uses for events it generates internally (such as key auto-repeat). */
void KEYBOARD_AddKeyRaw(KBD_KEYS keytype,bool pressed);
void Mouse_CursorMovedRaw(float xrel,float yrel,float x,float y,bool emulate);
/* true while Mouse_CursorMovedRaw applies relative motion from a remote play viewer */
extern bool mouse_motion_is_relative;
void Mouse_ButtonPressedRaw(uint8_t button);
void Mouse_ButtonReleasedRaw(uint8_t button);
void Mouse_WheelMovedRaw(int32_t scroll);

/* Emulated CPU speed synchronization.
 *
 * With "cycles=max" or "cycles=auto" each DOSBox-X instance continuously
 * retunes how many cycles it executes per emulated millisecond, based on how
 * fast its own host machine is. Two instances would therefore end up executing
 * a different amount of guest code per emulated millisecond and drift apart
 * even though they are fed identical input. So while a session is running the
 * server owns the cycle count: it publishes every change as part of its input
 * stream and all participants apply it at the same emulated time.
 *
 * INPUTNET_ProposeCycleMax() returns true when the shared input layer has taken
 * ownership of the change, meaning the caller must not apply it itself. */
bool INPUTNET_ProposeCycleMax(uint32_t newmax,bool reset_counters);

/* Implemented in dosbox.cpp. ApplyCycleMax() changes the speed immediately;
 * SetCycleMax() routes it through the shared input layer first and returns
 * false when the change was deferred. */
void DOSBOX_ApplyCycleMax(cpu_cycles_count_t newmax,bool reset_counters);
bool DOSBOX_SetCycleMax(cpu_cycles_count_t newmax,bool reset_counters);

/* Session state, for the INPUTNET command and the status line. */
bool INPUTNET_IsServer(void);
bool INPUTNET_IsConnected(void);

/* Module life cycle, mirrors the IPX emulation. */
void INPUTNET_Init(void);
void INPUTNET_Setup(Section*);
void INPUTNET_ShutDown(Section*);

void INPUTNET_ProgramStart(Program * * make);

#endif
