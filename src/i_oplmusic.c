//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//   System interface for music.
//


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memio.h"
#include "mus2mid.h"

#include "deh_main.h"
#include "i_sound.h"
#include "i_swap.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#include "opl.h"
#include "midifile.h"

// #define OPL_MIDI_DEBUG

#define MAXMIDLENGTH (96 * 1024)
#define GENMIDI_NUM_INSTRS  128
#define GENMIDI_NUM_PERCUSSION 47

#define GENMIDI_HEADER          "#OPL_II#"
#define GENMIDI_FLAG_FIXED      0x0001         /* fixed pitch */
#define GENMIDI_FLAG_2VOICE     0x0004         /* double voice (OPL3) */

#define PERCUSSION_LOG_LEN 16

typedef struct
{
    byte tremolo;
    byte attack;
    byte sustain;
    byte waveform;
    byte scale;
    byte level;
} PACKEDATTR genmidi_op_t;

typedef struct
{
    genmidi_op_t modulator;
    byte feedback;
    genmidi_op_t carrier;
    byte unused;
    short base_note_offset;
} PACKEDATTR genmidi_voice_t;

typedef struct
{
    unsigned short flags;
    byte fine_tuning;
    byte fixed_note;

    genmidi_voice_t voices[2];
} PACKEDATTR genmidi_instr_t;

// Data associated with a channel of a track that is currently playing.

typedef struct
{
    // The instrument currently used for this track.

    genmidi_instr_t *instrument;

    // Volume level

    int volume;
    int volume_base;

    // Pan

    int pan;

    // Pitch bend value:

    int bend;

} opl_channel_data_t;

// Data associated with a track that is currently playing.

typedef struct
{
    // Data for each channel.

    opl_channel_data_t channels[MIDI_CHANNELS_PER_TRACK];

    // Track iterator used to read new events.

    midi_track_iter_t *iter;
} opl_track_data_t;

typedef struct opl_voice_s opl_voice_t;

struct opl_voice_s
{
    // Index of this voice:
    int index;

    // The operators used by this voice:
    int op1, op2;

    // Array used by voice:
    int array;

    // Currently-loaded instrument data
    genmidi_instr_t *current_instr;

    // The voice number in the instrument to use.
    // This is normally set to zero; if this is a double voice
    // instrument, it may be one.
    unsigned int current_instr_voice;

    // The channel currently using this voice.
    opl_channel_data_t *channel;

    // The midi key that this voice is playing.
    unsigned int key;

    // The note being played.  This is normally the same as
    // the key, but if the instrument is a fixed pitch
    // instrument, it is different.
    unsigned int note;

    // The frequency value being used.
    unsigned int freq;

    // The volume of the note being played on this channel.
    unsigned int note_volume;

    // The current volume (register value) that has been set for this channel.
    unsigned int reg_volume;

    // Pan.
    unsigned int reg_pan;

    // Priority.
    unsigned int priority;

    // Next in linked list; a voice is always either in the
    // free list or the allocated list.
    opl_voice_t *next;
};

// Operators used by the different voices.

static const int voice_operators[2][OPL_NUM_VOICES] = {
    { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 },
    { 0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15 }
};

// Frequency values to use for each note.

static const unsigned short frequency_curve[] = {

    0x133, 0x133, 0x134, 0x134, 0x135, 0x136, 0x136, 0x137,   // -1
    0x137, 0x138, 0x138, 0x139, 0x139, 0x13a, 0x13b, 0x13b,
    0x13c, 0x13c, 0x13d, 0x13d, 0x13e, 0x13f, 0x13f, 0x140,
    0x140, 0x141, 0x142, 0x142, 0x143, 0x143, 0x144, 0x144,

    0x145, 0x146, 0x146, 0x147, 0x147, 0x148, 0x149, 0x149,   // -2
    0x14a, 0x14a, 0x14b, 0x14c, 0x14c, 0x14d, 0x14d, 0x14e,
    0x14f, 0x14f, 0x150, 0x150, 0x151, 0x152, 0x152, 0x153,
    0x153, 0x154, 0x155, 0x155, 0x156, 0x157, 0x157, 0x158,

    // These are used for the first seven MIDI note values:

    0x158, 0x159, 0x15a, 0x15a, 0x15b, 0x15b, 0x15c, 0x15d,   // 0
    0x15d, 0x15e, 0x15f, 0x15f, 0x160, 0x161, 0x161, 0x162,
    0x162, 0x163, 0x164, 0x164, 0x165, 0x166, 0x166, 0x167,
    0x168, 0x168, 0x169, 0x16a, 0x16a, 0x16b, 0x16c, 0x16c,

    0x16d, 0x16e, 0x16e, 0x16f, 0x170, 0x170, 0x171, 0x172,   // 1
    0x172, 0x173, 0x174, 0x174, 0x175, 0x176, 0x176, 0x177,
    0x178, 0x178, 0x179, 0x17a, 0x17a, 0x17b, 0x17c, 0x17c,
    0x17d, 0x17e, 0x17e, 0x17f, 0x180, 0x181, 0x181, 0x182,

    0x183, 0x183, 0x184, 0x185, 0x185, 0x186, 0x187, 0x188,   // 2
    0x188, 0x189, 0x18a, 0x18a, 0x18b, 0x18c, 0x18d, 0x18d,
    0x18e, 0x18f, 0x18f, 0x190, 0x191, 0x192, 0x192, 0x193,
    0x194, 0x194, 0x195, 0x196, 0x197, 0x197, 0x198, 0x199,

    0x19a, 0x19a, 0x19b, 0x19c, 0x19d, 0x19d, 0x19e, 0x19f,   // 3
    0x1a0, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a3, 0x1a4, 0x1a5,
    0x1a6, 0x1a6, 0x1a7, 0x1a8, 0x1a9, 0x1a9, 0x1aa, 0x1ab,
    0x1ac, 0x1ad, 0x1ad, 0x1ae, 0x1af, 0x1b0, 0x1b0, 0x1b1,

    0x1b2, 0x1b3, 0x1b4, 0x1b4, 0x1b5, 0x1b6, 0x1b7, 0x1b8,   // 4
    0x1b8, 0x1b9, 0x1ba, 0x1bb, 0x1bc, 0x1bc, 0x1bd, 0x1be,
    0x1bf, 0x1c0, 0x1c0, 0x1c1, 0x1c2, 0x1c3, 0x1c4, 0x1c4,
    0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1c9, 0x1ca, 0x1cb,

    0x1cc, 0x1cd, 0x1ce, 0x1ce, 0x1cf, 0x1d0, 0x1d1, 0x1d2,   // 5
    0x1d3, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d8,
    0x1d9, 0x1da, 0x1db, 0x1dc, 0x1dd, 0x1de, 0x1de, 0x1df,
    0x1e0, 0x1e1, 0x1e2, 0x1e3, 0x1e4, 0x1e5, 0x1e5, 0x1e6,

    0x1e7, 0x1e8, 0x1e9, 0x1ea, 0x1eb, 0x1ec, 0x1ed, 0x1ed,   // 6
    0x1ee, 0x1ef, 0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0x1f5,
    0x1f6, 0x1f6, 0x1f7, 0x1f8, 0x1f9, 0x1fa, 0x1fb, 0x1fc,
    0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, 0x201, 0x202, 0x203,

    // First note of looped range used for all octaves:

    0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b,   // 7
    0x20c, 0x20d, 0x20e, 0x20f, 0x210, 0x210, 0x211, 0x212,
    0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21a,
    0x21b, 0x21c, 0x21d, 0x21e, 0x21f, 0x220, 0x221, 0x222,

    0x223, 0x224, 0x225, 0x226, 0x227, 0x228, 0x229, 0x22a,   // 8
    0x22b, 0x22c, 0x22d, 0x22e, 0x22f, 0x230, 0x231, 0x232,
    0x233, 0x234, 0x235, 0x236, 0x237, 0x238, 0x239, 0x23a,
    0x23b, 0x23c, 0x23d, 0x23e, 0x23f, 0x240, 0x241, 0x242,

    0x244, 0x245, 0x246, 0x247, 0x248, 0x249, 0x24a, 0x24b,   // 9
    0x24c, 0x24d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253,
    0x254, 0x256, 0x257, 0x258, 0x259, 0x25a, 0x25b, 0x25c,
    0x25d, 0x25e, 0x25f, 0x260, 0x262, 0x263, 0x264, 0x265,

    0x266, 0x267, 0x268, 0x269, 0x26a, 0x26c, 0x26d, 0x26e,   // 10
    0x26f, 0x270, 0x271, 0x272, 0x273, 0x275, 0x276, 0x277,
    0x278, 0x279, 0x27a, 0x27b, 0x27d, 0x27e, 0x27f, 0x280,
    0x281, 0x282, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289,

    0x28b, 0x28c, 0x28d, 0x28e, 0x28f, 0x290, 0x292, 0x293,   // 11
    0x294, 0x295, 0x296, 0x298, 0x299, 0x29a, 0x29b, 0x29c,
    0x29e, 0x29f, 0x2a0, 0x2a1, 0x2a2, 0x2a4, 0x2a5, 0x2a6,
    0x2a7, 0x2a9, 0x2aa, 0x2ab, 0x2ac, 0x2ae, 0x2af, 0x2b0,

    0x2b1, 0x2b2, 0x2b4, 0x2b5, 0x2b6, 0x2b7, 0x2b9, 0x2ba,   // 12
    0x2bb, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c2, 0x2c3, 0x2c4,
    0x2c5, 0x2c7, 0x2c8, 0x2c9, 0x2cb, 0x2cc, 0x2cd, 0x2ce,
    0x2d0, 0x2d1, 0x2d2, 0x2d4, 0x2d5, 0x2d6, 0x2d8, 0x2d9,

    0x2da, 0x2dc, 0x2dd, 0x2de, 0x2e0, 0x2e1, 0x2e2, 0x2e4,   // 13
    0x2e5, 0x2e6, 0x2e8, 0x2e9, 0x2ea, 0x2ec, 0x2ed, 0x2ee,
    0x2f0, 0x2f1, 0x2f2, 0x2f4, 0x2f5, 0x2f6, 0x2f8, 0x2f9,
    0x2fb, 0x2fc, 0x2fd, 0x2ff, 0x300, 0x302, 0x303, 0x304,

    0x306, 0x307, 0x309, 0x30a, 0x30b, 0x30d, 0x30e, 0x310,   // 14
    0x311, 0x312, 0x314, 0x315, 0x317, 0x318, 0x31a, 0x31b,
    0x31c, 0x31e, 0x31f, 0x321, 0x322, 0x324, 0x325, 0x327,
    0x328, 0x329, 0x32b, 0x32c, 0x32e, 0x32f, 0x331, 0x332,

    0x334, 0x335, 0x337, 0x338, 0x33a, 0x33b, 0x33d, 0x33e,   // 15
    0x340, 0x341, 0x343, 0x344, 0x346, 0x347, 0x349, 0x34a,
    0x34c, 0x34d, 0x34f, 0x350, 0x352, 0x353, 0x355, 0x357,
    0x358, 0x35a, 0x35b, 0x35d, 0x35e, 0x360, 0x361, 0x363,

    0x365, 0x366, 0x368, 0x369, 0x36b, 0x36c, 0x36e, 0x370,   // 16
    0x371, 0x373, 0x374, 0x376, 0x378, 0x379, 0x37b, 0x37c,
    0x37e, 0x380, 0x381, 0x383, 0x384, 0x386, 0x388, 0x389,
    0x38b, 0x38d, 0x38e, 0x390, 0x392, 0x393, 0x395, 0x397,

    0x398, 0x39a, 0x39c, 0x39d, 0x39f, 0x3a1, 0x3a2, 0x3a4,   // 17
    0x3a6, 0x3a7, 0x3a9, 0x3ab, 0x3ac, 0x3ae, 0x3b0, 0x3b1,
    0x3b3, 0x3b5, 0x3b7, 0x3b8, 0x3ba, 0x3bc, 0x3bd, 0x3bf,
    0x3c1, 0x3c3, 0x3c4, 0x3c6, 0x3c8, 0x3ca, 0x3cb, 0x3cd,

    // The last note has an incomplete range, and loops round back to
    // the start.  Note that the last value is actually a buffer overrun
    // and does not fit with the other values.

    0x3cf, 0x3d1, 0x3d2, 0x3d4, 0x3d6, 0x3d8, 0x3da, 0x3db,   // 18
    0x3dd, 0x3df, 0x3e1, 0x3e3, 0x3e4, 0x3e6, 0x3e8, 0x3ea,
    0x3ec, 0x3ed, 0x3ef, 0x3f1, 0x3f3, 0x3f5, 0x3f6, 0x3f8,
    0x3fa, 0x3fc, 0x3fe, 0x36c,
};

