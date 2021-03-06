////////////////////////////////////////////////////////////////////////////////
/**
 * @file main.cpp
 * @date 2017-03-04
 * @author Tiago Lobato Gimenes    (tlgimenes@gmail.com)
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
////////////////////////////////////////////////////////////////////////////////

#include "log.hpp"
#include "utils.hpp"
#include "parser.hpp"
#include "su_gather.hpp"

#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <cassert>
#include <cstring>
#include <chrono>
#include <atomic>
#include <future>

#include <openacc.h>
#include <accelmath.h>

////////////////////////////////////////////////////////////////////////////////

#define MAX_W 5

#define EPSILON 1e-13

#define FACTOR 1e6

////////////////////////////////////////////////////////////////////////////////

std::chrono::high_resolution_clock::time_point main_beg, main_end, beg, end;

double kernel_execution_time = 0.0;

////////////////////////////////////////////////////////////////////////////////

struct real4_t {
  real a, b, c, d;
};

using real4 = struct real4_t;

////////////////////////////////////////////////////////////////////////////////

int na, nb, nc, aph, apm, ng, ttraces, ncdps, ns, ntrs, npar, max_gather, w, tau;
int *ntraces_by_cdp_id, *ctr, *size;
real a0, a1, b0, b1, c0, c1, itau, inc_a, inc_b, inc_c, dt, idt;
real *gx, *gy, *sx, *sy, *scalco, *samples, *h0, *m0x, *m0y, *num, *stt, *str, *stk, *cdpsmpl, *m2, *m, *h;
real4 * par;

////////////////////////////////////////////////////////////////////////////////
// Evaluate Cs - linspace
void init_c()
{
  beg = std::chrono::high_resolution_clock::now();
#pragma acc parallel loop present(par)
  for(int i=0; i < npar; i++) {
    par[i].a = a0 + inc_a*(i/(nc*nb));
    par[i].b = b0 + inc_b*((i/nc)%nb);
    par[i].c = c0 + inc_c*(i%nc);
  }
  end = std::chrono::high_resolution_clock::now();
  kernel_execution_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - beg).count();
}

////////////////////////////////////////////////////////////////////////////////
// Evaluate halfoffset points in x and y coordinates
void init_mid()
{
  beg = std::chrono::high_resolution_clock::now();
#pragma acc parallel loop present(gx,gy,sx,sy,m0x,m0y)
  for(int i=0; i < ttraces; i++) {
    real _s = scalco[i];

    if(-EPSILON < _s && _s < EPSILON) _s = 1.0;
    else if(_s < 0) _s = 1.0f / _s;

    m0x[i] = (gx[i] + sx[i]) * _s * 0.5;
    m0y[i] = (gy[i] + sy[i]) * _s * 0.5;

    real hx = (gx[i] - sx[i]) * _s;
    real hy = (gy[i] - sy[i]) * _s;

    h0[i] = 0.25 * (hx * hx + hy * hy) / FACTOR;
  }
  end = std::chrono::high_resolution_clock::now();
  kernel_execution_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - beg).count();
}

////////////////////////////////////////////////////////////////////////////////

void compute_semblances(int id, int acc_stream, int size, int cdp_id)
{
  int num_offset = id*ns*npar;
  int cdpsmpl_offset = id*ntrs*max_gather*ns;
  int attr_offset = id*ntrs*max_gather;

  beg = std::chrono::high_resolution_clock::now();
#pragma acc parallel loop present(h, m2, m, par, cdpsmpl, num, stt)
#pragma acc loop collapse(2)
  for(int t0=0; t0 < ns; t0++) {
    for(int par_id=0; par_id < npar; par_id++) {
      real _num[MAX_W], _ac_squared = 0, _ac_linear = 0, _den = 0, mm = 0;
      int err = 0;

      int id = t0*npar + par_id;

      real4 _p = par[par_id];
      real _t0 = dt * t0;

      // start _num with zeros
#pragma acc loop seq
      for(int j=0; j < w; j++) _num[j] = 0.0f;

      // Reduction for num
#pragma acc loop seq
      for(int k=0; k < size; k++) {
        // Evaluate t
        real _m2 = m2[attr_offset + k];
        real t = _t0 + _p.a * m[attr_offset + k];
        t = t*t + _p.b*_m2 + _p.c*h[attr_offset + k];
        t = t < 0.0 ? -1 : SQRT(t) * idt;

        int it = (int)( t );
        int ittau = it - tau;
        real x = t - (real)it;

        int k1 = ittau + k*ns;
        if(ittau >= 0 && it + tau + 1 < ns) {
          real sk1p1 = cdpsmpl[cdpsmpl_offset+k1], sk1;

          for(int j=0; j < w; j++) {
            k1++;
            sk1 = sk1p1;
            sk1p1 = cdpsmpl[cdpsmpl_offset+k1];
            // linear interpolation optmized for this problem
            real v = (sk1p1 - sk1) * x + sk1;

            _num[j] += v;
            _den += v * v;
            _ac_linear += v;
          }
          mm++;
        } else { err++; }
      }

      // Reduction for num
#pragma acc loop seq
      for(int j=0; j < w; j++) _ac_squared += _num[j] * _num[j];

      // Evaluate semblances
      if(_den > EPSILON && mm > EPSILON && w > EPSILON && err < 2) {
        num[num_offset+id] = _ac_squared / (_den * mm);
        stt[num_offset+id] = _ac_linear  / (w   * mm);
      }
      else {
        num[num_offset+id] = 0.0f;
        stt[num_offset+id] = 0.0f;
      }
    }
  }
  end = std::chrono::high_resolution_clock::now();
  kernel_execution_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - beg).count();
}

////////////////////////////////////////////////////////////////////////////////

void redux_semblances(int id, int acc_stream, int cdp_id)
{
  int num_offset = id*ns*npar;

  beg = std::chrono::high_resolution_clock::now();
  // Get max C for max semblance for each sample on this cdp
#pragma acc parallel loop present(num, stt, ctr, str, stk)
  for(int t0=0; t0 < ns; t0++) {
    real max_sem = 0.0f;
    int max_par = 0;

    for(int it=t0*npar; it < (t0+1)*npar ; it++) {
      if(num[num_offset+it] > max_sem) {
        max_sem = num[num_offset+it];
        max_par = it;
      }
    }

    ctr[cdp_id*ns + t0] = max_par % npar;
    str[cdp_id*ns + t0] = max_sem;
    stk[cdp_id*ns + t0] = stt[num_offset+max_par];
  }
  end = std::chrono::high_resolution_clock::now();
  kernel_execution_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - beg).count();
}

////////////////////////////////////////////////////////////////////////////////

int prepare_data(su_gather* gather, int cdp_id, int acc_stream, int id)
{
  real m0x_cdp_id = (cdp_id > 0) ? m0x[ntraces_by_cdp_id[cdp_id-1]] : 0;
  real m0y_cdp_id = (cdp_id > 0) ? m0y[ntraces_by_cdp_id[cdp_id-1]] : 0;
  int cdp0 = gather->cdps_by_cdp_id()[cdp_id].front();
  int cdpf = gather->cdps_by_cdp_id()[cdp_id].back();
  int cdpsmpl_offset = id*ntrs*max_gather*ns;
  int attr_offset = id*ntrs*max_gather;
  int ac = 0;

  for(int i=0; i < gather->cdps_by_cdp_id()[cdp_id].size(); i++) {
    int cdp = gather->cdps_by_cdp_id()[cdp_id][i];
    int t_id0 = cdp > 0 ? ntraces_by_cdp_id[cdp-1] : 0;
    int t_idf = ntraces_by_cdp_id[cdp];
    int stride = t_idf-t_id0;

    memcpy(cdpsmpl+cdpsmpl_offset+ac*ns, samples + t_id0*ns, stride*ns*sizeof(real));

    ac += stride;
  }

#pragma acc update device(cdpsmpl[cdpsmpl_offset:ac*ns])

  beg = std::chrono::high_resolution_clock::now();
#pragma acc parallel loop present(m0x, m0y, m2, m, h, h0, ntraces_by_cdp_id)
  for(int cdp=cdp0; cdp <= cdpf; cdp++) {
    int t_id00= cdp0 > 0 ? ntraces_by_cdp_id[cdp0-1] : 0;
    int t_id0 = cdp > 0 ? ntraces_by_cdp_id[cdp-1] : 0;
    int t_idf = ntraces_by_cdp_id[cdp];
    int sz = t_id0-t_id00;

    for(int it=0; it < t_idf-t_id0; it++) {
      real dx = m0x[t_id0 + it] - m0x_cdp_id;
      real dy = m0y[t_id0 + it] - m0y_cdp_id;
      real _m2 = dx*dx + dy*dy;

      m2[attr_offset + sz + it] = _m2;
      m [attr_offset + sz + it] = SQRT(_m2);
      h [attr_offset + sz + it] = h0[t_id0 + it];
    }
  }
  end = std::chrono::high_resolution_clock::now();
  kernel_execution_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - beg).count();

  return ac;
}

////////////////////////////////////////////////////////////////////////////////

acc_device_t get_device_from_env_variable() {
  try {
    std::string device = ::getenv("ACC_DEVICE_TYPE");

    LOG(DEBUG, "Device Type: " + device);

    if(device == "radeon"        ) return acc_device_radeon;
    if(device == "nvidia"        ) return acc_device_nvidia;
    if(device == "host"          ) return acc_device_host;
    if(device == "multicore"     ) return acc_device_host;
    if(device == "xeonphi"       ) return acc_device_xeonphi;
    if(device == "pgi_opencl"    ) return acc_device_pgi_opencl;
    if(device == "nvidia_opencl" ) return acc_device_nvidia_opencl;
    if(device == "opencl"        ) return acc_device_opencl;

    LOG(INFO, "Unknown device: " + device + " .Using default fallback: host");
  } catch(const std::logic_error& err) {
    LOG(FAIL, "Failed to read ACC_DEVICE_TYPE environment variable with err: " + err.what());
  }

  return acc_device_host;
}

////////////////////////////////////////////////////////////////////////////////

int main(int argc, const char** argv) {
  std::ofstream a_out("crs.a.su", std::ofstream::out | std::ios::binary);
  std::ofstream b_out("crs.b.su", std::ofstream::out | std::ios::binary);
  std::ofstream c_out("crs.c.su", std::ofstream::out | std::ios::binary);
  std::ofstream s_out("crs.coher.su", std::ofstream::out | std::ios::binary);
  std::ofstream stack("crs.stack.su", std::ofstream::out | std::ios::binary);

  // Parse command line and read arguments
  parser::add_argument("-a0", "A0 constant");
  parser::add_argument("-a1", "A1 constant");
  parser::add_argument("-na", "NA constant");
  parser::add_argument("-b0", "B0 constant");
  parser::add_argument("-b1", "B1 constant");
  parser::add_argument("-nb", "NB constant");
  parser::add_argument("-c0", "C0 constant");
  parser::add_argument("-c1", "C1 constant");
  parser::add_argument("-nc", "NC constant");
  parser::add_argument("-aph", "APH constant");
  parser::add_argument("-apm", "APM constant");
  parser::add_argument("-tau", "Tau constant");
  parser::add_argument("-i", "Data path");
  parser::add_argument("-v", "Verbosity Level 0-3");

  parser::parse(argc, argv);

  // Read parameters and input
  a0 = std::stod(parser::get("-a0", true));
  a1 = std::stod(parser::get("-a1", true));
  b0 = std::stod(parser::get("-b0", true));
  b1 = std::stod(parser::get("-b1", true));
  c0 = std::stod(parser::get("-c0", true)) * FACTOR;
  c1 = std::stod(parser::get("-c1", true)) * FACTOR;
  itau = std::stod(parser::get("-tau", true));
  na = std::stoi(parser::get("-na", true));
  nb = std::stoi(parser::get("-nb", true));
  nc = std::stoi(parser::get("-nc", true));
  aph = std::stoi(parser::get("-aph", true));
  apm = std::stoi(parser::get("-apm", true));
  ng = 1;
  std::string path = parser::get("-i", true);
  logger::verbosity_level(std::stoi(parser::get("-v", false)));

  // Reads *.su data and starts gather
  su_gather gather(path, aph, apm, nc);

  // Linearize gather data in order to improove data coalescence in GPU
  gather.linearize(ntraces_by_cdp_id, samples, dt, gx, gy, sx, sy, scalco, nc);
  ttraces = gather.ttraces(); // Total traces -> Total amount of traces read
  ncdps = gather().size();    // Number of cdps -> Total number of cdps read
  ns = gather.ns();           // Number of samples
  ntrs = gather.ntrs();       // Max number of traces per cdp (fold)
  inc_a = (a1-a0) * (1.0 / (real)na);
  inc_b = (b1-b0) * (1.0 / (real)nb);
  inc_c = (c1-c0) * (1.0 / (real)nc);
  npar = na * nb * nc;
  max_gather = gather.max_gather();
  int number_of_semblances = 0;

  // Linear structures
  par = new real4[ npar ];         // nc Cs
  h0   = new real [ ttraces ];    // One halfoffset per trace
  m0x  = new real [ ttraces ];    // One midpoint per trace
  m0y  = new real [ ttraces ];    // One midpoint per trace
  num = new real [ ng * ns * npar ];    // nc nums per sample
  stt = new real [ ng * ns * npar ];    // nc stts per sample
  ctr = new int  [ ncdps * ns ]; // ns Cs per cdp
  str = new real [ ncdps * ns ]; // ns semblance per cdp
  stk = new real [ ncdps * ns ]; // ns stacked values per cdp
  cdpsmpl = new real [ ng * ns * ntrs * max_gather ]; // Samples for current cdp
  m2 = new real [ ng * ntrs * max_gather ]; // Samples for current cdp
  m  = new real [ ng * ntrs * max_gather ]; // Samples for current cdp
  h  = new real [ ng * ntrs * max_gather ]; // Samples for current cdp

  // Evaluate values for each cdp
  dt = dt / 1000000.0f;
  idt = 1.0f / dt;
  tau = (int)( itau * idt) > 0 ? (int)( itau * idt)  : 0;
  w = (2 * tau) + 1;

  // Create streams for async API
  std::vector<int> streams(ng, 1);
  for(int i=1; i < ng; i++) {
    streams[i] = streams[i-1] + 2;
  }

  LOG(DEBUG, "Starting OpenACC devices");

  // Init acc in order to better evaluate compute perfomance
  acc_init(get_device_from_env_variable());

  // Copies data to Compute Device
#pragma acc data \
  copyin(gx[:ttraces], gy[:ttraces], sx[:ttraces], sy[:ttraces], scalco[:ttraces], ntraces_by_cdp_id[:ncdps])  \
  create(stt[:ng*ns*npar], num[:ng*ns*npar], par[:npar], h0[:ttraces], m0x[:ttraces], m0y[:ttraces]) \
  copyout(str[:ncdps*ns], ctr[ncdps*ns], stk[ncdps*ns]) \
  create(cdpsmpl[:ng*ntrs*max_gather*ns]) \
  create(h[:ng*ntrs*max_gather], m2[:ng*ntrs*max_gather], m[:ng*ntrs*max_gather])
  {
    // Chronometer
    main_beg = std::chrono::high_resolution_clock::now();

    // Evaluate Cs - linspace
    init_c();

    // Evaluate halfoffset points in x and y coordinates
    init_mid();

    // Updates halfoffset and midpoint on host
#pragma acc update self(m0x[:ttraces], m0y[:ttraces])

    // Compute max semblances and get max C for each CDP
    for(int cdp_id=0; cdp_id < ncdps; cdp_id++) {
      int id = (cdp_id)%ng;
      int acc_stream = streams[id];

      int ntraces = prepare_data(&gather, cdp_id, acc_stream, id);

      compute_semblances(id, acc_stream, ntraces, cdp_id);

      redux_semblances(id, acc_stream, cdp_id);

      number_of_semblances += ntraces;

      LOG(DEBUG, "OpenACC Progress: " + std::to_string(cdp_id) + "/" + std::to_string(ncdps));
    }

    // Gets time at end of computation
    main_end = std::chrono::high_resolution_clock::now();
  }

  // Logs stats (exec time and semblance-traces per second)
  double total_exec_time = std::chrono::duration_cast<std::chrono::duration<double>>(main_end - main_beg).count();
  double stps = (number_of_semblances / 1e9 ) * (ns * npar / total_exec_time);
  double kernel_stps = (number_of_semblances / 1e9 ) * (ns * npar / kernel_execution_time);
  std::string stats = "Total Execution Time: " + std::to_string(total_exec_time);
  stats += ": Giga-Semblances-Trace/s: " + std::to_string(stps);
  stats += ": Kernel Execution Time: " + std::to_string(kernel_execution_time);
  stats += ": Kernel Giga-Semblances-Trace/s: " + std::to_string(kernel_stps);
  LOG(INFO, stats);

  // Delinearizes data and save it into a *.su file
  for(int i=0; i < ncdps; i++) {
    su_trace atr_t = gather[i].traces()[0];
    su_trace btr_t = gather[i].traces()[0];
    su_trace ctr_t = gather[i].traces()[0];
    su_trace str_t = gather[i].traces()[0];
    su_trace stk_t = gather[i].traces()[0];

    atr_t.offset() = 0;
    atr_t.sx() = atr_t.gx() = (gather[i].traces()[0].sx() + gather[i].traces()[0].gx()) >> 1;
    atr_t.sy() = atr_t.gy() = (gather[i].traces()[0].sy() + gather[i].traces()[0].gy()) >> 1;
    btr_t.offset() = 0;
    btr_t.sx() = btr_t.gx() = (gather[i].traces()[0].sx() + gather[i].traces()[0].gx()) >> 1;
    btr_t.sy() = btr_t.gy() = (gather[i].traces()[0].sy() + gather[i].traces()[0].gy()) >> 1;
    ctr_t.offset() = 0;
    ctr_t.sx() = ctr_t.gx() = (gather[i].traces()[0].sx() + gather[i].traces()[0].gx()) >> 1;
    ctr_t.sy() = ctr_t.gy() = (gather[i].traces()[0].sy() + gather[i].traces()[0].gy()) >> 1;

    for(int k=0; k < ns; k++) atr_t.data()[k] = (a0 + inc_a * (ctr[i*ns+k]/(nc*nb)));
    for(int k=0; k < ns; k++) btr_t.data()[k] = (b0 + inc_b * ((ctr[i*ns+k]/nc)%nb));
    for(int k=0; k < ns; k++) ctr_t.data()[k] = (c0 + inc_c * (ctr[i*ns+k]%nc)) / FACTOR;
    str_t.data().assign(str + i*ns, str + (i+1)*ns);
    stk_t.data().assign(stk + i*ns, stk + (i+1)*ns);

    atr_t.fputtr(a_out);
    btr_t.fputtr(b_out);
    ctr_t.fputtr(c_out);
    str_t.fputtr(s_out);
    stk_t.fputtr(stack);
  }

  delete [] gx                ;
  delete [] gy                ;
  delete [] sx                ;
  delete [] sy                ;
  delete [] scalco            ;
  delete [] samples           ;
  delete [] ntraces_by_cdp_id ;
  delete [] h0                ;
  delete [] m0x               ;
  delete [] m0y               ;
  delete [] par               ;
  delete [] num               ;
  delete [] stt               ;
  delete [] ctr               ;
  delete [] str               ;
  delete [] stk               ;

  return EXIT_SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
