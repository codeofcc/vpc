/***********************************************************************************************************************
 * Copyright (C): 2025-2025, codeofcc. All rights reserved.
 * @file : thread.h
 * @brief : 通用线程类封装，在Windows上使用win32api，其他平台使用pthread。
 * @author : codeofcc
 * @email :
 * @version : 1.0.0
 * @date : 2025/6/6 16:34:27
******************************************************************************************************************/
#ifndef THREAD_H
#define THREAD_H
#ifdef _WIN32
#ifndef _WIN32_WINNT
//使用mingw编译时需要此定义。
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <synchapi.h>
/// @brief 线程句柄
typedef HANDLE Thread;
/// @brief 线程回调的返回定义，仅作为函数定义，不能作为类型。
#define TINT int WINAPI
/**
 * @brief 创建线程
 * @param thread 线程句柄,类型为Thread*
 * @param callback 回调方法，格式：TINT thread_callback(void* arg);  thread_callback中的返回类型是整型int，不同平台长度不同所以只能统一为int。
 * @param arg 回调参数,类型为void*
 * @return 0成功，其他值失败
 */
#define thread_create(thread, callback, arg) (*(thread) = CreateThread(NULL, 0,(LPTHREAD_START_ROUTINE)callback,arg, 0, NULL))==0
 /**
  * @brief 等待线程结束，结束后会销毁线程句柄
  * @param thread 线程句柄,类型为Thread
  * @param ret 返回值，类型未int*
  * @return 0成功，其他值失败
  */
#define thread_join(thread,ret) WaitForSingleObject(thread, INFINITE); if(ret)GetExitCodeThread(thread,(LPDWORD) ret);CloseHandle(thread)
  /**
   * @brief 分离线程，会销毁线程句柄，但线程依然执行
   * @param thread 线程句柄,类型为Thread
   * @return 0成功，其他值失败
   */
#define thread_detach(thread) CloseHandle(thread)==0
   /**
	* @brief 获取当前线程id
	* @return 线程id，类型为unsigned long
	*/
#define thread_self_id() (unsigned long)GetCurrentThreadId()
	/**
	 * @brief 休眠当前线程
	 * @param ms 毫秒数
	 * @return void无返回
	 */
#define thread_sleep(ms) Sleep(ms)
	 // typedef HANDLE  Mutex;
	 // #define mutex_create(mtx) *(mtx) =CreateMutex(NULL, FALSE, NULL)
	 // #define mutex_destroy(mtx) CloseHandle(mtx)
	 // #define mutex_lock(mtx)  WaitForSingleObject(mtx, INFINITE)
	 // #define mutex_trylock(mtx)   WaitForSingleObject(mtx, 0)
	 // #define mutex_unlock(mtx)  ReleaseMutex(mtx)
	 /// @brief 互斥变量
typedef CRITICAL_SECTION Mutex;
/**
 * @brief 创建互斥变量
 * @param mtx 互斥变量，类型为Mutex*
 * @return void无返回
 */
#define mutex_create(_mtx) InitializeCriticalSection(_mtx)
 /**
  * @brief 销毁互斥变量
  * @param mtx 互斥变量，类型为Mutex*
  * @return void无返回
  */
#define mutex_destroy(_mtx) DeleteCriticalSection(_mtx)
  /**
   * @brief 加锁
   * @param mtx 互斥变量，类型为Mutex*
   * @return void无返回
   */
#define mutex_lock(_mtx)  EnterCriticalSection(_mtx)
   /**
	* @brief 尝试加锁
	* @param mtx 互斥变量，类型为Mutex*
	* @return BOOL 成功返回1，失败返回0
	*/
#define mutex_trylock(_mtx)  TryEnterCriticalSection(_mtx)
	/**
	 * @brief 解锁
	 * @param mtx 互斥变量，类型为Mutex*
	 * @return void无返回
	 */
#define mutex_unlock(_mtx)  LeaveCriticalSection(_mtx)
	 /// @brief 信号量
typedef HANDLE Sema;
/**
 * @brief 创建信号量
 * @param sem 信号量，类型为Sem*
 * @param initial 初始值，类型为int
 * @return 0成功，其他值失败
 */
#define sema_create(sem, initial) (*(sem) = CreateSemaphore(NULL, initial, LONG_MAX, NULL))==0
 /**
  * @brief 销毁信号量
  * @param sem 信号量，类型为Sem*
  * @return void无返回
  */
