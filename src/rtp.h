/*
 *  Copyright (c) by Ralf Beck <ralfbeck1@gmx.de>
 *  Userspace Ethernet driver for the iConnectivity Mio/Midi+ (tm) devices
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#ifndef RTPMIDI_H
#define RTPMIDI_H

// Caution: These definitions are valid for little endian systems only!

typedef struct {
  unsigned char  cc:4;
  unsigned char  x:1;
  unsigned char  p:1;
  unsigned char  version:2;
  unsigned char  payload_type:7;
  unsigned char  m:1;
  unsigned short seq_num;
  unsigned int   ts;
  unsigned int   ssrc;
} rtp_header_s;

typedef struct {
   unsigned char len:4;
   unsigned char p:1;
   unsigned char z:1;
   unsigned char j:1;
   unsigned char b:1;
   unsigned char data[0];
}  rtp_midi_command_short_header_s;

typedef struct {
   unsigned short len:12;
   unsigned short p:1;
   unsigned short z:1;
   unsigned short j:1;
   unsigned short b:1;
   unsigned char data[0];
}  rtp_midi_command_long_header_s;

#define APPLE_MIDI_VERSION         0x02000000
#define APPLE_MIDI_INVITE          0x4e49ffff
#define APPLE_MIDI_INVITE_ACCEPTED 0x4b4fffff
#define APPLE_MIDI_INVITE_REJECTED 0x4f4effff
#define APPLE_MIDI_SYNC            0x4b43ffff
#define APPLE_MIDI_BYE             0x5942ffff
#define APPLE_MIDI_RECV_FEEDBACK   0x5352ffff
#define APPLE_MIDI_BITRATE_LIMIT   0x4c52ffff

#define APPLE_MIDI_TOKEN           0x11223344
#define APPLE_MIDI_SSRC            0xAABBCCDD

typedef struct {
  unsigned int command;
  unsigned int version;
  unsigned int token;
  unsigned int ssrc;
  // char name[256];
} apple_midi_invite;

typedef struct {
  unsigned int command;
  unsigned int ssrc;
  unsigned char count;
  unsigned char pad[3];
  unsigned int ts0_h;
  unsigned int ts0_l;
  unsigned int ts1_h;
  unsigned int ts1_l;
  unsigned int ts2_h;
  unsigned int ts2_l;
} apple_midi_sync;

typedef struct {
  unsigned int command;
  unsigned int version;
  unsigned int token;
  unsigned int ssrc;
  char         pad[2];
} apple_midi_bye;

typedef struct {
  unsigned int command;
  unsigned int version;
  unsigned int token;
  unsigned int ssrc;
  char         pad[2];
} apple_midi_feedback;

typedef struct {
  unsigned int command;
  unsigned int version;
  unsigned int token;
  unsigned int ssrc;
  char         pad[2];
} apple_midi_rate;

#endif

