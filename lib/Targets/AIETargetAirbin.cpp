//===- AIETargetAirbin.cpp --------------------------------------*- C++ -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2021 Xilinx Inc.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/None.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <ext/stdio_filebuf.h>
#include <fcntl.h> // open
#include <gelf.h>
#include <iostream>
#include <sstream>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> // read
#include <utility>  // pair
#include <vector>

#include "aie/Dialect/AIE/AIENetlistAnalysis.h"
#include "aie/Dialect/AIE/IR/AIEDialect.h"
#include "aie/Dialect/AIEX/IR/AIEXDialect.h"
#include "libelf.h"

// Marks a particular code path as unfinished.
#define TODO assert(false)

// Marks a particular code path as unused in normal execution.
#define UNREACHABLE assert(false)

#define DEBUG_AIRBIN
#ifdef DEBUG_AIRBIN
#define DBG_PRINTF printf
#else
#define DBG_PRINTF(...)
#endif // DEBUG_AIRBIN

namespace xilinx {
namespace AIE {

enum {
  SEC_IDX_NULL,
  SEC_IDX_SSMAST,
  SEC_IDX_SSSLVE,
  SEC_IDX_SSPCKT,
  SEC_IDX_SDMA_BD,
  SEC_IDX_SHMMUX,
  SEC_IDX_SDMA_CTL,
  SEC_IDX_PRGM_MEM,
  SEC_IDX_TDMA_BD,
  SEC_IDX_TDMA_CTL,
  SEC_IDX_DEPRECATED,
  SEC_IDX_DATA_MEM,
  SEC_IDX_MAX
};

static constexpr auto disable = 0u;
static constexpr auto enable = 1u;
static constexpr auto DMA_S2MM_CHANNEL_COUNT = 2u;
static constexpr auto DMA_MM2S_CHANNEL_COUNT = 2u;

static constexpr auto TILE_ADDR_OFF_WIDTH = 18u;

static constexpr auto TILE_ADDR_ROW_SHIFT = TILE_ADDR_OFF_WIDTH;
static constexpr auto TILE_ADDR_ROW_WIDTH = 5u;

static constexpr auto TILE_ADDR_COL_SHIFT =
    TILE_ADDR_ROW_SHIFT + TILE_ADDR_ROW_WIDTH;
static constexpr auto TILE_ADDR_COL_WIDTH = 7u;

static constexpr auto TILE_ADDR_ARR_SHIFT =
    TILE_ADDR_COL_SHIFT + TILE_ADDR_COL_WIDTH;

/*
        Tile DMA
*/
#define DMA_BD_COUNT 16

#define REG_DMA_BD_BLOCK_SIZE 0x20
#define REG_DMA_ADDR_A_BD(_idx) (0x1D000 + ((_idx)*REG_DMA_BD_BLOCK_SIZE) + 0x0)
#define REG_DMA_ADDR_B_BD(_idx) (0x1D000 + ((_idx)*REG_DMA_BD_BLOCK_SIZE) + 0x4)
#define REG_DMA_2D_X_BD(_idx) (0x1D000 + ((_idx)*REG_DMA_BD_BLOCK_SIZE) + 0x8)
#define REG_DMA_2D_Y_BD(_idx) (0x1D000 + ((_idx)*REG_DMA_BD_BLOCK_SIZE) + 0xC)
#define REG_DMA_PKT_BD(_idx) (0x1D000 + ((_idx)*REG_DMA_BD_BLOCK_SIZE) + 0x10)
#define REG_DMA_INT_STATE_BD(_idx)                                             \
  (0x1D000 + ((_idx)*REG_DMA_BD_BLOCK_SIZE) + 0x14)
#define REG_DMA_CTRL_BD(_idx) (0x1D000 + ((_idx)*REG_DMA_BD_BLOCK_SIZE) + 0x18)

// DMA S2MM channel control
#define REG_DMA_S2MM_BLOCK_SIZE 0x08
#define REG_DMA_S2MM_CTRL(_channel)                                            \
  (0x1DE00 + ((_channel)*REG_DMA_S2MM_BLOCK_SIZE) + 0x0)
#define REG_DMA_S2MM_QUEUE(_channel)                                           \
  (0x1DE00 + ((_channel)*REG_DMA_S2MM_BLOCK_SIZE) + 0x4)

// DMA MM2S channel control
#define REG_DMA_MM2S_BLOCK_SIZE 0x08
#define REG_DMA_MM2S_CTRL(_channel)                                            \
  (0x1DE10 + ((_channel)*REG_DMA_MM2S_BLOCK_SIZE) + 0x0)
#define REG_DMA_MM2S_QUEUE(_channel)                                           \
  (0x1DE10 + ((_channel)*REG_DMA_MM2S_BLOCK_SIZE) + 0x4)

/*
        Shim DMA
*/
#define SHIM_DMA_BD_COUNT 16
#define REG_SHIM_DMA_BD_SIZE 0x14
#define REG_SHIM_DMA_ADDR_LOW_BD(_idx)                                         \
  (0x1D000 + ((_idx)*REG_SHIM_DMA_BD_SIZE) + 0x0)

static constexpr auto REG_SHM_MUX = 0x1f000u; // a mux?

/*
        Common stream switch definitions
*/
static constexpr auto REG_SSM_CFG_0 = 0x3f000u;
static constexpr auto REG_SSS_CFG_0 = 0x3f100u;
static constexpr auto REG_SSS_CFG_SLOT_0 = 0x3f200u;
static constexpr auto SSS_CFG_SLOT_BLOCK_SIZE = 0x10;

/*
        ME stream switches
*/
static constexpr auto ME_SSM_BLOCK_SIZE = 0x64;
static constexpr auto ME_SSS_CFG_BLOCK_SIZE = 0x6C;
static constexpr auto ME_SSS_CFG_SLOT_COUNT = 26;

/*
        Shim stream switches
*/
static constexpr auto SHIM_SSM_BLOCK_SIZE = 0x5C;
static constexpr auto SHIM_SSS_CFG_BLOCK_SIZE = 0x60u;
static constexpr auto SHIM_SSS_CFG_SLOT_COUNT = 24;

// 32KB data memory
static constexpr auto DATA_MEM_OFFSET = 0x00000u;
static constexpr auto DATA_MEM_SIZE = 0x08000u;

// 16KB program memory
static constexpr auto PROG_MEM_OFFSET = 0x20000u;
static constexpr auto PROG_MEM_SIZE = 0x4000u;

static uint8_t sec_name_offset[SEC_IDX_MAX];

static const char *sec_name_str[SEC_IDX_MAX] = {
    "null",     ".ssmast",   ".ssslve",    ".sspckt",
    ".sdma.bd", ".shmmux",   ".sdma.ctl",  ".prgm.mem",
    ".tdma.bd", ".tdma.ctl", "deprecated", ".data.mem"};

static size_t stridx;

/*
        Holds a sorted list of all writes made to device memory
        All recorded writes are time/order invariant. This allows sorting to
   compact the airbin.
*/
static std::map<uint64_t, uint32_t> mem_writes;

/*
 * Tile address format:
 * --------------------------------------------
 * |                7 bits  5 bits   18 bits  |
 * --------------------------------------------
 * | Array offset | Column | Row | Tile addr  |
 * --------------------------------------------
 */
class TileAddress {
public:
  TileAddress(uint8_t column, uint8_t row, uint64_t array_offset = 0x000u)
      : array_offset{array_offset}, column{column}, row{row} {}