#define sema_destroy(sem)  CloseHandle(*sem)
  /**
   * @brief 等待信号量
   * @param sem 信号量，类型为Sem*
   * @return 0成功，其他值失败
   */
#define sema_wait(sem)  WaitForSingleObject(*sem, INFINITE)
   /**
	* @brief 尝试等待信号量
	* @param sem 信号量，类型为Sem*
	* @return 1成功，0失败
	*/
#define sema_trywait(sem)  WaitForSingleObject(*sem, 0)==0
	/**
	 * @brief 等待信号量，带超时时间
	 * @param sem 信号量，类型为Sem*
	 * @param ms 超时时间，相对时间，单位毫秒,类型为int
	 * @param ret 返回值，0成功，其他值失败。类型为int*
	 * @return void无返回
	 */
#define sema_wait_time(sem,ms,ret) *(ret)=WaitForSingleObject(*sem, ms)
	 /**
	  * @brief 释放信号量
	  * @param sem 信号量，类型为Sem*
	  * @return void无返回
	  */
#define sema_post(sem)  ReleaseSemaphore(*sem, 1, NULL)
	  /// @brief 条件变量
typedef CONDITION_VARIABLE Cond;
/**
 * @brief 创建条件变量
 * @param cond 条件变量，类型为Cond*
 * @return 0成功，其他值失败
 */
#define cond_create(cond) 1==0,InitializeConditionVariable(cond)
 /**
  * @brief 销毁条件变量
  * @param cond 条件变量，类型为Cond*
  * @return void无返回
  */
#define cond_destroy(cond) WakeAllConditionVariable(cond)
  /**
   * @brief 等待条件变量
   * @param cond 条件变量，类型为Cond*
   * @param mtx 互斥变量，类型为Mutex*
   * @return 0成功，其他值失败
   */
#define cond_wait(cond, _mtx) SleepConditionVariableCS(cond, _mtx, INFINITE)==0
   /**
	* @brief 等待条件变量，带超时时间
	* @param cond 条件变量，类型为Cond*
	* @param mtx 互斥变量，类型为Mutex*
	* @param ms 超时时间，相对时间，单位毫秒,类型为int
	* @param ret 返回值，0成功，其他值失败。类型为int*
	* @return void无返回
	*/
#define cond_wait_time(cond, _mtx,ms,ret) *(ret)=SleepConditionVariableCS(cond, _mtx, ms)==0
	/**
	 * @brief 释放条件变量
	 * @param cond 条件变量，类型为Cond*
	 * @return void无返回
	 */
#define cond_post(cond) WakeConditionVariable(cond)
	 /**
	  * @brief 释放条件变量,广播
	  * @param cond 条件变量，类型为Cond*
	  * @return void无返回
	  */
#define cond_broadcast(cond) WakeAllConditionVariable(cond)
	  /// @brief 读写锁
typedef SRWLOCK RWMutex;
/**
 * @brief 初始化读写锁
 * @param mtx 读写锁变量，类型为RWMutex*
 * @return void无返回
 */
#define rwmutex_create(_mtx) InitializeSRWLock(_mtx)
 /**
  * @brief 销毁读写锁
  * @param mtx 读写锁变量，类型为RWMutex*
  * @return void无返回
  */
#define rwmutex_destroy(_mtx) 
  /**
   * @brief 获取写锁，阻塞直到获取锁为止
   * @param mtx 读写锁变量，类型为RWMutex*
   * @return void无返回
   */
#define rwmutex_lock(_mtx)  AcquireSRWLockExclusive(_mtx)
   /**
	* @brief 尝试获取写锁，不阻塞
	* @param mtx 读写锁变量，类型为RWMutex*
	* @return BOOL 成功返回1，失败返回0
	*/
#define rwmutex_trylock(_mtx)  TryAcquireSRWLockExclusive(_mtx)
	/**
	 * @brief 释放写锁
	 * @param mtx 读写锁变量，类型为RWMutex*
	 * @return void无返回
	 */
#define rwmutex_unlock(_mtx)  ReleaseSRWLockExclusive(_mtx)
	 /**
	  * @brief 获取读锁，阻塞直到获取锁为止
	  * @param mtx 读写锁变量，类型为RWMutex*
	  * @return void无返回
	  */
#define rwmutex_read_lock(_mtx)  AcquireSRWLockShared(_mtx)
	  /**
	   * @brief 尝试获取读锁，不阻塞
	   * @param mtx 读写锁变量，类型为RWMutex*
	   * @return BOOL 成功返回1，失败返回0
	   */
