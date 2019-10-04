/* qs - Quick Serialization of R Objects
 Copyright (C) 2019-present Travers Ching

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <https://www.gnu.org/licenses/>.

You can contact the author at:
https://github.com/traversc/qs
*/

#ifndef QS_COMMON_H
#define QS_COMMON_H

#include <Rcpp.h>

#include <fstream>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>
#include <memory>
#include <array>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <string>
#include <vector>
#include <climits>
#include <cstdint>

// platform specific headers
#ifdef _WIN32
#define TRUE WINTRUE // TRUE is defined in one of these headers as 1; conflicts with #define in R headers
#include <sys/stat.h> // _S_IWRITE
#include <Fileapi.h>
#include <WinDef.h>
#include <Winbase.h>
#include <Handleapi.h>
#undef TRUE
#else
#include <sys/mman.h> // mmap
#endif

#include <atomic>
#include <thread>

#include <R.h>
#include <Rinternals.h>
#include <Rversion.h>

#include "RApiSerializeAPI.h"
#include "zstd.h"
#include "lz4.h"
#include "lz4hc.h"
#include "BLOSC/shuffle_routines.h"
#include "BLOSC/unshuffle_routines.h"

#include "xxhash/xxhash.c" // static linking
#include <R_ext/Rdynload.h>

// do not include by default due to CRAN policies
// import based on configure script
// #ifdef USE_R_CONNECTION
// #define class aclass
// #define private aprivate
// #include <R_ext/Connections.h>
// #undef class
// #undef private
// 
// #if defined(R_VERSION) && R_VERSION >= R_Version(3, 3, 0)
// Rconnection r_get_connection(SEXP con) {
//   return R_GetConnection(con);
// }
// #else
// Rconnection r_get_connection(SEXP con) {
//   if (!Rf_inherits(con, "connection"))
//     Rcpp::stop("invalid connection");
//   return getConnection(Rf_asInteger(con));
// }
// #endif
// #endif

using namespace Rcpp;

////////////////////////////////////////////////////////////////
// alt rep string class
////////////////////////////////////////////////////////////////
#if R_VERSION >= R_Version(3, 5, 0)
#define ALTREP_SUPPORTED

// load alt-rep header -- see altrepisode package by Romain Francois
#if R_VERSION < R_Version(3, 6, 0)
#define class klass
extern "C" {
#include <R_ext/Altrep.h>
}
#undef class
#else
#include <R_ext/Altrep.h>
#endif


struct stdvec_data {
  std::vector<std::string> strings;
  std::vector<uint8_t> encodings;
  R_xlen_t vec_size; 
  stdvec_data(uint64_t N) {
    strings = std::vector<std::string>(N);
    encodings = std::vector<uint8_t>(N);
    vec_size = N;
  }
};

// instead of defining a set of free functions, we structure them
// together in a struct
struct stdvec_string {
  static R_altrep_class_t class_t;
  static SEXP Make(stdvec_data* data, bool owner){
    SEXP xp = PROTECT(R_MakeExternalPtr(data, R_NilValue, R_NilValue));
    if (owner) {
      R_RegisterCFinalizerEx(xp, stdvec_string::Finalize, TRUE);
    }
    SEXP res = R_new_altrep(class_t, xp, R_NilValue);
    UNPROTECT(1);
    return res;
  }
  
  // finalizer for the external pointer
  static void Finalize(SEXP xp){
    delete static_cast<stdvec_data*>(R_ExternalPtrAddr(xp));
  }
  
  // get the std::vector<string>* from the altrep object `x`
  static stdvec_data* Ptr(SEXP vec) {
    return static_cast<stdvec_data*>(R_ExternalPtrAddr(R_altrep_data1(vec)));
  }
  
  // same, but as a reference, for convenience
  static stdvec_data& Get(SEXP vec) {
    return *static_cast<stdvec_data*>(R_ExternalPtrAddr(R_altrep_data1(vec)));
  }
  
  // ALTREP methods -------------------
  
  // The length of the object
  static R_xlen_t Length(SEXP vec){
    return Get(vec).vec_size;
  }
  
  // What gets printed when .Internal(inspect()) is used
  static Rboolean Inspect(SEXP x, int pre, int deep, int pvec, void (*inspect_subtree)(SEXP, int, int, int)){
    Rprintf("qs alt-rep stdvec_string (len=%d, ptr=%p)\n", Length(x), Ptr(x));
    return TRUE;
  }
  
  // ALTVEC methods ------------------
  static SEXP Materialize(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if (data2 != R_NilValue) {
      return data2;
    }
    R_xlen_t n = Length(vec);
    data2 = PROTECT(Rf_allocVector(STRSXP, n));
    
    auto data1 = Get(vec);
    for (R_xlen_t i = 0; i < n; i++) {
      switch(data1.encodings[i]) {
      case 1:
        SET_STRING_ELT(data2, i, Rf_mkCharLenCE(data1.strings[i].data(), data1.strings[i].size(), CE_NATIVE) );
        break;
      case 2:
        SET_STRING_ELT(data2, i, Rf_mkCharLenCE(data1.strings[i].data(), data1.strings[i].size(), CE_UTF8) );
        break;
      case 3:
        SET_STRING_ELT(data2, i, Rf_mkCharLenCE(data1.strings[i].data(), data1.strings[i].size(), CE_LATIN1) );
        break;
      case 4:
        SET_STRING_ELT(data2, i, Rf_mkCharLenCE(data1.strings[i].data(), data1.strings[i].size(), CE_BYTES) );
        break;
      default:
        SET_STRING_ELT(data2, i, NA_STRING);
      break;
      }
    }
    
    // free up some memory -- shrink to fit is a non-binding request
    data1.encodings.resize(0);
    data1.encodings.shrink_to_fit();
    data1.strings.resize(0);
    data1.strings.shrink_to_fit();
    
    R_set_altrep_data2(vec, data2);
    UNPROTECT(1);
    return data2;
  }
  
  // The start of the data, i.e. the underlying double* array from the std::vector<double>
  // This is guaranteed to never allocate (in the R sense)
  static const void* Dataptr_or_null(SEXP vec) {
    SEXP data2 = R_altrep_data2(vec);
    if (data2 == R_NilValue) return nullptr;
    return STDVEC_DATAPTR(data2);
  }
  
  // same in this case, writeable is ignored
  static void* Dataptr(SEXP vec, Rboolean writeable) {
    return STDVEC_DATAPTR(Materialize(vec));
  }
  
  
  // ALTSTRING methods -----------------
  // the element at the index `i`
  // this does not do bounds checking because that's expensive, so
  // the caller must take care of that
  static SEXP string_Elt(SEXP vec, R_xlen_t i){
    SEXP data2 = Materialize(vec);
    return STRING_ELT(data2, i);
    // switch(data1.encodings[i]) {
    // case 1:
    //   return Rf_mkCharLenCE(data1.strings[i].data(), data1.strings[i].size(), CE_NATIVE);
    // case 2:
    //   return Rf_mkCharLenCE(data1.strings[i].data(), data1.strings[i].size(), CE_UTF8);
    // case 3:
    //   return Rf_mkCharLenCE(data1.strings[i].data(), data1.strings[i].size(), CE_LATIN1);
    // case 4:
    //   return Rf_mkCharLenCE(data1.strings[i].data(), data1.strings[i].size(), CE_BYTES);
    // default:
    //   return NA_STRING;
    // break;
    // }
  }
  
  static void string_Set_elt(SEXP vec, R_xlen_t i, SEXP new_val) {
    SEXP data2 = Materialize(vec);
    SET_STRING_ELT(data2, i, new_val);
  }
  
  // -------- initialize the altrep class with the methods above
  
