#include "qs_common.h"

struct ZSTD_streamRead {
  std::ifstream & myFile;
  QsMetadata qm;
  uint64_t minblocksize;
  uint64_t maxblocksize;
  uint64_t decompressed_bytes_total;
  uint64_t decompressed_bytes_read;
  std::vector<char> outblock;
  std::vector<char> inblock;
  ZSTD_inBuffer zin;
  ZSTD_outBuffer zout;
  ZSTD_DStream* zds;
  ZSTD_streamRead(std::ifstream & mf, QsMetadata qm, uint64_t dbt) : 
    myFile(mf), qm(qm), decompressed_bytes_total(dbt) {
    size_t outblocksize = 4*ZSTD_DStreamOutSize();
    size_t inblocksize = ZSTD_DStreamInSize();
    decompressed_bytes_read = 0;
    outblock = std::vector<char>(outblocksize);
    inblock = std::vector<char>(inblocksize);
    minblocksize = ZSTD_DStreamOutSize();
    maxblocksize = 4*ZSTD_DStreamOutSize();
    zds = ZSTD_createDStream();
    ZSTD_initDStream(zds);
    zout.size = maxblocksize;
    zout.pos = 0;
    zout.dst = outblock.data();
    zin.size = 0;
    zin.pos = 0;
    zin.src = inblock.data();
  }
  ~ZSTD_streamRead() {
    ZSTD_freeDStream(zds);
  }
  inline void ZSTD_decompressStream_count(ZSTD_DStream* zds, ZSTD_outBuffer * zout, ZSTD_inBuffer * zin) {
    uint64_t temp = zout->pos;
    ZSTD_decompressStream(zds, zout, zin);
    decompressed_bytes_read += zout->pos - temp;
  }
  void getBlock(uint64_t & blocksize, uint64_t & bytesused) {
    if(decompressed_bytes_read >= decompressed_bytes_total) return;
    char * ptr = outblock.data();
    if(blocksize > bytesused) {
      // dst should never overlap since blocksize > minblocksize
      std::memcpy(ptr, ptr + bytesused, blocksize - bytesused); 
      zout.pos = blocksize - bytesused;
    } else {
      zout.pos = 0;
    }
    while(zout.pos < minblocksize) {
      if(zin.pos < zin.size) {
        ZSTD_decompressStream_count(zds, &zout, &zin);
      } else if(! myFile.eof()) {
        myFile.read(inblock.data(), inblock.size());
        size_t bytes_read = myFile.gcount();
        if(bytes_read == 0) continue; // EOF
        zin.pos = 0;
        zin.size = bytes_read;
        ZSTD_decompressStream_count(zds, &zout, &zin);
      } else {
        size_t current_pos = zout.pos;
        ZSTD_decompressStream_count(zds, &zout, &zin);
        if(zout.pos == current_pos) break; // no more data
      }
    }
    blocksize = zout.pos;
    bytesused = 0;
  }
  void copyData(uint64_t & blocksize, uint64_t & bytesused,
                char* dst,uint64_t dst_size) {
    char * ptr = outblock.data();
    // dst should never overlap since blocksize > MINBLOCKSIZE
    if(dst_size > blocksize - bytesused) {
      // std::cout << blocksize - bytesused << " data cpy\n";
      std::memcpy(dst, ptr + bytesused, blocksize - bytesused);
      zout.pos = blocksize - bytesused;
      zout.dst = dst;
      zout.size = dst_size;
      while(zout.pos < dst_size) {
        // std::cout << zout.pos << " " << dst_size << " zout position\n";
        if(zin.pos < zin.size) {
          ZSTD_decompressStream_count(zds, &zout, &zin);
          // std::cout << zin.pos << "/" << zin.size << " zin " << zout.pos << "/" << zout.size << " zout\n";
        } else if(! myFile.eof()) {
          myFile.read(inblock.data(), minblocksize);
          size_t bytes_read = myFile.gcount();
          if(bytes_read == 0) continue; // EOF
          zin.pos = 0;
          zin.size = bytes_read;
          ZSTD_decompressStream_count(zds, &zout, &zin);
          // std::cout << zin.pos << "/" << zin.size << " zin " << zout.pos << "/" << zout.size << " zout new inblock\n";
        } else {
          size_t current_pos = zout.pos;
          ZSTD_decompressStream_count(zds, &zout, &zin);
          // std::cout << zin.pos << "/" << zin.size << " zin " << zout.pos << "/" << zout.size << " zout flush\n";
          if(zout.pos == current_pos) break; // no more data, also we should throw an error as more data was expected
        }
      }
      bytesused = 0;
      blocksize = 0;
    } else {
      std::memcpy(dst, ptr + bytesused, dst_size);
      bytesused += dst_size;
    }
    zout.dst = outblock.data();
    zout.size = maxblocksize;
    if(blocksize - bytesused < BLOCKRESERVE) {
      getBlock(blocksize, bytesused);
    }
  }
};

