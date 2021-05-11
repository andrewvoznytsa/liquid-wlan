/*
 * Copyright (c) 2011 Joseph Gaeddert
 * Copyright (c) 2011 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// wlanframegen.c
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "liquid-wlan.internal.h"

#define DEBUG_WLANFRAMEGEN            0

struct wlanframegen_s {
    // options
    unsigned int rate;      // primitive data rate
    unsigned int length;    // original data length (bytes)
    unsigned int seed;      // data scrambler seed

    float g;                // scaling factor (gain)

    // transform object
    FFT_PLAN ifft;          // ifft object
    float complex * X;      // frequency-domain buffer
    float complex * x;      // time-domain buffer

    // pilot sequence generator
    wlan_lfsr ms_pilot;     // g = x^7 + x^4 + 1 = 1001 0001(bin) = 0x91(hex)
    
    // DATA field modulation scheme
    unsigned int mod_scheme;

    // window transition
    unsigned int rampup_len;        // number of samples in overlapping symbols
    float * rampup;                 // ramp up window (ramp down is time-reversed)
    float complex * postfix;        // overlapping symbol buffer

    // lengths
    unsigned int ndbps;             // number of data bits per OFDM symbol
    unsigned int ncbps;             // number of coded bits per OFDM symbol
    unsigned int nbpsc;             // number of bits per subcarrier (modulation depth)
    unsigned int dec_msg_len;       // length of decoded message (bytes)
    unsigned int enc_msg_len;       // length of encoded message (bytes)
    unsigned int nsym;              // number of OFDM symbols in the DATA field
    unsigned int ndata;             // number of bits in the DATA field
    unsigned int npad;              // number of pad bits
    unsigned int bytes_per_symbol;  // number of encoded data bytes per OFDM symbol

    // data arrays
    unsigned char   signal_dec[3];  // decoded message (SIGNAL field)
    unsigned char   signal_enc[6];  // encoded message (SIGNAL field)
    unsigned char   signal_int[6];  // interleaved message (SIGNAL field)
    unsigned char * msg_enc;        // encoded message (DATA field)
    unsigned char   modem_syms[48]; // modem symbols
    
    // counters/states
    enum {
        WLANFRAMEGEN_STATE_S0A=0,   // write first block of 'short' symbols
        WLANFRAMEGEN_STATE_S0B,     // write second block of 'short' symbols
        WLANFRAMEGEN_STATE_S1A,     // write first block of 'long' symbols
        WLANFRAMEGEN_STATE_S1B,     // write second block of 'long' symbols
        WLANFRAMEGEN_STATE_SIGNAL,  // write SIGNAL symbol
        WLANFRAMEGEN_STATE_DATA,    // write payload symbols
        WLANFRAMEGEN_STATE_NULL,    // write null (effectively ramp-down) symbol
    } state;
    int frame_assembled;            // frame assembled flag
    unsigned int data_symbol_counter;
};

// create WLAN framing generator object
wlanframegen wlanframegen_create()
{
    wlanframegen q = (wlanframegen) malloc(sizeof(struct wlanframegen_s));

    // allocate memory for transform objects
    q->X = (float complex*) malloc(64*sizeof(float complex));
    q->x = (float complex*) malloc(64*sizeof(float complex));
    q->ifft = FFT_CREATE_PLAN(64, q->X, q->x, FFT_DIR_BACKWARD, FFT_METHOD);

    // create pilot sequence generator
    q->ms_pilot = wlan_lfsr_create(7, 0x91, 0x7f);

    // DATA field (payload) modulator
    q->mod_scheme = WLAN_MODEM_BPSK;

    // create transition window/buffer
    // NOTE : ramp length must be less than cyclic prefix length (default: 1)
    // TODO : make ramp length an input parameter
    q->rampup_len = 1;
    q->rampup = (float*) malloc( q->rampup_len * sizeof(float) );
    q->postfix = (float complex*) malloc( q->rampup_len * sizeof(float complex) );

    // initialize ramp/up transition
    unsigned int i;
    for (i=0; i<q->rampup_len; i++) {
        float t = ((float)(i) + 0.5f) / (float)(q->rampup_len);
        float g = sinf(M_PI_2*t);
        q->rampup[i] = g*g;
    }
    
#if DEBUG_WLANFRAMEGEN
    // validate window symmetry
    for (i=0; i<q->rampup_len; i++) {
        printf("    ramp/up[%2u] = %12.8f (%12.8f)\n", i, q->rampup[i],
            q->rampup[i] + q->rampup[q->rampup_len - i - 1]);
    }
#endif

    // set initial properties
    q->rate   = WLANFRAME_RATE_6;
    q->length = 100;
    q->seed   = 0x5d;

    // allocate memory for encoded message
    q->enc_msg_len = wlan_packet_compute_enc_msg_len(q->rate, q->length);
    q->msg_enc = (unsigned char*) malloc(q->enc_msg_len*sizeof(unsigned char));

    // compute scaling factor
    q->g = 1.0f / 64.0f;

    // reset objects
    wlanframegen_reset(q);

    return q;
}

// destroy WLAN framing generator object
void wlanframegen_destroy(wlanframegen _q)
{
    // free transform array memory
    free(_q->X);
    free(_q->x);
    FFT_DESTROY_PLAN(_q->ifft);
    
    // destroy pilot sequence generator
    wlan_lfsr_destroy(_q->ms_pilot);

    // free transition window ramp array and postfix buffer
    free(_q->rampup);
    free(_q->postfix);

    // free memory for encoded message
    free(_q->msg_enc);

    // free main object memory
    free(_q);
}

// print WLAN framing generator object internals
void wlanframegen_print(wlanframegen _q)
{
    printf("wlanframegen:\n");
    if (_q->frame_assembled) {
        printf("    rate        :   %3u Mbits/s\n", wlanframe_ratetab[_q->rate].rate);
        printf("    payload     :   %3u bytes\n", _q->length);
        printf("    ndbps       :   %3u (data bits per OFDM symbol)\n", _q->ndbps);
        printf("    ncbps       :   %3u (coded bits per OFDM symbol)\n", _q->ncbps);
        printf("    nbpsc       :   %3u (bits per subcarrier, mod. depth)\n", _q->nbpsc);
        printf("    nsym        :   %3u (number of OFDM symbols)\n", _q->nsym);
        printf("    ndata       :   %3u (number of bits in DATA field)\n", _q->ndata);
        printf("    npad        :   %3u (number of pad bits)\n", _q->npad);
        printf("    dec msg len :   %3u (bytes in decoded message)\n", _q->dec_msg_len);
        printf("    enc msg len :   %3u (bytes in encoded message)\n", _q->enc_msg_len);
        printf("    bytes/sym   :   %3u (number of encoded data bytes per OFDM symbol)\n", _q->bytes_per_symbol);
        printf("    signal dec  :   [%.2x %.2x %.2x]\n",
                _q->signal_dec[0],
                _q->signal_dec[1],
                _q->signal_dec[2]);
        printf("    signal enc  :   [%.2x %.2x %.2x %.2x %.2x %.2x]\n",
                _q->signal_enc[0],
                _q->signal_enc[1],
                _q->signal_enc[2],
                _q->signal_enc[3],
                _q->signal_enc[4],
                _q->signal_enc[5]);
        printf("    signal int  :   [%.2x %.2x %.2x %.2x %.2x %.2x]\n",
                _q->signal_int[0],
                _q->signal_int[1],
                _q->signal_int[2],
                _q->signal_int[3],
                _q->signal_int[4],
                _q->signal_int[5]);
    }
}

// reset WLAN framing generator object internal state
void wlanframegen_reset(wlanframegen _q)
{
    // reset state/counters
    _q->frame_assembled = 0;
    _q->state = WLANFRAMEGEN_STATE_S0A;
    _q->data_symbol_counter = 0;

    // reset pilot sequence generator
    wlan_lfsr_reset(_q->ms_pilot);

    // clear internal postfix buffer
    unsigned int i;
    for (i=0; i<_q->rampup_len; i++)
        _q->postfix[i] = 0.0f;

    // force NULL subcarriers to zero
    _q->X[ 0] = 0.0f;
    _q->X[27] = 0.0f;
    _q->X[28] = 0.0f;
    _q->X[29] = 0.0f;
    _q->X[30] = 0.0f;
    _q->X[31] = 0.0f;
    _q->X[32] = 0.0f;
    _q->X[33] = 0.0f;
    _q->X[34] = 0.0f;
    _q->X[35] = 0.0f;
    _q->X[36] = 0.0f;
    _q->X[37] = 0.0f;
}

// get length of frame (symbols)
//  _q              :   framing object
unsigned int wlanframegen_getframelen(wlanframegen _q)
{
    return  5 /* S0a + S0b + S1a + S1b + signal */ + _q->nsym + 1 /* NULL */;
}


