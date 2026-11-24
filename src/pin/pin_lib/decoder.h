/* Copyright 2020 HPS/SAFARI Research Groups
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

#ifndef __DECODER_H__
#define __DECODER_H__

#undef UNUSED   // there is a name conflict between PIN and Scarab
#undef WARNING  // there is a name conflict between PIN and Scarab
#include "pin.H"
#undef UNUSED   // there is a name conflict between PIN and Scarab
#undef WARNING  // there is a name conflict between PIN and Scarab
#include <ostream>
#include <type_traits>
#include <utility>

#include "../../ctype_pin_inst.h"
#include "../../table_info.h"

#include "x86_decoder.h"

using namespace std;

void pin_decoder_init(bool translate_x87_regs, std::ostream* err_ostream);

void            pin_decoder_insert_analysis_functions(const INS& ins);
void            insert_analysis_functions(ctype_pin_inst* info, const INS& ins);
ctype_pin_inst* pin_decoder_get_latest_inst();

// Merged analysis calls: a caller (pin_exec) can fold the decoder's per-instruction work into its own single
// call. pin_decoder_add_*_args append only the ADDRINT args the instruction needs and return flags; the
// analysis routine gets them back as an array (merged_entry) and passes them to pin_decoder_parse/before/after.
// Flag bits 0-3, 8-10 and 16-31 are the decoder's; callers may use bits 4-7 and 11-15.
enum {
  DEC_LD = 1,
  DEC_LD2 = 2,
  DEC_ST = 4,
  DEC_CF = 8
};
struct Dec_Vals {
  ADDRINT ld_ea, ld_ea2, st_ea;
  UINT32 ld_size;
  bool taken;
  const ADDRINT* regs;
  const ADDRINT* end;  // first arg after the decoder's
};
ctype_pin_inst* pin_decoder_prepare(const INS& ins);
UINT32 pin_decoder_add_before_args(const INS& ins, ctype_pin_inst* info, IARGLIST args, uns* nargs);
UINT32 pin_decoder_add_after_args(const INS& ins, ctype_pin_inst* info, IARGLIST args, uns* nargs);
Dec_Vals pin_decoder_parse(UINT32 flags, const ADDRINT* a);
void pin_decoder_before(ctype_pin_inst* info, ADDRINT iaddr, UINT32 flags, const Dec_Vals& v);
const ADDRINT* pin_decoder_after(ADDRINT iaddr, UINT32 flags, const ADDRINT* a);

// An analysis routine taking a variable number of ADDRINT args: one instantiation per count.
typedef void (*Merged_Impl)(ctype_pin_inst* info, ADDRINT iaddr, UINT32 flags, const ADDRINT* a);
template <Merged_Impl F, typename... A>
void merged_entry_n(ctype_pin_inst* info, ADDRINT iaddr, UINT32 flags, A... a) {
  const ADDRINT v[] = {0, (ADDRINT)a...};
  F(info, iaddr, flags, v + 1);
}
template <Merged_Impl F, size_t... J>
AFUNPTR merged_entry_of(std::index_sequence<J...>) {
  return (AFUNPTR)merged_entry_n<F, std::conditional_t<true, ADDRINT, std::integral_constant<size_t, J>>...>;
}
template <Merged_Impl F, size_t... N>
AFUNPTR merged_entry_at(size_t nargs, std::index_sequence<N...>) {
  static const AFUNPTR fns[] = {merged_entry_of<F>(std::make_index_sequence<N>())...};
  ASSERTX(nargs < sizeof(fns) / sizeof(fns[0]));
  return fns[nargs];
}
template <Merged_Impl F>
AFUNPTR merged_entry(size_t nargs) {
  return merged_entry_at<F>(nargs, std::make_index_sequence<13>());
}

void pin_decoder_print_unknown_opcodes();

vector<PIN_MEM_ACCESS_INFO>
  get_gather_scatter_mem_access_infos_from_gather_scatter_info(
    const CONTEXT* ctxt, const PIN_MULTI_MEM_ACCESS_INFO* infos_from_pin);

#endif  // __DECODER_H__