  static void Init(DllInfo* dll){
    class_t = R_make_altstring_class("stdvec_string", "altrepisode", dll);
    
    // altrep
    R_set_altrep_Length_method(class_t, Length);
    R_set_altrep_Inspect_method(class_t, Inspect);
    
    // altvec
    R_set_altvec_Dataptr_method(class_t, Dataptr);
    R_set_altvec_Dataptr_or_null_method(class_t, Dataptr_or_null);
    
    // altstring
    R_set_altstring_Elt_method(class_t, string_Elt);
    R_set_altstring_Set_elt_method(class_t, string_Set_elt);
  }
  
};

// static initialization of stdvec_double::class_t
R_altrep_class_t stdvec_string::class_t;

// Called the package is loaded (needs Rcpp 0.12.18.3)
// [[Rcpp::init]]
void init_stdvec_double(DllInfo* dll){
  stdvec_string::Init(dll);
}

#else
// [[Rcpp::init]]
void init_stdvec_double(DllInfo* dll){
  // do nothing -- need a dummy function becasue of Rcpp attributes
}
#endif
////////////////////////////////////////////////////////////////
// common utility functions and constants
////////////////////////////////////////////////////////////////

// endian function defined in qs_functions.cpp
bool is_big_endian();

// https://stackoverflow.com/a/36835959/2723734
inline constexpr unsigned char operator "" _u8(unsigned long long arg) noexcept {
  return static_cast<uint8_t>(arg);
}

static constexpr uint64_t BLOCKRESERVE = 64ULL;
static constexpr uint32_t NA_STRING_LENGTH = 4294967295UL; // 2^32-1 -- length used to signify NA value; note maximum string size is defined by `int` in mkCharLen, so this value is safe
static constexpr uint64_t MIN_SHUFFLE_ELEMENTS = 4ULL;
static constexpr uint64_t BLOCKSIZE = 524288ULL;
static constexpr uint64_t MAX_SAFE_INTEGER = 9007199254740991ULL; // 2^53-1 -- the largest integer that can be "safely" represented as a double ~ (about 9000 terabytes)

static const std::array<uint8_t,4> magic_bits = {0x0B,0x0E,0x0A,0x0C};
static const std::array<uint8_t,4> empty_bits = {0,0,0,0};

static constexpr uint8_t list_header_5 = 0x20_u8; 
static constexpr uint8_t list_header_8 = 0x01_u8;
static constexpr uint8_t list_header_16 = 0x02_u8;
static constexpr uint8_t list_header_32 = 0x03_u8;
static constexpr uint8_t list_header_64 = 0x04_u8;

static constexpr uint8_t numeric_header_5 = 0x40_u8; 
static constexpr uint8_t numeric_header_8 = 0x05_u8;
static constexpr uint8_t numeric_header_16 = 0x06_u8;
static constexpr uint8_t numeric_header_32 = 0x07_u8;
static constexpr uint8_t numeric_header_64 = 0x08_u8;

static constexpr uint8_t integer_header_5 = 0x60_u8; 
static constexpr uint8_t integer_header_8 = 0x09_u8;
static constexpr uint8_t integer_header_16 = 0x0A_u8;
static constexpr uint8_t integer_header_32 = 0x0B_u8;
static constexpr uint8_t integer_header_64 = 0x0C_u8;

static constexpr uint8_t logical_header_5 = 0x80_u8; 
static constexpr uint8_t logical_header_8 = 0x0D_u8;
static constexpr uint8_t logical_header_16 = 0x0E_u8;
static constexpr uint8_t logical_header_32 = 0x0F_u8;
static constexpr uint8_t logical_header_64 = 0x10_u8;

static constexpr uint8_t raw_header_32 = 0x17_u8;
static constexpr uint8_t raw_header_64 = 0x18_u8;

static constexpr uint8_t null_header = 0x00_u8; 

static constexpr uint8_t character_header_5 = 0xA0_u8; 
static constexpr uint8_t character_header_8 = 0x11_u8;
static constexpr uint8_t character_header_16 = 0x12_u8;
static constexpr uint8_t character_header_32 = 0x13_u8;
static constexpr uint8_t character_header_64 = 0x14_u8;

static constexpr uint8_t string_header_NA = 0x0F_u8;
static constexpr uint8_t string_header_5 = 0x20_u8; 
static constexpr uint8_t string_header_8 = 0x01_u8;
static constexpr uint8_t string_header_16 = 0x02_u8;
static constexpr uint8_t string_header_32 = 0x03_u8;

static constexpr uint8_t string_enc_native = 0x00_u8; 
static constexpr uint8_t string_enc_utf8 = 0x40_u8;
static constexpr uint8_t string_enc_latin1 = 0x80_u8;
static constexpr uint8_t string_enc_bytes = 0xC0_u8;

static constexpr uint8_t attribute_header_5 = 0xE0_u8;
static constexpr uint8_t attribute_header_8 = 0x1E_u8;
static constexpr uint8_t attribute_header_32 = 0x1F_u8;

static constexpr uint8_t complex_header_32 = 0x15_u8;
static constexpr uint8_t complex_header_64 = 0x16_u8;

static constexpr uint8_t nstype_header_32 = 0x19_u8;
static constexpr uint8_t nstype_header_64 = 0x1A_u8;

static const std::set<SEXPTYPE> stypes = {REALSXP, INTSXP, LGLSXP, STRSXP, CHARSXP, NILSXP, VECSXP, CPLXSXP, RAWSXP};


///////////////////////////////////////////////////////
// There are three types of output input streams -- std::ifstream/ofstream, file descriptor, windows handle
// these methods are overloaded and normalized so we can use a common template interface
// to do: R connections

inline uint64_t read_check(std::ifstream & con, char * const ptr, const uint64_t count) {
  con.read(ptr, count);
  uint64_t return_value = con.gcount();
  if(return_value != count) {
    throw std::runtime_error("error reading from connection (not enough bytes read)");
  }
  return return_value;
}
inline uint64_t read_allow(std::ifstream & con, char * const ptr, const uint64_t count) {
  con.read(ptr, count);
  return con.gcount();
}
inline uint64_t write_check(std::ofstream & con, const char * const ptr, const uint64_t count) {
  // (void)check_size; // avoid unused variable warning
  con.write(ptr, count);
  return count;
}

inline bool isSeekable(std::ifstream & myFile) {
  return true;
}

///////////////////////////////////////////////////////
// helper functions for Rconnection

// #ifdef USE_R_CONNECTION
// // rconnection read and write with error checking
// inline uint64_t read_check(char * ptr, uint64_t count, Rconnection con, bool check_size=true) {
//   uint64_t return_value = R_ReadConnection(con, ptr, count);
//   if(check_size) {
//     if(return_value != count) {
//       throw std::runtime_error("error reading from connection (wrong size)");
//     }
//   }
//   return return_value;
// }
// // rconnection read and write with error checking
// inline uint64_t write_check(char * ptr, uint64_t count, Rconnection con, bool check_size=true) {
//   uint64_t return_value = R_WriteConnection(con, ptr, count);
//   if(check_size) {
//     if(return_value != count) {
//       throw std::runtime_error("error writing to connection (wrong size)");
//     }
//   }
//   return return_value;
// }
// inline void writeSizeToCon8(Rconnection con, uint64_t x) {
//   uint64_t x_temp = static_cast<uint64_t>(x); 
//   write_check(reinterpret_cast<char*>(&x_temp),8, con);
// }
// inline void writeSizeToCon4(Rconnection con, uint64_t x) {
//   uint32_t x_temp = static_cast<uint32_t>(x); 
//   write_check(reinterpret_cast<char*>(&x_temp),4, con);
//   }
// inline uint32_t readSize4(Rconnection con) {
//   std::array<char,4> a = {0,0,0,0};
//   read_check(a.data(),4, con);
//   return *reinterpret_cast<uint32_t*>(a.data());
// }
// inline uint64_t readSize8(Rconnection con) {
//   std::array<char,8> a = {0,0,0,0,0,0,0,0};
//   read_check(a.data(),8, con);
//   return *reinterpret_cast<uint64_t*>(a.data());
// }
// #endif

///////////////////////////////////////////////////////
// helper functions for file descriptors

