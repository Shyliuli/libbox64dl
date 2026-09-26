//! C-side `pt_mutex.c` wrapper: a data-carrying pthread recursive lock.
//!
//! Callbacks and re-entrant engine entries take the lock more than once on the
//! same thread, so it must be recursive.
//!
//! # Borrow discipline
//! A guard may be re-entered, but `&T` and `&mut T` stay exclusive: do not hold
//! two live dereferences across a re-entrant acquisition.
use std::cell::UnsafeCell;
use std::marker::PhantomData;
use std::ops::{Deref, DerefMut};
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};

unsafe extern "C" {
    fn pt_mutex_init(m: *mut libc::pthread_mutex_t) -> libc::c_int;
    fn pt_mutex_lock(m: *mut libc::pthread_mutex_t) -> libc::c_int;
    fn pt_mutex_unlock(m: *mut libc::pthread_mutex_t) -> libc::c_int;
}

pub struct PtMutex<T> {
    inited: AtomicBool,
    raw: UnsafeCell<libc::pthread_mutex_t>,
    data: UnsafeCell<T>,
}

// Safety: moving the lock across threads is fine (with T: Send).
unsafe impl<T: Send> Send for PtMutex<T> {}
// Safety: the &T/&mut T exclusivity is guaranteed by the lock protocol.
unsafe impl<T: Send> Sync for PtMutex<T> {}

impl<T> PtMutex<T> {
    pub const fn new(data: T) -> Self {
        Self {
            inited: AtomicBool::new(false),
            raw: UnsafeCell::new(libc::PTHREAD_MUTEX_INITIALIZER),
            data: UnsafeCell::new(data),
        }
    }

    fn ensure_init(&self) {
        if self.inited.load(Ordering::Acquire) {
            return;
        }
        // A static lock makes the first-time initialisation atomic.
        static INIT_LOCK: Mutex<()> = Mutex::new(());
        let _g = INIT_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        if self.inited.load(Ordering::Relaxed) {
            return;
        }
        let res = unsafe { pt_mutex_init(self.raw.get()) };
        assert_eq!(res, 0, "pt_mutex_init failed: {res}");
        self.inited.store(true, Ordering::Release);
    }

    pub fn lock(&self) -> PtMutexGuard<'_, T> {
        self.ensure_init();
        unsafe { pt_mutex_lock(self.raw.get()) };
        PtMutexGuard {
            mutex: self,
            _phantom: PhantomData,
        }
    }

    /// Raw pthread handle (used by the C entry points through `box64_ctx_mutex`).
    pub fn raw(&self) -> *mut libc::pthread_mutex_t {
        self.ensure_init();
        self.raw.get()
    }
}

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

    #[test]
    fn multi_instance_nested_reentry() {
        let a = PtMutex::new(0i32);
        let b = PtMutex::new(0i64);
        {
            let mut outer = a.lock();
            {
                let mut inner = a.lock();
                *inner += 1;
            }
            *outer += 1;
        }
        {
            let mut g = b.lock();
            *g += 1;
        }
        assert_eq!(*a.lock(), 2);
        assert_eq!(*b.lock(), 1);
    }

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
