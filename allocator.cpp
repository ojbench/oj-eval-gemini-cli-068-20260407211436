#include "allocator.hpp"
#include <cstdlib>
#include <algorithm>

TLSFAllocator::TLSFAllocator(std::size_t memoryPoolSize) {
    poolSize = memoryPoolSize;
    memoryPool = std::malloc(poolSize);
    
    index.fliBitmap = 0;
    index.sliBitmaps.fill(0);
    for (auto& fl : index.freeLists) {
        fl.fill(nullptr);
    }
    
    initializeMemoryPool(poolSize);
}

TLSFAllocator::~TLSFAllocator() {
    std::free(memoryPool);
}

void TLSFAllocator::initializeMemoryPool(std::size_t size) {
    if (size < sizeof(FreeBlock)) return;
    
    FreeBlock* initialBlock = static_cast<FreeBlock*>(memoryPool);
    initialBlock->data = reinterpret_cast<char*>(memoryPool) + sizeof(BlockHeader);
    initialBlock->size = size;
    initialBlock->isFree = true;
    initialBlock->prevPhysBlock = nullptr;
    initialBlock->nextPhysBlock = nullptr;
    initialBlock->prevFree = nullptr;
    initialBlock->nextFree = nullptr;
    
    insertFreeBlock(initialBlock);
}

void TLSFAllocator::mappingFunction(std::size_t size, int& fli, int& sli) {
    if (size == 0) {
        fli = 0;
        sli = 0;
        return;
    }
    fli = 63 - __builtin_clzll(size);
    if (fli >= FLI_SIZE) fli = FLI_SIZE - 1;
    int divisions = std::min(1 << fli, SLI_SIZE);
    std::size_t step = (1ULL << fli) / divisions;
    sli = (size - (1ULL << fli)) / step;
    if (sli >= SLI_SIZE) sli = SLI_SIZE - 1;
}

void TLSFAllocator::insertFreeBlock(FreeBlock* block) {
    int fli, sli;
    mappingFunction(block->size, fli, sli);
    
    block->isFree = true;
    block->prevFree = nullptr;
    block->nextFree = index.freeLists[fli][sli];
    if (index.freeLists[fli][sli]) {
        index.freeLists[fli][sli]->prevFree = block;
    }
    index.freeLists[fli][sli] = block;
    
    index.fliBitmap |= (1U << fli);
    index.sliBitmaps[fli] |= (1U << sli);
}

void TLSFAllocator::removeFreeBlock(FreeBlock* block) {
    int fli, sli;
    mappingFunction(block->size, fli, sli);
    
    if (block->prevFree) {
        block->prevFree->nextFree = block->nextFree;
    } else {
        index.freeLists[fli][sli] = block->nextFree;
    }
    
    if (block->nextFree) {
        block->nextFree->prevFree = block->prevFree;
    }
    
    if (index.freeLists[fli][sli] == nullptr) {
        index.sliBitmaps[fli] &= ~(1U << sli);
        if (index.sliBitmaps[fli] == 0) {
            index.fliBitmap &= ~(1U << fli);
        }
    }
}

TLSFAllocator::FreeBlock* TLSFAllocator::findSuitableBlock(std::size_t size) {
    int fli, sli;
    
    // mappingSearch logic
    int temp_fli = 63 - __builtin_clzll(size);
    if (temp_fli >= FLI_SIZE) temp_fli = FLI_SIZE - 1;
    int divisions = std::min(1 << temp_fli, SLI_SIZE);
    std::size_t step = (1ULL << temp_fli) / divisions;
    
    std::size_t search_size = size + step - 1;
    mappingFunction(search_size, fli, sli);
    
    uint16_t slMap = index.sliBitmaps[fli] & (~0U << sli);
    while (slMap) {
        int nextSli = __builtin_ctz(slMap);
        FreeBlock* block = index.freeLists[fli][nextSli];
        while (block) {
            if (block->size >= size) return block;
            block = block->nextFree;
        }
        slMap &= ~(1U << nextSli);
    }
    
    uint32_t flMap = index.fliBitmap & (~0U << (fli + 1));
    while (flMap) {
        int nextFli = __builtin_ctz(flMap);
        uint16_t slMap2 = index.sliBitmaps[nextFli];
        while (slMap2) {
            int nextSli = __builtin_ctz(slMap2);
            FreeBlock* block = index.freeLists[nextFli][nextSli];
            while (block) {
                if (block->size >= size) return block;
                block = block->nextFree;
            }
            slMap2 &= ~(1U << nextSli);
        }
        flMap &= ~(1U << nextFli);
    }
    
    return nullptr;
}