// low level interface
// no destructor -- left up to user
// whether fd is read or write is left up to user -- dont use both
// to do: evaluate whether wrapping in a FILE* or std::ostream is more efficient?
static constexpr uint64_t FD_BUFFER_SIZE = 524288; // 2^17
struct fd_wrapper {
  int fd;
  uint64_t bytes_processed;
  uint64_t buffered_bytes;
  uint64_t buffer_offset;
  std::array<char, FD_BUFFER_SIZE> buffer;
  fd_wrapper(int fd) : fd(fd), bytes_processed(0), buffered_bytes(0), buffer_offset(0) {
    if(ferror()) throw std::runtime_error("file descriptor is not valid");
  }
  int ferror() {
#ifdef _WIN32
    return errno == EBADF;
#else
    return fcntl(fd, F_GETFD) == -1 || errno == EBADF;
#endif
  }
  inline uint64_t read(char * const ptr, const uint64_t count) {
    uint64_t remaining_bytes = count;
    while(remaining_bytes > buffered_bytes - buffer_offset) {
      std::memcpy(ptr + count - remaining_bytes, buffer.data() + buffer_offset, buffered_bytes - buffer_offset);
      remaining_bytes -= buffered_bytes - buffer_offset;
      ssize_t temp = ::read(fd, buffer.data(), FD_BUFFER_SIZE);
      if(temp < 0) throw std::runtime_error("error reading fd");
      bytes_processed += temp;
      buffered_bytes = temp;
      buffer_offset = 0;
      if(buffered_bytes == 0) {
        return count - remaining_bytes; // if we reached this point, there wasn't enough data left in file
      }
    }
    // remaining_bytes <= buffered_bytes - buffer_offset
    std::memcpy(ptr + count - remaining_bytes, buffer.data() + buffer_offset, remaining_bytes);
    buffer_offset += remaining_bytes;
    // remaining_bytes = 0; -- no longer necessary to track
    return count;
  }
  // buffer_offset is not uesd
  inline uint64_t write(const char * const ptr, const uint64_t count) {
    uint64_t remaining_bytes = count;
    uint64_t ptr_offset = 0;
    // std::cout << "ptr: " << static_cast<void*>(ptr) << ", count: " << count << std::endl;
    while(remaining_bytes > 0) {
      if(remaining_bytes >= FD_BUFFER_SIZE - buffered_bytes) {
        // std::cout << "A: rb " << remaining_bytes << ", bb " << buffered_bytes << ", po " << ptr_offset << std::endl;
        uint64_t bytes_to_write = FD_BUFFER_SIZE - buffered_bytes;
        if(buffered_bytes == 0) {
          // skip memcpy since nothing in buffer
          ssize_t temp = ::write(fd, ptr + ptr_offset, FD_BUFFER_SIZE);
          if(temp < 0) throw std::runtime_error("error writing to fd");
        } else {
          std::memcpy(buffer.data() + buffered_bytes, ptr + ptr_offset, bytes_to_write);
          ssize_t temp = ::write(fd, buffer.data(), FD_BUFFER_SIZE);
          if(temp < 0) throw std::runtime_error("error writing to fd");
        }
        remaining_bytes -= bytes_to_write;
        buffered_bytes = 0;
        ptr_offset += bytes_to_write;
      } else { // remaining_bytes < FD_BUFFER_SIZE - buffered_bytes
        // std::cout << "B: rb " << remaining_bytes << ", bb " << buffered_bytes << ", po " << ptr_offset << std::endl;
        std::memcpy(buffer.data() + buffered_bytes, ptr + ptr_offset, remaining_bytes);
        buffered_bytes += remaining_bytes;
        remaining_bytes = 0;
        // ptr_offset += remaining_bytes
      }
    }
    bytes_processed += count;
    return count;
  }
  inline void flush() {
    ssize_t temp = ::write(fd, buffer.data(), buffered_bytes);
    if(temp < 0) throw std::runtime_error("error writing to fd");
    buffered_bytes = 0;
  }
  fd_wrapper * seekp(uint64_t pos) {
    throw std::runtime_error("file descriptor is not seekable");
    return nullptr;
  }
  fd_wrapper * seekg(uint64_t pos) {
    throw std::runtime_error("file descriptor is not seekable");
    return nullptr;
  }
};

inline uint64_t read_check(fd_wrapper & con, char * const ptr, const uint64_t count) {
  uint64_t return_value = con.read(ptr, count);
  if (con.ferror()) {
    throw std::runtime_error("error writing to connection");
  }
  if(return_value != count) {
    throw std::runtime_error("error reading from connection (not enough bytes read)");
  }
  return return_value;
}
inline uint64_t read_allow(fd_wrapper & con, char * const ptr, const uint64_t count) {
  uint64_t return_value = con.read(ptr, count);
  if (con.ferror()) {
    throw std::runtime_error("error writing to connection");
  }
  return return_value;
}
inline uint64_t write_check(fd_wrapper & con, const char * const ptr, const uint64_t count) {
  uint64_t return_value = con.write(ptr, count);
  if (con.ferror()) {
    throw std::runtime_error("error writing to connection");
  }
  // if(check_size) {
  //   if(return_value != count) {
  //     throw std::runtime_error("error writing to connection (not enough bytes written)");
  //   }
  // }
  return return_value;
}
inline bool isSeekable(fd_wrapper & myFile) {
  return false;
}

///////////////////////////////////////////////////////
// helper functions for reading/writing to memory

// Instance should only be used for reading or writing, not both
struct mem_wrapper {
  char * start;
  uint64_t available_bytes;
  uint64_t bytes_processed;
  mem_wrapper(void * s, uint64_t ab) : start(static_cast<char*>(s)), available_bytes(ab), bytes_processed(0) {}
  inline uint64_t read(char * const ptr, uint64_t count) {
    if(count + bytes_processed > available_bytes) {
      count = available_bytes - bytes_processed;
    }
    std::memcpy(ptr, start + bytes_processed, count);
    bytes_processed += count;
    return count;
  }
  inline uint64_t write(const char * const ptr, uint64_t count) {
    if(count + bytes_processed > available_bytes) {
      count = available_bytes - bytes_processed;
    }
    std::memcpy(start + bytes_processed, ptr, count);
    bytes_processed += count;
    return count;
  }
  inline void writeDirect(const char * const ptr, uint64_t count, uint64_t offset) {
    std::memcpy(start + offset, ptr, count);
  }
  mem_wrapper * seekp(uint64_t pos) {
    throw std::runtime_error("not seekable");
    return nullptr;
  }
  mem_wrapper * seekg(uint64_t pos) {
    throw std::runtime_error("not seekable");
    return nullptr;
  }
};

inline uint64_t read_check(mem_wrapper & con, char * const ptr, const uint64_t count) {
  uint64_t return_value = con.read(ptr, count);
  if(return_value != count) {
    throw std::runtime_error("error reading from connection (not enough bytes read)");
  }
  return return_value;
}
inline uint64_t read_allow(mem_wrapper & con, char * const ptr, const uint64_t count) {
  uint64_t return_value = con.read(ptr, count);
  return return_value;
}
inline uint64_t write_check(mem_wrapper & con, const char * const ptr, uint64_t const count) {
  uint64_t return_value = con.write(ptr, count);
  // if(check_size) {
  //   if(return_value != count) {
  //     throw std::runtime_error("error writing to connection (not enough bytes written)");
  //   }
  // }
  return return_value;
}

inline bool isSeekable(mem_wrapper & myFile) {
  return false;
}

///////////////////////////////////////////////////////
// helper functions for writing to std::vector
// This is only used for writing
// since we don't know the serialized size ahead of time, we need a resizable memory buffer
// reading -- use mem_wrapper

