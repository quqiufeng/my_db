#define _GNU_SOURCE
#include "mydb_internal.h"

int pool_init(db_pool_t* pool, const char* path, size_t initial_size) {
    memset(pool, 0, sizeof(db_pool_t));
    
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }
    
    size_t file_size = st.st_size;
    bool is_new = (file_size == 0);
    
    if (is_new) {
        if (ftruncate(fd, initial_size) < 0) {
            close(fd);
            return -1;
        }
        file_size = initial_size;
    }
    
    void* base = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return -1;
    }
    
    pool->fd = fd;
    pool->base = base;
    pool->size = file_size;
    pool->capacity = file_size;
    
    if (is_new) {
        memset(base, 0, file_size);
        pool->used = 0;
    } else {
        // 读取已用大小（存储在文件头）
        pool->used = *(size_t*)((char*)base + 8); // 偏移 8 是 used 字段
    }
    
    return 0;
}

void pool_close(db_pool_t* pool) {
    if (!pool || !pool->base) return;
    msync(pool->base, pool->size, MS_SYNC);
    munmap(pool->base, pool->size);
    close(pool->fd);
    memset(pool, 0, sizeof(db_pool_t));
}

int pool_sync(db_pool_t* pool) {
    if (!pool || !pool->base) return -1;
    return msync(pool->base, pool->size, MS_ASYNC);
}

void* pool_alloc(db_pool_t* pool, size_t size) {
    if (!pool || !pool->base) return NULL;
    
    size_t aligned_size = (size + MYDB_ALIGN - 1) & ~(MYDB_ALIGN - 1);
    if (pool->used + aligned_size > pool->size) {
        // 需要扩展
        size_t new_size = pool->size * 2;
        if (pool->used + aligned_size > new_size) {
            new_size = pool->used + aligned_size + (1024 * 1024);
        }
        if (pool_resize(pool, new_size) < 0) {
            return NULL;
        }
    }
    
    void* ptr = (char*)pool->base + pool->used;
    pool->used += aligned_size;
    
    // 更新文件头中的 used
    *(size_t*)((char*)pool->base + 8) = pool->used;
    
    return ptr;
}

int pool_resize(db_pool_t* pool, size_t new_size) {
    if (!pool || !pool->base) return -1;
    
    // 先同步数据
    msync(pool->base, pool->size, MS_SYNC);
    
    // 扩展文件
    if (ftruncate(pool->fd, new_size) < 0) {
        return -1;
    }
    
    // 重新映射
    void* new_base = mremap(pool->base, pool->size, new_size, MREMAP_MAYMOVE);
    if (new_base == MAP_FAILED) {
        // mremap 失败，尝试完全重新映射
        munmap(pool->base, pool->size);
        new_base = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, pool->fd, 0);
        if (new_base == MAP_FAILED) {
            return -1;
        }
    }
    
    pool->base = new_base;
    pool->size = new_size;
    pool->capacity = new_size;
    
    return 0;
}