// assemble frame (see Table 76)
//  _q          :   framing object
//  _payload    :   raw payload data [size: _opts.LENGTH x 1]
//  _txvector   :   framing options
void wlanframegen_assemble(wlanframegen           _q,
                           unsigned char *        _payload,
                           struct wlan_txvector_s _txvector)
{
    // validate input
    if (_txvector.DATARATE > 7) {
        fprintf(stderr,"error: wlanframegen_assemble(), invalid rate\n");
        exit(1);
    } else if (_txvector.LENGTH == 0 || _txvector.LENGTH > 4095) { 
        fprintf(stderr,"error: wlanframegen_assemble(), invalid data length\n");
        exit(1);
    }

#if 1
    if (_txvector.DATARATE == WLANFRAME_RATE_9) {
        fprintf(stderr,"error: wlanframegen_assemble(), the rate 9 M bits/s is currently unsupported\n");
        return;
    }
#endif

    // reset state
    wlanframegen_reset(_q);

    // set internal properties
    _q->rate   = _txvector.DATARATE;
    _q->length = _txvector.LENGTH;
    _q->seed   = 0x5d;  //(_txvector.SERVICE >> 9) & 0x7f;
    // TODO : strip off TXPWR_LEVEL

    _q->mod_scheme = wlanframe_ratetab[_q->rate].mod_scheme;

