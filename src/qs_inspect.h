/* qs - Quick Serialization of R Objects
 Copyright (C) 2019-present Travers Ching
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as
 published by the Free Software Foundation, either version 3 of the
 License, or (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.
 
 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <https://www.gnu.org/licenses/>.
 
 You can contact the author at:
 https://github.com/traversc/qs
 */

#include "qs_common.h"

////////////////////////////////////////////////////////////////
// de-serialization functions
////////////////////////////////////////////////////////////////

// Data integrity checks:
// Check file size matches
// Check number of blocks matches
// Check de-compression successful
// Check compress block size <= compressBound
// Check object depth

size_t LZ4_decompress_inspect_fun( void* dst, size_t dstCapacity,
                                   const void* src, size_t compressedSize) {
  int ret = LZ4_decompress_safe(reinterpret_cast<char*>(const_cast<void*>(src)), 
                                reinterpret_cast<char*>(const_cast<void*>(dst)),
                                static_cast<int>(compressedSize), static_cast<int>(dstCapacity));
  if(ret < 0) return 0;
  return ret;
}

size_t ZSTD_decompress_inspect_fun(void* dst, size_t dstCapacity,
                                   const void* src, size_t compressedSize) {
  size_t ret = ZSTD_decompress(dst, dstCapacity,
                src, compressedSize);
  if(ZSTD_isError(ret)) return 0;
  return ret;
}


struct Data_Inspect_Context {
  std::ifstream & myFile;
  
  QsMetadata qm;
  decompress_fun decompFun;
  cbound_fun cbFun;
  
  uint64_t number_of_blocks;
  std::vector<char> zblock;
  std::vector<char> block;
  
  Data_Inspect_Context(std::ifstream & mf, QsMetadata qm) : myFile(mf) {
    this->qm = qm;
    if(qm.compress_algorithm == 0) {
      decompFun = &ZSTD_decompress;
      cbFun = &ZSTD_compressBound;
    } else if(qm.compress_algorithm == 1 || qm.compress_algorithm == 2) { // algo == 1
      decompFun = &LZ4_decompress_fun;
      cbFun = &LZ4_compressBound_fun;
    } else {
      throw std::runtime_error("invalid compression algorithm selected");
    }
    number_of_blocks = readSizeFromFile8(myFile);
    zblock = std::vector<char>(cbFun(BLOCKSIZE));
    block = std::vector<char>(BLOCKSIZE);
  }
  bool inspectData() {
    uint64_t cbound = cbFun(BLOCKSIZE);
    std::array<char, 4> zsize_ar = {0,0,0,0};
    for(uint64_t block_i=0; block_i<number_of_blocks; block_i++) {
      myFile.read(zsize_ar.data(), 4);
      if(myFile.fail()) {
        Rcpp::Rcerr << "File read fail" << std::endl;
        return false;
      }
      uint64_t zsize = *reinterpret_cast<uint32_t*>(zsize_ar.data());
      if(zsize > cbound) {
        Rcpp::Rcerr << "Malformed compress block: too large" << std::endl;
        return false;
      }
      myFile.read(zblock.data(), zsize);
      if(myFile.fail()) {
        Rcpp::Rcerr << "File read fail" << std::endl;
        return false;
      }
      uint64_t block_size = decompFun(block.data(), BLOCKSIZE, zblock.data(), zsize);
      if(block_size == 0) {
        Rcpp::Rcerr << "Malformed compress block: decompression error" << std::endl;
        return false;
      }
    }
    if(myFile.peek() != EOF) {
      Rcpp::Rcerr << "End of file not reached" << std::endl;
      return false;
    }
    return true;
  }
};

struct ZSTD_DStream_wrapper {
  ZSTD_DStream* zds;
  ZSTD_DStream_wrapper() {
    zds = ZSTD_createDStream();
    ZSTD_initDStream(zds);
  }
  ~ZSTD_DStream_wrapper() {
    ZSTD_freeDStream(zds);
  }
};

inline bool ZSTD_decompressStream_count(ZSTD_DStream* zds, ZSTD_outBuffer * zout, ZSTD_inBuffer * zin, uint64_t & total) {
  uint64_t temp = zout->pos;
  size_t ret = ZSTD_decompressStream(zds, zout, zin);
  total += zout->pos - temp;
  if(ZSTD_isError(ret)) {
    return true;
  } else {
    return false;
  }
}

bool inspect_stream_zstd(std::ifstream & myFile, uint64_t decompressed_size) {
  size_t blocksize = ZSTD_DStreamOutSize();
  size_t insize = ZSTD_DStreamInSize();
  ZSTD_DStream_wrapper zx = ZSTD_DStream_wrapper();
  ZSTD_DStream* zds = zx.zds;
  ZSTD_inBuffer zin;
  ZSTD_outBuffer zout;
  std::vector<unsigned char> outblock(blocksize);
  std::vector<unsigned char> inblock(insize);
  zout.dst = outblock.data();
  zout.size = blocksize;
  zout.pos = 0;
  zin.src = inblock.data();
  zin.size = 0;
  zin.pos = 0;
  uint64_t bytes_decompressed = 0;
  while(bytes_decompressed < decompressed_size) {
    if(zin.pos < zin.size) {
      if(ZSTD_decompressStream_count(zds, &zout, &zin, bytes_decompressed)) {
        Rcpp::Rcerr << "Malformed compress data: decompression error" << std::endl;
        return false;
      }
      zout.pos = 0;
    } else if(! myFile.eof()) {
      myFile.read(reinterpret_cast<char*>(inblock.data()), inblock.size());
      if(myFile.fail() && !myFile.eof()) {
        Rcpp::Rcerr << "File read fail" << std::endl;
        return false;
      }
      size_t bytes_read = myFile.gcount();
      if(bytes_read == 0) continue; // EOF
      zin.pos = 0;
      zin.size = bytes_read;
      if(ZSTD_decompressStream_count(zds, &zout, &zin, bytes_decompressed)) {
        Rcpp::Rcerr << "Malformed compress data: decompression error" << std::endl;
        return false;
      }
      zout.pos = 0;
    } else {
      if(ZSTD_decompressStream_count(zds, &zout, &zin, bytes_decompressed)) {
        Rcpp::Rcerr << "Malformed compress data: decompression error" << std::endl;
        return false;
      }
      if(zout.pos == 0) break; // no more data
    }
  }
  if(bytes_decompressed != decompressed_size) {
    Rcpp::Rcerr << "Decompressed data size does not match recorded data size" << std::endl;
    return false;
  }
  return true;
}
