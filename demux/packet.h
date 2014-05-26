/*
 * This file is part of mplayer2.
 *
 * mplayer2 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mplayer2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mplayer2; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef MPLAYER_DEMUX_PACKET_H
#define MPLAYER_DEMUX_PACKET_H

#include <stdbool.h>
#include <inttypes.h>

// Holds one packet/frame/whatever
typedef struct demux_packet {
    int len;
    double pts;
    double dts;
    double duration;
    double stream_pts;
    int64_t pos; // position in source file byte stream
    unsigned char *buffer;
    bool keyframe;
    int stream; // source stream index
    struct demux_packet *next;
    void *allocation;
    struct AVPacket *avpacket;   // original libavformat packet (demux_lavf)
} demux_packet_t;

#endif /* MPLAYER_DEMUX_PACKET_H */
