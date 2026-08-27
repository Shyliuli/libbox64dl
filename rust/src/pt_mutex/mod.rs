/// C 侧 pt_mutex.c 的 Rust 包装：数据在内的 pthread 递归锁。
/// 回调等情况会同线程多次获取锁,因此需要递归锁
/// # 借用纪律
/// 不得破坏Rust的借用纪律，即guard可以重入，但是`&T`和`&mut T`不行!
use std::cell::UnsafeCell;
use std::marker::PhantomData;
use std::ops::{Deref, DerefMut};
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};

unsafe extern "C" {
    pub unsafe fn pt_mutex_init(m: *mut libc::pthread_mutex_t) -> libc::c_int;
    pub unsafe fn pt_mutex_lock(m: *mut libc::pthread_mutex_t) -> libc::c_int;
    pub unsafe fn pt_mutex_unlock(m: *mut libc::pthread_mutex_t) -> libc::c_int;
}

/// pthread 递归锁
pub struct PtMutex<T> {
    inited: AtomicBool,
    raw: UnsafeCell<libc::pthread_mutex_t>,
    data: UnsafeCell<T>,
}

// Safety: 跨线程移动锁本身没问题(T：Send约束数据也可跨线程)
unsafe impl<T: Send> Send for PtMutex<T> {}
// Safety: Deref 拿到的 &T/&mut T 的独占性由锁协议保证
unsafe impl<T: Send> Sync for PtMutex<T> {}

impl<T> PtMutex<T> {
    pub const fn new(data: T) -> Self {
        Self {
            inited: AtomicBool::new(false),
            raw: UnsafeCell::new(libc::PTHREAD_MUTEX_INITIALIZER),
            data: UnsafeCell::new(data),
        }
    }
    // 初始化
    fn ensure_init(&self) {
        // 快路径：已初始化，跳过
        if self.inited.load(Ordering::Acquire) {
            return;
        }
        // 慢路径：初始化
        // 这里需要一个互斥锁(static)来保证初始化是原子的
        static INIT_LOCK: Mutex<()> = Mutex::new(());
        let _g = INIT_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        if self.inited.load(Ordering::Relaxed) {
            return;
        }
        // init
        let res = unsafe { pt_mutex_init(self.raw.get()) };
        // init 失败(通常程序已经无法正确运行)
        assert_eq!(res, 0, "pt_mutex_init failed: {res}");
        self.inited.store(true, Ordering::Release);
    }
    
    /// 取锁(递归锁计数 +1)
    pub fn lock(&self) -> PtMutexGuard<'_, T> {
        self.ensure_init();
        unsafe { pt_mutex_lock(self.raw.get()) };
        PtMutexGuard {
            mutex: self,
            _phantom: PhantomData,
        }
    }

    /// 原始pthread句柄
    pub fn raw(&self) -> *mut libc::pthread_mutex_t {
        self.ensure_init();
        self.raw.get()
    }


}



/// # 借用纪律

/// 不得破坏Rust的借用纪律，即guard可以重入，但是`&T`和`&mut T`不行!
pub struct PtMutexGuard<'a, T> {
    mutex: &'a PtMutex<T>,
    _phantom: PhantomData<*mut ()>,
}

impl<T> Deref for PtMutexGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        unsafe { &*self.mutex.data.get() }
    }
}

impl<T> DerefMut for PtMutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        unsafe { &mut *self.mutex.data.get() }
    }
}

impl<T> Drop for PtMutexGuard<'_, T> {
    fn drop(&mut self) {
        unsafe { pt_mutex_unlock(self.mutex.raw.get()) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    /// 多实例 + 同线程嵌套重入
    #[test]
    fn multi_instance_nested_reentry() {
        let a = PtMutex::new(0i32);
        let b = PtMutex::new(0i64);
        {
            let mut outer = a.lock(); // 外层只持锁
            {
                let mut inner = a.lock(); // 重入：同线程第二层
                *inner += 1;
            }
            *outer += 1; // 外层首次解引用在内层借用死亡之后
        }
        {
            let mut g = b.lock();
            *g += 1;
        }
        assert_eq!(*a.lock(), 2);
        assert_eq!(*b.lock(), 1);
    }

    // 并发首次加锁：多线程同时触发两个实例的 ensure_init 慢路径，
    // 双检锁必须保证 pthread_mutex_init 恰好一次、无未初始化使用。
    #[test]
    fn concurrent_first_lock_two_instances() {
        let a = PtMutex::new(Vec::<usize>::new());
        let b = PtMutex::new(Vec::<usize>::new());
        const N: usize = 8;
        const M: usize = 1000;
        std::thread::scope(|s| {
            for i in 0..N {
                let (a, b) = (&a, &b);
                s.spawn(move || {
                    for _ in 0..M {
                        a.lock().push(i);
                        b.lock().push(i);
                        // 同线程嵌套重入再走一遍（内层不解引用，纯计数）
                        let _outer = a.lock();
                        let _inner = a.lock();
                    }
                });
            }
        });
        assert_eq!(a.lock().len(), N * M);
        assert_eq!(b.lock().len(), N * M);
    }
}
