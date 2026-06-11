#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* XXTEA 加密参数 */
#define XXTEA_KEY_LEN  16   /* 128-bit key */

/**
 * xxtea_set_key - 设置全局加密密钥
 * @key: 16 字节密钥
 */
void xxtea_set_key(const uint8_t *key);

/**
 * xxtea_get_key - 获取当前密钥
 * @key: 输出 16 字节密钥缓冲区
 */
void xxtea_get_key(uint8_t *key);

/**
 * xxtea_encrypt - 加密数据（原地）
 * @data: 明文输入/密文输出，长度需为 4 的倍数
 * @len:  数据长度（字节），必须 >= 8
 */
void xxtea_encrypt(uint8_t *data, size_t len);

/**
 * xxtea_decrypt - 解密数据（原地）
 * @data: 密文输入/明文输出
 * @len:  数据长度（字节）
 * 返回 0 成功，-1 数据异常
 */
int  xxtea_decrypt(uint8_t *data, size_t len);

/**
 * xxtea_encoded_len - 计算加密所需缓冲区大小（4 字节对齐）
 */
size_t xxtea_encoded_len(size_t raw_len);

/**
 * key_from_string - 从字符串派生 16 字节密钥（SHA256 → 取前 16 字节）
 * @str:  输入字符串
 * @key:  输出 16 字节密钥
 */
void key_from_string(const char *str, uint8_t *key);

#ifdef __cplusplus
}
#endif

#endif /* CRYPTO_H */