  // SFINAE is used here to choose the copy constructor for `TileAddress`,
  // and this constructor for all other classes.
  template <
      typename Op,
      std::enable_if_t<not std::is_same<Op, TileAddress>::value, bool> = true>
  TileAddress(Op &op)
      : TileAddress{static_cast<uint8_t>(op.colIndex()),
                    static_cast<uint8_t>(op.rowIndex())} {}

  uint64_t fullAddress(uint32_t register_offset) const {
    return (array_offset << TILE_ADDR_ARR_SHIFT) |
           (static_cast<uint64_t>(column) << TILE_ADDR_COL_SHIFT) |
           (static_cast<uint64_t>(row) << TILE_ADDR_ROW_SHIFT) |
           register_offset;
  }

  bool isShim() const { return row == 0; }

  operator uint16_t() const {
    return (static_cast<uint16_t>(column) << TILE_ADDR_ROW_WIDTH) | row;
  }

  uint8_t col() const { return column; }

  void clearRange(uint32_t range_start, uint32_t length);

private:
  uint64_t array_offset : 34;
  uint8_t column : TILE_ADDR_COL_WIDTH;
  uint8_t row : TILE_ADDR_ROW_WIDTH;
};

static_assert(sizeof(TileAddress) <= sizeof(uint64_t),
              "Tile addresses are at most 64-bits");

class Address {
public:
  Address(TileAddress tile, uint32_t offset) : tile{tile}, offset{offset} {}

  operator uint64_t() const { return tile.fullAddress(offset); }

