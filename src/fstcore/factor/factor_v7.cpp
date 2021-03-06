/*
  fst - An R-package for ultra fast storage and retrieval of datasets.
  Copyright (C) 2017, Mark AJ Klik

  BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
    copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the
    distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
    "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
    LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
    A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  You can contact the author at :
  - fst source repository : https://github.com/fstPackage/fst
*/

// Standard headers
#include <stdexcept>
#include <iostream>
#include <fstream>

// Framework headers
#include "fstdefines.h"
#include "iblockrunner.h"
#include "factor_v7.h"
#include "blockstreamer_v2.h"
#include "integer_v8.h"
#include "character_v6.h"

#include "compression.h"
#include "compressor.h"

// #include <boost/unordered_map.hpp>

using namespace std;


#define HEADER_SIZE_FACTOR 16
#define VERSION_NUMBER_FACTOR 1

void fdsWriteFactorVec_v7(ofstream &myfile, int* intP, IBlockWriter* blockRunner, unsigned int size, unsigned int compression)
{
  unsigned long long blockPos = myfile.tellp();  // offset for factor
  unsigned int nrOfFactorLevels = blockRunner->vecLength;

  // Vector meta data
  char meta[HEADER_SIZE_FACTOR];
  unsigned int* versionNr = (unsigned int*) &meta;
  unsigned int* nrOfLevels = (unsigned int*) &meta[4];
  unsigned long long* levelVecPos = (unsigned long long*) &meta[8];

  myfile.write(meta, HEADER_SIZE_FACTOR);  // number of levels

  *nrOfLevels = nrOfFactorLevels;

  // Use blockrunner to store factor levels
  fdsWriteCharVec_v6(myfile, blockRunner, compression);   // factor levels

  // Rewrite meta-data
  *versionNr = VERSION_NUMBER_FACTOR;
  *levelVecPos = myfile.tellp();  // offset for level vector

  myfile.seekp(blockPos);
  myfile.write(meta, HEADER_SIZE_FACTOR);  // number of levels
  myfile.seekp(*levelVecPos);  // return to end of file

  // Store factor vector here

  unsigned int nrOfRows = size;  // vector length

  // With zero compression only a fixed width compactor is used (int to byte or int to short)

  if (compression == 0)
  {
    if (*nrOfLevels < 128)
    {
      FixedRatioCompressor* compressor = new FixedRatioCompressor(CompAlgo::INT_TO_BYTE);  // compression level not relevant here
      fdsStreamUncompressed_v2(myfile, (char*) intP, nrOfRows, 4, BLOCKSIZE_INT, compressor);

      delete compressor;

      return;
    }

    if (*nrOfLevels < 32768)
    {
      FixedRatioCompressor* compressor = new FixedRatioCompressor(CompAlgo::INT_TO_SHORT);  // compression level not relevant here
      fdsStreamUncompressed_v2(myfile, (char*) intP, nrOfRows, 4, BLOCKSIZE_INT, compressor);
      delete compressor;

      return;
    }

    fdsStreamUncompressed_v2(myfile, (char*) intP, nrOfRows, 4, BLOCKSIZE_INT, NULL);

    return;
  }

  int blockSize = 4 * BLOCKSIZE_INT;  // block size in bytes

  if (*nrOfLevels < 128)  // use 1 byte per int
  {
    Compressor* defaultCompress = NULL;
    Compressor* compress2 = NULL;
    StreamCompressor* streamCompressor = NULL;

    if (compression <= 50)
    {
      defaultCompress = new SingleCompressor(CompAlgo::INT_TO_BYTE, 0);  // compression not relevant here
      compress2 = new SingleCompressor(CompAlgo::LZ4_INT_TO_BYTE, 100);  // use maximum compression for LZ4 algorithm
      streamCompressor = new StreamCompositeCompressor(defaultCompress, compress2, 2 * compression);
    }
    else
    {
      defaultCompress = new SingleCompressor(CompAlgo::LZ4_INT_TO_BYTE, 100);  // compression not relevant here
      compress2 = new SingleCompressor(CompAlgo::ZSTD_INT_TO_BYTE, compression - 70);  // use maximum compression for LZ4 algorithm
      streamCompressor = new StreamCompositeCompressor(defaultCompress, compress2, 2 * (compression - 50));
    }

    streamCompressor->CompressBufferSize(blockSize);

    fdsStreamcompressed_v2(myfile, (char*) intP, nrOfRows, 4, streamCompressor, BLOCKSIZE_INT);
    delete defaultCompress;
    delete compress2;
    delete streamCompressor;

    return;
  }

  if (*nrOfLevels < 32768)  // use 2 bytes per int
  {
    Compressor* defaultCompress = new SingleCompressor(CompAlgo::INT_TO_SHORT, 0);  // compression not relevant here
    Compressor* compress2 = new SingleCompressor(CompAlgo::LZ4_INT_TO_SHORT_SHUF2, 100);  // use maximum compression for LZ4 algorithm
    StreamCompressor* streamCompressor = new StreamCompositeCompressor(defaultCompress, compress2, compression);
    streamCompressor->CompressBufferSize(blockSize);

    fdsStreamcompressed_v2(myfile, (char*) intP, nrOfRows, 4, streamCompressor, BLOCKSIZE_INT);
    delete defaultCompress;
    delete compress2;
    delete streamCompressor;

    return;
  }

  // use default integer compression with shuffle

  Compressor* compress1 = new SingleCompressor(CompAlgo::LZ4_SHUF4, 0);
  StreamCompressor* streamCompressor = new StreamLinearCompressor(compress1, compression);
  streamCompressor->CompressBufferSize(blockSize);
  fdsStreamcompressed_v2(myfile, (char*) intP, nrOfRows, 4, streamCompressor, BLOCKSIZE_INT);
  delete compress1;
  delete streamCompressor;

  return;
}


// Parameter 'startRow' is zero based.
void fdsReadFactorVec_v7(istream &myfile, IBlockReader* blockReader, int* intP, unsigned long long blockPos, unsigned int startRow,
  unsigned int length, unsigned int size)
{
  // Jump to factor level
  myfile.seekg(blockPos);

  // Get vector meta data
  char meta[HEADER_SIZE_FACTOR];
  myfile.read(meta, HEADER_SIZE_FACTOR);
  unsigned int* versionNr = (unsigned int*) &meta;

  if (*versionNr > VERSION_NUMBER_FACTOR)
  {
	  throw runtime_error("Incompatible fst file.");
  }

  unsigned int* nrOfLevels = (unsigned int*) &meta[4];
  unsigned long long* levelVecPos = (unsigned long long*) &meta[8];

  // Read level strings

  fdsReadCharVec_v6(myfile, blockReader, blockPos + HEADER_SIZE_FACTOR, 0, *nrOfLevels, *nrOfLevels);  // get level strings

  // Read level values
  fdsReadColumn_v2(myfile, (char*) intP, *levelVecPos, startRow, length, size, 4);

  return;
}
