#include "array.h"
#include "codetime.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define _AC_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
void *_array_make(size_t elementSize, size_t arraySize, size_t arrayCap)
{
	if (arrayCap < arraySize)
		arrayCap = arraySize;
	if (!arraySize)
		arrayCap = sizeof(size_t);
	Array *arr = malloc(elementSize * arrayCap + sizeof(Array));
	if (arr)
	{
		arr->capacity = arrayCap;
		arr->length = arraySize;
		return arr + 1;
	}
	return NULL;
}

void _array_unmake(void *array)
{
	Array *arr = (Array *)array - 1;
	free(arr);
}

size_t _array_append(void **pArray, void *array, size_t elementSize, void *array2, size_t array2Size)
{
	Array *arr = (Array *)array - 1;
	if (arr->capacity < arr->length + array2Size)
	{
		arr->capacity = _AC_ALIGN((arr->length + array2Size) * 2, sizeof(size_t));
		if ((arr = realloc(arr, arr->capacity * elementSize + sizeof(Array))) == NULL)
		{
			printf("out of memory for array realloc\n");
			return 0;
		}
		*pArray = array = arr + 1;
	}
	size_t pos = arr->length;
	if (array2)
	{
		memcpy((char *)(array) + elementSize * pos, array2, elementSize * array2Size);
	}
	arr->length += array2Size;
	return pos;
}

void _array_remove_at(void *array, size_t elementSize, size_t i)
{
	Array *arr = (Array *)array - 1;
	if (!arr->length || i >= arr->length)
	{
		return;
	}
	size_t size = arr->length - i - 1;
	if (size)
	{
		memmove((char *)(array) + elementSize * i, (char *)(array) + elementSize * (i + 1), elementSize * size);
	}
	arr->length--;
}

static void test0()
{
	int *a = array_make(int, 0, 0);

	if (array_len(a) != 0)
	{
		printf("test0 error 1\n");
		return;
	}
	if (array_cap(a) == 0)
	{
		printf("test0 error 2\n");
		return;
	}

	array_append(a, 1);
	if (a[0] != 1)
	{
		printf("test0 error 3\n");
		return;
	}

	array_append(a, 23334);
	if (a[1] != 23334)
	{
		printf("test0 error 4\n");
		return;
	}

	int c[] = {789, 88, 78};
	array_extend(a, c, 3);
	if (a[2] != 789 || a[3] != 88 || a[4] != 78)
	{
		printf("test0 error 5\n");
		return;
	}

	array_remove_at(a, 0);
	if (a[1] != 789 || a[2] != 88 || a[3] != 78)
	{
		printf("test0 error 6\n");
		return;
	}

	array_remove_at(a, 2);
	if (a[1] != 789 || a[2] != 78)
	{
		printf("test0 error 7\n");
		return;
	}
	if (array_len(a) != 3)
	{
		printf("test0 error 8\n");
		return;
	}

	array_unmake(a);
	printf("test0 passed!\n");
}

static void test1()
{
	int *a = array_make(int, 15, 0);

	if (array_len(a) != 15)
	{
		printf("test1 error 1\n");
		return;
	}
	array_unmake(a);

	a = array_make(int, 10, 1500);
	if (array_len(a) != 10)
	{
		printf("test1 error 2\n");
		return;
	}
	if (array_cap(a) < 1500)
	{
		printf("test1 error 3\n");
		return;
	}

	array_unmake(a);
	printf("test1 passed!\n");
}

static void test2()
{
	int *a = array_make(int, 15, 0);
	a[0] = 1;
	a[2] = 2;
	a[3] = 3;
	a[4] = 4;

	array_append(a, 5478);
	if (a[15] != 5478)
	{
		printf("test2 error 1\n");
		return;
	}

	for (int i = 0; i < 10000; i++)
		array_append(a, 54587);
	if (array_len(a) != 10000 + 15 + 1)
	{
		printf("test2 error 2\n");
		return;
	}

	int ab[] = {123, 848, 871, 123, 45, 7, 4};
	for (int i = 0; i < 10000; i++)
		array_extend(a, ab, 7);
	if (array_len(a) != 10000 + 15 + 1 + 10000 * 7)
	{
		printf("test2 error 3\n");
		return;
	}

	array_unmake(a);
	printf("test2 passed!\n");
}

static void test3()
{
	int *a = array_make(int, 0, 0);

	for (int i = 0; i < 10000; i++)
		array_append(a, i);
	for (int i = 0; i < 5000; i++)
		array_remove_at(a, i);
	if (array_len(a) != 5000)
	{
		printf("test3 error 1\n");
		return;
	}

	for (int i = 0; i < 5000; i++)
		if (a[i] != i * 2 + 1)
		{
			printf("test3 error 2\n");
			return;
		}

	array_unmake(a);
	printf("test3 passed!\n");
}

static void test4()
{
	typedef struct
	{
		Array _header;
		int a[10];
	} testFrom;
	testFrom t;
	array_from(t.a);

	if (array_cap(t.a) != 10)
	{
		printf("test4 error 1\n");
		return;
	}
	if (array_len(t.a) != 0)
	{
		printf("test4 error 2\n");
		return;
	}

	for (int i = 0; i < 10; i++)
		array_append(t.a, i);
	if (array_len(t.a) != 10)
	{
		printf("test4 error 3\n");
		return;
	}
	for (int i = 0; i < 10; i++)
		if (t.a[i] != i)
		{
			printf("test4 error 4\n");
			return;
		}

	printf("test4 passed!\n");
}

static void test5()
{
	int *a = array_make(int, 10, 20);

	for (int i = 0; i < 10; i++)
		a[i] = i;
	if (array_len(a) != 10)
	{
		printf("test5 error 1\n");
		return;
	}

	array_clear(a);
	if (array_len(a) != 0)
	{
		printf("test5 error 2\n");
		return;
	}

	array_append(a, 100);
	if (a[0] != 100)
	{
		printf("test5 error 3\n");
		return;
	}

	array_unmake(a);
	printf("test5 passed!\n");
}

static void test6()
{
	RangeCodeTime t;
	int *a;
	int level = 100000;

	rangeCodeTime_reset(&t);
	for (int i = 0; i < 10; i++)
	{
		a = array_make(int, 0, 0);
		rangeCodeTime_begin(&t);
		for (int j = 0; j < level; j++) array_append(a, j);
		rangeCodeTime_end(&t, "append");
		array_unmake(a);
	}
	printf("average append %d times: %lfs\n", level, t.average);

	rangeCodeTime_reset(&t);
	for (int i = 0; i < 10; i++)
	{
		a = array_make(int, level, level);
		rangeCodeTime_begin(&t);
		for (int j = 0; j < level; j++) array_remove_at(a, 0);
		rangeCodeTime_end(&t, "remove_at 0");
		array_unmake(a);
	}
	printf("average remove_at 0 %d times: %lfs\n", level, t.average);

	rangeCodeTime_reset(&t);
	for (int i = 0; i < 10; i++)
	{
		a = array_make(int, level, level);
		rangeCodeTime_begin(&t);
		for (int j = 0; j < level; j++) array_remove_at(a, array_len(a) - 1);
		rangeCodeTime_end(&t, "remove_at end");
		array_unmake(a);
	}
	printf("average remove_at end %d times: %lfs\n", level, t.average);
}

void array_test()
{
	test0(); test1(); test2(); test3(); test4(); test5(); test6();
}