struct vec_wrapper {
  std::vector<char> buffer;
  uint64_t bytes_processed = 0;
  vec_wrapper() : buffer(std::vector<char>(BLOCKSIZE)) {}
  inline uint64_t write(const char * const ptr, uint64_t count) {
    if(count + bytes_processed > buffer.size()) {
      uint64_t new_buffer_size = buffer.size() * 3/2;
      while(new_buffer_size < count*3/2 + bytes_processed) {
        new_buffer_size = new_buffer_size * 3/2;
      }
      buffer.resize(new_buffer_size);
    }
    std::memcpy(buffer.data() + bytes_processed, ptr, count);
    bytes_processed += count;
    // std::cout << static_cast<void*>(buffer.data()) << " " << count << std::endl;
    return count;
  }
  inline void writeDirect(const char * const ptr, uint64_t count, uint64_t offset) {
    std::memcpy(buffer.data() + offset, ptr, count);
  }
  vec_wrapper * seekp(uint64_t pos) {
    throw std::runtime_error("not seekable");
    return nullptr;
  }
  vec_wrapper * seekg(uint64_t pos) {
    throw std::runtime_error("not seekable");
    return nullptr;
  }
  void shrink() {
    buffer.resize(bytes_processed);
  }
};

inline uint64_t write_check(vec_wrapper & con, const char * const ptr, uint64_t count) {
  uint64_t return_value = con.write(ptr, count);
  // if(check_size) {
  //   if(return_value != count) {
  //     throw std::runtime_error("error writing to connection (not enough bytes written)");
  //   }
  // }
  return return_value;
}

inline bool isSeekable(vec_wrapper & myFile) {
  return false;
}


///////////////////////////////////////////////////////
// windows handle wrapper

#ifdef _WIN32
struct handle_wrapper {
  HANDLE h;
  uint64_t bytes_processed;
  handle_wrapper(HANDLE h) : h(h), bytes_processed(0) {}
  inline DWORD read(char * const ptr, const uint64_t count) {
    DWORD bytes_read;
    bool ret = ReadFile(h, ptr, count, &bytes_read, NULL);
    if(!ret) throw std::runtime_error("error reading from handle");
    return bytes_read;
  }
  inline DWORD write(const char * const ptr, const uint64_t count) {
    DWORD bytes_written;
    bool ret = WriteFile(h, ptr, count, &bytes_written, NULL);
    if(!ret) throw std::runtime_error("error writing to handle");
    bytes_processed += bytes_written;
    return bytes_written;
  }
  handle_wrapper * seekp(uint64_t pos) {
    throw std::runtime_error("file descriptor is not seekable");
    return nullptr;
  }
  handle_wrapper * seekg(uint64_t pos) {
    throw std::runtime_error("file descriptor is not seekable");
    return nullptr;
  }
};

inline uint64_t read_check(handle_wrapper & con, char * const ptr, const uint64_t count) {
  uint64_t return_value = con.read(ptr, count);
  if(return_value != count) {
    throw std::runtime_error("error writing to handle (not enough bytes read)");
  }
  return return_value;
}
inline uint64_t read_allow(handle_wrapper & con, char * const ptr, const uint64_t count) {
  uint64_t return_value = con.read(ptr, count);
  return return_value;
}
inline uint64_t write_check(handle_wrapper & con, const char * const ptr, const uint64_t count) {
  uint64_t return_value = con.write(ptr, count);
  // if(check_size) {
  //   if(return_value != count) {
  //     throw std::runtime_error("error writing to handle (not enough bytes written)");
  //   }
  // }
  return return_value;
}



inline bool isSeekable(handle_wrapper & myFile) {
  return false;
}
#endif

///////////////////////////////////////////////////////
// templated classes for reading and writing integer sizes

template <class stream_writer>
inline void writeSize8(stream_writer & myFile, const uint64_t x) {
  auto x_temp = static_cast<uint64_t>(x); 
  write_check(myFile, reinterpret_cast<char*>(&x_temp),8);
}
template <class stream_writer>
inline void writeSize4(stream_writer & myFile, const uint64_t x) {
  auto x_temp = static_cast<uint32_t>(x); 
  write_check(myFile, reinterpret_cast<char*>(&x_temp),4);
}

template <class stream_writer>
inline uint32_t readSize4(stream_writer & myFile) {
  std::array<char,4> a;
  read_check(myFile, a.data(),4);
  return *reinterpret_cast<uint32_t*>(a.data());
}

template <class stream_writer>
inline uint64_t readSize8(stream_writer & myFile) {
  std::array<char,8> a;
  read_check(myFile, a.data(),8);
  return *reinterpret_cast<uint64_t*>(a.data());
}


///////////////////////////////////////////////////////

// unaligned cast to <POD>
template<typename POD>
inline POD unaligned_cast(const char * const data, const uint64_t offset) {
  POD y;
  std::memcpy(&y, data + offset, sizeof(y));
  return y;
}

// maximum value is 7, reserve bit shared with shuffle bit
// if we need more slots we will have to use other reserve bits
enum class compalg : uint8_t {
  zstd = 0, lz4 = 1, lz4hc = 2, zstd_stream = 3, uncompressed = 4
};
// qs reserve header details
// reserve[0] unused
// reserve[1] (low byte) 1 = hash of serialized object written to last 4 bytes of file -- before 16.3, no hash check was performed
// reserve[1] (high byte) unused
// reserve[2] (low byte) shuffle control: 0x01 = logical shuffle, 0x02 = integer shuffle, 0x04 = double shuffle
// reserve[2] (high byte) algorithm: 0x01 = lz4, 0x00 = zstd, 0x02 = "lz4hc", 0x03 = zstd_stream
// reserve[3] endian: 1 = big endian, 0 = little endian
struct QsMetadata {
  uint64_t clength; // compressed length -- for comparing bytes_read / blocks_read with recorded # ..
  bool check_hash;
  uint8_t endian;
  uint8_t compress_algorithm;
  int compress_level;
  bool lgl_shuffle;
  bool int_shuffle;
  bool real_shuffle;
  bool cplx_shuffle;

  //constructor from qsave
  QsMetadata(const std::string & preset, const std::string & algorithm, const int compress_level, int shuffle_control, const bool check_hash) : 
    clength(0), check_hash(check_hash), endian(is_big_endian()) {
    if(preset == "fast") {
      compress_algorithm = static_cast<uint8_t>(compalg::lz4);
      this->compress_level = 100;
      shuffle_control = 0;
    } else if(preset == "balanced") {
      compress_algorithm = static_cast<uint8_t>(compalg::lz4);
      this->compress_level = 1;
      shuffle_control = 15;
    } else if(preset == "high") {
      compress_algorithm = static_cast<uint8_t>(compalg::zstd);
      this->compress_level = 4;
      shuffle_control = 15;
    } else if(preset == "archive") {
      compress_algorithm = static_cast<uint8_t>(compalg::zstd_stream);
      this->compress_level = 14;
      shuffle_control = 15;
    } else if(preset == "uncompressed") {
      compress_algorithm = static_cast<uint8_t>(compalg::uncompressed);
      this->compress_level = 0;
      shuffle_control = 0;
    } else if(preset == "custom") {
      if(algorithm == "zstd") {
        compress_algorithm = static_cast<uint8_t>(compalg::zstd);
        this->compress_level = compress_level;
        if(compress_level > 22 || compress_level < -50) throw std::runtime_error("zstd compress_level must be an integer between -50 and 22");
      } else if(algorithm == "zstd_stream") {
        compress_algorithm = static_cast<uint8_t>(compalg::zstd_stream);
        this->compress_level = compress_level;
        if(compress_level > 22 || compress_level < -50) throw std::runtime_error("zstd compress_level must be an integer between -50 and 22");
      } else if(algorithm == "lz4") {
        compress_algorithm = static_cast<uint8_t>(compalg::lz4);
        this->compress_level = compress_level;
        if(compress_level < 1) throw std::runtime_error("lz4 compress_level must be an integer greater than 1");
      } else if(algorithm == "lz4hc") {
        compress_algorithm = static_cast<uint8_t>(compalg::lz4hc);
        this->compress_level = compress_level;
        if(compress_level < 1 || compress_level > 12) throw std::runtime_error("lz4hc compress_level must be an integer between 1 and 12");
      } else if(algorithm == "uncompressed") {
        compress_algorithm = static_cast<uint8_t>(compalg::uncompressed);
        this->compress_level = 0;
      } else {
        throw std::runtime_error("algorithm must be one of zstd, lz4, lz4hc or zstd_stream");
      }
    } else {
      throw std::runtime_error("preset must be one of fast, balanced (default), high, archive or custom");
    }
    if(shuffle_control < 0 || shuffle_control > 15) throw std::runtime_error("shuffle_control must be an integer between 0 and 15");
    lgl_shuffle = shuffle_control & 0x01;
    int_shuffle = shuffle_control & 0x02;
    real_shuffle = shuffle_control & 0x04;
    cplx_shuffle = shuffle_control & 0x08;
  }
  
