#include "mydb_internal.h"

int wal_init(db_wal_t* wal, const char* path) {
    memset(wal, 0, sizeof(db_wal_t));
    
    int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    
    // 读取最后 LSN
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        // 简单实现：LSN 从 1 开始递增
        wal->lsn = 1;
    }
    
    wal->fd = fd;
    return 0;
}

void wal_close(db_wal_t* wal) {
    if (!wal || wal->fd < 0) return;
    fsync(wal->fd);
    close(wal->fd);
    wal->fd = -1;
}

int wal_append(db_wal_t* wal, int op, const char* table_name,
               const void* row_data, size_t row_size, rowid_t rowid) {
    if (!wal || wal->fd < 0) return -1;
    
    wal->lsn++;
    
    // WAL Entry 格式：
    // [长度:4][CRC:4][LSN:8][操作类型:4][表名长度:4][表名:N][rowid:8][行大小:4][行数据:N]
    size_t name_len = strlen(table_name);
    size_t total_size = 4 + 4 + 8 + 4 + 4 + name_len + 8 + 4 + row_size;
    
    uint8_t* buf = (uint8_t*)malloc(total_size);
    if (!buf) return -1;
    
    size_t offset = 0;
    
    // 长度（不含自身）
    *(uint32_t*)(buf + offset) = (uint32_t)(total_size - 4);
    offset += 4;
    
    // CRC（占位，后面计算）
    uint32_t* crc_ptr = (uint32_t*)(buf + offset);
    offset += 4;
    
    // LSN
    *(uint64_t*)(buf + offset) = wal->lsn;
    offset += 8;
    
    // 操作类型
    *(uint32_t*)(buf + offset) = (uint32_t)op;
    offset += 4;
    
    // 表名长度 + 表名
    *(uint32_t*)(buf + offset) = (uint32_t)name_len;
    offset += 4;
    memcpy(buf + offset, table_name, name_len);
    offset += name_len;
    
    // rowid
    *(uint64_t*)(buf + offset) = rowid;
    offset += 8;
    
    // 行大小 + 行数据
    *(uint32_t*)(buf + offset) = (uint32_t)row_size;
    offset += 4;
    if (row_data && row_size > 0) {
        memcpy(buf + offset, row_data, row_size);
    }
    offset += row_size;
    
    // 计算 CRC
    *crc_ptr = crc32(buf + 8, total_size - 8);
    
    // 写入文件并 fsync
    ssize_t written = write(wal->fd, buf, total_size);
    free(buf);
    
    if (written != (ssize_t)total_size) return -1;
    
    return fsync(wal->fd);
}

int wal_fsync(db_wal_t* wal) {
    if (!wal || wal->fd < 0) return -1;
    return fsync(wal->fd);
}

int wal_checkpoint(db_wal_t* wal) {
    if (!wal || wal->fd < 0) return -1;
    
    // Checkpoint：清空 WAL 文件
    // 数据已经通过 db_sync 刷盘，WAL 不再需要
    if (ftruncate(wal->fd, 0) < 0) {
        return -1;
    }
    lseek(wal->fd, 0, SEEK_SET);
    wal->lsn = 0;
    
    return fsync(wal->fd);
}

int wal_replay(db_wal_t* wal, db_instance_t* db) {
    if (!wal || wal->fd < 0 || !db) return -1;
    
    struct stat st;
    if (fstat(wal->fd, &st) < 0 || st.st_size == 0) {
        return 0; // WAL 为空，无需回放
    }
    
    // 读取整个 WAL 文件
    uint8_t* buf = (uint8_t*)malloc(st.st_size);
    if (!buf) return -1;
    
    lseek(wal->fd, 0, SEEK_SET);
    ssize_t n = read(wal->fd, buf, st.st_size);
    if (n != st.st_size) {
        free(buf);
        return -1;
    }
    
    // 解析并重放每个 WAL Entry
    size_t offset = 0;
    uint64_t max_lsn = 0;
    
    while (offset < (size_t)n) {
        if (offset + 4 > (size_t)n) break;
        
        uint32_t entry_len = *(uint32_t*)(buf + offset);
        offset += 4;
        
        if (offset + entry_len > (size_t)n) break;
        
        uint32_t entry_crc = *(uint32_t*)(buf + offset);
        offset += 4;
        
        uint32_t calc_crc = crc32(buf + offset, entry_len - 4);
        if (entry_crc != calc_crc) {
            fprintf(stderr, "WAL CRC mismatch, skipping entry\n");
            offset += entry_len - 4;
            continue;
        }
        
        uint64_t lsn = *(uint64_t*)(buf + offset);
        offset += 8;
        
        uint32_t op = *(uint32_t*)(buf + offset);
        offset += 4;
        
        uint32_t name_len = *(uint32_t*)(buf + offset);
        offset += 4;
        
        char table_name[MYDB_TABLE_NAME_LEN];
        if (name_len >= MYDB_TABLE_NAME_LEN) name_len = MYDB_TABLE_NAME_LEN - 1;
        memcpy(table_name, buf + offset, name_len);
        table_name[name_len] = '\0';
        offset += name_len;
        
        uint64_t rowid = *(uint64_t*)(buf + offset);
        offset += 8;
        
        uint32_t row_size = *(uint32_t*)(buf + offset);
        offset += 4;
        
        void* row_data = buf + offset;
        offset += row_size;
        
        // 重放操作
        table_t table = db_table(db, table_name);
        if (table) {
            switch (op) {
                case 1: // INSERT
                    db_insert(table, row_data, row_size);
                    break;
                case 2: // UPDATE
                    db_update(table, rowid, row_data, row_size);
                    break;
                case 3: // DELETE
                    db_delete(table, rowid);
                    break;
            }
        }
        
        if (lsn > max_lsn) max_lsn = lsn;
    }
    
    wal->lsn = max_lsn;
    free(buf);
    
    // 清空 WAL 文件
    if (ftruncate(wal->fd, 0) < 0) {
        // 忽略错误
    }
    lseek(wal->fd, 0, SEEK_SET);
    
    return 0;
}