    // pack SIGNAL field
    unsigned int R = 0; // 'reserved' bit
    wlan_signal_pack(_q->rate, R, _q->length, _q->signal_dec);

    // encode SIGNAL field
    wlan_fec_signal_encode(_q->signal_dec, _q->signal_enc);

    // interleave SIGNAL field
    wlan_interleaver_encode_symbol(WLANFRAME_RATE_6, _q->signal_enc, _q->signal_int);

    // compute frame parameters
    _q->ndbps  = wlanframe_ratetab[_q->rate].ndbps; // number of data bits per OFDM symbol
    _q->ncbps  = wlanframe_ratetab[_q->rate].ncbps; // number of coded bits per OFDM symbol
    _q->nbpsc  = wlanframe_ratetab[_q->rate].nbpsc; // number of bits per subcarrier (modulation depth)

    // compute number of OFDM symbols:
    // prepend the 16 SERVICE bits and append the 6 tail bits
    div_t d = div(16 + 8*_q->length + 6, _q->ndbps);
    _q->nsym = d.quot + (d.rem == 0 ? 0 : 1);

    // compute number of bits in the DATA field
    _q->ndata = _q->nsym * _q->ndbps;

    // compute number of pad bits
    _q->npad = _q->ndata - (16 + 8*_q->length + 6);

    // compute decoded message length (number of data bytes)
    // NOTE : because ndbps is _always_ divisible by 8, so must ndata be
    // NOT TRUE: for rate 9, ndbps is 36 which is NOT divisible by 8!
    _q->dec_msg_len = _q->ndata / 8;