static const unsigned short frequency_curve_beta[] = {
    0x0159, 0x0159, 0x0159, 0x0159, 0x0159, 0x0159, 0x0159, 0x0159,
    0x0159, 0x0159, 0x0159, 0x0159, 0x0159, 0x0159, 0x0159, 0x0159,
    0x015a, 0x015b, 0x015c, 0x015e, 0x015f, 0x0160, 0x0161, 0x0163,
    0x0164, 0x0165, 0x0167, 0x0168, 0x0169, 0x016b, 0x016c, 0x016d,
    0x016e, 0x0170, 0x0171, 0x0172, 0x0174, 0x0175, 0x0176, 0x0178,
    0x0179, 0x017b, 0x017c, 0x017d, 0x017f, 0x0180, 0x0181, 0x0183,
    0x0184, 0x0186, 0x0187, 0x0188, 0x018a, 0x018b, 0x018d, 0x018e,
    0x0190, 0x0191, 0x0193, 0x0194, 0x0195, 0x0197, 0x0198, 0x019a,
    0x019b, 0x019d, 0x019e, 0x01a0, 0x01a1, 0x01a3, 0x01a4, 0x01a6,
    0x01a7, 0x01a9, 0x01ab, 0x01ac, 0x01ae, 0x01af, 0x01b1, 0x01b2,
    0x01b4, 0x01b5, 0x01b7, 0x01b9, 0x01ba, 0x01bc, 0x01bd, 0x01bf,
    0x01c1, 0x01c2, 0x01c4, 0x01c6, 0x01c7, 0x01c9, 0x01ca, 0x01cc,
    0x01ce, 0x01cf, 0x01d1, 0x01d3, 0x01d4, 0x01d6, 0x01d8, 0x01da,
    0x01db, 0x01dd, 0x01df, 0x01e0, 0x01e2, 0x01e4, 0x01e6, 0x01e7,
    0x01e9, 0x01eb, 0x01ed, 0x01ef, 0x01f0, 0x01f2, 0x01f4, 0x01f6,
    0x01f8, 0x01f9, 0x01fb, 0x01fd, 0x01ff, 0x0201, 0x0203, 0x0205,
    0x0207, 0x0208, 0x020a, 0x020c, 0x020e, 0x0210, 0x0212, 0x0214,
    0x0216, 0x0218, 0x021a, 0x021c, 0x021e, 0x0220, 0x0221, 0x0223,
    0x0225, 0x0227, 0x0229, 0x022b, 0x022d, 0x022f, 0x0231, 0x0234,
    0x0236, 0x0238, 0x023a, 0x023c, 0x023e, 0x0240, 0x0242, 0x0244,
    0x0246, 0x0248, 0x024a, 0x024c, 0x024f, 0x0251, 0x0253, 0x0255,
    0x0257, 0x0259, 0x025c, 0x025e, 0x0260, 0x0262, 0x0264, 0x0267,
    0x0269, 0x026b, 0x026d, 0x026f, 0x0272, 0x0274, 0x0276, 0x0279,
    0x027b, 0x027d, 0x027f, 0x0282, 0x0284, 0x0286, 0x0289, 0x028b,
    0x028d, 0x0290, 0x0292, 0x0295, 0x0297, 0x0299, 0x029c, 0x029e,
    0x02a1, 0x02a3, 0x02a5, 0x02a8, 0x02aa, 0x02ad, 0x02af, 0x02b2,
    0x02b4, 0x02b7, 0x02b9, 0x02bc, 0x02be, 0x02c1, 0x02c3, 0x02c6,
    0x02c9, 0x02cb, 0x02ce, 0x02d0, 0x02d3, 0x02d6, 0x02d8, 0x02db,
    0x02dd, 0x02e0, 0x02e3, 0x02e5, 0x02e8, 0x02eb, 0x02ed, 0x02f0,
    0x02f3, 0x02f6, 0x02f8, 0x02fb, 0x02fe, 0x0301, 0x0303, 0x0306,
    0x0309, 0x030c, 0x030f, 0x0311, 0x0314, 0x0317, 0x031a, 0x031d,
    0x0320, 0x0323, 0x0326, 0x0329, 0x032b, 0x032e, 0x0331, 0x0334,
    0x0337, 0x033a, 0x033d, 0x0340, 0x0343, 0x0346, 0x0349, 0x034c,
    0x034f, 0x0352, 0x0356, 0x0359, 0x035c, 0x035f, 0x0362, 0x0365,
    0x0368, 0x036b, 0x036f, 0x0372, 0x0375, 0x0378, 0x037b, 0x037f,
    0x0382, 0x0385, 0x0388, 0x038c, 0x038f, 0x0392, 0x0395, 0x0399,
    0x039c, 0x039f, 0x03a3, 0x03a6, 0x03a9, 0x03ad, 0x03b0, 0x03b4,
    0x03b7, 0x03bb, 0x03be, 0x03c1, 0x03c5, 0x03c8, 0x03cc, 0x03cf,
    0x03d3, 0x03d7, 0x03da, 0x03de, 0x03e1, 0x03e5, 0x03e8, 0x03ec,
    0x03f0, 0x03f3, 0x03f7, 0x03fb, 0x03fe, 0x0601, 0x0603, 0x0605,
    0x0607, 0x0608, 0x060a, 0x060c, 0x060e, 0x0610, 0x0612, 0x0614,
    0x0616, 0x0618, 0x061a, 0x061c, 0x061e, 0x0620, 0x0621, 0x0623,
    0x0625, 0x0627, 0x0629, 0x062b, 0x062d, 0x062f, 0x0631, 0x0634,
    0x0636, 0x0638, 0x063a, 0x063c, 0x063e, 0x0640, 0x0642, 0x0644,
    0x0646, 0x0648, 0x064a, 0x064c, 0x064f, 0x0651, 0x0653, 0x0655,
    0x0657, 0x0659, 0x065c, 0x065e, 0x0660, 0x0662, 0x0664, 0x0667,
    0x0669, 0x066b, 0x066d, 0x066f, 0x0672, 0x0674, 0x0676, 0x0679,
    0x067b, 0x067d, 0x067f, 0x0682, 0x0684, 0x0686, 0x0689, 0x068b,
    0x068d, 0x0690, 0x0692, 0x0695, 0x0697, 0x0699, 0x069c, 0x069e,
    0x06a1, 0x06a3, 0x06a5, 0x06a8, 0x06aa, 0x06ad, 0x06af, 0x06b2,
    0x06b4, 0x06b7, 0x06b9, 0x06bc, 0x06be, 0x06c1, 0x06c3, 0x06c6,
    0x06c9, 0x06cb, 0x06ce, 0x06d0, 0x06d3, 0x06d6, 0x06d8, 0x06db,
    0x06dd, 0x06e0, 0x06e3, 0x06e5, 0x06e8, 0x06eb, 0x06ed, 0x06f0,
    0x06f3, 0x06f6, 0x06f8, 0x06fb, 0x06fe, 0x0701, 0x0703, 0x0706,
    0x0709, 0x070c, 0x070f, 0x0711, 0x0714, 0x0717, 0x071a, 0x071d,
    0x0720, 0x0723, 0x0726, 0x0729, 0x072b, 0x072e, 0x0731, 0x0734,
    0x0737, 0x073a, 0x073d, 0x0740, 0x0743, 0x0746, 0x0749, 0x074c,
    0x074f, 0x0752, 0x0756, 0x0759, 0x075c, 0x075f, 0x0762, 0x0765,
    0x0768, 0x076b, 0x076f, 0x0772, 0x0775, 0x0778, 0x077b, 0x077f,
    0x0782, 0x0785, 0x0788, 0x078c, 0x078f, 0x0792, 0x0795, 0x0799,
    0x079c, 0x079f, 0x07a3, 0x07a6, 0x07a9, 0x07ad, 0x07b0, 0x07b4,
    0x07b7, 0x07bb, 0x07be, 0x07c1, 0x07c5, 0x07c8, 0x07cc, 0x07cf,
    0x07d3, 0x07d7, 0x07da, 0x07de, 0x07e1, 0x07e5, 0x07e8, 0x07ec,
    0x07f0, 0x07f3, 0x07f7, 0x07fb, 0x07fe, 0x0a01, 0x0a03, 0x0a05,
    0x0a07, 0x0a08, 0x0a0a, 0x0a0c, 0x0a0e, 0x0a10, 0x0a12, 0x0a14,
    0x0a16, 0x0a18, 0x0a1a, 0x0a1c, 0x0a1e, 0x0a20, 0x0a21, 0x0a23,
    0x0a25, 0x0a27, 0x0a29, 0x0a2b, 0x0a2d, 0x0a2f, 0x0a31, 0x0a34,
    0x0a36, 0x0a38, 0x0a3a, 0x0a3c, 0x0a3e, 0x0a40, 0x0a42, 0x0a44,
    0x0a46, 0x0a48, 0x0a4a, 0x0a4c, 0x0a4f, 0x0a51, 0x0a53, 0x0a55,
    0x0a57, 0x0a59, 0x0a5c, 0x0a5e, 0x0a60, 0x0a62, 0x0a64, 0x0a67,
    0x0a69, 0x0a6b, 0x0a6d, 0x0a6f, 0x0a72, 0x0a74, 0x0a76, 0x0a79,
    0x0a7b, 0x0a7d, 0x0a7f, 0x0a82, 0x0a84, 0x0a86, 0x0a89, 0x0a8b,
    0x0a8d, 0x0a90, 0x0a92, 0x0a95, 0x0a97, 0x0a99, 0x0a9c, 0x0a9e,
    0x0aa1, 0x0aa3, 0x0aa5, 0x0aa8, 0x0aaa, 0x0aad, 0x0aaf, 0x0ab2,
    0x0ab4, 0x0ab7, 0x0ab9, 0x0abc, 0x0abe, 0x0ac1, 0x0ac3, 0x0ac6,
    0x0ac9, 0x0acb, 0x0ace, 0x0ad0, 0x0ad3, 0x0ad6, 0x0ad8, 0x0adb,
    0x0add, 0x0ae0, 0x0ae3, 0x0ae5, 0x0ae8, 0x0aeb, 0x0aed, 0x0af0,
    0x0af3, 0x0af6, 0x0af8, 0x0afb, 0x0afe, 0x0b01, 0x0b03, 0x0b06,
    0x0b09, 0x0b0c, 0x0b0f, 0x0b11, 0x0b14, 0x0b17, 0x0b1a, 0x0b1d,
    0x0b20, 0x0b23, 0x0b26, 0x0b29, 0x0b2b, 0x0b2e, 0x0b31, 0x0b34,
    0x0b37, 0x0b3a, 0x0b3d, 0x0b40, 0x0b43, 0x0b46, 0x0b49, 0x0b4c,
    0x0b4f, 0x0b52, 0x0b56, 0x0b59, 0x0b5c, 0x0b5f, 0x0b62, 0x0b65,
    0x0b68, 0x0b6b, 0x0b6f, 0x0b72, 0x0b75, 0x0b78, 0x0b7b, 0x0b7f,
    0x0b82, 0x0b85, 0x0b88, 0x0b8c, 0x0b8f, 0x0b92, 0x0b95, 0x0b99,
    0x0b9c, 0x0b9f, 0x0ba3, 0x0ba6, 0x0ba9, 0x0bad, 0x0bb0, 0x0bb4,
    0x0bb7, 0x0bbb, 0x0bbe, 0x0bc1, 0x0bc5, 0x0bc8, 0x0bcc, 0x0bcf,
    0x0bd3, 0x0bd7, 0x0bda, 0x0bde, 0x0be1, 0x0be5, 0x0be8, 0x0bec,
    0x0bf0, 0x0bf3, 0x0bf7, 0x0bfb, 0x0bfe, 0x0e01, 0x0e03, 0x0e05,
    0x0e07, 0x0e08, 0x0e0a, 0x0e0c, 0x0e0e, 0x0e10, 0x0e12, 0x0e14,
    0x0e16, 0x0e18, 0x0e1a, 0x0e1c, 0x0e1e, 0x0e20, 0x0e21, 0x0e23,
    0x0e25, 0x0e27, 0x0e29, 0x0e2b, 0x0e2d, 0x0e2f, 0x0e31, 0x0e34,
    0x0e36, 0x0e38, 0x0e3a, 0x0e3c, 0x0e3e, 0x0e40, 0x0e42, 0x0e44,
    0x0e46, 0x0e48, 0x0e4a, 0x0e4c, 0x0e4f, 0x0e51, 0x0e53, 0x0e55,
    0x0e57, 0x0e59, 0x0e5c, 0x0e5e, 0x0e60, 0x0e62, 0x0e64, 0x0e67,
    0x0e69, 0x0e6b, 0x0e6d, 0x0e6f, 0x0e72, 0x0e74, 0x0e76, 0x0e79,
    0x0e7b, 0x0e7d, 0x0e7f, 0x0e82, 0x0e84, 0x0e86, 0x0e89, 0x0e8b,
    0x0e8d, 0x0e90, 0x0e92, 0x0e95, 0x0e97, 0x0e99, 0x0e9c, 0x0e9e,
    0x0ea1, 0x0ea3, 0x0ea5, 0x0ea8, 0x0eaa, 0x0ead, 0x0eaf, 0x0eb2,
    0x0eb4, 0x0eb7, 0x0eb9, 0x0ebc, 0x0ebe, 0x0ec1, 0x0ec3, 0x0ec6,
    0x0ec9, 0x0ecb, 0x0ece, 0x0ed0, 0x0ed3, 0x0ed6, 0x0ed8, 0x0edb,
    0x0edd, 0x0ee0, 0x0ee3, 0x0ee5, 0x0ee8, 0x0eeb, 0x0eed, 0x0ef0,
    0x0ef3, 0x0ef6, 0x0ef8, 0x0efb, 0x0efe, 0x0f01, 0x0f03, 0x0f06,
    0x0f09, 0x0f0c, 0x0f0f, 0x0f11, 0x0f14, 0x0f17, 0x0f1a, 0x0f1d,
    0x0f20, 0x0f23, 0x0f26, 0x0f29, 0x0f2b, 0x0f2e, 0x0f31, 0x0f34,
    0x0f37, 0x0f3a, 0x0f3d, 0x0f40, 0x0f43, 0x0f46, 0x0f49, 0x0f4c,
    0x0f4f, 0x0f52, 0x0f56, 0x0f59, 0x0f5c, 0x0f5f, 0x0f62, 0x0f65,
    0x0f68, 0x0f6b, 0x0f6f, 0x0f72, 0x0f75, 0x0f78, 0x0f7b, 0x0f7f,
    0x0f82, 0x0f85, 0x0f88, 0x0f8c, 0x0f8f, 0x0f92, 0x0f95, 0x0f99,
    0x0f9c, 0x0f9f, 0x0fa3, 0x0fa6, 0x0fa9, 0x0fad, 0x0fb0, 0x0fb4,
    0x0fb7, 0x0fbb, 0x0fbe, 0x0fc1, 0x0fc5, 0x0fc8, 0x0fcc, 0x0fcf,
    0x0fd3, 0x0fd7, 0x0fda, 0x0fde, 0x0fe1, 0x0fe5, 0x0fe8, 0x0fec,
    0x0ff0, 0x0ff3, 0x0ff7, 0x0ffb, 0x0ffe, 0x1201, 0x1203, 0x1205,
    0x1207, 0x1208, 0x120a, 0x120c, 0x120e, 0x1210, 0x1212, 0x1214,
    0x1216, 0x1218, 0x121a, 0x121c, 0x121e, 0x1220, 0x1221, 0x1223,
    0x1225, 0x1227, 0x1229, 0x122b, 0x122d, 0x122f, 0x1231, 0x1234,
    0x1236, 0x1238, 0x123a, 0x123c, 0x123e, 0x1240, 0x1242, 0x1244,
    0x1246, 0x1248, 0x124a, 0x124c, 0x124f, 0x1251, 0x1253, 0x1255,
    0x1257, 0x1259, 0x125c, 0x125e, 0x1260, 0x1262, 0x1264, 0x1267,
    0x1269, 0x126b, 0x126d, 0x126f, 0x1272, 0x1274, 0x1276, 0x1279,
    0x127b, 0x127d, 0x127f, 0x1282, 0x1284, 0x1286, 0x1289, 0x128b,
    0x128d, 0x1290, 0x1292, 0x1295, 0x1297, 0x1299, 0x129c, 0x129e,
    0x12a1, 0x12a3, 0x12a5, 0x12a8, 0x12aa, 0x12ad, 0x12af, 0x12b2,
    0x12b4, 0x12b7, 0x12b9, 0x12bc, 0x12be, 0x12c1, 0x12c3, 0x12c6,
    0x12c9, 0x12cb, 0x12ce, 0x12d0, 0x12d3, 0x12d6, 0x12d8, 0x12db,
    0x12dd, 0x12e0, 0x12e3, 0x12e5, 0x12e8, 0x12eb, 0x12ed, 0x12f0,
    0x12f3, 0x12f6, 0x12f8, 0x12fb, 0x12fe, 0x1301, 0x1303, 0x1306,
    0x1309, 0x130c, 0x130f, 0x1311, 0x1314, 0x1317, 0x131a, 0x131d,
    0x1320, 0x1323, 0x1326, 0x1329, 0x132b, 0x132e, 0x1331, 0x1334,
    0x1337, 0x133a, 0x133d, 0x1340, 0x1343, 0x1346, 0x1349, 0x134c,
    0x134f, 0x1352, 0x1356, 0x1359, 0x135c, 0x135f, 0x1362, 0x1365,
    0x1368, 0x136b, 0x136f, 0x1372, 0x1375, 0x1378, 0x137b, 0x137f,
    0x1382, 0x1385, 0x1388, 0x138c, 0x138f, 0x1392, 0x1395, 0x1399,
    0x139c, 0x139f, 0x13a3, 0x13a6, 0x13a9, 0x13ad, 0x13b0, 0x13b4,
    0x13b7, 0x13bb, 0x13be, 0x13c1, 0x13c5, 0x13c8, 0x13cc, 0x13cf,
    0x13d3, 0x13d7, 0x13da, 0x13de, 0x13e1, 0x13e5, 0x13e8, 0x13ec,
    0x13f0, 0x13f3, 0x13f7, 0x13fb, 0x13fe, 0x1601, 0x1603, 0x1605,
    0x1607, 0x1608, 0x160a, 0x160c, 0x160e, 0x1610, 0x1612, 0x1614,
    0x1616, 0x1618, 0x161a, 0x161c, 0x161e, 0x1620, 0x1621, 0x1623,
    0x1625, 0x1627, 0x1629, 0x162b, 0x162d, 0x162f, 0x1631, 0x1634,
    0x1636, 0x1638, 0x163a, 0x163c, 0x163e, 0x1640, 0x1642, 0x1644,
    0x1646, 0x1648, 0x164a, 0x164c, 0x164f, 0x1651, 0x1653, 0x1655,
    0x1657, 0x1659, 0x165c, 0x165e, 0x1660, 0x1662, 0x1664, 0x1667,
    0x1669, 0x166b, 0x166d, 0x166f, 0x1672, 0x1674, 0x1676, 0x1679,
    0x167b, 0x167d, 0x167f, 0x1682, 0x1684, 0x1686, 0x1689, 0x168b,
    0x168d, 0x1690, 0x1692, 0x1695, 0x1697, 0x1699, 0x169c, 0x169e,
    0x16a1, 0x16a3, 0x16a5, 0x16a8, 0x16aa, 0x16ad, 0x16af, 0x16b2,
    0x16b4, 0x16b7, 0x16b9, 0x16bc, 0x16be, 0x16c1, 0x16c3, 0x16c6,
    0x16c9, 0x16cb, 0x16ce, 0x16d0, 0x16d3, 0x16d6, 0x16d8, 0x16db,
    0x16dd, 0x16e0, 0x16e3, 0x16e5, 0x16e8, 0x16eb, 0x16ed, 0x16f0,
    0x16f3, 0x16f6, 0x16f8, 0x16fb, 0x16fe, 0x1701, 0x1703, 0x1706,
    0x1709, 0x170c, 0x170f, 0x1711, 0x1714, 0x1717, 0x171a, 0x171d,
    0x1720, 0x1723, 0x1726, 0x1729, 0x172b, 0x172e, 0x1731, 0x1734,
    0x1737, 0x173a, 0x173d, 0x1740, 0x1743, 0x1746, 0x1749, 0x174c,
    0x174f, 0x1752, 0x1756, 0x1759, 0x175c, 0x175f, 0x1762, 0x1765,
    0x1768, 0x176b, 0x176f, 0x1772, 0x1775, 0x1778, 0x177b, 0x177f,
    0x1782, 0x1785, 0x1788, 0x178c, 0x178f, 0x1792, 0x1795, 0x1799,
    0x179c, 0x179f, 0x17a3, 0x17a6, 0x17a9, 0x17ad, 0x17b0, 0x17b4,
    0x17b7, 0x17bb, 0x17be, 0x17c1, 0x17c5, 0x17c8, 0x17cc, 0x17cf,
    0x17d3, 0x17d7, 0x17da, 0x17de, 0x17e1, 0x17e5, 0x17e8, 0x17ec,
    0x17f0, 0x17f3, 0x17f7, 0x17fb, 0x17fe, 0x1a01, 0x1a03, 0x1a05,
    0x1a07, 0x1a08, 0x1a0a, 0x1a0c, 0x1a0e, 0x1a10, 0x1a12, 0x1a14,
    0x1a16, 0x1a18, 0x1a1a, 0x1a1c, 0x1a1e, 0x1a20, 0x1a21, 0x1a23,
    0x1a25, 0x1a27, 0x1a29, 0x1a2b, 0x1a2d, 0x1a2f, 0x1a31, 0x1a34,
    0x1a36, 0x1a38, 0x1a3a, 0x1a3c, 0x1a3e, 0x1a40, 0x1a42, 0x1a44,
    0x1a46, 0x1a48, 0x1a4a, 0x1a4c, 0x1a4f, 0x1a51, 0x1a53, 0x1a55,
    0x1a57, 0x1a59, 0x1a5c, 0x1a5e, 0x1a60, 0x1a62, 0x1a64, 0x1a67,
    0x1a69, 0x1a6b, 0x1a6d, 0x1a6f, 0x1a72, 0x1a74, 0x1a76, 0x1a79,
    0x1a7b, 0x1a7d, 0x1a7f, 0x1a82, 0x1a84, 0x1a86, 0x1a89, 0x1a8b,
    0x1a8d, 0x1a90, 0x1a92, 0x1a95, 0x1a97, 0x1a99, 0x1a9c, 0x1a9e,
    0x1aa1, 0x1aa3, 0x1aa5, 0x1aa8, 0x1aaa, 0x1aad, 0x1aaf, 0x1ab2,
    0x1ab4, 0x1ab7, 0x1ab9, 0x1abc, 0x1abe, 0x1ac1, 0x1ac3, 0x1ac6,
    0x1ac9, 0x1acb, 0x1ace, 0x1ad0, 0x1ad3, 0x1ad6, 0x1ad8, 0x1adb,
    0x1add, 0x1ae0, 0x1ae3, 0x1ae5, 0x1ae8, 0x1aeb, 0x1aed, 0x1af0,
    0x1af3, 0x1af6, 0x1af8, 0x1afb, 0x1afe, 0x1b01, 0x1b03, 0x1b06,
    0x1b09, 0x1b0c, 0x1b0f, 0x1b11, 0x1b14, 0x1b17, 0x1b1a, 0x1b1d,
    0x1b20, 0x1b23, 0x1b26, 0x1b29, 0x1b2b, 0x1b2e, 0x1b31, 0x1b34,
    0x1b37, 0x1b3a, 0x1b3d, 0x1b40, 0x1b43, 0x1b46, 0x1b49, 0x1b4c,
    0x1b4f, 0x1b52, 0x1b56, 0x1b59, 0x1b5c, 0x1b5f, 0x1b62, 0x1b65,
    0x1b68, 0x1b6b, 0x1b6f, 0x1b72, 0x1b75, 0x1b78, 0x1b7b, 0x1b7f,
    0x1b82, 0x1b85, 0x1b88, 0x1b8c, 0x1b8f, 0x1b92, 0x1b95, 0x1b99,
    0x1b9c, 0x1b9f, 0x1ba3, 0x1ba6, 0x1ba9, 0x1bad, 0x1bb0, 0x1bb4,
    0x1bb7, 0x1bbb, 0x1bbe, 0x1bc1, 0x1bc5, 0x1bc8, 0x1bcc, 0x1bcf,
    0x1bd3, 0x1bd7, 0x1bda, 0x1bde, 0x1be1, 0x1be5, 0x1be8, 0x1bec,
    0x1bf0, 0x1bf3, 0x1bf7, 0x1bfb, 0x1bfe, 0x1e01, 0x1e03, 0x1e05,
    0x1e07, 0x1e08, 0x1e0a, 0x1e0c, 0x1e0e, 0x1e10, 0x1e12, 0x1e14,
    0x1e16, 0x1e18, 0x1e1a, 0x1e1c, 0x1e1e, 0x1e20, 0x1e21, 0x1e23,
    0x1e25, 0x1e27, 0x1e29, 0x1e2b, 0x1e2d, 0x1e2f, 0x1e31, 0x1e34,
    0x1e36, 0x1e38, 0x1e3a, 0x1e3c, 0x1e3e, 0x1e40, 0x1e42, 0x1e44,
    0x1e46, 0x1e48, 0x1e4a, 0x1e4c, 0x1e4f, 0x1e51, 0x1e53, 0x1e55,
    0x1e57, 0x1e59, 0x1e5c, 0x1e5e, 0x1e60, 0x1e62, 0x1e64, 0x1e67,
    0x1e69, 0x1e6b, 0x1e6d, 0x1e6f, 0x1e72, 0x1e74, 0x1e76, 0x1e79,
    0x1e7b, 0x1e7d, 0x1e7f, 0x1e82, 0x1e84, 0x1e86, 0x1e89, 0x1e8b,
    0x1e8d, 0x1e90, 0x1e92, 0x1e95, 0x1e97, 0x1e99, 0x1e9c, 0x1e9e,
    0x1ea1, 0x1ea3, 0x1ea5, 0x1ea8, 0x1eaa, 0x1ead, 0x1eaf, 0x1eaf
};