  // 0x0B0E0A0C
  static bool checkMagicNumber(const std::array<uint8_t, 4> & reserve_bits) {
    if(reserve_bits[0] != magic_bits[0]) return false;
    if(reserve_bits[1] != magic_bits[1]) return false;
    if(reserve_bits[2] != magic_bits[2]) return false;
    if(reserve_bits[3] != magic_bits[3]) return false;
    return true;
  }
  
  QsMetadata(const uint64_t clength,
             const bool check_hash,
             const uint8_t endian,
             const uint8_t compress_algorithm,
             const int compress_level,
             const bool lgl_shuffle,
             const bool int_shuffle,
             const bool real_shuffle,
             const bool cplx_shuffle) : 
    clength(clength), check_hash(check_hash), endian(endian), compress_algorithm(compress_algorithm), 
    compress_level(compress_level), lgl_shuffle(lgl_shuffle), int_shuffle(int_shuffle), 
    real_shuffle(real_shuffle), cplx_shuffle(cplx_shuffle) {}
  
  // constructor from q_read
  template <class stream_reader>
  static QsMetadata create(stream_reader & myFile) {
    std::array<uint8_t,4> reserve_bits;
    read_check(myFile, reinterpret_cast<char*>(reserve_bits.data()),4);
    // version 2
    if(reserve_bits[0] != 0) {
      std::array<uint8_t,4> reserve_bits2;
      if(!checkMagicNumber(reserve_bits)) throw std::runtime_error("QS format not detected");
      read_check(myFile, reinterpret_cast<char*>(reserve_bits2.data()),4); // empty reserve bits
      read_check(myFile, reinterpret_cast<char*>(reserve_bits.data()),4);
    }
    uint8_t sys_endian = is_big_endian() ? 0x01 : 0x00;
    if(reserve_bits[3] != sys_endian) throw std::runtime_error("Endian of system doesn't match file endian");
    uint8_t compress_algorithm = reserve_bits[2] >> 4;
    int compress_level = 1;
    bool lgl_shuffle = reserve_bits[2] & 0x01;
    bool int_shuffle = reserve_bits[2] & 0x02;
    bool real_shuffle = reserve_bits[2] & 0x04;
    bool cplx_shuffle = reserve_bits[2] & 0x08;
    bool check_hash = reserve_bits[1];
    uint8_t endian = reserve_bits[3];
    uint64_t clength = readSize8(myFile);
    return {clength,
            check_hash,
            endian,
            compress_algorithm,
            compress_level,
            lgl_shuffle,
            int_shuffle,
            real_shuffle,
            cplx_shuffle};
  }
  
  // version 2
  template <class stream_writer>
  void writeToFile(stream_writer & myFile) {
    write_check(myFile, reinterpret_cast<const char*>(magic_bits.data()), 4);
    write_check(myFile, reinterpret_cast<const char*>(empty_bits.data()),4);
    std::array<uint8_t,4> reserve_bits = {0,0,0,0};
    reserve_bits[1] = check_hash;
    reserve_bits[2] += compress_algorithm << 4;
    reserve_bits[3] = is_big_endian() ? 0x01 : 0x00;
    reserve_bits[2] += (lgl_shuffle) + (int_shuffle << 1) + (real_shuffle << 2) + (cplx_shuffle << 3);
    write_check(myFile, reinterpret_cast<char*>(reserve_bits.data()),4);
  }
};

// Normalize lz4/zstd function arguments so we can use function types
using compress_fun = size_t (*)(void*, size_t, const void*, size_t, int);
using decompress_fun = size_t (*)(void*, size_t, const void*, size_t);
using cbound_fun = size_t (*)(size_t);
using iserror_fun = unsigned (*)(size_t);

size_t LZ4_compressBound_fun(size_t srcSize) {
  return LZ4_compressBound(srcSize);
}

size_t LZ4_compress_fun( void* dst, size_t dstCapacity,
                         const void* src, size_t srcSize,
                         int compressionLevel) {
  return LZ4_compress_fast(reinterpret_cast<char*>(const_cast<void*>(src)), 
                           reinterpret_cast<char*>(const_cast<void*>(dst)),
                           static_cast<int>(srcSize), static_cast<int>(dstCapacity), compressionLevel);
}

size_t LZ4_compress_HC_fun( void* dst, size_t dstCapacity,
                            const void* src, size_t srcSize,
                            int compressionLevel) {
  return LZ4_compress_HC(reinterpret_cast<char*>(const_cast<void*>(src)), 
                         reinterpret_cast<char*>(const_cast<void*>(dst)),
                         static_cast<int>(srcSize), static_cast<int>(dstCapacity), compressionLevel);
}

size_t LZ4_decompress_fun( void* dst, size_t dstCapacity,
                           const void* src, size_t compressedSize) {
  int ret = LZ4_decompress_safe(reinterpret_cast<char*>(const_cast<void*>(src)), 
                                reinterpret_cast<char*>(const_cast<void*>(dst)),
                                static_cast<int>(compressedSize), static_cast<int>(dstCapacity));
  if(ret < 0) {
    return SIZE_MAX;
  } else {
    return ret;
  }
}

unsigned LZ4_isError_fun(size_t retval) {
  if(retval == SIZE_MAX) {
    return 1;
  } else {
    return 0;
  }
}


////////////////////////////////////////////////////////////////
// common utility functions for serialization
////////////////////////////////////////////////////////////////