#define rwmutex_read_trylock(_mtx)  TryAcquireSRWLockShared(_mtx)
	   /**
		* @brief 释放读锁
		* @param mtx 读写锁变量，类型为RWMutex*
		* @return void无返回
		*/
#define rwmutex_read_unlock(_mtx)  ReleaseSRWLockShared(_mtx)
#else
#include <pthread.h>
#include <sys/time.h>
#include <semaphore.h>
#include <stdint.h>
typedef pthread_t Thread;
#define TINT intptr_t
#define thread_create(tid, callback, arg) pthread_create(tid, 0, (void*(*)(void*))callback, arg)
#define thread_join(tid,ret) { intptr_t retval;  pthread_join(tid, (void**)&retval); if (ret){*(int*)ret = (int)retval; } }
#define thread_detach(tid) pthread_detach(tid)
#define thread_self_id() (unsigned long)pthread_self()
#define thread_sleep(ms){struct timeval delay = {ms / 1000, (ms % 1000) * 1000}; select(0, 0, 0, 0, &delay);}   
typedef pthread_mutex_t Mutex;
#define mutex_create(_mtx) {pthread_mutexattr_t attr;pthread_mutexattr_init(&attr);pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);pthread_mutex_init(_mtx,&attr);}
#define mutex_destroy(_mtx) pthread_mutex_destroy(_mtx)
#define mutex_lock(_mtx)  pthread_mutex_lock(_mtx)
#define mutex_trylock(_mtx)  pthread_mutex_trylock(_mtx)==0
#define mutex_unlock(_mtx)  pthread_mutex_unlock(_mtx)      
typedef sem_t Sema;
#define sema_create(sem, initial) sem_init(sem, 0, initial)
#define sema_destroy(sem)  sem_destroy(sem)
#define sema_wait(sem)  sem_wait(sem)
#define sema_trywait(sem)  sem_trywait(&sem)==0
#if __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 30
#define sema_wait_time(sem,ms,ret)  { struct timespec time;clock_gettime(CLOCK_MONOTONIC, &time); time.tv_sec += (ms / 1000);time.tv_nsec+=((ms % 1000)) * 1000000; *(ret)=sem_clockwait(sem,CLOCK_MONOTONIC, &time);}
#else
#define sema_wait_time(sem,ms,ret)  { struct timespec time;clock_gettime(CLOCK_REALTIME, &time); time.tv_sec += (ms / 1000);time.tv_nsec+=((ms % 1000)) * 1000000; *(ret)=sem_timedwait(sem, &time);}
#endif
#define sema_post(sem)  sem_post(sem)
typedef pthread_cond_t Cond;
#define cond_create(cond) pthread_cond_init(cond,0)
#define cond_destroy(cond) pthread_cond_destroy(cond)
#define cond_wait(cond, _mtx) pthread_cond_wait(cond, _mtx)
#define cond_wait_time(cond, _mtx,ms,ret)  { struct timespec time;clock_gettime(CLOCK_REALTIME, &time); time.tv_sec += (ms / 1000);time.tv_nsec+=((ms % 1000)) * 1000000; *(ret)=pthread_cond_timedwait(cond, _mtx,&time);}
#define cond_post(cond) pthread_cond_signal(cond)
#define cond_broadcast(cond) pthread_cond_broadcast(cond)
typedef pthread_rwlock_t RWMutex;
#define rwmutex_create(_mtx) pthread_rwlock_init(_mtx,0)
#define rwmutex_destroy(_mtx) pthread_rwlock_destroy(_mtx)
#define rwmutex_lock(_mtx)  pthread_rwlock_wrlock(_mtx)
#define rwmutex_trylock(_mtx)  pthread_rwlock_trywrlock(_mtx)==0
#define rwmutex_unlock(_mtx)  pthread_rwlock_unlock(_mtx)
#define rwmutex_read_lock(_mtx)  pthread_rwlock_rdlock(_mtx)
#define rwmutex_read_trylock(_mtx)  pthread_rwlock_tryrdlock(_mtx)==0
#define rwmutex_read_unlock(_mtx)  pthread_rwlock_unlock(_mtx)
#endif

/*tls 优先级：C11标准 > MSVC扩展 > GCC/Clang扩展 > 报错 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_THREADS__)
#define THREAD_LOCAL _Thread_local
#elif defined(_MSC_VER)
#define THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__) || defined(__clang__)
#define THREAD_LOCAL __thread
#else
#error "No supported TLS mechanism found for this compiler"
#endif
#endif