// Mapping from MIDI volume level to OPL level value.

static const unsigned int volume_mapping_table[] = {
    0, 1, 3, 5, 6, 8, 10, 11,
    13, 14, 16, 17, 19, 20, 22, 23,
    25, 26, 27, 29, 30, 32, 33, 34,
    36, 37, 39, 41, 43, 45, 47, 49,
    50, 52, 54, 55, 57, 59, 60, 61,
    63, 64, 66, 67, 68, 69, 71, 72,
    73, 74, 75, 76, 77, 79, 80, 81,
    82, 83, 84, 84, 85, 86, 87, 88,
    89, 90, 91, 92, 92, 93, 94, 95,
    96, 96, 97, 98, 99, 99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127
};

static opl_driver_ver_t opl_drv_ver = opl_doom_1_9;
static boolean music_initialized = false;

//static boolean musicpaused = false;
static int start_music_volume;
static int current_music_volume;
static int current_fader_volume;
static int current_fader_step_volume;

// GENMIDI lump instrument data:

static genmidi_instr_t *main_instrs;
static genmidi_instr_t *percussion_instrs;
static char (*main_instr_names)[32];
static char (*percussion_names)[32];

// Voices:

static opl_voice_t voices[OPL_NUM_VOICES * 2];
static opl_voice_t *voice_free_list;
static opl_voice_t *voice_alloced_list;
static int voice_alloced_num;
static boolean opl_opl3mode;
static boolean opl_opl3param;
static int num_opl_voices;