// struct brotli_streamRead {
//   std::ifstream & myFile;
//   QsMetadata qm;
//   uint64_t minblocksize;
//   uint64_t maxblocksize;
//   uint64_t decompressed_bytes_total;
//   std::vector<char> outblock;
//   std::vector<char> inblock;
//   size_t available_in;
//   size_t available_out;
//   size_t total_out;
//   const uint8_t * next_in;
//   uint8_t * next_out;
//   BrotliDecoderState* zds;
//   brotli_streamRead(std::ifstream & mf, QsMetadata qm, uint64_t dbt) : 
//     myFile(mf), qm(qm), decompressed_bytes_total(dbt) {
//     size_t outblocksize = 2*BLOCKSIZE;
//     size_t inblocksize = 2*BLOCKSIZE;
//     outblock = std::vector<char>(outblocksize);
//     inblock = std::vector<char>(inblocksize);
//     minblocksize = BLOCKSIZE / 2;
//     maxblocksize = 2*BLOCKSIZE;
//     available_in = 0;
//     available_out = 0;
//     total_out = 0;
//     next_in = reinterpret_cast<uint8_t*>(inblock.data());
//     next_out = reinterpret_cast<uint8_t*>(outblock.data());
//     zds = BrotliDecoderCreateInstance(NULL, NULL, NULL);
//   }
//   ~brotli_streamRead() {
//     BrotliDecoderDestroyInstance(zds);
//   }
//   void getBlock(uint64_t & blocksize, uint64_t & bytesused) {
//     if(total_out >= decompressed_bytes_total) return;
//     if(blocksize > bytesused) {
//       std::memcpy(outblock.data(), outblock.data() + bytesused, blocksize - bytesused); 
//       available_out = outblock.size() - (blocksize - bytesused);
//       next_out = reinterpret_cast<uint8_t*>(outblock.data()) + (blocksize - bytesused);
//     } else {
//       available_out = outblock.size();
//       next_out = reinterpret_cast<uint8_t*>(outblock.data());
//     }
//     while(outblock.size() - available_out < minblocksize) {
//       if(available_in > 0) {
//         BrotliDecoderDecompressStream(zds, &available_in, &next_in, &available_out, &next_out, &total_out);
//       } else if(! myFile.eof()) {
//         myFile.read(inblock.data(), inblock.size());
//         available_in = myFile.gcount();
//         next_in = reinterpret_cast<uint8_t*>(inblock.data());
//         if(available_in == 0) continue; // EOF
//         BrotliDecoderDecompressStream(zds, &available_in, &next_in, &available_out, &next_out, &total_out);
//       } else {
//         BrotliDecoderResult ret = BrotliDecoderDecompressStream(zds, &available_in, &next_in, &available_out, &next_out, &total_out);
//         if( ret == BROTLI_DECODER_RESULT_SUCCESS ) break; // no more data
//       }
//     }
//     blocksize = outblock.size() - available_out;
//     bytesused = 0;
//   }
//   void copyData(uint64_t & blocksize, uint64_t & bytesused, char* dst, uint64_t dst_size) {
//     if(dst_size > blocksize - bytesused) {
//       std::memcpy(dst, outblock.data() + bytesused, blocksize - bytesused);
//       next_out = reinterpret_cast<uint8_t*>(dst) + blocksize - bytesused;
//       available_out = dst_size - (blocksize - bytesused);
//       while(available_out > 0) {
//         if(available_in > 0) {
//           BrotliDecoderDecompressStream(zds, &available_in, &next_in, &available_out, &next_out, &total_out);
//         } else if(! myFile.eof()) {
//           myFile.read(inblock.data(), inblock.size());
//           available_in = myFile.gcount();
//           next_in = reinterpret_cast<uint8_t*>(inblock.data());
//           if(available_in == 0) continue; // EOF
//           BrotliDecoderDecompressStream(zds, &available_in, &next_in, &available_out, &next_out, &total_out);
//         } else {
//           BrotliDecoderResult ret = BrotliDecoderDecompressStream(zds, &available_in, &next_in, &available_out, &next_out, &total_out);
//           if( ret == BROTLI_DECODER_RESULT_SUCCESS ) break; // no more data
//         }
//       }
//       bytesused = 0;
//       blocksize = 0;
//       getBlock(blocksize, bytesused);
//     } else {
//       std::memcpy(dst, outblock.data() + bytesused, dst_size);
//       bytesused += dst_size;
//       if(blocksize - bytesused < BLOCKRESERVE) {
//         getBlock(blocksize, bytesused);
//       }
//     }
//   }
// };

