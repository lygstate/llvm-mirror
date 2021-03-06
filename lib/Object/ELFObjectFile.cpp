//===- ELFObjectFile.cpp - ELF object file implementation -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the ELFObjectFile class.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include <limits>
#include <utility>

using namespace llvm;
using namespace object;

// Templates to choose Elf_Addr and Elf_Off depending on is64Bits.
namespace {
template<support::endianness target_endianness>
struct ELFDataTypeTypedefHelperCommon {
  typedef support::detail::packed_endian_specific_integral
    <uint16_t, target_endianness, support::aligned> Elf_Half;
  typedef support::detail::packed_endian_specific_integral
    <uint32_t, target_endianness, support::aligned> Elf_Word;
  typedef support::detail::packed_endian_specific_integral
    <int32_t, target_endianness, support::aligned> Elf_Sword;
  typedef support::detail::packed_endian_specific_integral
    <uint64_t, target_endianness, support::aligned> Elf_Xword;
  typedef support::detail::packed_endian_specific_integral
    <int64_t, target_endianness, support::aligned> Elf_Sxword;
};
}

namespace {
template<support::endianness target_endianness, bool is64Bits>
struct ELFDataTypeTypedefHelper;

/// ELF 32bit types.
template<support::endianness target_endianness>
struct ELFDataTypeTypedefHelper<target_endianness, false>
  : ELFDataTypeTypedefHelperCommon<target_endianness> {
  typedef support::detail::packed_endian_specific_integral
    <uint32_t, target_endianness, support::aligned> Elf_Addr;
  typedef support::detail::packed_endian_specific_integral
    <uint32_t, target_endianness, support::aligned> Elf_Off;
};

/// ELF 64bit types.
template<support::endianness target_endianness>
struct ELFDataTypeTypedefHelper<target_endianness, true>
  : ELFDataTypeTypedefHelperCommon<target_endianness>{
  typedef support::detail::packed_endian_specific_integral
    <uint64_t, target_endianness, support::aligned> Elf_Addr;
  typedef support::detail::packed_endian_specific_integral
    <uint64_t, target_endianness, support::aligned> Elf_Off;
};
}

// I really don't like doing this, but the alternative is copypasta.
#define LLVM_ELF_IMPORT_TYPES(target_endianness, is64Bits) \
typedef typename \
  ELFDataTypeTypedefHelper<target_endianness, is64Bits>::Elf_Addr Elf_Addr; \
typedef typename \
  ELFDataTypeTypedefHelper<target_endianness, is64Bits>::Elf_Off Elf_Off; \
typedef typename \
  ELFDataTypeTypedefHelper<target_endianness, is64Bits>::Elf_Half Elf_Half; \
typedef typename \
  ELFDataTypeTypedefHelper<target_endianness, is64Bits>::Elf_Word Elf_Word; \
typedef typename \
  ELFDataTypeTypedefHelper<target_endianness, is64Bits>::Elf_Sword Elf_Sword; \
typedef typename \
  ELFDataTypeTypedefHelper<target_endianness, is64Bits>::Elf_Xword Elf_Xword; \
typedef typename \
  ELFDataTypeTypedefHelper<target_endianness, is64Bits>::Elf_Sxword Elf_Sxword;

  // Section header.
namespace {
template<support::endianness target_endianness, bool is64Bits>
struct Elf_Shdr_Base;

template<support::endianness target_endianness>
struct Elf_Shdr_Base<target_endianness, false> {
  LLVM_ELF_IMPORT_TYPES(target_endianness, false)
  Elf_Word sh_name;     // Section name (index into string table)
  Elf_Word sh_type;     // Section type (SHT_*)
  Elf_Word sh_flags;    // Section flags (SHF_*)
  Elf_Addr sh_addr;     // Address where section is to be loaded
  Elf_Off  sh_offset;   // File offset of section data, in bytes
  Elf_Word sh_size;     // Size of section, in bytes
  Elf_Word sh_link;     // Section type-specific header table index link
  Elf_Word sh_info;     // Section type-specific extra information
  Elf_Word sh_addralign;// Section address alignment
  Elf_Word sh_entsize;  // Size of records contained within the section
};

template<support::endianness target_endianness>
struct Elf_Shdr_Base<target_endianness, true> {
  LLVM_ELF_IMPORT_TYPES(target_endianness, true)
  Elf_Word  sh_name;     // Section name (index into string table)
  Elf_Word  sh_type;     // Section type (SHT_*)
  Elf_Xword sh_flags;    // Section flags (SHF_*)
  Elf_Addr  sh_addr;     // Address where section is to be loaded
  Elf_Off   sh_offset;   // File offset of section data, in bytes
  Elf_Xword sh_size;     // Size of section, in bytes
  Elf_Word  sh_link;     // Section type-specific header table index link
  Elf_Word  sh_info;     // Section type-specific extra information
  Elf_Xword sh_addralign;// Section address alignment
  Elf_Xword sh_entsize;  // Size of records contained within the section
};

template<support::endianness target_endianness, bool is64Bits>
struct Elf_Shdr_Impl : Elf_Shdr_Base<target_endianness, is64Bits> {
  using Elf_Shdr_Base<target_endianness, is64Bits>::sh_entsize;
  using Elf_Shdr_Base<target_endianness, is64Bits>::sh_size;