// Track data for playing tracks:

static opl_track_data_t *tracks;
static unsigned int num_tracks = 0;
static unsigned int running_tracks = 0;
static boolean song_looping;

// Tempo control variables

static unsigned int ticks_per_beat;
static unsigned int us_per_beat;

// Mini-log of recently played percussion instruments:

static uint8_t last_perc[PERCUSSION_LOG_LEN];
static unsigned int last_perc_count;

// Configuration file variable, containing the port number for the
// adlib chip.

char *snd_dmxoption = "";
int opl_io_port = 0x388;

// If true, OPL sound channels are reversed to their correct arrangement
// (as intended by the MIDI standard) rather than the backwards one
// used by DMX due to a bug.

static boolean opl_stereo_correct = false;

// Load instrument table from GENMIDI lump:

static boolean LoadInstrumentTable(void)
{
    byte *lump;

    lump = W_CacheLumpName("GENMIDI", PU_STATIC);

    // Check header

    if (strncmp((char *) lump, GENMIDI_HEADER, strlen(GENMIDI_HEADER)) != 0)
    {
        W_ReleaseLumpName("GENMIDI");

        return false;
    }

    main_instrs = (genmidi_instr_t *) (lump + strlen(GENMIDI_HEADER));
    percussion_instrs = main_instrs + GENMIDI_NUM_INSTRS;
    main_instr_names =
        (char (*)[32]) (percussion_instrs + GENMIDI_NUM_PERCUSSION);
    percussion_names = main_instr_names + GENMIDI_NUM_INSTRS;

    return true;
}