    // compute encoded message length (number of data bytes)
    _q->enc_msg_len = (_q->dec_msg_len * _q->ncbps) / _q->ndbps;
    
    // compute number of encoded data bytes per OFDM symbol
    _q->bytes_per_symbol = _q->enc_msg_len / _q->nsym;

    // validate encoded message length
    //assert(_q->enc_msg_len == wlan_packet_compute_enc_msg_len(_q->rate, _q->length));

    // re-allocate buffer for encoded message
    _q->msg_enc = (unsigned char*) realloc(_q->msg_enc, _q->enc_msg_len*sizeof(unsigned char));

    // encode message
    wlan_packet_encode(_q->rate, _q->seed, _q->length, _payload, _q->msg_enc);

    // flag frame as being assembled
    _q->frame_assembled = 1;
}


// write OFDM symbol, returning '1' when frame is complete
//  _q          :   framing generator object
//  _buffer     :   output sample buffer [size: 80 x 1]
int wlanframegen_writesymbol(wlanframegen    _q,
                             float complex * _buffer)
{
    // validate input
    if (!_q->frame_assembled) {
        fprintf(stderr,"error: wlanframegen_writesymbol(), frame not assembled\n");
        exit(1);
    }

    //
    switch (_q->state) {
    case WLANFRAMEGEN_STATE_S0A:
        wlanframegen_writesymbol_S0a(_q, _buffer);
        _q->state = WLANFRAMEGEN_STATE_S0B;
        return 0;
    case WLANFRAMEGEN_STATE_S0B:
        wlanframegen_writesymbol_S0b(_q, _buffer);
        _q->state = WLANFRAMEGEN_STATE_S1A;
        return 0;
    case WLANFRAMEGEN_STATE_S1A:
        wlanframegen_writesymbol_S1a(_q, _buffer);
        _q->state = WLANFRAMEGEN_STATE_S1B;
        return 0;
    case WLANFRAMEGEN_STATE_S1B:
        wlanframegen_writesymbol_S1b(_q, _buffer);
        _q->state = WLANFRAMEGEN_STATE_SIGNAL;
        return 0;
    case WLANFRAMEGEN_STATE_SIGNAL:
        wlanframegen_writesymbol_signal(_q, _buffer);
        _q->state = WLANFRAMEGEN_STATE_DATA;
        return 0;
    case WLANFRAMEGEN_STATE_DATA:
        wlanframegen_writesymbol_data(_q, _buffer);
        _q->data_symbol_counter++;

        if (_q->data_symbol_counter == _q->nsym)
            _q->state = WLANFRAMEGEN_STATE_NULL;
        return 0;
    case WLANFRAMEGEN_STATE_NULL:
        wlanframegen_writesymbol_null(_q, _buffer);
        return 1;
    default:
        // should never get to this point
        fprintf(stderr,"error: wlanframegen_writesymbol(), invalid state\n");
        exit(1);
    }

    // reset and return
    wlanframegen_reset(_q);
    return 1;
}

// 
// internal methods
//

// compute symbol: add/update pilots, add nulls and compute transform
//  * input stored in 'X' (internal ifft input)
//  * output stored in 'x' (internal ifft output)
void wlanframegen_compute_symbol(wlanframegen _q)
{
    // update pilot phase
    unsigned int pilot_phase = wlan_lfsr_advance(_q->ms_pilot);

    // set pilots
    _q->X[43] = pilot_phase ? -1.0f :  1.0f;
    _q->X[57] = pilot_phase ? -1.0f :  1.0f;
    _q->X[ 7] = pilot_phase ? -1.0f :  1.0f;
    _q->X[21] = pilot_phase ?  1.0f : -1.0f;

    // NOTE : NULL subcarriers have been set to zero in reset() method

    // run inverse transform
    FFT_EXECUTE(_q->ifft);
}

