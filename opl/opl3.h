//
// Copyright (C) 2013-2015 Alexey Khokholov (Nuke.YKT)
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
//
//  Nuked OPL3 emulator.
//  Thanks:
//      MAME Development Team(Jarek Burczynski, Tatsuyuki Satoh):
//          Feedback and Rhythm part calculation information.
//      forums.submarine.org.uk(carbon14, opl3):
//          Tremolo and phase generator calculation information.
//      OPLx decapsulated(Matthew Gambrell, Olli Niemitalo):
//          OPL2 ROMs.
//
// version: 1.6.2
//

#include <inttypes.h>

#include <samplerate.h>

typedef uintptr_t       Bitu;
typedef intptr_t        Bits;
typedef uint32_t        Bit32u;
typedef int32_t         Bit32s;
typedef uint16_t        Bit16u;
typedef int16_t         Bit16s;
typedef uint8_t         Bit8u;
typedef int8_t          Bit8s;

typedef struct _opl_slot opl_slot;
typedef struct _opl_channel opl_channel;
typedef struct _opl_chip opl_chip;

struct _opl_slot {
    opl_channel *channel;
    opl_chip *chip;
    Bit16s out;
    Bit16s fbmod;
    Bit16s *mod;
    Bit16s prout;
    Bit16s eg_rout;
    Bit16s eg_out;
    Bit8u eg_inc;
    Bit8u eg_gen;
    Bit8u eg_rate;
    Bit8u eg_ksl;
    Bit8u *trem;
    Bit8u reg_vib;
    Bit8u reg_type;
    Bit8u reg_ksr;
    Bit8u reg_mult;
    Bit8u reg_ksl;
    Bit8u reg_tl;
    Bit8u reg_ar;
    Bit8u reg_dr;
    Bit8u reg_sl;
    Bit8u reg_rr;
    Bit8u reg_wf;
    Bit8u key;
    Bit32u pg_phase;
    Bit32u timer;
};

struct _opl_channel {
    opl_slot *slots[2];
    opl_channel *pair;
    opl_chip *chip;
    Bit16s *out[4];
    Bit8u chtype;
    Bit16u f_num;
    Bit8u block;
    Bit8u fb;
    Bit8u con;
    Bit8u alg;
    Bit8u ksv;
    Bit16u cha, chb;
};

struct _opl_chip {
    opl_channel channel[18];
    opl_slot slot[36];
    Bit16u timer;
    Bit8u newm;
    Bit8u nts;
    Bit8u rhy;
    Bit8u vibpos;
    Bit8u vibshift;
    Bit8u tremolo;
    Bit8u tremolopos;
    Bit8u tremoloshift;
    Bit32u noise;
    Bit16s zeromod;
    Bit32s mixbuff[2];
    float rsm_buff[128][2];
    Bit16u rsm_counter;
    Bit32u rsm_status;
    double rsm_ratio;
    SRC_STATE *rsm_state;
    //OPL3L
    Bit32s samplecnt;
    Bit16s oldsamples[2];
    Bit16s samples[2];
};


void chip_reset(opl_chip *chip, Bit32u samplerate);
void chip_write(opl_chip *chip, Bit16u reg, Bit8u v);
void chip_update(opl_chip *chip, Bit16s* sndptr, Bit32u numsamples);
void chip_remove(opl_chip *chip);