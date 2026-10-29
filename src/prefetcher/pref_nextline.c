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

/* Next-line prefetcher (dcache only)
 *
 * Usage:
 *   --pref_framework_on 1
 *   --pref_num_dcache_prefetchers 1
 *   --pref_dcache_prefetchers TYPE_NEXTLINE,DEST_DCACHE
 */

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "core.param.h"
#include "general.param.h"
#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"

#include "prefetcher/pref_common.h"
#include "prefetcher/pref_nextline.h"

#define NEXTLINE_DEGREE 1  // how many next lines to prefetch on each miss

static HWP_Info* nl_hwp_info = NULL;
static HWP_Type nl_dest;

void pref_nextline_init(HWP* hwp) {
  if (!pref_hwp_instance_enabled(hwp, PREF_TRAIN_LEVEL_DCACHE))
    return;
  hwp->hwp_info->enabled = TRUE;
  nl_hwp_info = hwp->hwp_info;
  nl_dest = pref_hwp_instance_dest(hwp, PREF_TRAIN_LEVEL_DCACHE);
}

void pref_nextline_dl0_miss(Addr lineAddr, Addr loadPC) {
  if (nl_hwp_info == NULL)
    return;

  Addr line_idx = lineAddr >> LOG2(DCACHE_LINE_SIZE);  // byte address -> line index

  for (uns i = 1; i <= NEXTLINE_DEGREE; i++) {
    if (!pref_addto_dest_req_queue(0, nl_dest, line_idx + i, nl_hwp_info->id))
      break;  // queue full
  }
}
