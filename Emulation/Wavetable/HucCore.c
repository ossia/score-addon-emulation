// SPDX-License-Identifier: BSD-3-Clause
// Thin local interface to the pinned Charles MacDonald / libvgm core.
#include "emu/cores/c6280_mame.c"
void* score_huc_new(unsigned clock, unsigned rate) {
  void* p = device_start_c6280mame(clock, rate);
  if(p) device_reset_c6280mame(p);
  return p;
}
void score_huc_delete(void* p) { device_stop_c6280mame(p); }
void score_huc_write(void* p, unsigned reg, unsigned data) { c6280mame_w(p, reg, data); }
void score_huc_sample(void* p, int* left, int* right) {
  DEV_SMPL* out[2] = {left, right}; c6280mame_update(p, 1, out);
}
int score_huc_wave(void* p, int ch, int sample) { return ((c6280_t*)p)->channel[ch].waveform[sample]; }