  TileAddress destTile() const { return tile; }
  uint32_t get_offset() const { return offset; }

private:
  TileAddress tile;
  uint32_t offset : TILE_ADDR_OFF_WIDTH;
};

typedef std::pair<uint64_t, uint32_t> Write;

class Section {
public:
  Section(uint64_t addr) : address(addr){};
  uint64_t get_addr(void) const { return address; }
  size_t get_length(void) const { return data.size() * sizeof(uint32_t); }
  void add_data(uint32_t value) { data.push_back(value); }
  const uint32_t *get_data(void) const { return data.data(); }

private:
  uint64_t address;           // start address of this section
  std::vector<uint32_t> data; // data to be written starting at 'address'
};

// This template can be instantiated to represent a bitfield in a register.
template <uint8_t high_bit, uint8_t low_bit = high_bit> class Field final {
public:
  static_assert(high_bit >= low_bit,
                "The high bit should be higher than the low bit");
  static_assert(high_bit < sizeof(uint32_t) * 8u,
                "The field must live in a 32-bit register");

  static constexpr auto num_bits_used = (high_bit - low_bit) + 1u;
  static constexpr auto unshifted_mask = (1u << num_bits_used) - 1u;
  static_assert((low_bit != high_bit) xor (unshifted_mask == 1),
                "1 is a valid mask iff the field is 1 bit wide");

  static constexpr auto shifted_mask = unshifted_mask << low_bit;

  [[nodiscard]] constexpr uint32_t operator()(uint32_t value) const {
    return (value << low_bit) & shifted_mask;
  }
};

/*
        Add or replace a register value in 'mem_writes'
*/
static void write32(Address addr, uint32_t value) {
  // printf("%s: 0x%lx < 0x%x\n", __func__, static_cast<uint64_t>(addr), value);
  assert(addr.destTile().col() > 0);

  auto ret = mem_writes.emplace(addr, value);
  if (!ret.second)
    (ret.first)->second = value;
}

/*
        Look up a value for a given address

        If the address is found return the value, otherwise 0
*/
static uint32_t read32(Address addr) {
  auto ret = mem_writes.find(addr);
  if (ret != mem_writes.end())
    return ret->second;

  return 0;
}

/*
        Set every address in the range to 0
*/
void TileAddress::clearRange(uint32_t start, uint32_t length) {
  assert(start % 4 == 0);
  assert(length % 4 == 0);

  DBG_PRINTF("%s <%u,%u> 0x%x - 0x%x\n", __func__, column, row, start,
             start + length);

  for (auto off = start; off < start + length; off += 4u)
    write32(Address{*this, off}, 0);
}

// The SHIM row is always 0.
// SHIM resets are handled by the runtime.
static void config_shim_tile(TileOp &tileOp) {
  printf("%s\n", __func__);

  assert(tileOp.isShimTile() &&
         "The tile must be a Shim to generate Shim Config");

  TileAddress tileAddress{tileOp};

  if (tileOp.isShimNOCTile()) {
    tileAddress.clearRange(REG_SHIM_DMA_ADDR_LOW_BD(0),
                           REG_SHIM_DMA_BD_SIZE * SHIM_DMA_BD_COUNT);
  }
  if (tileOp.isShimNOCTile() or tileOp.isShimTile()) {
    tileAddress.clearRange(REG_SSM_CFG_0, SHIM_SSM_BLOCK_SIZE);
    tileAddress.clearRange(REG_SSS_CFG_0, SHIM_SSS_CFG_BLOCK_SIZE);
    tileAddress.clearRange(REG_SSS_CFG_SLOT_0,
                           SSS_CFG_SLOT_BLOCK_SIZE * SHIM_SSS_CFG_SLOT_COUNT);
  }
}

/*
   Read the ELF produced by the AIE compiler and include its loadable
   output in the airbin ELF
*/
static bool loadElf(TileAddress tile, const std::string &filename) {
  llvm::dbgs() << "Reading ELF file " << filename << " for tile " << tile
               << '\n';

  int elf_fd = open(filename.c_str(), O_RDONLY);
  if (elf_fd < 0) {
    printf("Can't open %s\n", filename.c_str());
    return false;
  }

  elf_version(EV_CURRENT);
  Elf *inelf = elf_begin(elf_fd, ELF_C_READ, NULL);

  // check the characteristics
  GElf_Ehdr *ehdr;
  GElf_Ehdr ehdr_mem;
  ehdr = gelf_getehdr(inelf, &ehdr_mem);
  if (ehdr == NULL) {
    printf("cannot get ELF header: %s\n", elf_errmsg(-1));
    exit(1);
  }

  // Read data as 32-bit little endian
  assert(ehdr->e_ident[EI_CLASS] == ELFCLASS32);
  assert(ehdr->e_ident[EI_DATA] == ELFDATA2LSB);

  size_t phnum;
  if (elf_getphdrnum(inelf, &phnum) != 0)
    printf("cannot get program header count: %s", elf_errmsg(-1));

  // iterate through all program headers
  for (unsigned int ndx = 0; ndx < phnum; ndx++) {
    GElf_Phdr phdr_mem;
    GElf_Phdr *phdr = gelf_getphdr(inelf, ndx, &phdr_mem);
    if (phdr == NULL)
      printf("cannot get program header entry %d: %s", ndx, elf_errmsg(-1));

    // for each loadable program header
    if (phdr->p_type != PT_LOAD)
      continue;

    // decide destination address based on header attributes
    uint32_t dest;
    if (phdr->p_flags & PF_X)
      dest = PROG_MEM_OFFSET + phdr->p_vaddr;
    else
      dest = DATA_MEM_OFFSET + (phdr->p_vaddr & (DATA_MEM_SIZE - 1));

    printf("ELF flags=0x%x vaddr=0x%lx dest=0x%x\r\n", phdr->p_flags,
           phdr->p_vaddr, dest);
    // read data one word at a time and write it to the output list
    // TODO since we know these are data and not registers, we could likely
    // bypass the output list and write a section directly into the AIRBIN
    size_t elfsize;
    uint32_t offset;
    char *raw = elf_rawfile(inelf, &elfsize);

    for (offset = phdr->p_offset; offset < phdr->p_offset + phdr->p_filesz;
         offset += 4) {
      Address dest_addr{tile, dest};
      uint32_t data = *(uint32_t *)(raw + offset);
      write32(dest_addr, data);
      dest += 4;
    }
  }

  elf_end(inelf);

  // close the file
  close(elf_fd);

  return true;
}

/*
        Generate the config for an ME tile
*/
static void config_ME_tile(TileOp tileOp) {
  printf("%s\n", __func__);
  TileAddress tileAddress{tileOp};
  // Reset configuration

  // clear program and data memory
  tileAddress.clearRange(PROG_MEM_OFFSET, PROG_MEM_SIZE);
  tileAddress.clearRange(DATA_MEM_OFFSET, DATA_MEM_SIZE);

  // TileDMA
  tileAddress.clearRange(REG_DMA_ADDR_A_BD(0),
                         DMA_BD_COUNT * REG_DMA_BD_BLOCK_SIZE);
  tileAddress.clearRange(REG_DMA_S2MM_CTRL(0),
                         DMA_S2MM_CHANNEL_COUNT * REG_DMA_S2MM_BLOCK_SIZE);
  tileAddress.clearRange(REG_DMA_MM2S_CTRL(0),
                         DMA_MM2S_CHANNEL_COUNT * REG_DMA_MM2S_BLOCK_SIZE);

  // Stream Switches
  tileAddress.clearRange(REG_SSM_CFG_0, ME_SSM_BLOCK_SIZE);
  tileAddress.clearRange(REG_SSS_CFG_0, ME_SSS_CFG_BLOCK_SIZE);
  tileAddress.clearRange(REG_SSS_CFG_SLOT_0,
                         SSS_CFG_SLOT_BLOCK_SIZE * ME_SSS_CFG_SLOT_COUNT);

  // NOTE: Here is usually where locking is done.
  // However, the runtime will handle that when loading the airbin.

  // read the AIE executable and copy the loadable parts
  if (auto coreOp = tileOp.getCoreOp()) {
    std::string fileName;
    if (auto fileAttr = coreOp->getAttrOfType<StringAttr>("elf_file")) {
      fileName = std::string(fileAttr.getValue());
    } else {
      std::stringstream ss;
      ss << "core_" << tileOp.colIndex() << '_' << tileOp.rowIndex() << ".elf";
      fileName = ss.str();
    }
    if (not loadElf(tileAddress, fileName)) {
      llvm::outs() << "Error loading " << fileName;
    }
  }
}

// Write the initial configuration for every tile specified in the MLIR.
static void configure_cores(DeviceOp &targetOp) {
  printf("%s\n", __func__);

  // set up each tile
  for (auto tileOp : targetOp.getOps<TileOp>()) {

    printf("CC: tile=<%u,%u>\n", static_cast<uint8_t>(tileOp.colIndex()),
           static_cast<uint8_t>(tileOp.rowIndex()));
    if (tileOp.isShimTile()) {
      config_shim_tile(tileOp);
    } else {
      config_ME_tile(tileOp);
    }
  }
}

struct BDInfo {
  bool foundBdPacket = false;
  int packetType = 0;
  int packetID = 0;
  bool foundBd = false;
  int lenA = 0;
  int lenB = 0;
  unsigned bytesA = 0;
  unsigned bytesB = 0;
  int offsetA = 0;
  int offsetB = 0;
  uint64_t BaseAddrA = 0;
  uint64_t BaseAddrB = 0;
  bool hasA = false;
  bool hasB = false;
  std::string bufA = "0";
  std::string bufB = "0";
  uint32_t AbMode = disable;
  uint32_t FifoMode = disable; // FIXME: when to enable FIFO mode?
};

static BDInfo getBDInfo(Block &block, const NetlistAnalysis &NL) {
  BDInfo bdInfo;
  for (auto op : block.getOps<DMABDOp>()) {
    bdInfo.foundBd = true;
    auto bufferType = op.getBuffer().getType().cast<::mlir::MemRefType>();

    if (op.isA()) {
      bdInfo.BaseAddrA =
          NL.getBufferBaseAddress(op.getBuffer().getDefiningOp());
      bdInfo.lenA = op.getLenValue();
      bdInfo.bytesA = bufferType.getElementTypeBitWidth() / 8u;
      bdInfo.offsetA = op.getOffsetValue();
      bdInfo.bufA = "XAIEDMA_TILE_BD_ADDRA";
      bdInfo.hasA = true;
    }

    if (op.isB()) {
      bdInfo.BaseAddrB =
          NL.getBufferBaseAddress(op.getBuffer().getDefiningOp());
      bdInfo.lenB = op.getLenValue();
      bdInfo.bytesB = bufferType.getElementTypeBitWidth() / 8u;
      bdInfo.offsetB = op.getOffsetValue();
      bdInfo.bufB = "XAIEDMA_TILE_BD_ADDRB";
      bdInfo.hasB = true;
    }
  }
  return bdInfo;
}

static void configure_dmas(DeviceOp &targetOp, NetlistAnalysis &NL) {
  printf("%s\n", __func__);
  Field<1> dmaChannelReset;
  Field<0> dmaChannelEnable;

  for (auto memOp : targetOp.getOps<MemOp>()) {
    TileAddress tile{memOp};
    printf("DMA: tile=<%u,%u>\n", static_cast<uint8_t>(memOp.colIndex()),
           static_cast<uint8_t>(memOp.rowIndex()));
    // Clear the CTRL and QUEUE registers for the DMA channels.
    for (auto chNum = 0u; chNum < DMA_S2MM_CHANNEL_COUNT; ++chNum) {
      write32({tile, REG_DMA_S2MM_CTRL(chNum)},
              dmaChannelReset(disable) | dmaChannelEnable(disable));
      write32({tile, REG_DMA_S2MM_QUEUE(chNum)}, 0);
    }
    for (auto chNum = 0u; chNum < DMA_MM2S_CHANNEL_COUNT; ++chNum) {
      write32({tile, REG_DMA_MM2S_CTRL(chNum)},
              dmaChannelReset(disable) | dmaChannelEnable(disable));
      write32({tile, REG_DMA_MM2S_QUEUE(chNum)}, 0);
    }

    DenseMap<Block *, int> blockMap;

    {
      // Assign each block a BD number
      auto bdNum = 0;
      for (auto &block : memOp.getBody()) {
        if (!block.getOps<DMABDOp>().empty()) {
          blockMap[&block] = bdNum;
          bdNum++;
        }
      }
    }

    for (auto &block : memOp.getBody()) {
      auto bdInfo = getBDInfo(block, NL);

      struct BdData {
        uint32_t addr_a{0};
        uint32_t addr_b{0};
        // The X register has some fields
        // which need to be nonzero in the default state.
        uint32_t x{0x00ff0001u};
        // The Y register has some fields
        // which need to be nonzero in the default state.
        uint32_t y{0xffff0100u};
        uint32_t packet{0};
        uint32_t interleave{0};
        uint32_t control{0};
      };

      if (bdInfo.hasA and bdInfo.hasB) {
        bdInfo.AbMode = enable;
        if (bdInfo.lenA != bdInfo.lenB)
          llvm::errs() << "ABmode must have matching lengths.\n";
        if (bdInfo.bytesA != bdInfo.bytesB)
          llvm::errs() << "ABmode must have matching element data types.\n";
      }

      int acqValue = 0, relValue = 0;
      auto acqEnable = disable;
      auto relEnable = disable;
      Optional<int> lockID = std::nullopt;

      for (auto op : block.getOps<UseLockOp>()) {
        LockOp lock = dyn_cast<LockOp>(op.getLock().getDefiningOp());
        lockID = lock.getLockIDValue();
        if (op.acquire()) {
          acqEnable = enable;
          acqValue = op.getLockValue();
        } else if (op.release()) {
          relEnable = enable;
          relValue = op.getLockValue();
        } else
          UNREACHABLE;
      }

      // We either
      //  a. went thru the loop once (`lockID` should be something) xor
      //  b. did not enter the loop (the enables should be both disable)
      assert(lockID.has_value() xor
             (acqEnable == disable and relEnable == disable));

      for (auto op : block.getOps<DMABDPACKETOp>()) {
        bdInfo.foundBdPacket = true;
        bdInfo.packetType = op.getPacketType();
        bdInfo.packetID = op.getPacketID();
      }

      auto bdNum = blockMap[&block];
      BdData bdData;
      if (bdInfo.foundBd) {
        Field<25, 22> bdAddressLockID;
        Field<21> bdAddressReleaseEnable;
        Field<20> bdAddressReleaseValue;
        Field<19> bdAddressReleaseValueEnable;
        Field<18> bdAddressAcquireEnable;
        Field<17> bdAddressAcquireValue;
        Field<16> bdAddressAcquireValueEnable;

        if (bdInfo.hasA) {
          bdData.addr_a = bdAddressLockID(lockID.value()) |
                          bdAddressReleaseEnable(relEnable) |
                          bdAddressAcquireEnable(acqEnable);

          if (relValue != 0xFFu) {
            bdData.addr_a |= bdAddressReleaseValueEnable(true) |
                             bdAddressReleaseValue(relValue);
          }
          if (acqValue != 0xFFu) {
            bdData.addr_a |= bdAddressAcquireValueEnable(true) |
                             bdAddressAcquireValue(acqValue);
          }
        }
        if (bdInfo.hasB) {
          TODO;
        }

        auto addr_a = bdInfo.BaseAddrA + bdInfo.offsetA;
        auto addr_b = bdInfo.BaseAddrB + bdInfo.offsetB;

        Field<12, 0> bdAddressBase, bdControlLength;

        Field<30> bdControlABMode;
        Field<28> bdControlFifo;

        bdData.addr_a |= bdAddressBase(addr_a >> 2u);
        bdData.addr_b |= bdAddressBase(addr_b >> 2u);
        bdData.control |= bdControlLength(bdInfo.lenA - 1) |
                          bdControlFifo(bdInfo.FifoMode) |
                          bdControlABMode(bdInfo.AbMode);

        if (block.getNumSuccessors() > 0) {
          // should have only one successor block
          assert(block.getNumSuccessors() == 1);
          auto *nextBlock = block.getSuccessors()[0];
          auto nextBdNum = blockMap[nextBlock];

          Field<16, 13> bdControlNextBD;
          Field<17> bdControlEnableNextBD;

          bdData.control |= bdControlEnableNextBD(nextBdNum != 0xFFu) |
                            bdControlNextBD(nextBdNum);
        }

        if (bdInfo.foundBdPacket) {

          Field<14, 12> bdPacketType;
          Field<4, 0> bdPacketID;

          Field<27> bdControlEnablePacket;

          bdData.packet =
              bdPacketID(bdInfo.packetID) | bdPacketType(bdInfo.packetType);

          bdData.control |= bdControlEnablePacket(enable);
        }

        Field<31> bdControlValid;

        uint32_t bdOffset = REG_DMA_ADDR_A_BD(bdNum);
        assert(bdOffset < REG_DMA_ADDR_A_BD(DMA_BD_COUNT));

        write32({tile, bdOffset}, bdData.addr_a);
        write32({tile, bdOffset + 4u}, bdData.addr_b);
        write32({tile, bdOffset + 8u}, bdData.x);
        write32({tile, bdOffset + 0xCu}, bdData.y);
        write32({tile, bdOffset + 0x10u}, bdData.packet);
        write32({tile, bdOffset + 0x14u}, bdData.interleave);
        write32({tile, bdOffset + 0x18u},
                bdData.control | bdControlValid(true));
      }
    }

    for (auto &block : memOp.getBody()) {
      for (auto op : block.getOps<DMAStartOp>()) {
        auto bdNum = blockMap[op.getDest()];
        if (bdNum != 0xFFU) {
          Field<4, 0> dmaChannelQueueStartBd;

          uint32_t chNum = op.getChannelIndex();
          if (op.getChannelDir() == DMAChannelDir::MM2S) {
            write32(Address{tile, REG_DMA_MM2S_QUEUE(chNum)},
                    dmaChannelQueueStartBd(bdNum));
            write32({tile, REG_DMA_MM2S_CTRL(chNum)},
                    dmaChannelEnable(enable) | dmaChannelReset(disable));
          } else {
            write32(Address{tile, REG_DMA_S2MM_QUEUE(chNum)},
                    dmaChannelQueueStartBd(bdNum));
            write32({tile, REG_DMA_S2MM_CTRL(chNum)},
                    dmaChannelEnable(enable) | dmaChannelReset(disable));
          }
        }
      }
    }
  }
}

static uint8_t computeSlavePort(WireBundle bundle, int index, bool isShim) {
  assert(index >= 0);
  assert(index < UINT8_MAX - 21);

  switch (bundle) {
  case WireBundle::DMA:
    return 2u + index;
  case WireBundle::East:
    if (isShim)
      return 19u + index;
    else
      return 21u + index;
  case WireBundle::North:
    if (isShim)
      return 15u + index;
    else
      return 17u + index;
  case WireBundle::South:
    if (isShim)
      return 3u + index;
    else
      return 7u + index;
  case WireBundle::West:
    if (isShim)
      return 11u + index;
    else
      return 13u + index;
  default:
    // To implement a new WireBundle,
    // look in libXAIE for the macros that handle the port.
    TODO;
  }
}

static uint8_t computeMasterPort(WireBundle bundle, int index, bool isShim) {
  assert(index >= 0);
  assert(index < UINT8_MAX - 21);

  switch (bundle) {
  case WireBundle::DMA:
    return 2u + index;
  case WireBundle::East:
    if (isShim)
      return 19u + index;
    else
      return 21u + index;
  case WireBundle::North:
    if (isShim)
      return 13u + index;
    else
      return 15u + index;
  case WireBundle::South:
    if (isShim)
      return 3u + index;
    else
      return 7u + index;
  case WireBundle::West:
    if (isShim)
      return 9u + index;
    else
      return 11u + index;
  default:
    // To implement a new WireBundle,
    // look in libXAIE for the macros that handle the port.
    TODO;
  }
}

static void configure_switchboxes(DeviceOp &targetOp) {
  printf("%s\n", __func__);
  for (auto switchboxOp : targetOp.getOps<SwitchboxOp>()) {
    Region &r = switchboxOp.getConnections();
    Block &b = r.front();
    bool isEmpty = b.getOps<ConnectOp>().empty() &&
                   b.getOps<MasterSetOp>().empty() &&
                   b.getOps<PacketRulesOp>().empty();

    // NOTE: may not be needed
    auto switchbox_set = [&] {
      std::set<TileAddress> result;
      if (isa<TileOp>(switchboxOp.getTile().getDefiningOp())) {
        if (!isEmpty) {
          result.emplace(switchboxOp);
        }
      } else if (AIEX::SelectOp sel = dyn_cast<AIEX::SelectOp>(
                     switchboxOp.getTile().getDefiningOp())) {
        // TODO: Use XAIEV1 target and translate into write32s
        TODO;
      }

      return result;
    }();

    constexpr Field<31> streamEnable;
    constexpr Field<30> streamPacketEnable;
    for (auto connectOp : b.getOps<ConnectOp>()) {
      for (auto tile : switchbox_set) {

        auto slave_port =
            computeSlavePort(connectOp.getSourceBundle(),
                             connectOp.sourceIndex(), tile.isShim());

        auto master_port = computeMasterPort(
            connectOp.getDestBundle(), connectOp.destIndex(), tile.isShim());

        Field<7> streamMasterDropHeader;
        Field<6, 0> streamMasterConfig;

        // Configure master side
        {
          Address address{tile, 0x3F000u + master_port * 4u};

          // TODO: `Field::extract(uint32_t)`?
          auto drop_header = (slave_port & 0x80u) >> 7u;

          auto value = streamEnable(true) | streamPacketEnable(false) |
                       streamMasterDropHeader(drop_header) |
                       streamMasterConfig(slave_port);
          assert(value < UINT32_MAX);
          write32(address, value);
        }

        // Configure slave side
        {
          Address address{tile, 0x3F100u + slave_port * 4u};

          write32(address, streamEnable(true) | streamPacketEnable(false));
        }

        for (auto connectOp : b.getOps<MasterSetOp>()) {
          auto mask = 0u;
          int arbiter = -1;
          for (auto val : connectOp.getAmsels()) {
            auto amsel = dyn_cast<AMSelOp>(val.getDefiningOp());
            arbiter = amsel.arbiterIndex();
            int msel = amsel.getMselValue();
            mask |= (1u << msel);
          }

          static constexpr auto STREAM_SWITCH_MSEL_SHIFT = 3u;
          static constexpr auto STREAM_SWITCH_ARB_SHIFT = 0u;

          const auto dropHeader = connectOp.getDestBundle() == WireBundle::DMA;
          auto config = streamMasterDropHeader(dropHeader) |
                        (mask << STREAM_SWITCH_MSEL_SHIFT) |
                        (arbiter << STREAM_SWITCH_ARB_SHIFT);

          Address dest{tile, REG_SSM_CFG_0 + 4u * master_port};

          write32(dest, streamEnable(enable) | streamPacketEnable(enable) |
                            streamMasterDropHeader(dropHeader) |
                            streamMasterConfig(config));
        }
      }
    }

    for (auto connectOp : b.getOps<PacketRulesOp>()) {
      int slot = 0;
      Block &block = connectOp.getRules().front();
      for (auto slotOp : block.getOps<PacketRuleOp>()) {
        AMSelOp amselOp = dyn_cast<AMSelOp>(slotOp.getAmsel().getDefiningOp());
        int arbiter = amselOp.arbiterIndex();
        int msel = amselOp.getMselValue();

        for (auto tile : switchbox_set) {
          auto slavePort =
              computeSlavePort(connectOp.getSourceBundle(),
                               connectOp.sourceIndex(), tile.isShim());
          write32({tile, REG_SSS_CFG_0 + 4u * slavePort},
                  streamEnable(enable) | streamPacketEnable(enable));

          static constexpr auto STREAM_NUM_SLOTS = 4u;

          Field<28, 24> streamSlotId;
          Field<20, 16> streamSlotMask;
          Field<8> streamSlotEnable;
          Field<5, 4> streamSlotMSel;
          Field<2, 0> streamSlotArbit;

          auto config = streamSlotId(slotOp.valueInt()) |
                        streamSlotMask(slotOp.maskInt()) |
                        streamSlotEnable(enable) | streamSlotMSel(msel) |
                        streamSlotArbit(arbiter);

          write32(
              {tile, REG_SSS_CFG_SLOT_0 + STREAM_NUM_SLOTS * slavePort + slot},
              config);
          slot++;
        }
      }
    }
  }

  Optional<TileAddress> currentTile = std::nullopt;
  for (auto op : targetOp.getOps<ShimMuxOp>()) {
    Region &r = op.getConnections();
    Block &b = r.front();

    if (isa<TileOp>(op.getTile().getDefiningOp())) {
      bool isEmpty = b.getOps<ConnectOp>().empty();
      if (!isEmpty) {
        currentTile = op;
      }
    }

    for (auto connectOp : b.getOps<ConnectOp>()) {

      const auto inputMaskFor = [](WireBundle bundle, uint8_t shiftAmt) {
        switch (bundle) {
        case WireBundle::PLIO:
          return 0u << shiftAmt;
        case WireBundle::DMA:
          return 1u << shiftAmt;
        case WireBundle::NOC:
          return 2u << shiftAmt;
        default:
          UNREACHABLE;
        }
      };

      if (connectOp.getSourceBundle() == WireBundle::North) {
        // demux!
        // XAieTile_ShimStrmDemuxConfig(&(TileInst[col][0]),
        // XAIETILE_SHIM_STRM_DEM_SOUTH3, XAIETILE_SHIM_STRM_DEM_DMA);
        assert(currentTile.has_value());

        auto shiftAmt = [index = connectOp.sourceIndex()] {
          // NOTE: hardcoded to SOUTH to match definitions from libxaie
          switch (index) {
          case 2:
            return 4u;
          case 3:
            return 6u;
          case 6:
            return 8u;
          case 7:
            return 10u;
          default:
            UNREACHABLE; // Unsure about this, but seems safe to assume
          }
        }();

        // We need to add to the possibly preexisting mask.
        Address addr{currentTile.value(), 0x1F004u};
        auto currentMask = read32(addr);

        write32(addr, currentMask |
                          inputMaskFor(connectOp.getDestBundle(), shiftAmt));

      } else if (connectOp.getDestBundle() == WireBundle::North) {
        // mux
        // XAieTile_ShimStrmMuxConfig(&(TileInst[col][0]),
        // XAIETILE_SHIM_STRM_MUX_SOUTH3, XAIETILE_SHIM_STRM_MUX_DMA);
        assert(currentTile.has_value());

        auto shiftAmt = [index = connectOp.destIndex()] {
          // NOTE: hardcoded to SOUTH to match definitions from libxaie
          switch (index) {
          case 2:
            return 8u;
          case 3:
            return 10u;
          case 6:
            return 12u;
          case 7:
            return 14u;
          default:
            UNREACHABLE; // Unsure about this, but seems safe to assume
          }
        }();

        Address addr{currentTile.value(), 0x1F000u};
        auto currentMask = read32(addr);

        write32(addr, currentMask |
                          inputMaskFor(connectOp.getSourceBundle(), shiftAmt));
      }
    }
  }

  /* TODO: Implement the following
  for (auto switchboxOp : targetOp.getOps<ShimSwitchboxOp>()) {
    Region &r = switchboxOp.getConnections();
    Block &b = r.front();
    for (auto connectOp : b.getOps<ConnectOp>()) {
      output << "XAieTile_StrmConnectCct(" << tileInstStr(col, 0) << ",\n";
      output << "\tXAIETILE_STRSW_SPORT_"
             << stringifyWireBundle(connectOp.sourceBundle()).upper() << "("
             << tileInstStr(col, 0) << ", " << connectOp.sourceIndex()
             << "),\n";
      output << "\tXAIETILE_STRSW_MPORT_"
             << stringifyWireBundle(connectOp.destBundle()).upper() << "("
             << tileInstStr(col, 0) << ", " << connectOp.destIndex() << "),\n";
      output << "\t" << enable << ");\n";
    }
  }
  */
}

/*
        Convert memory address to index

        Used to look up register/region name
*/
static uint8_t sec_addr2index(uint64_t in) {
  switch (in & ((1 << TILE_ADDR_OFF_WIDTH) - 1)) {
  case 0:
    return SEC_IDX_DATA_MEM;
  case REG_SSM_CFG_0:
    return SEC_IDX_SSMAST;
  case REG_SSS_CFG_0:
    return SEC_IDX_SSSLVE;
  case REG_SSS_CFG_SLOT_0:
    return SEC_IDX_SSPCKT;
  case REG_DMA_ADDR_A_BD(0):
    return SEC_IDX_SDMA_BD;
  case REG_SHM_MUX:
    return SEC_IDX_SHMMUX;
  case REG_DMA_ADDR_A_BD(10):
    return SEC_IDX_SDMA_CTL;
  case PROG_MEM_OFFSET:
    return SEC_IDX_PRGM_MEM;
  case REG_DMA_S2MM_CTRL(0):
    return SEC_IDX_TDMA_CTL;
  default:
    return 0;
  }
  return 0;
}

/*
        Group the writes into contiguous sections
*/
static void group_sections(std::vector<Section *> &sections) {
  uint64_t last_addr = 0;
  Section *section = NULL;

  for (auto write : mem_writes) {
    if (write.first != last_addr + 4) {
      if (section)
        sections.push_back(section);
      section = new Section(write.first);
      printf("Starting new section @ 0x%lx (last=0x%lx)\n", write.first,
             last_addr);
    }
    section->add_data(write.second);
    last_addr = write.first;
  }
  sections.push_back(section);
}

/*
   Add a string to the section header string table and return the offset of
   the start of the string
*/
static size_t add_string(Elf_Scn *scn, const char *str) {
  size_t lastidx = stridx;
  size_t size = strlen(str) + 1;

  Elf_Data *data = elf_newdata(scn);
  if (data == NULL) {
    printf("cannot create data SHSTRTAB section: %s\n", elf_errmsg(-1));
    return -1;
  }

  data->d_buf = (void *)str;
  data->d_type = ELF_T_BYTE;
  data->d_size = size;
  data->d_align = 1;
  data->d_version = EV_CURRENT;

  stridx += size;
  return lastidx;
}

Elf_Data *section_add_data(Elf_Scn *scn, const Section *section) {
  size_t size = section->get_length();
  uint32_t *buf = (uint32_t *)malloc(size);
  if (!buf)
    return NULL;

  // create a data object for the section
  Elf_Data *data = elf_newdata(scn);
  if (!data) {
    printf("cannot add data to section: %s\n", elf_errmsg(-1));
    free(data);
    return NULL;
  }

  data->d_buf = buf;
  data->d_type = ELF_T_BYTE;
  data->d_size = size;
  data->d_off = 0;
  data->d_align = 1;
  data->d_version = EV_CURRENT;

  // fill the data
  memcpy(buf, section->get_data(), size);

  return data;
}

mlir::LogicalResult AIETranslateToAirbin(mlir::ModuleOp module,
                                         llvm::raw_ostream &output) {
  int tmp_elf_fd;
  Elf *outelf;
  GElf_Ehdr ehdr_mem;
  GElf_Ehdr *ehdr;
  GElf_Shdr *shdr;
  GElf_Shdr shdr_mem;
  char empty_str[] = "";
  char strtab_name[] = ".shstrtab";
  std::vector<Section *> sections;

  assert(not output.is_displayed());

  DenseMap<std::pair<int, int>, Operation *> tiles;
  DenseMap<Operation *, CoreOp> cores;
  DenseMap<Operation *, MemOp> mems;
  DenseMap<std::pair<Operation *, int>, LockOp> locks;
  DenseMap<Operation *, SmallVector<BufferOp, 4>> buffers;
  DenseMap<Operation *, SwitchboxOp> switchboxes;

  if (module.getOps<DeviceOp>().empty()) {
    module.emitOpError("no operations found");
  }

  DeviceOp targetOp = *(module.getOps<DeviceOp>().begin());

  NetlistAnalysis NL(targetOp, tiles, cores, mems, locks, buffers, switchboxes);
  NL.collectTiles(tiles);
  NL.collectBuffers(buffers);

  configure_cores(targetOp);
  configure_switchboxes(targetOp);
  configure_dmas(targetOp, NL);

  group_sections(sections);

  printf("mem_writes: %lu in %lu sections\n", mem_writes.size(),
         sections.size());

  elf_version(EV_CURRENT);
  tmp_elf_fd = open("airbin.elf", O_RDWR | O_CREAT | O_TRUNC, DEFFILEMODE);
  outelf = elf_begin(tmp_elf_fd, ELF_C_WRITE, NULL);

  if (gelf_newehdr(outelf, ELFCLASS64) == 0) {
    printf("Error creating ELF64 header: %s\n", elf_errmsg(-1));
    return success();
  }

  ehdr = gelf_getehdr(outelf, &ehdr_mem);
  if (ehdr == NULL) {
    printf("cannot get ELF header: %s\n", elf_errmsg(-1));
    exit(1);
  }

  // Initialize header.
  ehdr->e_ident[EI_DATA] = ELFDATA2LSB;
  ehdr->e_ident[EI_OSABI] = ELFOSABI_GNU;
  ehdr->e_type = ET_NONE;
  ehdr->e_machine = EM_AMDAIR;
  ehdr->e_version = EV_CURRENT;
  if (gelf_update_ehdr(outelf, ehdr) == 0) {
    printf("cannot update ELF header: %s\n", elf_errmsg(-1));
    exit(1);
  }

  // Create new section for the 'section header string table'
  Elf_Scn *shstrtab_scn = elf_newscn(outelf);
  if (shstrtab_scn == NULL) {
    printf("cannot create new shstrtab section: %s\n", elf_errmsg(-1));
    exit(1);
  }

  // the first entry in the string table must be a NULL string
  add_string(shstrtab_scn, empty_str);

  shdr = gelf_getshdr(shstrtab_scn, &shdr_mem);
  if (shdr == NULL) {
    printf("cannot get header for sh_strings section: %s\n", elf_errmsg(-1));
    exit(1);
  }

  shdr->sh_type = SHT_STRTAB;
  shdr->sh_flags = 0;
  shdr->sh_addr = 0;
  shdr->sh_link = SHN_UNDEF;
  shdr->sh_info = SHN_UNDEF;
  shdr->sh_addralign = 1;
  shdr->sh_entsize = 0;
  shdr->sh_name = add_string(shstrtab_scn, strtab_name);

  // add all the AIRBIN-specific section names up front and index them
  for (uint8_t sec_idx = SEC_IDX_SSMAST; sec_idx < SEC_IDX_MAX; sec_idx++)
    sec_name_offset[sec_idx] = add_string(shstrtab_scn, sec_name_str[sec_idx]);
  sec_name_offset[SEC_IDX_NULL] = 0;

  // We have to store the section strtab index in the ELF header so sections
  // have actual names.
  int ndx = elf_ndxscn(shstrtab_scn);
  ehdr->e_shstrndx = ndx;

  if (gelf_update_ehdr(outelf, ehdr) == 0) {
    printf("cannot update ELF header: %s\n", elf_errmsg(-1));
    exit(1);
  }

  // Finished new shstrtab section, update the header.
  if (gelf_update_shdr(shstrtab_scn, shdr) == 0) {
    printf("cannot update new shstrtab section header: %s\n", elf_errmsg(-1));
    exit(1);
  }

  // output the rest of the sections
  for (const Section *section : sections) {
    uint64_t addr = section->get_addr();
    Elf_Scn *scn = elf_newscn(outelf);
    if (!scn) {
      printf("cannot create new %s section: %s\n",
             sec_name_str[sec_addr2index(addr)], elf_errmsg(-1));
      break;
    }

    shdr = gelf_getshdr(scn, &shdr_mem);
    if (!shdr) {
      printf("cannot get header for %s section: %s\n",
             sec_name_str[sec_addr2index(addr)], elf_errmsg(-1));
      break;
    }

    Elf_Data *data = section_add_data(scn, section);

    shdr->sh_type = SHT_PROGBITS;
    shdr->sh_flags = SHF_ALLOC;
    shdr->sh_addr = section->get_addr();
    shdr->sh_link = SHN_UNDEF;
    shdr->sh_info = SHN_UNDEF;
    shdr->sh_addralign = 1;
    shdr->sh_entsize = 0;
    shdr->sh_size = data->d_size;
    shdr->sh_name = sec_name_offset[sec_addr2index(addr)];

    if (!gelf_update_shdr(scn, shdr)) {
      printf("cannot update section header: %s\n", elf_errmsg(-1));
      break;
    }
  }

  // Write everything to disk.
  if (elf_update(outelf, ELF_C_WRITE) < 0) {
    printf("failure in elf_update: %s\n", elf_errmsg(-1));
    exit(1);
  }

  // close the elf object
  elf_end(outelf);

  // copy the file to the compiler's output stream
  /*
          lseek(tmp_elf_fd, 0, SEEK_SET);
          __gnu_cxx::stdio_filebuf<char> filebuf(tmp_elf_fd, std::ios::in);
          std::istream is(&filebuf);
          output << is.rdbuf();
  */

  close(tmp_elf_fd);

  std::ifstream is;
  is.open("airbin.elf", std::ios::in | std::ios::binary);
  output << is.rdbuf();
  is.close();

  return success();
}
} // namespace AIE
} // namespace xilinx
