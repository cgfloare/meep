/* Copyright (C) 2005-2009 Massachusetts Institute of Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* This file implements dispersive materials for Meep via a polarization P = \chi(\omega) W,
   where W is e.g. E or H.  Each subclass of the susceptibility class should implement a different
   type of \chi(\omega).  The subclass knows how to timestep P given W at the current (and possibly
   previous) timestep, and any additional internal data that needs to be allocated along with P. 

   Each \chi(\omega) is spatially multiplied by a (scalar) sigma array.  The meep::fields class is
   responsible for allocating P and sigma and passing them to susceptibility::update_P. */


#include <string.h>
#include "meep.hpp"
#include "meep_internals.hpp"

namespace meep {

int susceptibility::cur_id = 0;

susceptibility *susceptibility::clone() const {
  susceptibility *sus = new susceptibility(*this);
  sus->next = 0;
  sus->ntot = ntot;
  sus->id = id;
  FOR_COMPONENTS(c) FOR_DIRECTIONS(d) {
    if (sigma[c][d]) {
      sus->sigma[c][d] = new realnum[ntot];
      memcpy(sus->sigma[c][d], sigma[c][d], sizeof(realnum) * ntot);
    }
    else sus->sigma[c][d] = NULL;
    sus->trivial_sigma[c][d] = trivial_sigma[c][d];
  }
  return sus;
}

/* Return whether or not we need to allocate P[c].  (We don't need to
   allocate P[c] if we can be sure it will be zero.)

   We are a bit wasteful because if sigma is nontrivial in *any* chunk,
   we allocate the corresponding P on *every* owned chunk.  This greatly
   simplifies communication in boundaries.cpp, because we can be sure that
   one chunk has a P then any chunk it borders has the same P, so we don't
   have to worry about communicating with something that doesn't exist.
   TODO: reduce memory usage (bookkeeping seem much harder, though).
*/
bool susceptibility::needs_P(component c, realnum *W[NUM_FIELD_COMPONENTS][2])
  const {
  if (!is_electric(c) && !is_magnetic(c)) return false;
  FOR_DIRECTIONS(d)
    if (!trivial_sigma[c][d] && W[direction_component(c, d)][0]) return true;
  return false;
}

/* return whether we need the notowned parts of the W field --
   by default, this is only the case if sigma has offdiagonal components
   coupling P to W.   (See needs_P: again, this true if the notowned
   W is needed in *any* chunk.) */
bool susceptibility::needs_W_notowned(component c,
				realnum *W[NUM_FIELD_COMPONENTS][2]) const {
  FOR_DIRECTIONS(d) if (d != component_direction(c)) {
    component cP = direction_component(c, d);
    if (needs_P(cP, W) && !trivial_sigma[cP][component_direction(c)])
      return true;
  }
  return false;
}

// for Lorentzian susc. the internal data is just a backup of P from
// the previous timestep.
int lorentzian_susceptibility::num_internal_data(
			 realnum *P[NUM_FIELD_COMPONENTS][2],
			 const grid_volume &gv) const {
  int num = 0;
  FOR_COMPONENTS(c) DOCMP2 if (P[c][cmp]) num += gv.ntot();
  return num;
}

#define SWAP(t,a,b) { t SWAP_temp = a; a = b; b = SWAP_temp; }

  // stable averaging of offdiagonal components
#define OFFDIAG(u,g,sx,s) (0.25 * ((g[i]+g[i-sx])*u[i]		\
		   	         + (g[i+s]+g[(i+s)-sx])*u[i+s]))

void lorentzian_susceptibility::update_P
       (realnum *P[NUM_FIELD_COMPONENTS][2],
	realnum *W[NUM_FIELD_COMPONENTS][2],
	realnum *W_prev[NUM_FIELD_COMPONENTS][2], 
	double dt, const grid_volume &gv, realnum *P_internal_data) const {
  const double omega2pi = 2*pi*omega_0, g2pi = gamma*2*pi;
  const double omega0dtsqr = omega2pi * omega2pi * dt * dt;
  const double gamma1inv = 1 / (1 + g2pi*dt/2), gamma1 = (1 - g2pi*dt/2);
  const double omega0dtsqr_denom = no_omega_0_denominator ? 0 : omega0dtsqr;
  (void) W_prev; // unused;
  
  realnum *P_prev;
  P_prev = P_internal_data;
  FOR_COMPONENTS(c) DOCMP2 if (P[c][cmp]) {
    const realnum *w = W[c][cmp], *s = sigma[c][component_direction(c)];
    if (w && s) {
      realnum *p = P[c][cmp], *pp = P_prev;

      // directions/strides for offdiagonal terms, similar to update_eh
      const direction d = component_direction(c);
      const int is = gv.stride(d) * (is_magnetic(c) ? -1 : +1);
      direction d1 = cycle_direction(gv.dim, d, 1);
      component c1 = direction_component(c, d1);
      int is1 = gv.stride(d1) * (is_magnetic(c) ? -1 : +1);
      const realnum *w1 = W[c1][cmp];
      const realnum *s1 = w1 ? sigma[c][d1] : NULL;
      direction d2 = cycle_direction(gv.dim, d, 2);
      component c2 = direction_component(c, d2);
      int is2 = gv.stride(d2) * (is_magnetic(c) ? -1 : +1);
      const realnum *w2 = W[c2][cmp];
      const realnum *s2 = w2 ? sigma[c][d2] : NULL;

      if (s2 && !s1) { // make s1 the non-NULL one if possible
	SWAP(direction, d1, d2);
	SWAP(component, c1, c2);
	SWAP(int, is1, is2);
	SWAP(const realnum *, w1, w2);
	SWAP(const realnum *, s1, s2);
      }
      if (s1 && s2) { // 3x3 anisotropic
	LOOP_OVER_VOL_OWNED(gv, c, i) {
	  realnum pcur = p[i];
	  p[i] = gamma1inv * (pcur * (2 - omega0dtsqr_denom) 
			      - gamma1 * pp[i] 
			      + omega0dtsqr * (s[i] * w[i]
					       + OFFDIAG(s1,w1,is1,is)
					       + OFFDIAG(s2,w2,is2,is)));
	  pp[i] = pcur;
	}
      }
      else if (s1) { // 2x2 anisotropic
	LOOP_OVER_VOL_OWNED(gv, c, i) {
	  realnum pcur = p[i];
	  p[i] = gamma1inv * (pcur * (2 - omega0dtsqr_denom) 
			      - gamma1 * pp[i] 
			      + omega0dtsqr * (s[i] * w[i]
					       + OFFDIAG(s1,w1,is1,is)));
	  pp[i] = pcur;
	}
      }
      else { // isotropic
	LOOP_OVER_VOL_OWNED(gv, c, i) {
	  realnum pcur = p[i];
	  p[i] = gamma1inv * (pcur * (2 - omega0dtsqr_denom) 
			      - gamma1 * pp[i] 
			      + omega0dtsqr * (s[i] * w[i]));
	  pp[i] = pcur;
	}
      }
    }
    P_prev += gv.ntot();
  }
}

} // namespace meep