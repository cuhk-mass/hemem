/*
 * Copyright 2019 University of Washington, Max Planck Institute for
 * Software Systems, and The University of Texas at Austin
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>

#include "iokvs.h"

struct settings settings;

int settings_init(int argc, char *argv[])
{
    settings.udpport = 11211;
    settings.verbose = 1;
    //settings.segsize = 256 * 1024;
    //settings.segsize = 128 * 65536;
    settings.segsize = 1024 * 1024 * 1024;
    //settings.segmaxnum = 4096;
    //settings.segmaxnum = 64 * 4096;
    settings.segmaxnum = 700;
    //settings.segcqsize = 32 * 1024;
    settings.segcqsize = 1500;
    settings.clean_ratio = 0.8;
    //settings.clean_ratio = 1.1;

    if (argc != 3) {
        fprintf(stderr, "Usage: flexkvs CONFIG THREADS\n");
        return -1;
    }

    settings.numcores = atoi(argv[2]);
    settings.config_file = argv[1];
    return 0;
}