template <class DestreamClass> 
struct Data_Context_Stream {
  DestreamClass & dsc;
  QsMetadata qm;
  bool use_alt_rep_bool;
  std::vector<uint8_t> shuffleblock = std::vector<uint8_t>(256);
  uint64_t data_offset;
  uint64_t block_size;
  char * data_ptr;
  std::string temp_string;
  
  Data_Context_Stream(DestreamClass & d, QsMetadata q, bool use_alt_rep) : dsc(d), qm(q), use_alt_rep_bool(use_alt_rep) {
    data_offset = 0;
    block_size = 0;
    data_ptr = dsc.outblock.data();
    temp_string = std::string(256, '\0');
  }
  void readHeader(SEXPTYPE & object_type, uint64_t & r_array_len) {
    if(data_offset + BLOCKRESERVE >= block_size) dsc.getBlock(block_size, data_offset);
    char* header = data_ptr;
    unsigned char h5 = reinterpret_cast<unsigned char*>(header)[data_offset] & 0xE0;
    switch(h5) {
    case numeric_header_5:
      r_array_len = *reinterpret_cast<uint8_t*>(header+data_offset) & 0x1F ;
      data_offset += 1;
      object_type = REALSXP;
      return;
    case list_header_5:
      r_array_len = *reinterpret_cast<uint8_t*>(header+data_offset) & 0x1F ;
      data_offset += 1;
      object_type = VECSXP;
      return;
    case integer_header_5:
      r_array_len = *reinterpret_cast<uint8_t*>(header+data_offset) & 0x1F ;
      data_offset += 1;
      object_type = INTSXP;
      return;
    case logical_header_5:
      r_array_len = *reinterpret_cast<uint8_t*>(header+data_offset) & 0x1F ;
      data_offset += 1;
      object_type = LGLSXP;
      return;
    case character_header_5:
      r_array_len = *reinterpret_cast<uint8_t*>(header+data_offset) & 0x1F ;
      data_offset += 1;
      object_type = STRSXP;
      return;
    case attribute_header_5:
      r_array_len = *reinterpret_cast<uint8_t*>(header+data_offset) & 0x1F ;
      data_offset += 1;
      object_type = ANYSXP;
      return;
    }
    unsigned char hd = reinterpret_cast<unsigned char*>(header)[data_offset];
    switch(hd) {
    case numeric_header_8:
      r_array_len =  *reinterpret_cast<uint8_t*>(header+data_offset+1) ;
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
      r_array_len =  *reinterpret_cast<uint8_t*>(header+data_offset+1) ;
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
      r_array_len =  *reinterpret_cast<uint8_t*>(header+data_offset+1) ;
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
      r_array_len =  *reinterpret_cast<uint8_t*>(header+data_offset+1) ;
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
      r_array_len =  *reinterpret_cast<uint8_t*>(header+data_offset+1) ;
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
      r_array_len =  *reinterpret_cast<uint8_t*>(header+data_offset+1) ;
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
      r_array_len =  unaligned_cast<uint32_t>(header, data_offset+1) ;
      data_offset += 9;
      object_type = S4SXP;
      return;
    }
    // additional types
    throw exception("something went wrong (reading object header)");
  }
  void readStringHeader(uint32_t & r_string_len, cetype_t & ce_enc) {
    if(data_offset + BLOCKRESERVE >= block_size) dsc.getBlock(block_size, data_offset);
    char* header = data_ptr;
    unsigned char enc = reinterpret_cast<unsigned char*>(header)[data_offset] & 0xC0;
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
    
    if((reinterpret_cast<unsigned char*>(header)[data_offset] & 0x20) == string_header_5) {
      r_string_len = *reinterpret_cast<uint8_t*>(header+data_offset) & 0x1F ;
      data_offset += 1;
      return;
    } else {
      unsigned char hd = reinterpret_cast<unsigned char*>(header)[data_offset] & 0x1F;
      switch(hd) {
      case string_header_8:
        r_string_len =  *reinterpret_cast<uint8_t*>(header+data_offset+1) ;
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
    throw exception("something went wrong (reading string header)");
  }
  void getBlockData(char* outp, uint64_t data_size) {
    // std::cout << data_size << " get block\n";
    dsc.copyData(block_size, data_offset, outp, data_size);
  }
  void getShuffleBlockData(char* outp, uint64_t data_size, uint64_t bytesoftype) {
    // std::cout << data_size << " get shuffle block\n";
    if(data_size >= MIN_SHUFFLE_ELEMENTS) {
      if(data_size > shuffleblock.size()) shuffleblock.resize(data_size);
      getBlockData(reinterpret_cast<char*>(shuffleblock.data()), data_size);
      blosc_unshuffle(shuffleblock.data(), reinterpret_cast<uint8_t*>(outp), data_size, bytesoftype);
    } else if(data_size > 0) {
      getBlockData(outp, data_size);
    }
  }
  SEXP processBlock() {
    SEXPTYPE obj_type;
    uint64_t r_array_len;
    readHeader(obj_type, r_array_len);
    // std::cout << r_array_len << " " << obj_type << "\n";
    uint64_t number_of_attributes;
    if(obj_type == ANYSXP) {
      number_of_attributes = r_array_len;
      readHeader(obj_type, r_array_len);
    } else {
      number_of_attributes = 0;
    }
    SEXP obj;
    switch(obj_type) {
    case VECSXP: 
      obj = PROTECT(Rf_allocVector(VECSXP, r_array_len));
      for(uint64_t i=0; i<r_array_len; i++) {
        SET_VECTOR_ELT(obj, i, processBlock());
      }
      break;
    case REALSXP:
      obj = PROTECT(Rf_allocVector(REALSXP, r_array_len));
      if(qm.real_shuffle) {
        getShuffleBlockData(reinterpret_cast<char*>(REAL(obj)), r_array_len*8, 8);
      } else {
        getBlockData(reinterpret_cast<char*>(REAL(obj)), r_array_len*8);
      }
      break;
    case INTSXP:
      obj = PROTECT(Rf_allocVector(INTSXP, r_array_len));
      if(qm.int_shuffle) {
        getShuffleBlockData(reinterpret_cast<char*>(INTEGER(obj)), r_array_len*4, 4);
      } else {
        getBlockData(reinterpret_cast<char*>(INTEGER(obj)), r_array_len*4);
      }
      break;
    case LGLSXP:
      obj = PROTECT(Rf_allocVector(LGLSXP, r_array_len));
      if(qm.lgl_shuffle) {
        getShuffleBlockData(reinterpret_cast<char*>(LOGICAL(obj)), r_array_len*4, 4);
      } else {
        getBlockData(reinterpret_cast<char*>(LOGICAL(obj)), r_array_len*4);
      }
      break;
    case CPLXSXP:
      obj = PROTECT(Rf_allocVector(CPLXSXP, r_array_len));
      if(qm.cplx_shuffle) {
        getShuffleBlockData(reinterpret_cast<char*>(COMPLEX(obj)), r_array_len*16, 8);
      } else {
        getBlockData(reinterpret_cast<char*>(COMPLEX(obj)), r_array_len*16);
      }
      break;
    case RAWSXP:
      obj = PROTECT(Rf_allocVector(RAWSXP, r_array_len));
      if(r_array_len > 0) getBlockData(reinterpret_cast<char*>(RAW(obj)), r_array_len);
      break;
    case STRSXP:
      if(use_alt_rep_bool) {
        auto ret = new stdvec_data(r_array_len);
        for(uint64_t i=0; i < r_array_len; i++) {
          uint32_t r_string_len;
          cetype_t string_encoding = CE_NATIVE;
          readStringHeader(r_string_len, string_encoding);
          if(r_string_len == NA_STRING_LENGTH) {
            ret->encodings[i] = 5;
          } else if(r_string_len == 0) {
            ret->encodings[i] = 1;
            ret->strings[i] = "";
          } else {
            switch(string_encoding) {
            case CE_NATIVE:
              ret->encodings[i] = 1;
              break;
            case CE_UTF8:
              ret->encodings[i] = 2;
              break;
            case CE_LATIN1:
              ret->encodings[i] = 3;
              break;
            case CE_BYTES:
              ret->encodings[i] = 4;
              break;
            default:
              ret->encodings[i] = 5;
            break;
            }
            ret->strings[i].resize(r_string_len);
            getBlockData(&(ret->strings[i])[0], r_string_len);
          }
        }
        obj = PROTECT(stdvec_string::Make(ret, true));
      } else {
        obj = PROTECT(Rf_allocVector(STRSXP, r_array_len));
        for(uint64_t i=0; i<r_array_len; i++) {
          uint32_t r_string_len;
          cetype_t string_encoding = CE_NATIVE;
          readStringHeader(r_string_len, string_encoding);
          if(r_string_len == NA_STRING_LENGTH) {
            SET_STRING_ELT(obj, i, NA_STRING);
          } else if(r_string_len == 0) {
            SET_STRING_ELT(obj, i, Rf_mkCharLen("", 0));
          } else if(r_string_len > 0) {
            if(r_string_len > temp_string.size()) {
              temp_string.resize(r_string_len);
            }
            getBlockData(&temp_string[0], r_string_len);
            SET_STRING_ELT(obj, i, Rf_mkCharLenCE(temp_string.data(), r_string_len, string_encoding));
          }
        }
      }
      break;
    case S4SXP:
    {
      SEXP obj_data = PROTECT(Rf_allocVector(RAWSXP, r_array_len));
      getBlockData(reinterpret_cast<char*>(RAW(obj_data)), r_array_len);
      obj = PROTECT(unserializeFromRaw(obj_data));
      UNPROTECT(2);
      return obj;
    }
    default: // also NILSXP
      obj = R_NilValue;
      return obj;
    }
    if(number_of_attributes > 0) {
      for(uint64_t i=0; i<number_of_attributes; i++) {
        uint32_t r_string_len;
        cetype_t string_encoding;
        readStringHeader(r_string_len, string_encoding);
        if(r_string_len > temp_string.size()) {
          temp_string.resize(r_string_len);
        }
        std::string temp_attribute_string = std::string(r_string_len, '\0');
        getBlockData(&temp_attribute_string[0], r_string_len);
        Rf_setAttrib(obj, Rf_install(temp_attribute_string.data()), processBlock());
      }
    }
    UNPROTECT(1);
    return std::move(obj);
  }
};
