#ifndef ARRAY_H
#define ARRAY_H
#include <stddef.h>

/**
 * @brief 动态数组
 * @author xin 2024.3.25
 * @note 修改记录:
 * - 初始化版本   2024.3.25
 * - 重构为简化版本  2026.6.16
 * @note 不可以传递使用，数组长度增加时地址值可能会变，需要传递可以包装到结构体中。固定长度可以传递使用。
 */

/**
 * @brief 创建数组
 * @param t 元素类型
 * @param len 长度
 * @param cap 容量，容量必须大于等于长度
 * @return 数组
 */
#define array_make(t, len, cap) _array_make(sizeof(t), len, cap)

/**
 * @brief 通过静态数组构造数组
 * @param a 静态数组，不需要unmake
 * @note 可以使用append、remove、len、cap，需要自行管理cap,数组必须放在结构体中，且上一面是Array
 */
#define array_from(a) ((Array*)a - 1)->length = 0; ((Array*)a - 1)->capacity = sizeof(a) / sizeof(*a)

/**
 * @brief 销毁数组
 * @param a 数组
 */
#define array_unmake(a) _array_unmake(a); a = 0

/**
 * @brief 添加元素
 * @param a 数组
 * @param e 元素
 */
#define array_append(a, e) (a[_array_append((void**)&a, a, sizeof(*a), 0, 1)] = e)

/**
 * @brief 添加数组
 * @param a 数组
 * @param e 数组
 * @param l 长度
 */
#define array_extend(a, e, l) _array_append((void**)&a, a, sizeof(*a), e, l)

/**
 * @brief 移除元素
 * @param a 数组
 * @param i 下标
 */
#define array_remove_at(a, i) _array_remove_at(a, sizeof(*(a)), i)

/**
 * @brief 数组长度
 * @param a 数组
 * @return 长度
 */
#define array_len(a) ((const Array*)a - 1)->length

/**
 * @brief 数组容量
 * @param a 数组
 * @return 容量
 */
#define array_cap(a) ((const Array*)a - 1)->capacity

/**
 * @brief 清空数组
 * @param a 数组
 */
#define array_clear(a) ((Array*)a - 1)->length = 0
typedef struct Array
{
	size_t length;
	size_t capacity;
} Array;
void* _array_make(size_t elementSize, size_t arraySize, size_t arrayCap);
void _array_unmake(void* array);
size_t _array_append(void** pArray, void* array, size_t elementSize, void* array2, size_t array2Size);
void _array_remove_at(void* array, size_t elementSize, size_t i);
void array_test();
#endif