  /// @brief Get the number of entities this section contains if it has any.
  unsigned getEntityCount() const {
    if (sh_entsize == 0)
      return 0;
    return sh_size / sh_entsize;
  }
};
}

namespace {
template<support::endianness target_endianness, bool is64Bits>
struct Elf_Sym_Base;

template<support::endianness target_endianness>
struct Elf_Sym_Base<target_endianness, false> {
  LLVM_ELF_IMPORT_TYPES(target_endianness, false)
  Elf_Word      st_name;  // Symbol name (index into string table)
  Elf_Addr      st_value; // Value or address associated with the symbol
  Elf_Word      st_size;  // Size of the symbol
  unsigned char st_info;  // Symbol's type and binding attributes
  unsigned char st_other; // Must be zero; reserved
  Elf_Half      st_shndx; // Which section (header table index) it's defined in
};

template<support::endianness target_endianness>
struct Elf_Sym_Base<target_endianness, true> {
  LLVM_ELF_IMPORT_TYPES(target_endianness, true)
  Elf_Word      st_name;  // Symbol name (index into string table)
  unsigned char st_info;  // Symbol's type and binding attributes
  unsigned char st_other; // Must be zero; reserved
  Elf_Half      st_shndx; // Which section (header table index) it's defined in
  Elf_Addr      st_value; // Value or address associated with the symbol
  Elf_Xword     st_size;  // Size of the symbol
};

template<support::endianness target_endianness, bool is64Bits>
struct Elf_Sym_Impl : Elf_Sym_Base<target_endianness, is64Bits> {
  using Elf_Sym_Base<target_endianness, is64Bits>::st_info;

  // These accessors and mutators correspond to the ELF32_ST_BIND,
  // ELF32_ST_TYPE, and ELF32_ST_INFO macros defined in the ELF specification:
  unsigned char getBinding() const { return st_info >> 4; }
  unsigned char getType() const { return st_info & 0x0f; }
  void setBinding(unsigned char b) { setBindingAndType(b, getType()); }
  void setType(unsigned char t) { setBindingAndType(getBinding(), t); }
  void setBindingAndType(unsigned char b, unsigned char t) {
    st_info = (b << 4) + (t & 0x0f);
  }
};
}

namespace {
template<support::endianness target_endianness, bool is64Bits, bool isRela>
struct Elf_Rel_Base;

template<support::endianness target_endianness>
struct Elf_Rel_Base<target_endianness, false, false> {
  LLVM_ELF_IMPORT_TYPES(target_endianness, false)
  Elf_Addr      r_offset; // Location (file byte offset, or program virtual addr)
  Elf_Word      r_info;  // Symbol table index and type of relocation to apply
};

template<support::endianness target_endianness>
struct Elf_Rel_Base<target_endianness, true, false> {
  LLVM_ELF_IMPORT_TYPES(target_endianness, true)
  Elf_Addr      r_offset; // Location (file byte offset, or program virtual addr)
  Elf_Xword     r_info;   // Symbol table index and type of relocation to apply
};

template<support::endianness target_endianness>
struct Elf_Rel_Base<target_endianness, false, true> {
  LLVM_ELF_IMPORT_TYPES(target_endianness, false)
  Elf_Addr      r_offset; // Location (file byte offset, or program virtual addr)
  Elf_Word      r_info;   // Symbol table index and type of relocation to apply
  Elf_Sword     r_addend; // Compute value for relocatable field by adding this
};

template<support::endianness target_endianness>
struct Elf_Rel_Base<target_endianness, true, true> {
  LLVM_ELF_IMPORT_TYPES(target_endianness, true)
  Elf_Addr      r_offset; // Location (file byte offset, or program virtual addr)
  Elf_Xword     r_info;   // Symbol table index and type of relocation to apply
  Elf_Sxword    r_addend; // Compute value for relocatable field by adding this.
};

template<support::endianness target_endianness, bool is64Bits, bool isRela>
struct Elf_Rel_Impl;

template<support::endianness target_endianness, bool isRela>
struct Elf_Rel_Impl<target_endianness, true, isRela>
       : Elf_Rel_Base<target_endianness, true, isRela> {
  using Elf_Rel_Base<target_endianness, true, isRela>::r_info;
  LLVM_ELF_IMPORT_TYPES(target_endianness, true)

  // These accessors and mutators correspond to the ELF64_R_SYM, ELF64_R_TYPE,
  // and ELF64_R_INFO macros defined in the ELF specification:
  uint64_t getSymbol() const { return (r_info >> 32); }
  unsigned char getType() const {
    return (unsigned char) (r_info & 0xffffffffL);
  }
  void setSymbol(uint64_t s) { setSymbolAndType(s, getType()); }
  void setType(unsigned char t) { setSymbolAndType(getSymbol(), t); }
  void setSymbolAndType(uint64_t s, unsigned char t) {
    r_info = (s << 32) + (t&0xffffffffL);
  }
};

template<support::endianness target_endianness, bool isRela>
struct Elf_Rel_Impl<target_endianness, false, isRela>
       : Elf_Rel_Base<target_endianness, false, isRela> {
  using Elf_Rel_Base<target_endianness, false, isRela>::r_info;
  LLVM_ELF_IMPORT_TYPES(target_endianness, false)

  // These accessors and mutators correspond to the ELF32_R_SYM, ELF32_R_TYPE,
  // and ELF32_R_INFO macros defined in the ELF specification:
  uint32_t getSymbol() const { return (r_info >> 8); }
  unsigned char getType() const { return (unsigned char) (r_info & 0x0ff); }
  void setSymbol(uint32_t s) { setSymbolAndType(s, getType()); }
  void setType(unsigned char t) { setSymbolAndType(getSymbol(), t); }
  void setSymbolAndType(uint32_t s, unsigned char t) {
    r_info = (s << 8) + t;
  }
};

}