void TLSFAllocator::splitBlock(FreeBlock* block, std::size_t size) {
    if (block->size <= size + sizeof(FreeBlock)) {
        return;
    }
    
    std::size_t remainingSize = block->size - size;
    block->size = size;
    
    FreeBlock* newBlock = reinterpret_cast<FreeBlock*>(reinterpret_cast<char*>(block) + size);
    newBlock->data = reinterpret_cast<char*>(newBlock) + sizeof(BlockHeader);
    newBlock->size = remainingSize;
    newBlock->isFree = true;
    
    newBlock->prevPhysBlock = block;
    newBlock->nextPhysBlock = block->nextPhysBlock;
    if (newBlock->nextPhysBlock) {
        newBlock->nextPhysBlock->prevPhysBlock = newBlock;
    }
    block->nextPhysBlock = newBlock;
    
    insertFreeBlock(newBlock);
}

void* TLSFAllocator::allocate(std::size_t size) {
    std::size_t totalSize = size + sizeof(BlockHeader);
    if (totalSize < sizeof(FreeBlock)) {
        totalSize = sizeof(FreeBlock);
    }
    
    FreeBlock* block = findSuitableBlock(totalSize);
    if (!block) return nullptr;
    
    removeFreeBlock(block);
    splitBlock(block, totalSize);
    block->isFree = false;
    
    return block->data;
}

void TLSFAllocator::mergeAdjacentFreeBlocks(FreeBlock* block) {
    if (block->nextPhysBlock && block->nextPhysBlock->isFree) {
        FreeBlock* nextBlock = static_cast<FreeBlock*>(block->nextPhysBlock);
        removeFreeBlock(nextBlock);
        block->size += nextBlock->size;
        block->nextPhysBlock = nextBlock->nextPhysBlock;
        if (block->nextPhysBlock) {
            block->nextPhysBlock->prevPhysBlock = block;
        }
    }
    
    if (block->prevPhysBlock && block->prevPhysBlock->isFree) {
        FreeBlock* prevBlock = static_cast<FreeBlock*>(block->prevPhysBlock);
        removeFreeBlock(prevBlock);
        prevBlock->size += block->size;
        prevBlock->nextPhysBlock = block->nextPhysBlock;
        if (prevBlock->nextPhysBlock) {
            prevBlock->nextPhysBlock->prevPhysBlock = prevBlock;
        }
        block = prevBlock;
    }
    
    insertFreeBlock(block);
}

void TLSFAllocator::deallocate(void* ptr) {
    if (!ptr) return;
    
    BlockHeader* header = reinterpret_cast<BlockHeader*>(reinterpret_cast<char*>(ptr) - sizeof(BlockHeader));
    FreeBlock* block = static_cast<FreeBlock*>(header);
    
    mergeAdjacentFreeBlocks(block);
}

void* TLSFAllocator::getMemoryPoolStart() const {
    return memoryPool;
}

std::size_t TLSFAllocator::getMemoryPoolSize() const {
    return poolSize;
}

std::size_t TLSFAllocator::getMaxAvailableBlockSize() const {
    std::size_t maxSize = 0;
    for (int fli = FLI_SIZE - 1; fli >= 0; --fli) {
        if (index.fliBitmap & (1U << fli)) {
            for (int sli = SLI_SIZE - 1; sli >= 0; --sli) {
                if (index.sliBitmaps[fli] & (1U << sli)) {
                    FreeBlock* block = index.freeLists[fli][sli];
                    while (block) {
                        if (block->size > maxSize) {
                            maxSize = block->size;
                        }
                        block = block->nextFree;
                    }
                }
            }
        }
    }
    return maxSize;
}