// Get the next available voice from the freelist.

static opl_voice_t *GetFreeVoice(void)
{
    opl_voice_t *result;
    opl_voice_t **rover;

    // None available?

    if (voice_free_list == NULL)
    {
        return NULL;
    }

    // Remove from free list

    result = voice_free_list;
    voice_free_list = voice_free_list->next;

    // Add to allocated list

    rover = &voice_alloced_list;

    while (*rover != NULL)
    {
        rover = &(*rover)->next;
    }

    *rover = result;
    result->next = NULL;

    voice_alloced_num++;

    return result;
}

// Remove a voice from the allocated voices list.

static void RemoveVoiceFromAllocedList(opl_voice_t *voice)
{
    opl_voice_t **rover;

    rover = &voice_alloced_list;

    // Search the list until we find the voice, then remove it.

    while (*rover != NULL)
    {
        if (*rover == voice)
        {
            *rover = voice->next;
            voice->next = NULL;
            voice_alloced_num--;
            break;
        }

        rover = &(*rover)->next;
    }
}

// Release a voice back to the freelist.

static void VoiceKeyOff(opl_voice_t *voice);

static void ReleaseVoice(opl_voice_t *voice)
{
    opl_voice_t **rover;
    opl_voice_t *next;
    boolean double_voice;

    voice->channel = NULL;
    voice->note = 0;

    double_voice = voice->current_instr_voice != 0;
    next = voice->next;

    // Remove from alloced list.

    RemoveVoiceFromAllocedList(voice);

    // Search to the end of the freelist (This is how Doom behaves!)

    rover = &voice_free_list;

    while (*rover != NULL)
    {
        rover = &(*rover)->next;
    }

    *rover = voice;
    voice->next = NULL;

    if (next != NULL && double_voice && opl_drv_ver < opl_doom_1_9)
    {
        VoiceKeyOff(next);
        ReleaseVoice(next);
    }
}

// Load data to the specified operator

static void LoadOperatorData(int operator, genmidi_op_t *data,
                             boolean max_level)
{
    int level;

    // The scale and level fields must be combined for the level register.
    // For the carrier wave we always set the maximum level.

    level = (data->scale & 0xc0) | (data->level & 0x3f);

    if (max_level)
    {
        level |= 0x3f;
    }

    OPL_WriteRegister(OPL_REGS_LEVEL + operator, level);
    OPL_WriteRegister(OPL_REGS_TREMOLO + operator, data->tremolo);
    OPL_WriteRegister(OPL_REGS_ATTACK + operator, data->attack);
    OPL_WriteRegister(OPL_REGS_SUSTAIN + operator, data->sustain);
    OPL_WriteRegister(OPL_REGS_WAVEFORM + operator, data->waveform);
}

// Set the instrument for a particular voice.

static void SetVoiceInstrument(opl_voice_t *voice,
                               genmidi_instr_t *instr,
                               unsigned int instr_voice)
{
    genmidi_voice_t *data;
    unsigned int modulating;

    // Instrument already set for this channel?

    if (voice->current_instr == instr
     && voice->current_instr_voice == instr_voice)
    {
        return;
    }

    voice->current_instr = instr;
    voice->current_instr_voice = instr_voice;

    data = &instr->voices[instr_voice];

    // Are we usind modulated feedback mode?

    modulating = (data->feedback & 0x01) == 0;

    // Doom loads the second operator first, then the first.
    // The carrier is set to minimum volume until the voice volume
    // is set in SetVoiceVolume (below).  If we are not using
    // modulating mode, we must set both to minimum volume.

    LoadOperatorData(voice->op2 | voice->array, &data->carrier, true);
    LoadOperatorData(voice->op1 | voice->array, &data->modulator, !modulating);

    // Set feedback register that control the connection between the
    // two operators.  Turn on bits in the upper nybble; I think this
    // is for OPL3, where it turns on channel A/B.

    OPL_WriteRegister((OPL_REGS_FEEDBACK + voice->index) | voice->array,
                      data->feedback | voice->reg_pan);

    // Hack to force a volume update.

    voice->reg_volume = 999;

    // Calculate voice priority.

    voice->priority = 0x0f - (data->carrier.attack >> 4)
                    + 0x0f - (data->carrier.sustain & 0x0f);
}

static void SetVoiceVolume(opl_voice_t *voice, unsigned int volume)
{
    genmidi_voice_t *opl_voice;
    unsigned int midi_volume;
    unsigned int full_volume;
    unsigned int car_volume;
    unsigned int mod_volume;

    voice->note_volume = volume;

    opl_voice = &voice->current_instr->voices[voice->current_instr_voice];

    // Multiply note volume and channel volume to get the actual volume.

    midi_volume = 2 * (volume_mapping_table[voice->channel->volume] + 1);

    full_volume = (volume_mapping_table[voice->note_volume] * midi_volume)
                >> 9;

    // The volume value to use in the register:
    car_volume = 0x3f - full_volume;

    // Update the volume register(s) if necessary.

    if (car_volume != voice->reg_volume)
    {
        voice->reg_volume = car_volume | (opl_voice->carrier.scale & 0xc0);

        OPL_WriteRegister((OPL_REGS_LEVEL + voice->op2) | voice->array,
                          voice->reg_volume);

        // If we are using non-modulated feedback mode, we must set the
        // volume for both voices.

        if ((opl_voice->feedback & 0x01) != 0
         && opl_voice->modulator.level != 0x3f)
        {
            mod_volume = 0x3f - opl_voice->modulator.level;
            if (mod_volume >= car_volume)
            {
                mod_volume = car_volume;
            }
            OPL_WriteRegister((OPL_REGS_LEVEL + voice->op1) | voice->array,
                              mod_volume |
                              (opl_voice->modulator.scale & 0xc0));
        }
    }
}

static void SetVoicePan(opl_voice_t *voice, unsigned int pan)
{
    genmidi_voice_t *opl_voice;

    voice->reg_pan = pan;
    opl_voice = &voice->current_instr->voices[voice->current_instr_voice];;

    OPL_WriteRegister((OPL_REGS_FEEDBACK + voice->index) | voice->array,
                      opl_voice->feedback | pan);
}

// Initialize the voice table and freelist

static void InitVoices(void)
{
    int i;

    // Start with an empty free list.

    voice_free_list = NULL;

    // Initialize each voice.

    for (i = 0; i < num_opl_voices; ++i)
    {
        voices[i].index = i % OPL_NUM_VOICES;
        voices[i].op1 = voice_operators[0][i % OPL_NUM_VOICES];
        voices[i].op2 = voice_operators[1][i % OPL_NUM_VOICES];
        voices[i].array = (i / OPL_NUM_VOICES) << 8;
        voices[i].current_instr = NULL;

        // Add this voice to the freelist.

        ReleaseVoice(&voices[i]);
    }
}

static void SetChannelVolume(opl_channel_data_t *channel, unsigned int volume,
                             boolean clip_start);

// Set music volume (0 - 127)

static void I_OPL_SetMusicVolume(int volume)
{
    unsigned int i, j;

    if (current_music_volume == volume)
    {
        return;
    }

    // Internal state variable.

    current_music_volume = volume;

    // Update the volume of all voices.

    for (i = 0; i < num_tracks; ++i)
    {
        for (j = 0; j < MIDI_CHANNELS_PER_TRACK; ++j)
        {
            if (j == 15)
            {
                SetChannelVolume(&tracks[i].channels[j], volume, false);
            }
            else
            {
                SetChannelVolume(&tracks[i].channels[j],
                                 tracks[i].channels[j].volume_base, false);
            }
        }
    }
}

static void VoiceKeyOff(opl_voice_t *voice)
{
    OPL_WriteRegister((OPL_REGS_FREQ_2 + voice->index) | voice->array,
                      voice->freq >> 8);
}

static opl_channel_data_t *TrackChannelForEvent(opl_track_data_t *track,
                                                midi_event_t *event)
{
    unsigned int channel_num = event->data.channel.channel;

    // MIDI uses track #9 for percussion, but for MUS it's track #15
    // instead. Because DMX works on MUS data internally, we need to
    // swap back to the MUS version of the channel number.
    if (channel_num == 9)
    {
        channel_num = 15;
    }
    else if (channel_num == 15)
    {
        channel_num = 9;
    }

    return &track->channels[channel_num];
}

// Get the frequency that we should be using for a voice.

static void KeyOffEvent(opl_track_data_t *track, midi_event_t *event)
{
    opl_channel_data_t *channel;
    opl_voice_t *rover;
    opl_voice_t *prev;
    unsigned int key;

/*
    printf("note off: channel %i, %i, %i\n",
           event->data.channel.channel,
           event->data.channel.param1,
           event->data.channel.param2);
*/

    channel = TrackChannelForEvent(track, event);
    key = event->data.channel.param1;

    // Turn off voices being used to play this key.
    // If it is a double voice instrument there will be two.

    rover = voice_alloced_list;
    prev = NULL;

    while (rover != NULL)
    {
        if (rover->channel == channel && rover->key == key)
        {
            VoiceKeyOff(rover);

            // Finished with this voice now.

            ReleaseVoice(rover);
            if (prev == NULL)
            {
                rover = voice_alloced_list;
            }
            else
            {
                rover = prev->next;
            }
        }
        else
        {
            prev = rover;
            rover = rover->next;
        }
    }
}

