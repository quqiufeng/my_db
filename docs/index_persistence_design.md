# Cache Index Persistence Design

## Overview

Persist all in-memory indexes to disk using zero-copy mmap. Supports append-only updates (no modifications after import).

## File Format: `{db_dir}/index.bin`

```
┌──────────────────────────────────────────────────────┐
│ Index File Layout                                    │
├──────────────────────────────────────────────────────┤
│ Magic "MYIX"              (4 bytes)                  │
│ Version                   (4 bytes) = 1              │
│ Total Size                (8 bytes)                  │
│ Index Count               (4 bytes)                  │
│ Reserved                  (4 bytes)                  │
├──────────────────────────────────────────────────────┤
│ Index Headers[]                                       │
│  Each header:                                         │
│   - Type                  (4 bytes)                  │
│     1=Hash, 2=Sorted, 3=Vector, 4=Tag, 5=HNSW       │
│   - Data Offset           (8 bytes)                  │
│   - Data Size             (8 bytes)                  │
│   - Entry Count           (8 bytes)                  │
│   - Reserved              (8 bytes)                  │
├──────────────────────────────────────────────────────┤
│ Index Data[]                                         │
│   [Hash Index Data]                                  │
│   [Sorted Array Data]                                │
│   [Vector Index Data]                                │
│   [Tag Index Data]                                   │
│   [HNSW Graph Data]                                  │
└──────────────────────────────────────────────────────┘
```

Total header: 24 bytes + N * 36 bytes per index entry

## 1. Hash Index Persistence

Hash index uses pool-allocated buckets with offset-based linked lists.

```
Hash Index Data:
├─ bucket_count     (8 bytes)
├─ size             (8 bytes)
└─ buckets[]        (cache_hash_bucket_t array)
     Each: entry_offset (8) + next_offset (8) = 16 bytes
```

**Zero-copy load**: mmap file, set `cache->hash.buckets_offset = file_offset_of_buckets`

## 2. Sorted Array Persistence

```
Sorted Array Data:
├─ count            (8 bytes)
├─ capacity         (8 bytes)
├─ dirty            (4 bytes)
├─ padding          (4 bytes)
└─ offsets[]        (size_t array)
```

**Zero-copy load**: `cache->sorted.offsets = mmap_ptr + data_offset + 24`

## 3. Vector Index (Brute Force) Persistence

```
Vector Index Data:
├─ count            (8 bytes)
├─ capacity         (8 bytes)
└─ entries[]        (cache_vector_entry_t array)
     Each: 24 bytes (entry_offset + vector_offset + dim + padding)
```

Note: vector_offset points to pool data, not index file. The actual vector data remains in cache.bin.

## 4. Tag Index Persistence

```
Tag Index Data:
├─ bucket_count     (8 bytes)
├─ size             (8 bytes)
├─ buckets[]        (variable)
     Each bucket is a linked list of tag entries:
     ├─ tag_len      (4 bytes)
     ├─ tag[]        (char array, no null terminator)
     ├─ count        (8 bytes)
     ├─ capacity     (8 bytes)
     ├─ offsets[]    (size_t array)
     ├─ next_offset  (8 bytes)  // relative to index data start, 0 = end
```

## 5. HNSW Graph Persistence (Special Design)

HNSW is the most complex. We need to persist:
- Graph topology (neighbor connections)
- Node metadata (id, level, valid)
- Vectors (or reuse pool vectors?)

**Decision**: Store vectors in index file (duplicated from pool) for self-contained loading. Alternatively, store node→entry_offset mapping and load vectors from pool on demand.

```
HNSW Data:
├─ dim              (8 bytes)
├─ M                (4 bytes)
├─ M_max            (4 bytes)
├─ ef_construction  (4 bytes)
├─ ef_search        (4 bytes)
├─ max_level        (4 bytes)
├─ entry_point      (8 bytes)
├─ node_count       (8 bytes)
├─ node_capacity    (8 bytes)
├─ padding          (8 bytes)
│
├─ Node Metadata[]:
│    Each node (aligned to 8):
│    ├─ id              (8 bytes)   // entry_offset
│    ├─ level           (4 bytes)
│    ├─ valid           (4 bytes)
│    ├─ vector_offset   (8 bytes)   // offset in HNSW vectors section
│    └─ neighbor_offsets[] (8 * (level+1) bytes)
│        // Each points to a neighbor_list in the lists section
│
├─ Vectors[]:
│    Each: float[dim] array
│
└─ Neighbor Lists[]:
     Each list:
     ├─ count        (8 bytes)
     ├─ capacity     (8 bytes)
     └─ ids[]        (size_t array)  // neighbor entry_offsets
```

