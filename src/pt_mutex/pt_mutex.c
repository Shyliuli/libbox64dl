#include "pt_mutex.h"

int pt_mutex_init(pthread_mutex_t* m) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  int rc = pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);
  return rc;
}

int pt_mutex_lock(pthread_mutex_t* m) {
  return pthread_mutex_lock(m);
}

int pt_mutex_unlock(pthread_mutex_t* m) {
  return pthread_mutex_unlock(m);
}