// generate symbol (add cyclic prefix/postfix, overlap)
//  _x          :   input time-domain symbol [size: 64 x 1]
//  _x_prime    :   post-fix from previous symbol [size: _p x 1], output
//                  post-fix from this new symbol
//  _rampup     :   ramp up window; ramp down is time-reversed [size: _p x 1]
//  _p          :   post-fix size
//  _symbol     :   output symbol [size: 80 x 1]
void wlanframegen_gensymbol(float complex * _x,
                            float complex * _x_prime,
                            float         * _rampup,
                            unsigned int    _p,
                            float complex * _symbol)
{
    // validate input
    if (_p >= 16) {
        fprintf(stderr,"error: wlanframegen_gensymbol(), transition length cannot exceed cyclic prefix\n");
        exit(1);
    }

    // copy input symbol with cyclic prefix to output symbol
    memmove(&_symbol[ 0], &_x[48], 16*sizeof(float complex));
    memmove(&_symbol[16], &_x[ 0], 64*sizeof(float complex));

    // apply window to over-lapping regions
    unsigned int i;
    for (i=0; i<_p; i++) {
        _symbol[i] *= _rampup[i];
        _symbol[i] += _x_prime[i] * _rampup[_p-i-1];
    }

    // copy post-fix to output (first _p samples of input symbol)
    memmove(_x_prime, _x, _p*sizeof(float complex));
}

// write first PLCP short sequence 'symbol' to buffer; this is the first
// five 'short' symbols
//
//  0         32        64        96       128       160
//  +----+----+----+----+----+----+----+----+----+----+
//  | s0 | s0 | s0 | s0 | s0 | s0 | s0 | s0 | s0 | s0 |
//  +----+----+----+----+----+----+----+----+----+----+-----> time
//       |                   |    |                   |
//       |<-     s0[a]     ->|    |<-     s0[b]     ->|
//
void wlanframegen_writesymbol_S0a(wlanframegen _q,
                                  float complex * _buffer)
{
    // generate first 'short sequence' symbol
    wlanframegen_gensymbol((float complex*) wlanframe_s0,
                           _q->postfix,
                           _q->rampup,
                           _q->rampup_len,
                           _buffer);
}

// write second PLCP short sequence 'symbol' to buffer
void wlanframegen_writesymbol_S0b(wlanframegen _q,
                                  float complex * _buffer)
{
    // same as s0[a]
    wlanframegen_writesymbol_S0a(_q, _buffer);
}

// write first PLCP long sequence 'symbol' to buffer
//
//  0         32        64        96       128       160
//  +----+----+----+----+----+----+----+----+----+----+
//  |/////////|       s1[0]       |       s1[1]       | ...
//  +----+----+----+----+----+----+----+----+----+----+-----> time
//       |                   |    |                   |
//       |<-     s1[a]     ->|    |<-     s1[b]     ->|
//
void wlanframegen_writesymbol_S1a(wlanframegen _q,
                                  float complex * _buffer)
{
    // NOTE : the 'long' sequence is like a 128-sample symbol with
    //        a 32-sample cyclic prefix; need to split appropriately
    //        (see diagram above)
    memmove(&_q->x[ 0], &wlanframe_s1[48], 16*sizeof(float complex));
    memmove(&_q->x[16], &wlanframe_s1[ 0], 48*sizeof(float complex));
    
    // generate first 'long sequence' symbol
    wlanframegen_gensymbol(_q->x,
                           _q->postfix,
                           _q->rampup,
                           _q->rampup_len,
                           _buffer);
}

// write second PLCP long sequence 'symbol' to buffer
void wlanframegen_writesymbol_S1b(wlanframegen _q,
                                  float complex * _buffer)
{
    // NOTE : the 'long' sequence is like a 128-sample symbol with
    //        a 32-sample cyclic prefix; need to split appropriately
    //        (see diagram above)
    memmove(_q->x, wlanframe_s1, 64*sizeof(float complex));
    
    // generate first 'long sequence' symbol
    wlanframegen_gensymbol(_q->x,
                           _q->postfix,
                           _q->rampup,
                           _q->rampup_len,
                           _buffer);
}