namespace {
template<support::endianness target_endianness, bool is64Bits>
class ELFObjectFile : public ObjectFile {
  LLVM_ELF_IMPORT_TYPES(target_endianness, is64Bits)

  typedef Elf_Shdr_Impl<target_endianness, is64Bits> Elf_Shdr;
  typedef Elf_Sym_Impl<target_endianness, is64Bits> Elf_Sym;
  typedef Elf_Rel_Impl<target_endianness, is64Bits, false> Elf_Rel;
  typedef Elf_Rel_Impl<target_endianness, is64Bits, true> Elf_Rela;

  struct Elf_Ehdr {
    unsigned char e_ident[ELF::EI_NIDENT]; // ELF Identification bytes
    Elf_Half e_type;     // Type of file (see ET_*)
    Elf_Half e_machine;  // Required architecture for this file (see EM_*)
    Elf_Word e_version;  // Must be equal to 1
    Elf_Addr e_entry;    // Address to jump to in order to start program
    Elf_Off  e_phoff;    // Program header table's file offset, in bytes
    Elf_Off  e_shoff;    // Section header table's file offset, in bytes
    Elf_Word e_flags;    // Processor-specific flags
    Elf_Half e_ehsize;   // Size of ELF header, in bytes
    Elf_Half e_phentsize;// Size of an entry in the program header table
    Elf_Half e_phnum;    // Number of entries in the program header table
    Elf_Half e_shentsize;// Size of an entry in the section header table
    Elf_Half e_shnum;    // Number of entries in the section header table
    Elf_Half e_shstrndx; // Section header table index of section name
                                  // string table
    bool checkMagic() const {
      return (memcmp(e_ident, ELF::ElfMagic, strlen(ELF::ElfMagic))) == 0;
    }
    unsigned char getFileClass() const { return e_ident[ELF::EI_CLASS]; }
    unsigned char getDataEncoding() const { return e_ident[ELF::EI_DATA]; }
  };

  typedef SmallVector<const Elf_Shdr*, 1> Sections_t;
  typedef DenseMap<unsigned, unsigned> IndexMap_t;

  const Elf_Ehdr *Header;
  const Elf_Shdr *SectionHeaderTable;
  const Elf_Shdr *dot_shstrtab_sec; // Section header string table.
  const Elf_Shdr *dot_strtab_sec;   // Symbol header string table.
  Sections_t SymbolTableSections;
  IndexMap_t SymbolTableSectionsIndexMap;
  Sections_t RelocationTableSections;

  void            validateSymbol(DataRefImpl Symb) const;
  bool            isRelocationHasAddend(DataRefImpl Rel) const;
  template<typename T>
  const T        *getEntry(DataRefImpl Entry, Sections_t Sections) const;
  const Elf_Sym  *getSymbol(DataRefImpl Symb) const;
  const Elf_Shdr *getSection(DataRefImpl index) const;
  const Elf_Shdr *getSection(uint16_t index) const;
  const Elf_Rel  *getRel(DataRefImpl Rel) const;
  const Elf_Rela *getRela(DataRefImpl Rela) const;
  const char     *getString(uint16_t section, uint32_t offset) const;
  const char     *getString(const Elf_Shdr *section, uint32_t offset) const;

protected:
  virtual error_code getSymbolNext(DataRefImpl Symb, SymbolRef &Res) const;
  virtual error_code getSymbolName(DataRefImpl Symb, StringRef &Res) const;
  virtual error_code getSymbolOffset(DataRefImpl Symb, uint64_t &Res) const;
  virtual error_code getSymbolAddress(DataRefImpl Symb, uint64_t &Res) const;
  virtual error_code getSymbolSize(DataRefImpl Symb, uint64_t &Res) const;
  virtual error_code getSymbolNMTypeChar(DataRefImpl Symb, char &Res) const;
  virtual error_code isSymbolInternal(DataRefImpl Symb, bool &Res) const;
  virtual error_code isSymbolGlobal(DataRefImpl Symb, bool &Res) const;
  virtual error_code getSymbolType(DataRefImpl Symb, SymbolRef::SymbolType &Res) const;

  virtual error_code getSectionNext(DataRefImpl Sec, SectionRef &Res) const;
  virtual error_code getSectionName(DataRefImpl Sec, StringRef &Res) const;
  virtual error_code getSectionAddress(DataRefImpl Sec, uint64_t &Res) const;
  virtual error_code getSectionSize(DataRefImpl Sec, uint64_t &Res) const;
  virtual error_code getSectionContents(DataRefImpl Sec, StringRef &Res) const;
  virtual error_code isSectionText(DataRefImpl Sec, bool &Res) const;
  virtual error_code sectionContainsSymbol(DataRefImpl Sec, DataRefImpl Symb,
                                           bool &Result) const;

