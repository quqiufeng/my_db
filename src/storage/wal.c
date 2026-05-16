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

int wal_replay(db_wal_t* wal, db_instance_t* db) {
    if (!wal || wal->fd < 0 || !db) return -1;
    
    // 简单的 WAL 回放实现
    // 完整实现需要解析 WAL 文件并重放操作
    // 这里先返回成功（简化版）
    
    return 0;
}