template <class T>
void writeHeader_common(const SEXPTYPE object_type, const uint64_t length, T * const sobj) {
  switch(object_type) {
  case REALSXP:
    if(length < 32) {
      sobj->push_pod_noncontiguous( static_cast<uint8_t>(numeric_header_5 | static_cast<uint8_t>(length)) );
    } else if(length < 256) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&numeric_header_8), 1);
      sobj->push_pod_contiguous( static_cast<uint8_t>(length) );
    } else if(length < 65536) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&numeric_header_16), 1);
      sobj->push_pod_contiguous( static_cast<uint16_t>(length) );
    } else if(length < 4294967296) {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&numeric_header_32), 1);
      sobj->push_pod_contiguous( static_cast<uint32_t>(length) );
    } else {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&numeric_header_64), 1);
      sobj->push_pod_contiguous(length);
    }
    return;
  case VECSXP:
    if(length < 32) {
      sobj->push_pod_noncontiguous( static_cast<uint8_t>(list_header_5 | static_cast<uint8_t>(length)) );
    } else if(length < 256) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&list_header_8), 1);
      sobj->push_pod_contiguous(static_cast<uint8_t>(length) );
    } else if(length < 65536) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&list_header_16), 1);
      sobj->push_pod_contiguous(static_cast<uint16_t>(length) );
    } else if(length < 4294967296) {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&list_header_32), 1);
      sobj->push_pod_contiguous(static_cast<uint32_t>(length) );
    } else {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&list_header_64), 1);
      sobj->push_pod_contiguous(length);
    }
    return;
  case INTSXP:
    if(length < 32) {
      sobj->push_pod_noncontiguous( static_cast<uint8_t>(integer_header_5 | static_cast<uint8_t>(length)) );
    } else if(length < 256) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&integer_header_8), 1);
      sobj->push_pod_contiguous(static_cast<uint8_t>(length) );
    } else if(length < 65536) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&integer_header_16), 1);
      sobj->push_pod_contiguous(static_cast<uint16_t>(length) );
    } else if(length < 4294967296) {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&integer_header_32), 1);
      sobj->push_pod_contiguous(static_cast<uint32_t>(length) );
    } else {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&integer_header_64), 1);
      sobj->push_pod_contiguous(static_cast<uint64_t>(length) );
    }
    return;
  case LGLSXP:
    if(length < 32) {
      sobj->push_pod_noncontiguous( static_cast<uint8_t>(logical_header_5 | static_cast<uint8_t>(length)) );
    } else if(length < 256) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&logical_header_8), 1);
      sobj->push_pod_contiguous(static_cast<uint8_t>(length) );
    } else if(length < 65536) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&logical_header_16), 1);
      sobj->push_pod_contiguous(static_cast<uint16_t>(length) );
    } else if(length < 4294967296) {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&logical_header_32), 1);
      sobj->push_pod_contiguous(static_cast<uint32_t>(length) );
    } else {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&logical_header_64), 1);
      sobj->push_pod_contiguous(length);
    }
    return;
  case RAWSXP:
    if(length < 4294967296) {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&raw_header_32), 1);
      sobj->push_pod_contiguous(static_cast<uint32_t>(length) );
    } else {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&raw_header_64), 1);
      sobj->push_pod_contiguous(length);
    }
    return;
  case STRSXP:
    if(length < 32) {
      sobj->push_pod_noncontiguous( static_cast<uint8_t>(character_header_5 | static_cast<uint8_t>(length)) );
    } else if(length < 256) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&character_header_8), 1);
      sobj->push_pod_contiguous(static_cast<uint8_t>(length) );
    } else if(length < 65536) { 
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&character_header_16), 1);
      sobj->push_pod_contiguous(static_cast<uint16_t>(length) );
    } else if(length < 4294967296) {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&character_header_32), 1);
      sobj->push_pod_contiguous(static_cast<uint32_t>(length) );
    } else {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&character_header_64), 1);
      sobj->push_pod_contiguous(static_cast<uint64_t>(length) );
    }
    return;
  case CPLXSXP:
    if(length < 4294967296) {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&complex_header_32), 1);
      sobj->push_pod_contiguous(static_cast<uint32_t>(length) );
    } else {
      sobj->push_noncontiguous(reinterpret_cast<const char * const>(&complex_header_64), 1);
      sobj->push_pod_contiguous(length);
    }
    return;
  case NILSXP:
    sobj->push_noncontiguous(reinterpret_cast<const char * const>(&null_header), 1);
    return;
  default:
    throw std::runtime_error("something went wrong writing object header");  // should never reach here
  }
}
template <class T>
void writeAttributeHeader_common(uint64_t length, T * sobj) {
  if(length < 32) {
    sobj->push_pod_noncontiguous( static_cast<uint8_t>(attribute_header_5 | static_cast<uint8_t>(length)) );
  } else if(length < 256) {
    sobj->push_noncontiguous(reinterpret_cast<const char * const>(&attribute_header_8), 1);
    sobj->push_pod_contiguous(static_cast<uint8_t>(length) );
  } else {
    sobj->push_noncontiguous(reinterpret_cast<const char * const>(&attribute_header_32), 1);
    sobj->push_pod_contiguous(static_cast<uint32_t>(length) );
  }
}

template <class T>
void writeStringHeader_common(const uint64_t length, const cetype_t ce_enc, T * const sobj) {
  uint8_t enc;
  switch(ce_enc) {
  case CE_NATIVE:
    enc = string_enc_native; break;
  case CE_UTF8:
    enc = string_enc_utf8; break;
  case CE_LATIN1:
    enc = string_enc_latin1; break;
  case CE_BYTES:
    enc = string_enc_bytes; break;
  default:
    enc = string_enc_native;
  }
  if(length < 32) {
    sobj->push_pod_noncontiguous( static_cast<uint8_t>(string_header_5 | enc | static_cast<uint8_t>(length)) );
  } else if(length < 256) {
    sobj->push_pod_noncontiguous( static_cast<uint8_t>(string_header_8 | static_cast<uint8_t>(enc)) );
    sobj->push_pod_contiguous(static_cast<uint8_t>(length) );
  } else if(length < 65536) {
    sobj->push_pod_noncontiguous( static_cast<uint8_t>(string_header_16 | static_cast<uint8_t>(enc)) );
    sobj->push_pod_contiguous(static_cast<uint16_t>(length) );
  } else {
    sobj->push_pod_noncontiguous( static_cast<uint8_t>(string_header_32 | static_cast<uint8_t>(enc)) );
    sobj->push_pod_contiguous(static_cast<uint32_t>(length) );
  }
}

////////////////////////////////////////////////////////////////
// common utility functions for deserialization
////////////////////////////////////////////////////////////////
template <class stream_reader>
uint32_t validate_data(const QsMetadata & qm, stream_reader & myFile, const uint32_t recorded_hash, 
                       const uint32_t computed_hash, const uint64_t computed_length, const bool strict) {
  // destructively check EOF -- cannot putback data
  std::array<char,4> temp;
  uint64_t remaining_bytes = read_allow(myFile, temp.data(), 4);
  if(remaining_bytes != 0) {
    if(strict) {
      throw std::runtime_error("end of file not reached");
    } else {
      Rcerr << "Warning: end of file not reached, data may be corrupted";
    }
  }
  if((qm.clength != 0) && (computed_length != 0) && (computed_length != qm.clength)) {
    if(strict) {
      throw std::runtime_error("computed object length does not match recorded object length");
    } else {
      Rcerr << "Warning: computed object length does not match recorded object length, data may be corrupted";
    }
  }
  if(qm.check_hash) {
    if(computed_hash != recorded_hash) {
      if(strict) {
        throw std::runtime_error("Warning: hash checksum does not match (Recorded, Computed) (" + 
                                 std::to_string(recorded_hash) + "," + std::to_string(computed_hash) + "), data may be corrupted");
      } else {
        Rcerr << "Warning: hash checksum does not match (Recorded, Computed) (" << recorded_hash << "," << computed_hash << "), data may be corrupted" << std::endl;
      }
    }
    return recorded_hash;
  }
  return 0;
}

// R stack tracker using RAII
// protection handling using RAII should have no issues with longjmp
// ref: https://developer.r-project.org/Blog/public/2019/03/28/use-of-c---in-packages/
// "R restores the protection stack depth before taking a long jump,
// so if a C++ destructor includes say UNPROTECT(1) call to restore 
// the protection stack depth, it does not matter it is not executed, 
// because R will do that automatically."
// 
// There is also a limit of 10,000 on the protection stack.  
// Theoretically, we'd need to track it globally to be 100% error proof.  
// Realistically, it should never occur in this package as you'd need a list with depth 10,000.
// Ref: https://cran.r-project.org/doc/manuals/r-release/R-exts.html#Garbage-Collection
struct Protect_Tracker {
  unsigned int n = 0;
  Protect_Tracker() {}
  ~Protect_Tracker() {
    UNPROTECT(n);
  }
  void operator++(int) {
    n++;
  }
};

