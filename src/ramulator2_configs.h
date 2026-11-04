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
 * File         : ramulator2_configs.h
 * Author       : Litz Lab
 * Date         : 10/2026
 * Description  : The Ramulator 2 configs built into scarab: every src/ramulator2/<name>.yaml,
 *                embedded at build time by src/ramulator2/embed_configs.cmake.
 ***************************************************************************************/

#ifndef __RAMULATOR2_CONFIGS_H__
#define __RAMULATOR2_CONFIGS_H__

struct Ramulator2_Builtin_Config {
  const char* name;  // the file name without .yaml, e.g. "DDR4_2400R_8Gb_x8_1ch"
  const char* yaml;  // the file's contents
};

extern const Ramulator2_Builtin_Config ramulator2_builtin_configs[];
extern const unsigned ramulator2_num_builtin_configs;

#endif  // __RAMULATOR2_CONFIGS_H__