// write SIGNAL symbol
void wlanframegen_writesymbol_signal(wlanframegen _q,
                                     float complex * _buffer)
{
    // load 48 SIGNAL BPSK symbols onto appropriate subcarriers
    _q->X[38] = (_q->signal_int[0] & 0x80) ? 1.0f : -1.0f;
    _q->X[39] = (_q->signal_int[0] & 0x40) ? 1.0f : -1.0f;
    _q->X[40] = (_q->signal_int[0] & 0x20) ? 1.0f : -1.0f;
    _q->X[41] = (_q->signal_int[0] & 0x10) ? 1.0f : -1.0f;
    _q->X[42] = (_q->signal_int[0] & 0x08) ? 1.0f : -1.0f;
    //    43  : pilot
    _q->X[44] = (_q->signal_int[0] & 0x04) ? 1.0f : -1.0f;
    _q->X[45] = (_q->signal_int[0] & 0x02) ? 1.0f : -1.0f;
    _q->X[46] = (_q->signal_int[0] & 0x01) ? 1.0f : -1.0f;
    _q->X[47] = (_q->signal_int[1] & 0x80) ? 1.0f : -1.0f;
    _q->X[48] = (_q->signal_int[1] & 0x40) ? 1.0f : -1.0f;
    _q->X[49] = (_q->signal_int[1] & 0x20) ? 1.0f : -1.0f;
    _q->X[50] = (_q->signal_int[1] & 0x10) ? 1.0f : -1.0f;
    _q->X[51] = (_q->signal_int[1] & 0x08) ? 1.0f : -1.0f;
    _q->X[52] = (_q->signal_int[1] & 0x04) ? 1.0f : -1.0f;
    _q->X[53] = (_q->signal_int[1] & 0x02) ? 1.0f : -1.0f;
    _q->X[54] = (_q->signal_int[1] & 0x01) ? 1.0f : -1.0f;
    _q->X[55] = (_q->signal_int[2] & 0x80) ? 1.0f : -1.0f;
    _q->X[56] = (_q->signal_int[2] & 0x40) ? 1.0f : -1.0f;
    //    57  : pilot
    _q->X[58] = (_q->signal_int[2] & 0x20) ? 1.0f : -1.0f;
    _q->X[59] = (_q->signal_int[2] & 0x10) ? 1.0f : -1.0f;
    _q->X[60] = (_q->signal_int[2] & 0x08) ? 1.0f : -1.0f;
    _q->X[61] = (_q->signal_int[2] & 0x04) ? 1.0f : -1.0f;
    _q->X[62] = (_q->signal_int[2] & 0x02) ? 1.0f : -1.0f;
    _q->X[63] = (_q->signal_int[2] & 0x01) ? 1.0f : -1.0f;
    //     0  : NULL
    _q->X[ 1] = (_q->signal_int[3] & 0x80) ? 1.0f : -1.0f;
    _q->X[ 2] = (_q->signal_int[3] & 0x40) ? 1.0f : -1.0f;
    _q->X[ 3] = (_q->signal_int[3] & 0x20) ? 1.0f : -1.0f;
    _q->X[ 4] = (_q->signal_int[3] & 0x10) ? 1.0f : -1.0f;
    _q->X[ 5] = (_q->signal_int[3] & 0x08) ? 1.0f : -1.0f;
    _q->X[ 6] = (_q->signal_int[3] & 0x04) ? 1.0f : -1.0f;
    //     7  : pilot
    _q->X[ 8] = (_q->signal_int[3] & 0x02) ? 1.0f : -1.0f;
    _q->X[ 9] = (_q->signal_int[3] & 0x01) ? 1.0f : -1.0f;
    _q->X[10] = (_q->signal_int[4] & 0x80) ? 1.0f : -1.0f;
    _q->X[11] = (_q->signal_int[4] & 0x40) ? 1.0f : -1.0f;
    _q->X[12] = (_q->signal_int[4] & 0x20) ? 1.0f : -1.0f;
    _q->X[13] = (_q->signal_int[4] & 0x10) ? 1.0f : -1.0f;
    _q->X[14] = (_q->signal_int[4] & 0x08) ? 1.0f : -1.0f;
    _q->X[15] = (_q->signal_int[4] & 0x04) ? 1.0f : -1.0f;
    _q->X[16] = (_q->signal_int[4] & 0x02) ? 1.0f : -1.0f;
    _q->X[17] = (_q->signal_int[4] & 0x01) ? 1.0f : -1.0f;
    _q->X[18] = (_q->signal_int[5] & 0x80) ? 1.0f : -1.0f;
    _q->X[19] = (_q->signal_int[5] & 0x40) ? 1.0f : -1.0f;
    _q->X[20] = (_q->signal_int[5] & 0x20) ? 1.0f : -1.0f;
    //    21  : pilot
    _q->X[22] = (_q->signal_int[5] & 0x10) ? 1.0f : -1.0f;
    _q->X[23] = (_q->signal_int[5] & 0x08) ? 1.0f : -1.0f;
    _q->X[24] = (_q->signal_int[5] & 0x04) ? 1.0f : -1.0f;
    _q->X[25] = (_q->signal_int[5] & 0x02) ? 1.0f : -1.0f;
    _q->X[26] = (_q->signal_int[5] & 0x01) ? 1.0f : -1.0f;

    // run transform
    wlanframegen_compute_symbol(_q);

    // validate against Table G.11

    // apply gain
    unsigned int i;
    for (i=0; i<64; i++)
        _q->x[i] /= sqrtf(64.0f);
    
    // validate against Table G.12

    // generate SIGNAL symbol
    wlanframegen_gensymbol(_q->x,
                           _q->postfix,
                           _q->rampup,
                           _q->rampup_len,
                           _buffer);
}

