#include "qs_common.h"

struct ZSTD_streamWrite {
  std::ofstream & myFile;
  xxhash_env xenv;
  QsMetadata qm;
  uint64_t bytes_written;
  std::vector<char> outblock;
  ZSTD_inBuffer zin;
  ZSTD_outBuffer zout;
  ZSTD_CStream* zcs;
  ZSTD_streamWrite(std::ofstream & mf, QsMetadata qm) : myFile(mf), xenv(xxhash_env()), qm(qm) {
    bytes_written = 0;
    size_t outblocksize = ZSTD_CStreamOutSize();
    outblock = std::vector<char>(outblocksize);
    zcs = ZSTD_createCStream();
    ZSTD_initCStream(zcs, qm.compress_level);
    zout.size = outblocksize;
    zout.pos = 0;
    zout.dst = outblock.data();
  }
  ~ZSTD_streamWrite() {
    ZSTD_freeCStream(zcs);
  }
  void push(char * data, uint64_t length) {
    if(qm.check_hash) xenv.update(data, length);
    zin.pos = 0;
    zin.src = data;
    zin.size = length;
    bytes_written += zin.size;
    while(zin.pos < zin.size) {
      zout.pos = 0;
      size_t return_value = ZSTD_compressStream(zcs, &zout, &zin);
      if(ZSTD_isError(return_value)) throw std::runtime_error("zstd stream compression error; output is likely corrupted");
      if(zout.pos > 0) myFile.write(reinterpret_cast<char*>(zout.dst), zout.pos);
    }
  }
  template<typename POD>
  void push_pod(POD pod) {
    push(reinterpret_cast<char*>(&pod), sizeof(pod));
  }
  
  void flush() {
    size_t remain;
    do {
      zout.pos = 0;
      remain = ZSTD_flushStream(zcs, &zout);
      if(ZSTD_isError(remain)) throw std::runtime_error("zstd stream compression error; output is likely corrupted");
      if(zout.pos > 0) myFile.write(reinterpret_cast<char*>(zout.dst), zout.pos);
    } while (remain != 0);
  }
};

template <class StreamClass> 
struct CompressBufferStream {
  StreamClass & sobj;
  std::vector<uint8_t> shuffleblock = std::vector<uint8_t>(256);
  std::vector<char> block = std::vector<char>(BLOCKSIZE);
  QsMetadata qm;

  CompressBufferStream(StreamClass & so, QsMetadata qm) : sobj(so), qm(qm) {}
  void shuffle_push(char* data, uint64_t len, size_t bytesoftype) {
    if(len > MIN_SHUFFLE_ELEMENTS) {
      if(len > shuffleblock.size()) shuffleblock.resize(len);
      blosc_shuffle(reinterpret_cast<uint8_t*>(data), shuffleblock.data(), len, bytesoftype);
      sobj.push(reinterpret_cast<char*>(shuffleblock.data()), len);
    } else if(len > 0) {
      sobj.push(data, len);
    }
  }
  
  // to do: use SEXP instead of RObject?
  void pushObj(SEXP & x, bool attributes_processed = false) {
    if(!attributes_processed && stypes.find(TYPEOF(x)) != stypes.end()) {
      std::vector<SEXP> anames;
      std::vector<SEXP> attrs;
      SEXP alist = ATTRIB(x);
      while(alist != R_NilValue) {
        anames.push_back(PRINTNAME(TAG(alist)));
        attrs.push_back(CAR(alist));
        alist = CDR(alist);
      }
      if(anames.size() != 0) {
        writeAttributeHeader_stream(anames.size(), &sobj);
        pushObj(x, true);
        for(uint64_t i=0; i<anames.size(); i++) {
          uint64_t alen = strlen(CHAR(anames[i]));
          writeStringHeader_stream(alen,CE_NATIVE, &sobj);
          sobj.push(const_cast<char*>(CHAR(anames[i])), alen);
          pushObj(attrs[i]);
        }
      } else {
        pushObj(x, true);
      }
    } else if(TYPEOF(x) == STRSXP) {
      uint64_t dl = Rf_xlength(x);
      writeHeader_stream(STRSXP, dl, &sobj);
      for(uint64_t i=0; i<dl; i++) {
        SEXP xi = STRING_ELT(x, i);
        if(xi == NA_STRING) {
          sobj.push(reinterpret_cast<char*>(const_cast<unsigned char*>(&string_header_NA)), 1);
        } else {
          uint64_t dl = LENGTH(xi);
          writeStringHeader_stream(dl, Rf_getCharCE(xi), &sobj);
          sobj.push(const_cast<char*>(CHAR(xi)), dl);
        }
      }
    } else if(stypes.find(TYPEOF(x)) != stypes.end()) {
      uint64_t dl = Rf_xlength(x);
      writeHeader_stream(TYPEOF(x), dl, &sobj);
      if(TYPEOF(x) == VECSXP) {
        for(uint64_t i=0; i<dl; i++) {
          SEXP xi = VECTOR_ELT(x, i);
          pushObj(xi);
        }
      } else {
        switch(TYPEOF(x)) {
        case REALSXP:
          if(qm.real_shuffle) {
            shuffle_push(reinterpret_cast<char*>(REAL(x)), dl*8, 8);
          } else {
            sobj.push(reinterpret_cast<char*>(REAL(x)), dl*8); 
          }
          break;
        case INTSXP:
          if(qm.int_shuffle) {
            shuffle_push(reinterpret_cast<char*>(INTEGER(x)), dl*4, 4); break;
          } else {
            sobj.push(reinterpret_cast<char*>(INTEGER(x)), dl*4); 
          }
          break;
        case LGLSXP:
          if(qm.lgl_shuffle) {
            shuffle_push(reinterpret_cast<char*>(LOGICAL(x)), dl*4, 4); break;
          } else {
            sobj.push(reinterpret_cast<char*>(LOGICAL(x)), dl*4); 
          }
          break;
        case RAWSXP:
          sobj.push(reinterpret_cast<char*>(RAW(x)), dl); 
          break;
        case CPLXSXP:
          if(qm.cplx_shuffle) {
            shuffle_push(reinterpret_cast<char*>(COMPLEX(x)), dl*16, 8); break;
          } else {
            sobj.push(reinterpret_cast<char*>(COMPLEX(x)), dl*16); 
          }
          break;
        case NILSXP:
          break;
        }
      }
    } else { // other non-supported SEXPTYPEs use the built in R serialization method
      SEXP xserialized = serializeToRaw(x);
      uint64_t xs_size = Rf_xlength(xserialized);
      if(xs_size < 4294967296) {
        sobj.push(reinterpret_cast<char*>(const_cast<unsigned char*>(&nstype_header_32)), 1);
        sobj.push_pod(static_cast<uint32_t>(xs_size));
      } else {
        sobj.push(reinterpret_cast<char*>(const_cast<unsigned char*>(&nstype_header_64)), 1);
        sobj.push_pod(static_cast<uint64_t>(xs_size));
      }
      sobj.push(reinterpret_cast<char*>(RAW(xserialized)), xs_size);
    }
  }
};
