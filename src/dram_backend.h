/*
 * Copyright 2026 University of California Santa Cruz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : dram_backend.h
 * Author       : Litz Lab
 * Date         : 10/2026
 * Description  : The DRAM simulator behind ramulator.cc. The glue there owns everything
 *                Scarab-specific (Mem_Req bookkeeping, merged reads, write acks, the
 *                response queue); a backend only models DRAM timing. Kept while
 *                Ramulator 1 and Ramulator 2 both exist.
 ***************************************************************************************/

#ifndef __DRAM_BACKEND_H__
#define __DRAM_BACKEND_H__

struct Ramulator_Config_struct;

class DramBackend {
 public:
  /* Called by the backend, from tick(), when the read to line address addr completes. */
  typedef void (*Read_Done_Func)(long addr);

  virtual ~DramBackend() {}

  /* Offers one line-sized request. Returns false if the controller's queue is full, in which
     case nothing was enqueued. A write is never called back: the glue acknowledges it once
     send returns true. */
  virtual bool send(long addr, bool is_write, int proc_id) = 0;
  /* Advances DRAM by one memory-clock cycle. */
  virtual void tick() = 0;
  /* Ends the simulation and writes the backend's own stats file. */
  virtual void finish() = 0;

  /* The organization and timing being simulated (see Ramulator_Config in ramulator.h). */
  virtual void get_config(Ramulator_Config_struct* config) const = 0;

  /* Chip geometry for the power model. */
  virtual int get_chip_width() const = 0;
  virtual int get_chip_size() const = 0;
  virtual int get_num_chips() const = 0;
  virtual int get_chip_row_buffer_size() const = 0;
};

DramBackend* ramulator1_backend_create(DramBackend::Read_Done_Func read_done);

#endif  // __DRAM_BACKEND_H__