// write data symbol(s)
void wlanframegen_writesymbol_data(wlanframegen _q,
                                   float complex * _buffer)
{
    // unpack modem symbols
    //printf("  %3u = %3u * %3u\n", _q->enc_msg_len, _q->nsym, _q->bytes_per_symbol);
    unsigned int num_written;
    liquid_wlan_repack_bytes(&_q->msg_enc[_q->data_symbol_counter * _q->bytes_per_symbol], 8, _q->bytes_per_symbol,
                             _q->modem_syms, _q->nbpsc, 48,
                             &num_written);
    assert(num_written == 48);

    // modulate symbols onto subcarriers
    // TODO : do this more efficiently
    unsigned int i;
    unsigned int n=0;
    for (i=0; i<64; i++) {
        unsigned int k = (i + 32) % 64;

        if ( k==0 || (k > 26 && k < 38) ) {
            // NULL subcarrier
        } else if (k==43 || k==57 || k==7 || k==21) {
            // PILOT subcarrier
        } else {
            // DATA subcarrier
            assert(n<48);
            _q->X[k] = wlan_modulate(_q->mod_scheme, _q->modem_syms[n]);
            n++;
        }
    }
    assert(n==48);

    // run transform
    wlanframegen_compute_symbol(_q);

    // apply gain
    for (i=0; i<64; i++)
        _q->x[i] /= sqrtf(64.0f);
    
    // generate SIGNAL symbol
    wlanframegen_gensymbol(_q->x,
                           _q->postfix,
                           _q->rampup,
                           _q->rampup_len,
                           _buffer);
}

// write null symbol(s)
void wlanframegen_writesymbol_null(wlanframegen _q,
                                   float complex * _buffer)
{
#if 0
    memset(_q->x, 0x00, 64*sizeof(float complex));
#else
    unsigned int i;
    for (i=0; i<64; i++)
        _q->x[i] = 0.0f;
#endif
    
    // generate symbol
    wlanframegen_gensymbol(_q->x,
                           _q->postfix,
                           _q->rampup,
                           _q->rampup_len,
                           _buffer);
}