  virtual error_code getRelocationNext(DataRefImpl Rel,
                                       RelocationRef &Res) const;
  virtual error_code getRelocationAddress(DataRefImpl Rel,
                                          uint64_t &Res) const;
  virtual error_code getRelocationSymbol(DataRefImpl Rel,
                                         SymbolRef &Res) const;
  virtual error_code getRelocationType(DataRefImpl Rel,
                                       uint32_t &Res) const;
  virtual error_code getRelocationAdditionalInfo(DataRefImpl Rel,
                                                 int64_t &Res) const;

public:
  ELFObjectFile(MemoryBuffer *Object, error_code &ec);
  virtual symbol_iterator begin_symbols() const;
  virtual symbol_iterator end_symbols() const;
  virtual section_iterator begin_sections() const;
  virtual section_iterator end_sections() const;
  virtual relocation_iterator begin_relocations() const;
  virtual relocation_iterator end_relocations() const;

  virtual uint8_t getBytesInAddress() const;
  virtual StringRef getFileFormatName() const;
  virtual unsigned getArch() const;
};
} // end namespace

template<support::endianness target_endianness, bool is64Bits>
void ELFObjectFile<target_endianness, is64Bits>
                  ::validateSymbol(DataRefImpl Symb) const {
  const Elf_Sym  *symb = getSymbol(Symb);
  const Elf_Shdr *SymbolTableSection = SymbolTableSections[Symb.d.b];
  // FIXME: We really need to do proper error handling in the case of an invalid
  //        input file. Because we don't use exceptions, I think we'll just pass
  //        an error object around.
  if (!(  symb
        && SymbolTableSection
        && symb >= (const Elf_Sym*)(base()
                   + SymbolTableSection->sh_offset)
        && symb <  (const Elf_Sym*)(base()
                   + SymbolTableSection->sh_offset
                   + SymbolTableSection->sh_size)))
    // FIXME: Proper error handling.
    report_fatal_error("Symb must point to a valid symbol!");
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSymbolNext(DataRefImpl Symb,
                                        SymbolRef &Result) const {
  validateSymbol(Symb);
  const Elf_Shdr *SymbolTableSection = SymbolTableSections[Symb.d.b];

  ++Symb.d.a;
  // Check to see if we are at the end of this symbol table.
  if (Symb.d.a >= SymbolTableSection->getEntityCount()) {
    // We are at the end. If there are other symbol tables, jump to them.
    ++Symb.d.b;
    Symb.d.a = 1; // The 0th symbol in ELF is fake.
    // Otherwise return the terminator.
    if (Symb.d.b >= SymbolTableSections.size()) {
      Symb.d.a = std::numeric_limits<uint32_t>::max();
      Symb.d.b = std::numeric_limits<uint32_t>::max();
    }
  }

  Result = SymbolRef(Symb, this);
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSymbolName(DataRefImpl Symb,
                                        StringRef &Result) const {
  validateSymbol(Symb);
  const Elf_Sym  *symb = getSymbol(Symb);
  if (symb->st_name == 0) {
    const Elf_Shdr *section = getSection(symb->st_shndx);
    if (!section)
      Result = "";
    else
      Result = getString(dot_shstrtab_sec, section->sh_name);
    return object_error::success;
  }

  // Use the default symbol table name section.
  Result = getString(dot_strtab_sec, symb->st_name);
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSymbolOffset(DataRefImpl Symb,
                                           uint64_t &Result) const {
  validateSymbol(Symb);
  const Elf_Sym  *symb = getSymbol(Symb);
  const Elf_Shdr *Section;
  switch (symb->st_shndx) {
  case ELF::SHN_COMMON:
   // Undefined symbols have no address yet.
  case ELF::SHN_UNDEF:
    Result = UnknownAddressOrSize;
    return object_error::success;
  case ELF::SHN_ABS:
    Result = symb->st_value;
    return object_error::success;
  default: Section = getSection(symb->st_shndx);
  }

  switch (symb->getType()) {
  case ELF::STT_SECTION:
    Result = Section ? Section->sh_addr : UnknownAddressOrSize;
    return object_error::success;
  case ELF::STT_FUNC:
  case ELF::STT_OBJECT:
  case ELF::STT_NOTYPE:
    Result = symb->st_value;
    return object_error::success;
  default:
    Result = UnknownAddressOrSize;
    return object_error::success;
  }
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSymbolAddress(DataRefImpl Symb,
                                           uint64_t &Result) const {
  validateSymbol(Symb);
  const Elf_Sym  *symb = getSymbol(Symb);
  const Elf_Shdr *Section;
  switch (symb->st_shndx) {
  case ELF::SHN_COMMON: // Fall through.
   // Undefined symbols have no address yet.
  case ELF::SHN_UNDEF:
    Result = UnknownAddressOrSize;
    return object_error::success;
  case ELF::SHN_ABS:
    Result = reinterpret_cast<uintptr_t>(base()+symb->st_value);
    return object_error::success;
  default: Section = getSection(symb->st_shndx);
  }
  const uint8_t* addr = base();
  if (Section)
    addr += Section->sh_offset;
  switch (symb->getType()) {
  case ELF::STT_SECTION:
    Result = reinterpret_cast<uintptr_t>(addr);
    return object_error::success;
  case ELF::STT_FUNC: // Fall through.
  case ELF::STT_OBJECT: // Fall through.
  case ELF::STT_NOTYPE:
    addr += symb->st_value;
    Result = reinterpret_cast<uintptr_t>(addr);
    return object_error::success;
  default:
    Result = UnknownAddressOrSize;
    return object_error::success;
  }
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSymbolSize(DataRefImpl Symb,
                                        uint64_t &Result) const {
  validateSymbol(Symb);
  const Elf_Sym  *symb = getSymbol(Symb);
  if (symb->st_size == 0)
    Result = UnknownAddressOrSize;
  Result = symb->st_size;
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSymbolNMTypeChar(DataRefImpl Symb,
                                              char &Result) const {
  validateSymbol(Symb);
  const Elf_Sym  *symb = getSymbol(Symb);
  const Elf_Shdr *Section = getSection(symb->st_shndx);

  char ret = '?';

  if (Section) {
    switch (Section->sh_type) {
    case ELF::SHT_PROGBITS:
    case ELF::SHT_DYNAMIC:
      switch (Section->sh_flags) {
      case (ELF::SHF_ALLOC | ELF::SHF_EXECINSTR):
        ret = 't'; break;
      case (ELF::SHF_ALLOC | ELF::SHF_WRITE):
        ret = 'd'; break;
      case ELF::SHF_ALLOC:
      case (ELF::SHF_ALLOC | ELF::SHF_MERGE):
      case (ELF::SHF_ALLOC | ELF::SHF_MERGE | ELF::SHF_STRINGS):
        ret = 'r'; break;
      }
      break;
    case ELF::SHT_NOBITS: ret = 'b';
    }
  }

  switch (symb->st_shndx) {
  case ELF::SHN_UNDEF:
    if (ret == '?')
      ret = 'U';
    break;
  case ELF::SHN_ABS: ret = 'a'; break;
  case ELF::SHN_COMMON: ret = 'c'; break;
  }

  switch (symb->getBinding()) {
  case ELF::STB_GLOBAL: ret = ::toupper(ret); break;
  case ELF::STB_WEAK:
    if (symb->st_shndx == ELF::SHN_UNDEF)
      ret = 'w';
    else
      if (symb->getType() == ELF::STT_OBJECT)
        ret = 'V';
      else
        ret = 'W';
  }

  if (ret == '?' && symb->getType() == ELF::STT_SECTION) {
    StringRef name;
    if (error_code ec = getSymbolName(Symb, name))
      return ec;
    Result = StringSwitch<char>(name)
      .StartsWith(".debug", 'N')
      .StartsWith(".note", 'n')
      .Default('?');
    return object_error::success;
  }

  Result = ret;
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSymbolType(DataRefImpl Symb,
                                        SymbolRef::SymbolType &Result) const {
  validateSymbol(Symb);
  const Elf_Sym  *symb = getSymbol(Symb);

  if (symb->st_shndx == ELF::SHN_UNDEF) {
    Result = SymbolRef::ST_External;
    return object_error::success;
  }

  switch (symb->getType()) {
  case ELF::STT_FUNC:
    Result = SymbolRef::ST_Function;
    break;
  case ELF::STT_OBJECT:
    Result = SymbolRef::ST_Data;
    break;
  default:
    Result = SymbolRef::ST_Other;
    break;
  }
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::isSymbolGlobal(DataRefImpl Symb,
                                        bool &Result) const {
  validateSymbol(Symb);
  const Elf_Sym  *symb = getSymbol(Symb);

  Result = symb->getBinding() == ELF::STB_GLOBAL;
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::isSymbolInternal(DataRefImpl Symb,
                                           bool &Result) const {
  validateSymbol(Symb);
  const Elf_Sym  *symb = getSymbol(Symb);

  if (  symb->getType() == ELF::STT_FILE
     || symb->getType() == ELF::STT_SECTION)
    Result = true;
  Result = false;
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSectionNext(DataRefImpl Sec, SectionRef &Result) const {
  const uint8_t *sec = reinterpret_cast<const uint8_t *>(Sec.p);
  sec += Header->e_shentsize;
  Sec.p = reinterpret_cast<intptr_t>(sec);
  Result = SectionRef(Sec, this);
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSectionName(DataRefImpl Sec,
                                         StringRef &Result) const {
  const Elf_Shdr *sec = reinterpret_cast<const Elf_Shdr *>(Sec.p);
  Result = StringRef(getString(dot_shstrtab_sec, sec->sh_name));
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSectionAddress(DataRefImpl Sec,
                                            uint64_t &Result) const {
  const Elf_Shdr *sec = reinterpret_cast<const Elf_Shdr *>(Sec.p);
  Result = sec->sh_addr;
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSectionSize(DataRefImpl Sec,
                                         uint64_t &Result) const {
  const Elf_Shdr *sec = reinterpret_cast<const Elf_Shdr *>(Sec.p);
  Result = sec->sh_size;
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getSectionContents(DataRefImpl Sec,
                                             StringRef &Result) const {
  const Elf_Shdr *sec = reinterpret_cast<const Elf_Shdr *>(Sec.p);
  const char *start = (const char*)base() + sec->sh_offset;
  Result = StringRef(start, sec->sh_size);
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::isSectionText(DataRefImpl Sec,
                                        bool &Result) const {
  const Elf_Shdr *sec = reinterpret_cast<const Elf_Shdr *>(Sec.p);
  if (sec->sh_flags & ELF::SHF_EXECINSTR)
    Result = true;
  else
    Result = false;
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                          ::sectionContainsSymbol(DataRefImpl Sec,
                                                  DataRefImpl Symb,
                                                  bool &Result) const {
  // FIXME: Unimplemented.
  Result = false;
  return object_error::success;
}

// Relocations
template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getRelocationNext(DataRefImpl Rel,
                                            RelocationRef &Result) const {
  const Elf_Shdr *RelocationTableSection = RelocationTableSections[Rel.d.b];

  // Check to see if we are at the end of this relocation table.
  if (++Rel.d.a >= RelocationTableSection->getEntityCount()) {
    // We are at the end. If there are other relocation tables, jump to them.
    Rel.d.a = 0;
    // Otherwise return the terminator.
    if (++Rel.d.b >= SymbolTableSections.size()) {
      Rel.d.a = std::numeric_limits<uint32_t>::max();
      Rel.d.b = std::numeric_limits<uint32_t>::max();
    }
  }

  Result = RelocationRef(Rel, this);
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getRelocationSymbol(DataRefImpl Rel,
                                              SymbolRef &Result) const {
  uint32_t symbolIdx;
  const Elf_Shdr *sec = RelocationTableSections[Rel.d.b];
  switch (sec->sh_type) {
    default :
      report_fatal_error("Invalid section type in Rel!");
    case ELF::SHT_REL : {
      symbolIdx = getRel(Rel)->getSymbol();
      break;
    }
    case ELF::SHT_RELA : {
      symbolIdx = getRela(Rel)->getSymbol();
      break;
    }
  }
  DataRefImpl SymbolData;
  IndexMap_t::const_iterator it = SymbolTableSectionsIndexMap.find(sec->sh_link);
  if (it == SymbolTableSectionsIndexMap.end())
    report_fatal_error("Relocation symbol table not found!");
  SymbolData.d.a = symbolIdx;
  SymbolData.d.b = it->second;
  Result = SymbolRef(SymbolData, this);
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getRelocationAddress(DataRefImpl Rel,
                                               uint64_t &Result) const {
  uint64_t offset;
  const Elf_Shdr *sec = RelocationTableSections[Rel.d.b];
  switch (sec->sh_type) {
    default :
      report_fatal_error("Invalid section type in Rel!");
    case ELF::SHT_REL : {
      offset = getRel(Rel)->r_offset;
      break;
    }
    case ELF::SHT_RELA : {
      offset = getRela(Rel)->r_offset;
      break;
    }
  }

  const Elf_Shdr *secAddr = getSection(sec->sh_info);
  Result = offset + reinterpret_cast<uintptr_t>(base() + secAddr->sh_offset);
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getRelocationType(DataRefImpl Rel,
                                            uint32_t &Result) const {
  const Elf_Shdr *sec = RelocationTableSections[Rel.d.b];
  switch (sec->sh_type) {
    default :
      report_fatal_error("Invalid section type in Rel!");
    case ELF::SHT_REL : {
      Result = getRel(Rel)->getType();
      break;
    }
    case ELF::SHT_RELA : {
      Result = getRela(Rel)->getType();
      break;
    }
  }
  return object_error::success;
}

template<support::endianness target_endianness, bool is64Bits>
error_code ELFObjectFile<target_endianness, is64Bits>
                        ::getRelocationAdditionalInfo(DataRefImpl Rel,
                                                      int64_t &Result) const {
  const Elf_Shdr *sec = RelocationTableSections[Rel.d.b];
  switch (sec->sh_type) {
    default :
      report_fatal_error("Invalid section type in Rel!");
    case ELF::SHT_REL : {
      Result = 0;
      return object_error::success;
    }
    case ELF::SHT_RELA : {
      Result = getRela(Rel)->r_addend;
      return object_error::success;
    }
  }
}



template<support::endianness target_endianness, bool is64Bits>
ELFObjectFile<target_endianness, is64Bits>::ELFObjectFile(MemoryBuffer *Object
                                                          , error_code &ec)
  : ObjectFile(Binary::isELF, Object, ec)
  , SectionHeaderTable(0)
  , dot_shstrtab_sec(0)
  , dot_strtab_sec(0) {
  Header = reinterpret_cast<const Elf_Ehdr *>(base());

  if (Header->e_shoff == 0)
    return;

  SectionHeaderTable =
    reinterpret_cast<const Elf_Shdr *>(base() + Header->e_shoff);
  uint32_t SectionTableSize = Header->e_shnum * Header->e_shentsize;
  if (!(  (const uint8_t *)SectionHeaderTable + SectionTableSize
         <= base() + Data->getBufferSize()))
    // FIXME: Proper error handling.
    report_fatal_error("Section table goes past end of file!");


  // To find the symbol tables we walk the section table to find SHT_STMTAB.
  const Elf_Shdr* sh =
                    reinterpret_cast<const Elf_Shdr*>(SectionHeaderTable);
  for (unsigned i = 0; i < Header->e_shnum; ++i) {
    if (sh->sh_type == ELF::SHT_SYMTAB) {
      SymbolTableSectionsIndexMap[i] = SymbolTableSections.size();
      SymbolTableSections.push_back(sh);
    }
    if (sh->sh_type == ELF::SHT_REL || sh->sh_type == ELF::SHT_RELA) {
      RelocationTableSections.push_back(sh);
    }
    ++sh;
  }

  // Get string table sections.
  dot_shstrtab_sec = getSection(Header->e_shstrndx);
  if (dot_shstrtab_sec) {
    // Verify that the last byte in the string table in a null.
    if (((const char*)base() + dot_shstrtab_sec->sh_offset)
        [dot_shstrtab_sec->sh_size - 1] != 0)
      // FIXME: Proper error handling.
      report_fatal_error("String table must end with a null terminator!");
  }

  // Merge this into the above loop.
  for (const char *i = reinterpret_cast<const char *>(SectionHeaderTable),
                  *e = i + Header->e_shnum * Header->e_shentsize;
                   i != e; i += Header->e_shentsize) {
    const Elf_Shdr *sh = reinterpret_cast<const Elf_Shdr*>(i);
    if (sh->sh_type == ELF::SHT_STRTAB) {
      StringRef SectionName(getString(dot_shstrtab_sec, sh->sh_name));
      if (SectionName == ".strtab") {
        if (dot_strtab_sec != 0)
          // FIXME: Proper error handling.
          report_fatal_error("Already found section named .strtab!");
        dot_strtab_sec = sh;
        const char *dot_strtab = (const char*)base() + sh->sh_offset;
          if (dot_strtab[sh->sh_size - 1] != 0)
            // FIXME: Proper error handling.
            report_fatal_error("String table must end with a null terminator!");
      }
    }
  }
}

template<support::endianness target_endianness, bool is64Bits>
ObjectFile::symbol_iterator ELFObjectFile<target_endianness, is64Bits>
                                         ::begin_symbols() const {
  DataRefImpl SymbolData;
  memset(&SymbolData, 0, sizeof(SymbolData));
  if (SymbolTableSections.size() == 0) {
    SymbolData.d.a = std::numeric_limits<uint32_t>::max();
    SymbolData.d.b = std::numeric_limits<uint32_t>::max();
  } else {
    SymbolData.d.a = 1; // The 0th symbol in ELF is fake.
    SymbolData.d.b = 0;
  }
  return symbol_iterator(SymbolRef(SymbolData, this));
}

template<support::endianness target_endianness, bool is64Bits>
ObjectFile::symbol_iterator ELFObjectFile<target_endianness, is64Bits>
                                         ::end_symbols() const {
  DataRefImpl SymbolData;
  memset(&SymbolData, 0, sizeof(SymbolData));
  SymbolData.d.a = std::numeric_limits<uint32_t>::max();
  SymbolData.d.b = std::numeric_limits<uint32_t>::max();
  return symbol_iterator(SymbolRef(SymbolData, this));
}

template<support::endianness target_endianness, bool is64Bits>
ObjectFile::section_iterator ELFObjectFile<target_endianness, is64Bits>
                                          ::begin_sections() const {
  DataRefImpl ret;
  memset(&ret, 0, sizeof(DataRefImpl));
  ret.p = reinterpret_cast<intptr_t>(base() + Header->e_shoff);
  return section_iterator(SectionRef(ret, this));
}

template<support::endianness target_endianness, bool is64Bits>
ObjectFile::section_iterator ELFObjectFile<target_endianness, is64Bits>
                                          ::end_sections() const {
  DataRefImpl ret;
  memset(&ret, 0, sizeof(DataRefImpl));
  ret.p = reinterpret_cast<intptr_t>(base()
                                     + Header->e_shoff
                                     + (Header->e_shentsize * Header->e_shnum));
  return section_iterator(SectionRef(ret, this));
}

template<support::endianness target_endianness, bool is64Bits>
ObjectFile::relocation_iterator ELFObjectFile<target_endianness, is64Bits>
                                         ::begin_relocations() const {
  DataRefImpl RelData;
  memset(&RelData, 0, sizeof(RelData));
  if (RelocationTableSections.size() == 0) {
    RelData.d.a = std::numeric_limits<uint32_t>::max();
    RelData.d.b = std::numeric_limits<uint32_t>::max();
  } else {
    RelData.d.a = 0;
    RelData.d.b = 0;
  }
  return relocation_iterator(RelocationRef(RelData, this));
}

template<support::endianness target_endianness, bool is64Bits>
ObjectFile::relocation_iterator ELFObjectFile<target_endianness, is64Bits>
                                         ::end_relocations() const {
  DataRefImpl RelData;
  memset(&RelData, 0, sizeof(RelData));
  RelData.d.a = std::numeric_limits<uint32_t>::max();
  RelData.d.b = std::numeric_limits<uint32_t>::max();
  return relocation_iterator(RelocationRef(RelData, this));
}

template<support::endianness target_endianness, bool is64Bits>
uint8_t ELFObjectFile<target_endianness, is64Bits>::getBytesInAddress() const {
  return is64Bits ? 8 : 4;
}

template<support::endianness target_endianness, bool is64Bits>
StringRef ELFObjectFile<target_endianness, is64Bits>
                       ::getFileFormatName() const {
  switch(Header->e_ident[ELF::EI_CLASS]) {
  case ELF::ELFCLASS32:
    switch(Header->e_machine) {
    case ELF::EM_386:
      return "ELF32-i386";
    case ELF::EM_X86_64:
      return "ELF32-x86-64";
    case ELF::EM_ARM:
      return "ELF32-arm";
    default:
      return "ELF32-unknown";
    }
  case ELF::ELFCLASS64:
    switch(Header->e_machine) {
    case ELF::EM_386:
      return "ELF64-i386";
    case ELF::EM_X86_64:
      return "ELF64-x86-64";
    default:
      return "ELF64-unknown";
    }
  default:
    // FIXME: Proper error handling.
    report_fatal_error("Invalid ELFCLASS!");
  }
}

template<support::endianness target_endianness, bool is64Bits>
unsigned ELFObjectFile<target_endianness, is64Bits>::getArch() const {
  switch(Header->e_machine) {
  case ELF::EM_386:
    return Triple::x86;
  case ELF::EM_X86_64:
    return Triple::x86_64;
  case ELF::EM_ARM:
    return Triple::arm;
  default:
    return Triple::UnknownArch;
  }
}

template<support::endianness target_endianness, bool is64Bits>
template<typename T>
inline const T *
ELFObjectFile<target_endianness, is64Bits>::getEntry(DataRefImpl Entry,
                                                     Sections_t Sections) const {
  const Elf_Shdr *sec = Sections[Entry.d.b];
  return reinterpret_cast<const T *>(
           base()
           + sec->sh_offset
           + (Entry.d.a * sec->sh_entsize));
}

template<support::endianness target_endianness, bool is64Bits>
const typename ELFObjectFile<target_endianness, is64Bits>::Elf_Sym *
ELFObjectFile<target_endianness, is64Bits>::getSymbol(DataRefImpl Symb) const {
  return getEntry<Elf_Sym>(Symb, SymbolTableSections);
}

template<support::endianness target_endianness, bool is64Bits>
const typename ELFObjectFile<target_endianness, is64Bits>::Elf_Rel *
ELFObjectFile<target_endianness, is64Bits>::getRel(DataRefImpl Rel) const {
  return getEntry<Elf_Rel>(Rel, RelocationTableSections);
}

template<support::endianness target_endianness, bool is64Bits>
const typename ELFObjectFile<target_endianness, is64Bits>::Elf_Rela *
ELFObjectFile<target_endianness, is64Bits>::getRela(DataRefImpl Rela) const {
  return getEntry<Elf_Rela>(Rela, RelocationTableSections);
}

template<support::endianness target_endianness, bool is64Bits>
const typename ELFObjectFile<target_endianness, is64Bits>::Elf_Shdr *
ELFObjectFile<target_endianness, is64Bits>::getSection(DataRefImpl Symb) const {
  const Elf_Shdr *sec = getSection(Symb.d.b);
  if (sec->sh_type != ELF::SHT_SYMTAB)
    // FIXME: Proper error handling.
    report_fatal_error("Invalid symbol table section!");
  return sec;
}

template<support::endianness target_endianness, bool is64Bits>
const typename ELFObjectFile<target_endianness, is64Bits>::Elf_Shdr *
ELFObjectFile<target_endianness, is64Bits>::getSection(uint16_t index) const {
  if (index == 0 || index >= ELF::SHN_LORESERVE)
    return 0;
  if (!SectionHeaderTable || index >= Header->e_shnum)
    // FIXME: Proper error handling.
    report_fatal_error("Invalid section index!");

  return reinterpret_cast<const Elf_Shdr *>(
         reinterpret_cast<const char *>(SectionHeaderTable)
         + (index * Header->e_shentsize));
}

template<support::endianness target_endianness, bool is64Bits>
const char *ELFObjectFile<target_endianness, is64Bits>
                         ::getString(uint16_t section,
                                     ELF::Elf32_Word offset) const {
  return getString(getSection(section), offset);
}

template<support::endianness target_endianness, bool is64Bits>
const char *ELFObjectFile<target_endianness, is64Bits>
                         ::getString(const Elf_Shdr *section,
                                     ELF::Elf32_Word offset) const {
  assert(section && section->sh_type == ELF::SHT_STRTAB && "Invalid section!");
  if (offset >= section->sh_size)
    // FIXME: Proper error handling.
    report_fatal_error("Symbol name offset outside of string table!");
  return (const char *)base() + section->sh_offset + offset;
}

// EI_CLASS, EI_DATA.
static std::pair<unsigned char, unsigned char>
getElfArchType(MemoryBuffer *Object) {
  if (Object->getBufferSize() < ELF::EI_NIDENT)
    return std::make_pair((uint8_t)ELF::ELFCLASSNONE,(uint8_t)ELF::ELFDATANONE);
  return std::make_pair( (uint8_t)Object->getBufferStart()[ELF::EI_CLASS]
                       , (uint8_t)Object->getBufferStart()[ELF::EI_DATA]);
}

namespace llvm {

  ObjectFile *ObjectFile::createELFObjectFile(MemoryBuffer *Object) {
    std::pair<unsigned char, unsigned char> Ident = getElfArchType(Object);
    error_code ec;
    if (Ident.first == ELF::ELFCLASS32 && Ident.second == ELF::ELFDATA2LSB)
      return new ELFObjectFile<support::little, false>(Object, ec);
    else if (Ident.first == ELF::ELFCLASS32 && Ident.second == ELF::ELFDATA2MSB)
      return new ELFObjectFile<support::big, false>(Object, ec);
    else if (Ident.first == ELF::ELFCLASS64 && Ident.second == ELF::ELFDATA2LSB)
      return new ELFObjectFile<support::little, true>(Object, ec);
    else if (Ident.first == ELF::ELFCLASS64 && Ident.second == ELF::ELFDATA2MSB)
      return new ELFObjectFile<support::big, true>(Object, ec);
    // FIXME: Proper error handling.
    report_fatal_error("Not an ELF object file!");
  }

} // end namespace llvm