**Loading strategy**:
1. mmap the entire HNSW section
2. Reconstruct hnsw_index_t structure in memory
3. Node metadata array can be used directly (after fixing relative offsets)
4. Vectors can be used directly from mmap
5. Neighbor lists need reconstruction (convert offsets to neighbor_list_t structs)

## Append-Only Strategy

Since ebooks never change after import, we only need append:

```
Scenario: Cache has Book A (1000 entries). Import Book B (500 entries).

1. Load existing index.bin (mmap, zero-copy)
2. Book B import creates new entries in cache.bin
3. Build increment index for Book B:
   - New hash buckets (or extend existing)
   - New sorted array entries (merge later)
   - New vector entries
   - New HNSW nodes + edges
4. Write append block to index.bin:
   - Update header (new total size, new entry count)
   - Append new index data
   - Or: rewrite entire index (simpler, acceptable for <100K entries)
```

**Simplification**: For simplicity, rewrite the entire index on save. For 5.8万 entries, index.bin is ~50MB, rewrite takes <100ms. This is acceptable since import is batch operation.

## Zero-Copy Loading

```c
typedef struct {
    void* mmap_base;
    size_t mmap_size;
} cache_index_mmap_t;

// Load all indexes from index.bin
int cache_index_load(cache_t* cache) {
    char path[512];
    snprintf(path, sizeof(path), "%s/index.bin", cache->db_dir);
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;  // No index file, trigger rebuild
    
    struct stat st;
    fstat(fd, &st);
    
    void* base = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    
    if (base == MAP_FAILED) return -1;
    
    // Parse header
    uint32_t magic = *(uint32_t*)base;
    if (magic != *(uint32_t*)"MYIX") {
        munmap(base, st.st_size);
        return -1;
    }
    
    uint32_t count = *(uint32_t*)((char*)base + 12);
    
    // For each index entry in header
    for (int i = 0; i < count; i++) {
        cache_index_entry_t* entry = (cache_index_entry_t*)
            ((char*)base + 24 + i * 36);
        
        void* data = (char*)base + entry->offset;
        
        switch (entry->type) {
            case INDEX_TYPE_HASH:
                // Direct use: buckets array is already offset-based
                cache->hash.buckets_offset = (size_t)((char*)data + 16);
                cache->hash.bucket_count = *(size_t*)data;
                cache->hash.size = *(size_t*)((char*)data + 8);
                break;
                
            case INDEX_TYPE_SORTED:
                // Direct use: offsets array
                cache->sorted.offsets = (size_t*)((char*)data + 24);
                cache->sorted.count = *(size_t*)data;
                cache->sorted.capacity = *(size_t*)((char*)data + 8);
                cache->sorted.dirty = 0;
                break;
                
            case INDEX_TYPE_VECTOR:
                // Direct use: entries array
                cache->vector_index.entries = (cache_vector_entry_t*)((char*)data + 16);
                cache->vector_index.count = *(size_t*)data;
                cache->vector_index.capacity = *(size_t*)((char*)data + 8);
                break;
                
            case INDEX_TYPE_HNSW:
                // Special reconstruction
                cache->vector_index.hnsw = hnsw_load_from_mmap(data);
                cache->vector_index.use_hnsw = 1;
                break;
        }
    }
    
    cache->index_mmap = base;
    cache->index_mmap_size = st.st_size;
    
    return 0;
}
```

## Save Strategy

```c
int cache_index_save(cache_t* cache) {
    // 1. Calculate total size
    // 2. Create temp file
    // 3. mmap temp file
    // 4. Write header
    // 5. Write each index data sequentially
    // 6. Update header with offsets
    // 7. msync + rename (atomic)
}
```

## Integration Points

1. **cache_open()**: Try load index.bin first. If fail or version mismatch, fall back to rebuild.
2. **cache_sync()**: Optionally save index (configurable).
3. **import_book**: After import completes, save index.
4. **cache_close()**: Save index if dirty.

## File Size Estimation

For 5.8万 entries (3 books, ~1200 paragraphs):

| Index | Size Calculation | Estimated |
|-------|-----------------|-----------|
| Hash | 65536 buckets * 16B | 1 MB |
| Sorted | 58674 offsets * 8B | 0.5 MB |
| Vector | 58674 entries * 24B | 1.4 MB |
| HNSW | nodes + vectors + lists | ~30 MB |
| **Total** | | **~35 MB** |

Load time: <50ms (mmap, zero-copy)
Save time: <200ms (write 35MB)

## Implementation Order

1. Define `cache_index.h` with all structures
2. Implement `cache_index_save()` for Hash + Sorted (simplest)
3. Implement `cache_index_load()` for Hash + Sorted
4. Integrate into `cache_open()` with fallback
5. Add Vector index persistence
6. Add HNSW persistence (most complex)
7. Add Tag index persistence
8. Add Namespace tree persistence
9. Test with existing ebook imports
10. Measure load time improvement