// When all voices are in use, we must discard an existing voice to
// play a new note.  Find and free an existing voice.  The channel
// passed to the function is the channel for the new note to be
// played.

static void ReplaceExistingVoice(void)
{
    opl_voice_t *rover;
    opl_voice_t *result;

    // Check the allocated voices, if we find an instrument that is
    // of a lower priority to the new instrument, discard it.
    // If a voice is being used to play the second voice of an instrument,
    // use that, as second voices are non-essential.
    // Lower numbered MIDI channels implicitly have a higher priority
    // than higher-numbered channels, eg. MIDI channel 1 is never
    // discarded for MIDI channel 2.

    result = voice_alloced_list;

    for (rover = voice_alloced_list; rover != NULL; rover = rover->next)
    {
        if (rover->current_instr_voice != 0
         || rover->channel >= result->channel)
        {
            result = rover;
        }
    }

    VoiceKeyOff(result);
    ReleaseVoice(result);
}

// Alternate versions of ReplaceExistingVoice() used when emulating old
// versions of the DMX library used in Doom 1.666, Heretic and Hexen.

static void ReplaceExistingVoiceDoom1(void)
{
    opl_voice_t *rover;
    opl_voice_t *result;

    result = voice_alloced_list;

    for (rover = voice_alloced_list; rover != NULL; rover = rover->next)
    {
        if (rover->channel > result->channel)
        {
            result = rover;
        }
    }

    VoiceKeyOff(result);
    ReleaseVoice(result);
}

static void ReplaceExistingVoiceDoom2(opl_channel_data_t *channel)
{
    opl_voice_t *rover;
    opl_voice_t *result;
    opl_voice_t *roverend;
    int i;
    int priority;

    result = voice_alloced_list;

    roverend = voice_alloced_list;

    for (i = 0; i < voice_alloced_num - 3; i++)
    {
        roverend = roverend->next;
    }

    priority = 0x8000;

    for (rover = voice_alloced_list; rover != roverend; rover = rover->next)
    {
        if (rover->priority < priority
         && rover->channel >= channel)
        {
            priority = rover->priority;
            result = rover;
        }
    }

    VoiceKeyOff(result);
    ReleaseVoice(result);
}

static void ReplaceExistingVoiceOld(opl_channel_data_t *channel)
{
    opl_voice_t *rover;
    opl_voice_t *result;

    result = voice_alloced_list;

    for (rover = voice_alloced_list; rover != NULL; rover = rover->next)
    {
        if (rover->channel == channel
         || rover->current_instr == channel->instrument)
        {
            result = rover;
            break;
        }
    }

    VoiceKeyOff(result);
    ReleaseVoice(result);
}


static unsigned int FrequencyForVoice(opl_voice_t *voice)
{
    genmidi_voice_t *gm_voice;
    signed int freq_index;
    unsigned int octave;
    unsigned int fnum;
    signed int tune;
    unsigned int sub_index;
    signed int note;

    note = voice->note;

    // Apply note offset.
    // Don't apply offset if the instrument is a fixed note instrument.

    gm_voice = &voice->current_instr->voices[voice->current_instr_voice];

    if ((SHORT(voice->current_instr->flags) & GENMIDI_FLAG_FIXED) == 0)
    {
        note += (signed short) SHORT(gm_voice->base_note_offset);
    }

    // Avoid possible overflow due to base note offset:

    while (note < 0)
    {
        note += 12;
    }

    while (note > 95)
    {
        note -= 12;
    }

    if (opl_drv_ver == opl_doom_beta)
    {
        freq_index = 15 + 16 * note + voice->channel->bend;

        // If this is the second voice of a double voice instrument, the
        // frequency index can be adjusted by the fine tuning field.

        if (voice->current_instr_voice != 0)
        {
            tune = voice->current_instr->fine_tuning;
            if (tune >= 128)
            {
                tune += 3;
            }
            freq_index += tune / 4 - 32;
        }

        if (freq_index < 0)
        {
            freq_index = 0;
        }
        
        if (freq_index > 1551)
        {
            freq_index = 1551;
        }

        return frequency_curve_beta[freq_index];
    }

    freq_index = 64 + 32 * note + voice->channel->bend;

    // If this is the second voice of a double voice instrument, the
    // frequency index can be adjusted by the fine tuning field.

    if (voice->current_instr_voice != 0)
    {
        freq_index += (voice->current_instr->fine_tuning / 2) - 64;
    }

    if (freq_index < 0)
    {
        freq_index = 0;
    }

    // The first 7 notes use the start of the table, while
    // consecutive notes loop around the latter part.

    if (freq_index < 284)
    {
        return frequency_curve[freq_index];
    }

    sub_index = (freq_index - 284) % (12 * 32);
    octave = (freq_index - 284) / (12 * 32);

    // Once the seventh octave is reached, things break down.
    // We can only go up to octave 7 as a maximum anyway (the OPL
    // register only has three bits for octave number), but for the
    // notes in octave 7, the first five bits have octave=7, the
    // following notes have octave=6.  This 7/6 pattern repeats in
    // following octaves (which are technically impossible to
    // represent anyway).

    if (octave >= 7)
    {
        octave = 7;
    }

    // Calculate the resulting register value to use for the frequency.

    return frequency_curve[sub_index + 284] | (octave << 10);
}

// Update the frequency that a voice is programmed to use.

static void UpdateVoiceFrequency(opl_voice_t *voice)
{
    unsigned int freq;

    // Calculate the frequency to use for this voice and update it
    // if neccessary.

    freq = FrequencyForVoice(voice);

    if (voice->freq != freq)
    {
        OPL_WriteRegister((OPL_REGS_FREQ_1 + voice->index) | voice->array,
                          freq & 0xff);
        OPL_WriteRegister((OPL_REGS_FREQ_2 + voice->index) | voice->array,
                          (freq >> 8) | 0x20);

        voice->freq = freq;
    }
}

// Program a single voice for an instrument.  For a double voice
// instrument (GENMIDI_FLAG_2VOICE), this is called twice for each
// key on event.

static void VoiceKeyOn(opl_channel_data_t *channel,
                       genmidi_instr_t *instrument,
                       unsigned int instrument_voice,
                       unsigned int note,
                       unsigned int key,
                       unsigned int volume)
{
    opl_voice_t *voice;

    if (!opl_opl3mode && opl_drv_ver == opl_doom1_1_666)
    {
        instrument_voice = 0;
    }

    // Find a voice to use for this new note.

    voice = GetFreeVoice();

    if (voice == NULL)
    {
        return;
    }

    voice->channel = channel;
    voice->key = key;

    // Work out the note to use.  This is normally the same as
    // the key, unless it is a fixed pitch instrument.

    if ((SHORT(instrument->flags) & GENMIDI_FLAG_FIXED) != 0)
    {
        voice->note = instrument->fixed_note;
    }
    else
    {
        voice->note = note;
    }

    voice->reg_pan = channel->pan;

    // Program the voice with the instrument data:

    SetVoiceInstrument(voice, instrument, instrument_voice);

    // Set the volume level.

    SetVoiceVolume(voice, volume);

    // Write the frequency value to turn the note on.

    voice->freq = 0;
    UpdateVoiceFrequency(voice);
}

static void KeyOnEvent(opl_track_data_t *track, midi_event_t *event)
{
    genmidi_instr_t *instrument;
    opl_channel_data_t *channel;
    unsigned int note, key, volume, voicenum;
    boolean double_voice;

/*
    printf("note on: channel %i, %i, %i\n",
           event->data.channel.channel,
           event->data.channel.param1,
           event->data.channel.param2);
*/

    note = event->data.channel.param1;
    key = event->data.channel.param1;
    volume = event->data.channel.param2;

    // A volume of zero means key off. Some MIDI tracks, eg. the ones
    // in AV.wad, use a second key on with a volume of zero to mean
    // key off.
    if (volume <= 0)
    {
        KeyOffEvent(track, event);
        return;
    }

    // The channel.
    channel = TrackChannelForEvent(track, event);

    // Percussion channel is treated differently.
    if (event->data.channel.channel == 9)
    {
        if (key < 35 || key > 81)
        {
            return;
        }

        instrument = &percussion_instrs[key - 35];

        last_perc[last_perc_count] = key;
        last_perc_count = (last_perc_count + 1) % PERCUSSION_LOG_LEN;
        note = 60;
    }
    else
    {
        instrument = channel->instrument;
    }

    double_voice = (SHORT(instrument->flags) & GENMIDI_FLAG_2VOICE) != 0;

    switch (opl_drv_ver)
    {
        case opl_doom_beta:
            if (voice_alloced_num == num_opl_voices)
            {
                ReplaceExistingVoiceOld(channel);
            }
            if (voice_alloced_num == num_opl_voices - 1 && double_voice)
            {
                ReplaceExistingVoiceOld(channel);
            }

            // Find and program a voice for this instrument.  If this
            // is a double voice instrument, we must do this twice.

            if (double_voice)
            {
                VoiceKeyOn(channel, instrument, 1, note, key, volume);
            }

            VoiceKeyOn(channel, instrument, 0, note, key, volume);
            break;
        case opl_doom1_1_666:
            voicenum = double_voice + 1;
            if (!opl_opl3mode)
            {
                voicenum = 1;
            }
            while (voice_alloced_num > num_opl_voices - voicenum)
            {
                ReplaceExistingVoiceDoom1();
            }

            // Find and program a voice for this instrument.  If this
            // is a double voice instrument, we must do this twice.

            if (double_voice)
            {
                VoiceKeyOn(channel, instrument, 1, note, key, volume);
            }

            VoiceKeyOn(channel, instrument, 0, note, key, volume);
            break;
        case opl_doom2_1_666:
            if (voice_alloced_num == num_opl_voices)
            {
                ReplaceExistingVoiceDoom2(channel);
            }
            if (voice_alloced_num == num_opl_voices - 1 && double_voice)
            {
                ReplaceExistingVoiceDoom2(channel);
            }

            // Find and program a voice for this instrument.  If this
            // is a double voice instrument, we must do this twice.

            if (double_voice)
            {
                VoiceKeyOn(channel, instrument, 1, note, key, volume);
            }

            VoiceKeyOn(channel, instrument, 0, note, key, volume);
            break;
        default:
        case opl_doom_1_9:
            if (voice_free_list == NULL)
            {
                ReplaceExistingVoice();
            }

            // Find and program a voice for this instrument.  If this
            // is a double voice instrument, we must do this twice.

            VoiceKeyOn(channel, instrument, 0, note, key, volume);

            if (double_voice)
            {
                VoiceKeyOn(channel, instrument, 1, note, key, volume);
            }
            break;
    }
}

