/* pt_mutex — C 侧的 pthread 递归锁包装
 *
 * 锁实例由 Rust 侧持有（PtMutex<T>，见 pt_mutex.rs），字段先以标准的
 * PTHREAD_MUTEX_INITIALIZER 占位，由 Rust 的 ensure_init（每实例双检锁）
 * 在首次使用前调 pt_mutex_init 完成真正的初始化。
 *
 * Rust 的 PtMutex<T> 经 extern "C" 调用这一组函数
 *
 */
#ifndef PT_MUTEX_H
#define PT_MUTEX_H

#include <pthread.h>

/* 用 PTHREAD_MUTEX_RECURSIVE 属性初始化 *m（覆盖静态占位）。
 * 由 Rust 的 ensure_init 在 init 互斥下调用。成功返回 0。 */
int pt_mutex_init(pthread_mutex_t* m);

/* 纯 pthread 包装，不含初始化——调用前锁必须已 init。
 * Rust 侧保证：lock() 加锁、raw() 交出句柄前都会先走 ensure_init。 */
int pt_mutex_lock(pthread_mutex_t* m);
int pt_mutex_unlock(pthread_mutex_t* m);

#endif /* PT_MUTEX_H */