inline void readHeader_common(SEXPTYPE & object_type, uint64_t & r_array_len, uint64_t & data_offset, const char * const header) {
  uint8_t h5 = reinterpret_cast<const uint8_t *>(header)[data_offset] & 0xE0;
  switch(h5) {
  case numeric_header_5:
    r_array_len = *reinterpret_cast<const uint8_t*>(header+data_offset) & 0x1F ;
    data_offset += 1;
    object_type = REALSXP;
    return;
  case list_header_5:
    r_array_len = *reinterpret_cast<const uint8_t*>(header+data_offset) & 0x1F ;
    data_offset += 1;
    object_type = VECSXP;
    return;
  case integer_header_5:
    r_array_len = *reinterpret_cast<const uint8_t*>(header+data_offset) & 0x1F ;
    data_offset += 1;
    object_type = INTSXP;
    return;
  case logical_header_5:
    r_array_len = *reinterpret_cast<const uint8_t*>(header+data_offset) & 0x1F ;
    data_offset += 1;
    object_type = LGLSXP;
    return;
  case character_header_5:
    r_array_len = *reinterpret_cast<const uint8_t*>(header+data_offset) & 0x1F ;
    data_offset += 1;
    object_type = STRSXP;
    return;
  case attribute_header_5:
    r_array_len = *reinterpret_cast<const uint8_t*>(header+data_offset) & 0x1F ;
    data_offset += 1;
    object_type = ANYSXP;
    return;
  }
  uint8_t hd = reinterpret_cast<const uint8_t*>(header)[data_offset];
  switch(hd) {
  case numeric_header_8:
    r_array_len =  *reinterpret_cast<const uint8_t*>(header+data_offset+1) ;
    data_offset += 2;
    object_type = REALSXP;
    return;
  case numeric_header_16:
    r_array_len = unaligned_cast<uint16_t>(header, data_offset+1) ;
    data_offset += 3;
    object_type = REALSXP;
    return;
  case numeric_header_32:
    r_array_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
    data_offset += 5;
    object_type = REALSXP;
    return;
  case numeric_header_64:
    r_array_len =  unaligned_cast<uint64_t>(header, data_offset+1) ;
    data_offset += 9;
    object_type = REALSXP;
    return;
  case list_header_8:
    r_array_len =  *reinterpret_cast<const uint8_t*>(header+data_offset+1) ;
    data_offset += 2;
    object_type = VECSXP;
    return;
  case list_header_16:
    r_array_len = unaligned_cast<uint16_t>(header, data_offset+1) ;
    data_offset += 3;
    object_type = VECSXP;
    return;
  case list_header_32:
    r_array_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
    data_offset += 5;
    object_type = VECSXP;
    return;
  case list_header_64:
    r_array_len =  unaligned_cast<uint64_t>(header, data_offset+1) ;
    data_offset += 9;
    object_type = VECSXP;
    return;
  case integer_header_8:
    r_array_len =  *reinterpret_cast<const uint8_t*>(header+data_offset+1) ;
    data_offset += 2;
    object_type = INTSXP;
    return;
  case integer_header_16:
    r_array_len = unaligned_cast<uint16_t>(header, data_offset+1) ;
    data_offset += 3;
    object_type = INTSXP;
    return;
  case integer_header_32:
    r_array_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
    data_offset += 5;
    object_type = INTSXP;
    return;
  case integer_header_64:
    r_array_len =  unaligned_cast<uint64_t>(header, data_offset+1) ;
    data_offset += 9;
    object_type = INTSXP;
    return;
  case logical_header_8:
    r_array_len =  *reinterpret_cast<const uint8_t*>(header+data_offset+1) ;
    data_offset += 2;
    object_type = LGLSXP;
    return;
  case logical_header_16:
    r_array_len = unaligned_cast<uint16_t>(header, data_offset+1) ;
    data_offset += 3;
    object_type = LGLSXP;
    return;
  case logical_header_32:
    r_array_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
    data_offset += 5;
    object_type = LGLSXP;
    return;
  case logical_header_64:
    r_array_len =  unaligned_cast<uint64_t>(header, data_offset+1) ;
    data_offset += 9;
    object_type = LGLSXP;
    return;
  case raw_header_32:
    r_array_len = unaligned_cast<uint32_t>(header, data_offset+1) ;
    data_offset += 5;
    object_type = RAWSXP;
    return;
  case raw_header_64:
    r_array_len =  unaligned_cast<uint64_t>(header, data_offset+1) ;
    data_offset += 9;
    object_type = RAWSXP;
    return;
  case character_header_8:
    r_array_len =  *reinterpret_cast<const uint8_t*>(header+data_offset+1) ;
    data_offset += 2;
    object_type = STRSXP;
    return;
  case character_header_16:
    r_array_len = unaligned_cast<uint16_t>(header, data_offset+1) ;
    data_offset += 3;
    object_type = STRSXP;
    return;
  case character_header_32:
    r_array_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
    data_offset += 5;
    object_type = STRSXP;
    return;
  case character_header_64:
    r_array_len =  unaligned_cast<uint64_t>(header, data_offset+1) ;
    data_offset += 9;
    object_type = STRSXP;
    return;
  case complex_header_32:
    r_array_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
    data_offset += 5;
    object_type = CPLXSXP;
    return;
  case complex_header_64:
    r_array_len =  unaligned_cast<uint64_t>(header, data_offset+1) ;
    data_offset += 9;
    object_type = CPLXSXP;
    return;
  case null_header:
    r_array_len =  0;
    data_offset += 1;
    object_type = NILSXP;
    return;
  case attribute_header_8:
    r_array_len =  *reinterpret_cast<const uint8_t*>(header+data_offset+1) ;
    data_offset += 2;
    object_type = ANYSXP;
    return;
  case attribute_header_32:
    r_array_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
    data_offset += 5;
    object_type = ANYSXP;
    return;
  case nstype_header_32:
    r_array_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
    data_offset += 5;
    object_type = S4SXP;
    return;
  case nstype_header_64:
    r_array_len =  unaligned_cast<uint64_t>(header, data_offset+1) ;
    data_offset += 9;
    object_type = S4SXP;
    return;
  }
  // additional types
  throw std::runtime_error("something went wrong (reading object header)");
}

inline void readStringHeader_common(uint32_t & r_string_len, cetype_t & ce_enc, uint64_t & data_offset, const char * const header) {
  uint8_t enc = reinterpret_cast<const uint8_t*>(header)[data_offset] & 0xC0;
  switch(enc) {
  case string_enc_native:
    ce_enc = CE_NATIVE; break;
  case string_enc_utf8:
    ce_enc = CE_UTF8; break;
  case string_enc_latin1:
    ce_enc = CE_LATIN1; break;
  case string_enc_bytes:
    ce_enc = CE_BYTES; break;
  }
  
  if((reinterpret_cast<const uint8_t*>(header)[data_offset] & 0x20) == string_header_5) {
    r_string_len = *reinterpret_cast<const uint8_t*>(header+data_offset) & 0x1F ;
    data_offset += 1;
    return;
  } else {
    uint8_t hd = reinterpret_cast<const uint8_t*>(header)[data_offset] & 0x1F;
    switch(hd) {
    case string_header_8:
      r_string_len =  *reinterpret_cast<const uint8_t*>(header+data_offset+1) ;
      data_offset += 2;
      return;
    case string_header_16:
      r_string_len = unaligned_cast<uint16_t>(header, data_offset+1) ;
      data_offset += 3;
      return;
    case string_header_32:
      r_string_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
      data_offset += 5;
      return;
    case string_header_NA:
      r_string_len = NA_STRING_LENGTH;
      data_offset += 1;
      return;
    }
  } 
  throw std::runtime_error("something went wrong (reading string header)");
}

////////////////////////////////////////////////////////////////
// Compress and decompress templates
////////////////////////////////////////////////////////////////

#define XXH_SEED 12345
// seed is 12345
struct xxhash_env {
  XXH32_state_s* x;
  xxhash_env() : x(XXH32_createState()) {
    XXH_errorcode ret = XXH32_reset(x, XXH_SEED);
    if(ret == XXH_ERROR) throw std::runtime_error("error in hashing function");
  }
  ~xxhash_env() {
    XXH32_freeState(x);
  }
  void reset() {
    XXH_errorcode ret = XXH32_reset(x, XXH_SEED);
    if(ret == XXH_ERROR) throw std::runtime_error("error in hashing function");
  }
  void update(const void * const input, const uint64_t length) {
    XXH_errorcode ret = XXH32_update(x, input, length);
    if(ret == XXH_ERROR) throw std::runtime_error("error in hashing function");
    // std::cout << digest() << std::endl;
  }
  uint32_t digest() {
    return XXH32_digest(x);
  }
};