static void ProgramChangeEvent(opl_track_data_t *track, midi_event_t *event)
{
    opl_channel_data_t *channel;
    int instrument;

    // Set the instrument used on this channel.

    channel = TrackChannelForEvent(track, event);
    instrument = event->data.channel.param1;
    channel->instrument = &main_instrs[instrument];

    // TODO: Look through existing voices that are turned on on this
    // channel, and change the instrument.
}

static void SetChannelVolume(opl_channel_data_t *channel, unsigned int volume,
                             boolean clip_start)
{
    unsigned int i;

    channel->volume_base = volume;

    if (volume > current_music_volume)
    {
        volume = current_music_volume;
    }

    if (volume > current_fader_volume)
    {
        volume = current_fader_volume;
    }

    if (clip_start && volume > start_music_volume)
    {
        volume = start_music_volume;
    }

    channel->volume = volume;

    // Update all voices that this channel is using.

    for (i = 0; i < num_opl_voices; ++i)
    {
        if (voices[i].channel == channel)
        {
            SetVoiceVolume(&voices[i], voices[i].note_volume);
        }
    }
}

static void SetChannelPan(opl_channel_data_t *channel, unsigned int pan)
{
    unsigned int reg_pan;
    unsigned int i;

    // The DMX library has the stereo channels backwards, maybe because
    // Paul Radek had a Soundblaster card with the channels reversed, or
    // perhaps it was just a bug in the OPL3 support that was never
    // finished. By default we preserve this bug, but we also provide a
    // secret DMXOPTION to fix it.
    if (opl_stereo_correct)
    {
        pan = 144 - pan;
    }

    if (opl_opl3mode)
    {
        if (pan >= 96)
        {
            reg_pan = 0x10;
        }
        else if (pan <= 48)
        {
            reg_pan = 0x20;
        }
        else
        {
            reg_pan = 0x30;
        }
        if (channel->pan != reg_pan)
        {
            channel->pan = reg_pan;
            for (i = 0; i < num_opl_voices; i++)
            {
                if (voices[i].channel == channel)
                {
                    SetVoicePan(&voices[i], reg_pan);
                }
            }
        }
    }
}

// Handler for the MIDI_CONTROLLER_ALL_NOTES_OFF channel event.
static void AllNotesOff(opl_channel_data_t *channel, unsigned int param)
{
    opl_voice_t *rover;
    opl_voice_t *prev;

    rover = voice_alloced_list;
    prev = NULL;

    while (rover!=NULL)
    {
        if (rover->channel == channel)
        {
            VoiceKeyOff(rover);

            // Finished with this voice now.

            ReleaseVoice(rover);
            if (prev == NULL)
            {
                rover = voice_alloced_list;
            }
            else
            {
                rover = prev->next;
            }
        }
        else
        {
            prev = rover;
            rover = rover->next;
        }
    }
}

static void ControllerEvent(opl_track_data_t *track, midi_event_t *event)
{
    opl_channel_data_t *channel;
    unsigned int controller;
    unsigned int param;

/*
    printf("change controller: channel %i, %i, %i\n",
           event->data.channel.channel,
           event->data.channel.param1,
           event->data.channel.param2);
*/

    channel = TrackChannelForEvent(track, event);
    controller = event->data.channel.param1;
    param = event->data.channel.param2;

    switch (controller)
    {
        case MIDI_CONTROLLER_MAIN_VOLUME:
            SetChannelVolume(channel, param, true);
            break;

        case MIDI_CONTROLLER_PAN:
            SetChannelPan(channel, param);
            break;

        case MIDI_CONTROLLER_ALL_NOTES_OFF:
            AllNotesOff(channel, param);
            break;

        default:
#ifdef OPL_MIDI_DEBUG
            fprintf(stderr, "Unknown MIDI controller type: %i\n", controller);
#endif
            break;
    }
}

// Process a pitch bend event.

static void PitchBendEvent(opl_track_data_t *track, midi_event_t *event)
{
    opl_channel_data_t *channel;
    unsigned int i;
    unsigned int full_bend;

    // Update the channel bend value.  Only the MSB of the pitch bend
    // value is considered: this is what Doom does.

    channel = TrackChannelForEvent(track, event);
    if (opl_drv_ver == opl_doom_beta)
    {
        full_bend = (event->data.channel.param2 << 1)
                  | ((event->data.channel.param1 >> 6) & 1);
        if (full_bend >= 128)
        {
            full_bend += 3;
        }
        channel->bend = full_bend / 4 - 30;
    }
    else
    {
        channel->bend = event->data.channel.param2 - 64;
    }
    // Update all voices for this channel.

    for (i = 0; i < num_opl_voices; ++i)
    {
        if (voices[i].channel == channel)
        {
            UpdateVoiceFrequency(&voices[i]);
        }
    }
}

static void MetaSetTempo(unsigned int tempo)
{
    OPL_AdjustCallbacks((float) us_per_beat / tempo);
    us_per_beat = tempo;
}

// Process a meta event.

static void MetaEvent(opl_track_data_t *track, midi_event_t *event)
{
    byte *data = event->data.meta.data;
    unsigned int data_len = event->data.meta.length;

    switch (event->data.meta.type)
    {
        // Things we can just ignore.

        case MIDI_META_SEQUENCE_NUMBER:
        case MIDI_META_TEXT:
        case MIDI_META_COPYRIGHT:
        case MIDI_META_TRACK_NAME:
        case MIDI_META_INSTR_NAME:
        case MIDI_META_LYRICS:
        case MIDI_META_MARKER:
        case MIDI_META_CUE_POINT:
        case MIDI_META_SEQUENCER_SPECIFIC:
            break;

        case MIDI_META_SET_TEMPO:
            if (data_len == 3)
            {
                MetaSetTempo((data[0] << 16) | (data[1] << 8) | data[2]);
            }
            break;

        // End of track - actually handled when we run out of events
        // in the track, see below.

        case MIDI_META_END_OF_TRACK:
            break;

        default:
#ifdef OPL_MIDI_DEBUG
            fprintf(stderr, "Unknown MIDI meta event type: %i\n",
                            event->data.meta.type);
#endif
            break;
    }
}

// Process a MIDI event from a track.

static void ProcessEvent(opl_track_data_t *track, midi_event_t *event)
{
    switch (event->event_type)
    {
        case MIDI_EVENT_NOTE_OFF:
            KeyOffEvent(track, event);
            break;

        case MIDI_EVENT_NOTE_ON:
            KeyOnEvent(track, event);
            break;

        case MIDI_EVENT_CONTROLLER:
            ControllerEvent(track, event);
            break;

        case MIDI_EVENT_PROGRAM_CHANGE:
            ProgramChangeEvent(track, event);
            break;

        case MIDI_EVENT_PITCH_BEND:
            PitchBendEvent(track, event);
            break;

        case MIDI_EVENT_META:
            MetaEvent(track, event);
            break;

        // SysEx events can be ignored.

        case MIDI_EVENT_SYSEX:
        case MIDI_EVENT_SYSEX_SPLIT:
            break;

        default:
#ifdef OPL_MIDI_DEBUG
            fprintf(stderr, "Unknown MIDI event type %i\n", event->event_type);
#endif
            break;
    }
}

static void ScheduleTrack(opl_track_data_t *track);
static void InitChannel(opl_track_data_t *track, opl_channel_data_t *channel);

// Restart a song from the beginning.

static void RestartSong(void *unused)
{
    unsigned int i, j;

    running_tracks = num_tracks;

    start_music_volume = current_music_volume;

    for (i = 0; i < num_tracks; ++i)
    {
        MIDI_RestartIterator(tracks[i].iter);
        ScheduleTrack(&tracks[i]);
        for (j = 0; j < MIDI_CHANNELS_PER_TRACK; ++j)
        {
            InitChannel(&tracks[i], &tracks[i].channels[j]);
        }
    }
}

// Callback function invoked when another event needs to be read from
// a track.

static void TrackTimerCallback(void *arg)
{
    opl_track_data_t *track = arg;
    midi_event_t *event;

    // Get the next event and process it.

    if (!MIDI_GetNextEvent(track->iter, &event))
    {
        return;
    }

    ProcessEvent(track, event);

    // End of track?

    if (event->event_type == MIDI_EVENT_META
     && event->data.meta.type == MIDI_META_END_OF_TRACK)
    {
        --running_tracks;

        // When all tracks have finished, restart the song.
        // Don't restart the song immediately, but wait for 5ms
        // before triggering a restart.  Otherwise it is possible
        // to construct an empty MIDI file that causes the game
        // to lock up in an infinite loop. (5ms should be short
        // enough not to be noticeable by the listener).

        if (running_tracks <= 0 && song_looping)
        {
            OPL_SetCallback(5000, RestartSong, NULL);
        }

        return;
    }

    // Reschedule the callback for the next event in the track.

    ScheduleTrack(track);
}

static void ScheduleTrack(opl_track_data_t *track)
{
    unsigned int nticks;
    uint64_t us;

    // Get the number of microseconds until the next event.

    nticks = MIDI_GetDeltaTime(track->iter);
    us = ((uint64_t) nticks * us_per_beat) / ticks_per_beat;

    // Set a timer to be invoked when the next event is
    // ready to play.

    OPL_SetCallback(us, TrackTimerCallback, track);
}

// Initialize a channel.

static void InitChannel(opl_track_data_t *track, opl_channel_data_t *channel)
{
    // TODO: Work out sensible defaults?

    channel->instrument = &main_instrs[0];
    channel->volume = current_music_volume;
    channel->volume_base = 100;
    if (channel->volume > channel->volume_base)
    {
        channel->volume = channel->volume_base;
    }
    if (channel->volume > current_fader_volume)
    {
        channel->volume = current_fader_volume;
    }
    channel->pan = 0x30;
    channel->bend = 0;
}

// Start a MIDI track playing:

