#![allow(dead_code)] // a general-purpose buffer: not every method is used yet
use std::borrow::Borrow;
use std::ffi::{CStr, c_char};
use std::fmt;
use std::ops::Deref;

/// 对齐 ShyOS 的三个错误码。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CStringError {
    /// 入参非法：含内部 NUL、容量溢出等
    InvalidArgument,
    /// 预留位：std 分配器耗尽时直接中止，实际到不了这里
    OutOfMemory,
    /// 对空串 pop
    NoValue,
}

pub type Result<T> = std::result::Result<T, CStringError>;

#[derive(Clone, PartialEq, Eq)]
pub struct CString {
    /// 内容加结尾 NUL；恒非空
    buf: Vec<u8>,
}

impl CString {
    pub fn new() -> Self {
        Self { buf: vec![0] }
    }

    pub fn with_capacity(capacity: usize) -> Result<Self> {
        if capacity == usize::MAX {
            return Err(CStringError::InvalidArgument);
        }
        let mut buf = Vec::with_capacity(capacity + 1);
        buf.push(0);
        Ok(Self { buf })
    }

    pub fn from_bytes(value: &[u8]) -> Result<Self> {
        if value.contains(&0) {
            return Err(CStringError::InvalidArgument);
        }
        let mut buf = Vec::with_capacity(value.len() + 1);
        buf.extend_from_slice(value);
        buf.push(0);
        Ok(Self { buf })
    }

    pub fn from_c_str(value: &CStr) -> Result<Self> {
        Self::from_bytes(value.to_bytes())
    }

    pub fn from_str(value: &str) -> Self {
        let end = value.find('\0').unwrap_or(value.len());
        let mut buf = Vec::with_capacity(end + 1);
        buf.extend_from_slice(&value.as_bytes()[..end]);
        buf.push(0);
        Self { buf }
    }

    pub fn len(&self) -> usize {
        self.buf.len() - 1
    }

    /// 对外容量不含 NUL 占位，与 ShyOS 的 cap-1 口径一致。
    pub fn capacity(&self) -> usize {
        self.buf.capacity().saturating_sub(1)
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn as_ptr(&self) -> *const c_char {
        self.buf.as_ptr() as *const c_char
    }

    pub fn as_c_str(&self) -> &CStr {
        CStr::from_bytes_with_nul(&self.buf).expect("CString 不变式：结尾 NUL")
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.buf[..self.len()]
    }

    pub fn reserve(&mut self, additional: usize) -> Result<()> {
        if additional > usize::MAX - self.buf.len() - 1 {
            return Err(CStringError::OutOfMemory);
        }
        self.buf.reserve(additional);
        Ok(())
    }

    pub fn push(&mut self, byte: u8) -> Result<()> {
        if byte == 0 {
            return Err(CStringError::InvalidArgument);
        }
        self.buf.reserve(1);
        let n = self.buf.len() - 1;
        self.buf[n] = byte;
        self.buf.push(0);
        Ok(())
    }

    pub fn push_str(&mut self, value: &str) -> Result<()> {
        let bytes = value.as_bytes();
        if bytes.contains(&0) {
            return Err(CStringError::InvalidArgument);
        }
        self.push_bytes(bytes)
    }

    /// ShyOS 版 push_str 的签名（&CStr）；CStr 保证无内部 NUL，免检查。
    pub fn push_c_str(&mut self, value: &CStr) -> Result<()> {
        self.push_bytes(value.to_bytes())
    }

    fn push_bytes(&mut self, bytes: &[u8]) -> Result<()> {
        self.buf.reserve(bytes.len());
        let n = self.buf.len() - 1;
        self.buf.truncate(n);
        self.buf.extend_from_slice(bytes);
        self.buf.push(0);
        Ok(())
    }

    pub fn insert(&mut self, index: usize, byte: u8) -> Result<()> {
        assert!(index <= self.len(), "string insertion index out of bounds");
        if byte == 0 {
            return Err(CStringError::InvalidArgument);
        }
        self.buf.insert(index, byte);
        Ok(())
    }

    pub fn insert_str(&mut self, index: usize, value: &str) -> Result<()> {
        assert!(index <= self.len(), "string insertion index out of bounds");
        let bytes = value.as_bytes();
        if bytes.contains(&0) {
            return Err(CStringError::InvalidArgument);
        }
        self.buf.reserve(bytes.len());
        self.buf.splice(index..index, bytes.iter().copied());
        Ok(())
    }

    pub fn remove(&mut self, index: usize) -> u8 {
        assert!(index < self.len(), "string removal index out of bounds");
        self.buf.remove(index)
    }

    pub fn split_off(&mut self, at: usize) -> Result<Self> {
        assert!(at <= self.len(), "string split index out of bounds");
        let tail = Self::from_bytes(&self.as_bytes()[at..])?;
        self.truncate(at);
        Ok(tail)
    }

    pub fn retain<F>(&mut self, mut keep: F)
    where
        F: FnMut(u8) -> bool,
    {
        let n = self.len();
        let mut w = 0;
        for r in 0..n {
            let byte = self.buf[r];
            if keep(byte) {
                self.buf[w] = byte;
                w += 1;
            }
        }
        self.buf.truncate(w);
        self.buf.push(0);
    }

    pub fn clear(&mut self) {
        self.buf.clear();
        self.buf.push(0);
    }

    pub fn truncate(&mut self, len: usize) {
        assert!(len <= self.len(), "string truncate out of bounds");
        self.buf.truncate(len + 1);
    }

    pub fn pop(&mut self) -> Result<u8> {
        if self.is_empty() {
            return Err(CStringError::NoValue);
        }
        let n = self.buf.len();
        let byte = self.buf[n - 2];
        self.buf[n - 2] = 0;
        self.buf.truncate(n - 1);
        Ok(byte)
    }
}

impl Default for CString {
    fn default() -> Self {
        Self::new()
    }
}

impl Deref for CString {
    type Target = CStr;

    fn deref(&self) -> &Self::Target {
        self.as_c_str()
    }
}

impl AsRef<CStr> for CString {
    fn as_ref(&self) -> &CStr {
        self.as_c_str()
    }
}

impl Borrow<CStr> for CString {
    fn borrow(&self) -> &CStr {
        self.as_c_str()
    }
}

impl fmt::Debug for CString {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.as_c_str().fmt(formatter)
    }
}

/// 以 lossy UTF-8 呈现内容（无效字节替换为 U+FFFD），使 to_string() 可用；
/// CStr 自身没有 Display。
impl fmt::Display for CString {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&String::from_utf8_lossy(self.as_bytes()))
    }
}

impl From<&str> for CString {
    fn from(value: &str) -> Self {
        Self::from_str(value)
    }
}