struct zstd_compress_env {
  // ZSTD_CCtx* zcs;
  // zstd_compress_env() : zcs(ZSTD_createCCtx()) {}
  // ~zstd_compress_env() {
  //   ZSTD_freeCCtx(zcs);
  // }
  uint64_t compress( void * dst, size_t dstCapacity,
                   const void * src, size_t srcSize,
                   int compressionLevel) {
    // return ZSTD_compressCCtx(zcs, dst, dstCapacity, src, srcSize, compressionLevel);
    uint64_t return_value = ZSTD_compress(dst, dstCapacity, src, srcSize, compressionLevel);
    if(ZSTD_isError(return_value)) throw std::runtime_error("zstd compression error");
    return return_value;
  }
  uint64_t compressBound(uint64_t srcSize) {
    return ZSTD_compressBound(srcSize);
  }
};

struct lz4_compress_env {
  // std::vector<char> zcs;
  // char* state;
  // lz4_compress_env() {
  //   zcs = std::vector<char>(LZ4_sizeofState());
  //   state = zcs.data();
  // }
  uint64_t compress( char * dst, int dstCapacity,
                   const char * src, int srcSize,
                   int compressionLevel) {
    // return LZ4_compress_fast_extState(state, reinterpret_cast<char*>(const_cast<void*>(src)), 
    //                          reinterpret_cast<char*>(const_cast<void*>(dst)),
    //                          static_cast<int>(srcSize), static_cast<int>(dstCapacity), compressionLevel);
    int return_value = LZ4_compress_fast(src, dst, srcSize, dstCapacity, compressionLevel);
    if(return_value == 0) throw std::runtime_error("lz4 compression error");
    return return_value;
  }
  uint64_t compressBound(uint64_t srcSize) {
    return LZ4_compressBound(srcSize);
  }
};

struct lz4hc_compress_env {
  // std::vector<char> zcs;
  // char* state;
  // lz4hc_compress_env() {
  //   zcs = std::vector<char>(LZ4_sizeofStateHC());
  //   state = zcs.data();
  // }
  uint64_t compress( char * dst, int dstCapacity,
                   const char * src, int srcSize,
                   int compressionLevel) {
    // xenv.update(src, srcSize);
    // return LZ4_compress_HC_extStateHC(state, reinterpret_cast<char*>(const_cast<void*>(src)), 
    //                                   reinterpret_cast<char*>(const_cast<void*>(dst)),
    //                                   static_cast<int>(srcSize), static_cast<int>(dstCapacity), compressionLevel);
    int return_value = LZ4_compress_HC(src, dst, srcSize, dstCapacity, compressionLevel);
    if(return_value == 0) throw std::runtime_error("lz4hc compression error");
    return return_value;
  }
  uint64_t compressBound(uint64_t srcSize) {
    return LZ4_compressBound(srcSize);
  }
};


// Explicit decompression context (zstd v. 1.4.0)
struct zstd_decompress_env {
  // ZSTD_DCtx* zcs;
  // zstd_decompress_env() : zcs(ZSTD_createDCtx()) {}
  // ~zstd_decompress_env() {
  //   ZSTD_freeDCtx(zcs);
  // }
  uint64_t bound;
  zstd_decompress_env() : bound(ZSTD_compressBound(BLOCKSIZE)) {}
  uint64_t decompress( void* dst, size_t dstCapacity,
                     const void* src, size_t compressedSize) {
    // return ZSTD_decompress(dst, dstCapacity, src, compressedSize);
    // return ZSTD_decompressDCtx(zcs, dst, dstCapacity, src, compressedSize);
    if(compressedSize > bound) throw std::runtime_error("Malformed compress block: compressed size > compress bound");
    // std::cout << "decompressing " << dst << " " << dstCapacity << " " << src << " " << compressedSize << "\n";
    uint64_t return_value = ZSTD_decompress(dst, dstCapacity, src, compressedSize);
    if(ZSTD_isError(return_value)) throw std::runtime_error("zstd decompression error");
    if(return_value > BLOCKSIZE) throw std::runtime_error("Malformed compress block: decompressed size > max blocksize " + std::to_string(return_value));
    return return_value;
  }
  uint64_t compressBound(uint64_t srcSize) {
    return ZSTD_compressBound(srcSize);
  }
};

struct lz4_decompress_env {
  uint64_t bound;
  lz4_decompress_env() : bound(LZ4_compressBound(BLOCKSIZE)) {}
  uint64_t decompress( char * dst, int dstCapacity,
                     const char* src, int compressedSize) {
    // std::cout << "decomp " << compressedSize << std::endl;
    if(static_cast<uint64_t>(compressedSize) > bound) throw std::runtime_error("Malformed compress block: compressed size > compress bound");
    int return_value = LZ4_decompress_safe(src, dst, compressedSize, dstCapacity);
    if(return_value < 0) throw std::runtime_error("lz4 decompression error");
    if(static_cast<uint64_t>(return_value) > BLOCKSIZE) throw std::runtime_error("Malformed compress block: decompressed size > max blocksize" + std::to_string(return_value));
    return return_value;
    // return LZ4_decompress_safe(reinterpret_cast<char*>(const_cast<void*>(src)),
    //                                        reinterpret_cast<char*>(const_cast<void*>(dst)),
    //                                        static_cast<int>(compressedSize), static_cast<int>(dstCapacity));
  }
  uint64_t compressBound(uint64_t srcSize) {
    return LZ4_compressBound(srcSize);
  }
};


////////////////////////////////////////////////////////////////
// qdump/debug helper functions
////////////////////////////////////////////////////////////////

void dumpMetadata(List & output, const QsMetadata & qm) {
  switch(qm.compress_algorithm) {
  case 0:
    output["compress_algorithm"] = "zstd";
    break;
  case 1:
    output["compress_algorithm"] = "lz4";
    break;
  case 2:
    output["compress_algorithm"] = "lz4hc";
    break;
  case 3:
    output["compress_algorithm"] = "zstd_stream";
    break;
  case 4:
    output["compress_algorithm"] = "uncompressed";
    break;
  default:
    output["compress_algorithm"] = "unknown";
    break;
  }
  output["compress_level"] = qm.compress_level;
  output["lgl_shuffle"] = qm.lgl_shuffle;
  output["int_shuffle"] = qm.int_shuffle;
  output["real_shuffle"] = qm.real_shuffle;
  output["cplx_shuffle"] = qm.cplx_shuffle;
  output["endian"] = static_cast<int>(qm.endian);
  output["check_hash"] = qm.check_hash;
}

// simple decompress stream context

struct zstd_decompress_stream_simple {
  ZSTD_inBuffer zin;
  ZSTD_outBuffer zout;
  ZSTD_DStream* zds;
  std::vector<char> outblock;
  zstd_decompress_stream_simple(uint64_t outsize, char* inp, uint64_t insize) {
    if(outsize == 0) {
      outblock = std::vector<char>(BLOCKSIZE);
      zout.size = BLOCKSIZE;
    } else {
      outblock = std::vector<char>(outsize);
      zout.size = outsize;
    }
    zout.pos = 0;
    zout.dst = outblock.data();
    zin.pos = 0;
    zin.src = inp;
    zin.size = insize;
    zds = ZSTD_createDStream();
  }
  
  bool decompress() {
    uint64_t return_value = ZSTD_decompressStream(zds, &zout, &zin);
    if(ZSTD_isError(return_value)) return true;
    while(zout.pos == zout.size) {
      outblock.resize(outblock.size() * 3 / 2);
      zout.dst = outblock.data();
      zout.size = outblock.size();
      return_value = ZSTD_decompressStream(zds, &zout, &zin);
      if(ZSTD_isError(return_value)) return true;
    }
    outblock.resize(zout.pos);
    return false;
  }
  ~zstd_decompress_stream_simple() {
    ZSTD_freeDStream(zds);
  }
};

#endif