static void StartTrack(midi_file_t *file, unsigned int track_num)
{
    opl_track_data_t *track;
    unsigned int i;

    track = &tracks[track_num];
    track->iter = MIDI_IterateTrack(file, track_num);

    for (i = 0; i < MIDI_CHANNELS_PER_TRACK; ++i)
    {
        InitChannel(track, &track->channels[i]);
    }

    // Schedule the first event.

    ScheduleTrack(track);
}

void FaderCallback(void *unused)
{
    int i, j;
    current_fader_step_volume++;
    current_fader_volume = (current_fader_step_volume * 127) / 50;
    if (current_fader_volume < 127)
    {
        for (i = 0; i < num_tracks; ++i)
        {
            for (j = 0; j < MIDI_CHANNELS_PER_TRACK; ++j)
            {
                SetChannelVolume(&tracks[i].channels[j],
                    tracks[i].channels[j].volume_base, false);
            }
        }


        OPL_SetCallback(20000, FaderCallback, NULL);
    }
    else
    {
        current_fader_volume = 127;
    }
}

static void StartFader(void)
{
    current_fader_volume = 0;
    current_fader_step_volume = 0;
    OPL_SetCallback(20000, FaderCallback, NULL);
}

// Start playing a mid

static void I_OPL_PlaySong(void *handle, boolean looping)
{
    midi_file_t *file;
    unsigned int i;

    if (!music_initialized || handle == NULL)
    {
        return;
    }

    file = handle;

    // Allocate track data.

    tracks = malloc(MIDI_NumTracks(file) * sizeof(opl_track_data_t));

    num_tracks = MIDI_NumTracks(file);
    running_tracks = num_tracks;
    song_looping = looping;

    ticks_per_beat = MIDI_GetFileTimeDivision(file);

    // Default is 120 bpm.
    // TODO: this is wrong

    us_per_beat = 500 * 1000;

    start_music_volume = current_music_volume;

    if (opl_drv_ver == opl_doom_beta)
    {
        StartFader();
    }
    else
    {
        current_fader_volume = 127;
    }
    for (i = 0; i < num_tracks; ++i)
    {
        StartTrack(file, i);
    }
}

static void I_OPL_PauseSong(void)
{
    unsigned int i;

    if (!music_initialized)
    {
        return;
    }

    // Pause OPL callbacks.

    OPL_SetPaused(1);

    // Turn off all main instrument voices (not percussion).
    // This is what Vanilla does.

    for (i = 0; i < num_opl_voices; ++i)
    {
        if (voices[i].channel != NULL
         && voices[i].current_instr < percussion_instrs)
        {
            VoiceKeyOff(&voices[i]);
        }
    }
}

static void I_OPL_ResumeSong(void)
{
    if (!music_initialized)
    {
        return;
    }

    OPL_SetPaused(0);
}

static void I_OPL_StopSong(void)
{
    unsigned int i;

    if (!music_initialized)
    {
        return;
    }

    OPL_Lock();

    // Stop all playback.

    OPL_ClearCallbacks();

    // Free all voices.

    for (i = 0; i < num_opl_voices; ++i)
    {
        if (voices[i].channel != NULL)
        {
            VoiceKeyOff(&voices[i]);
            ReleaseVoice(&voices[i]);
        }
    }

    // Free all track data.

    for (i = 0; i < num_tracks; ++i)
    {
        MIDI_FreeIterator(tracks[i].iter);
    }

    free(tracks);

    tracks = NULL;
    num_tracks = 0;

    OPL_Unlock();
}

static void I_OPL_UnRegisterSong(void *handle)
{
    if (!music_initialized)
    {
        return;
    }

    if (handle != NULL)
    {
        MIDI_FreeFile(handle);
    }
}

// Determine whether memory block is a .mid file

static boolean IsMid(byte *mem, int len)
{
    return len > 4 && !memcmp(mem, "MThd", 4);
}

static boolean ConvertMus(byte *musdata, int len, char *filename)
{
    MEMFILE *instream;
    MEMFILE *outstream;
    void *outbuf;
    size_t outbuf_len;
    int result;

    instream = mem_fopen_read(musdata, len);
    outstream = mem_fopen_write();

    result = mus2mid(instream, outstream);

    if (result == 0)
    {
        mem_get_buf(outstream, &outbuf, &outbuf_len);

        M_WriteFile(filename, outbuf, outbuf_len);
    }

    mem_fclose(instream);
    mem_fclose(outstream);

    return result;
}

static void *I_OPL_RegisterSong(void *data, int len)
{
    midi_file_t *result;
    char *filename;

    if (!music_initialized)
    {
        return NULL;
    }

    // MUS files begin with "MUS"
    // Reject anything which doesnt have this signature

    filename = M_TempFile("doom.mid");

    if (IsMid(data, len) && len < MAXMIDLENGTH)
    {
        M_WriteFile(filename, data, len);
    }
    else
    {
        // Assume a MUS file and try to convert

        ConvertMus(data, len, filename);
    }

    result = MIDI_LoadFile(filename);

    if (result == NULL)
    {
        fprintf(stderr, "I_OPL_RegisterSong: Failed to load MID.\n");
    }

    // remove file now

    remove(filename);
    free(filename);

    return result;
}

// Is the song playing?

static boolean I_OPL_MusicIsPlaying(void)
{
    if (!music_initialized)
    {
        return false;
    }

    return num_tracks > 0;
}

// Shutdown music

static void I_OPL_ShutdownMusic(void)
{
    if (music_initialized)
    {
        // Stop currently-playing track, if there is one:

        I_OPL_StopSong();

        OPL_Shutdown();

        // Release GENMIDI lump

        W_ReleaseLumpName("GENMIDI");

        music_initialized = false;
    }
}

// Initialize music subsystem

static boolean I_OPL_InitMusic(void)
{
    char *dmxoption;
    opl_init_result_t chip_type;

    OPL_SetSampleRate(snd_samplerate);

    chip_type = OPL_Init(opl_io_port);
    if (chip_type == OPL_INIT_NONE)
    {
        printf("Dude.  The Adlib isn't responding.\n");
        return false;
    }

    // The DMXOPTION variable must be set to enable OPL3 support.
    // As an extension, we also allow it to be set from the config file.
    dmxoption = getenv("DMXOPTION");
    if (dmxoption == NULL)
    {
        dmxoption = snd_dmxoption != NULL ? snd_dmxoption : "";
    }

    if (chip_type == OPL_INIT_OPL3 && strstr(dmxoption, "-opl3") != NULL)
    {
        opl_opl3param = true;
    }
    else
    {
        opl_opl3param = false;
    }

    // Secret, undocumented DMXOPTION that reverses the stereo channels
    // into their correct orientation.
    opl_stereo_correct = strstr(dmxoption, "-reverse") != NULL;

    opl_drv_ver = I_GetOPLDriverVer();

    if (opl_drv_ver >= opl_doom1_1_666 && opl_opl3param)
    {
        opl_opl3mode = true;
        num_opl_voices = OPL_NUM_VOICES * 2;
    }
    else
    {
        opl_opl3mode = false;
        num_opl_voices = OPL_NUM_VOICES;
    }

    // Initialize all registers.

    OPL_InitRegisters(opl_opl3mode);

    // Load instruments from GENMIDI lump:

    if (!LoadInstrumentTable())
    {
        OPL_Shutdown();
        return false;
    }

    InitVoices();

    tracks = NULL;
    num_tracks = 0;
    music_initialized = true;

    return true;
}

static snddevice_t music_opl_devices[] =
{
    SNDDEVICE_ADLIB,
    SNDDEVICE_SB,
};

music_module_t music_opl_module =
{
    music_opl_devices,
    arrlen(music_opl_devices),
    I_OPL_InitMusic,
    I_OPL_ShutdownMusic,
    I_OPL_SetMusicVolume,
    I_OPL_PauseSong,
    I_OPL_ResumeSong,
    I_OPL_RegisterSong,
    I_OPL_UnRegisterSong,
    I_OPL_PlaySong,
    I_OPL_StopSong,
    I_OPL_MusicIsPlaying,
    NULL,  // Poll
};

//----------------------------------------------------------------------
//
// Development / debug message generation, to help developing GENMIDI
// lumps.
//
//----------------------------------------------------------------------

static int NumActiveChannels(void)
{
    int i;

    for (i = MIDI_CHANNELS_PER_TRACK - 1; i >= 0; --i)
    {
        if (tracks[0].channels[i].instrument != &main_instrs[0])
        {
            return i + 1;
        }
    }

    return 0;
}

static int ChannelInUse(opl_channel_data_t *channel)
{
    opl_voice_t *voice;

    for (voice = voice_alloced_list; voice != NULL; voice = voice->next)
    {
        if (voice->channel == channel)
        {
            return 1;
        }
    }

    return 0;
}

void I_OPL_DevMessages(char *result, size_t result_len)
{
    char tmp[80];
    int instr_num;
    int lines;
    int i;

    if (num_tracks == 0)
    {
        M_snprintf(result, result_len, "No OPL track!");
        return;
    }

    M_snprintf(result, result_len, "Tracks:\n");
    lines = 1;

    for (i = 0; i < NumActiveChannels(); ++i)
    {
        if (tracks[0].channels[i].instrument == NULL)
        {
            continue;
        }

        instr_num = tracks[0].channels[i].instrument - main_instrs;

        M_snprintf(tmp, sizeof(tmp),
                   "chan %i: %c i#%i (%s)\n",
                   i,
                   ChannelInUse(&tracks[0].channels[i]) ? '\'' : ' ',
                   instr_num + 1,
                   main_instr_names[instr_num]);
        M_StringConcat(result, tmp, result_len);

        ++lines;
    }

    M_snprintf(tmp, sizeof(tmp), "\nLast percussion:\n");
    M_StringConcat(result, tmp, result_len);
    lines += 2;

    i = (last_perc_count + PERCUSSION_LOG_LEN - 1) % PERCUSSION_LOG_LEN;

    do {
        if (last_perc[i] == 0)
        {
            break;
        }

        M_snprintf(tmp, sizeof(tmp),
                   "%cp#%i (%s)\n",
                   i == 0 ? '\'' : ' ',
                   last_perc[i],
                   percussion_names[last_perc[i] - 35]);
        M_StringConcat(result, tmp, result_len);
        ++lines;

        i = (i + PERCUSSION_LOG_LEN - 1) % PERCUSSION_LOG_LEN;
    } while (lines < 25 && i != last_perc_count);
}

