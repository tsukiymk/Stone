/*
 * Copyright (C) 2023 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// Adapted for Stone: Android asm-generic numeric codes, not Windows errno macros.
// See docs/third-party/bionic.md for upstream file identities and the mechanical adaptation.

STONE_ERRNO_DESCRIPTION(0, "Success") // 0
STONE_ERRNO_DESCRIPTION(1, "Operation not permitted") // EPERM
STONE_ERRNO_DESCRIPTION(2, "No such file or directory") // ENOENT
STONE_ERRNO_DESCRIPTION(3, "No such process") // ESRCH
STONE_ERRNO_DESCRIPTION(4, "Interrupted system call") // EINTR
STONE_ERRNO_DESCRIPTION(5, "I/O error") // EIO
STONE_ERRNO_DESCRIPTION(6, "No such device or address") // ENXIO
STONE_ERRNO_DESCRIPTION(7, "Argument list too long") // E2BIG
STONE_ERRNO_DESCRIPTION(8, "Exec format error") // ENOEXEC
STONE_ERRNO_DESCRIPTION(9, "Bad file descriptor") // EBADF
STONE_ERRNO_DESCRIPTION(10, "No child processes") // ECHILD
STONE_ERRNO_DESCRIPTION(11, "Try again") // EAGAIN
STONE_ERRNO_DESCRIPTION(12, "Out of memory") // ENOMEM
STONE_ERRNO_DESCRIPTION(13, "Permission denied") // EACCES
STONE_ERRNO_DESCRIPTION(14, "Bad address") // EFAULT
STONE_ERRNO_DESCRIPTION(15, "Block device required") // ENOTBLK
STONE_ERRNO_DESCRIPTION(16, "Device or resource busy") // EBUSY
STONE_ERRNO_DESCRIPTION(17, "File exists") // EEXIST
STONE_ERRNO_DESCRIPTION(18, "Cross-device link") // EXDEV
STONE_ERRNO_DESCRIPTION(19, "No such device") // ENODEV
STONE_ERRNO_DESCRIPTION(20, "Not a directory") // ENOTDIR
STONE_ERRNO_DESCRIPTION(21, "Is a directory") // EISDIR
STONE_ERRNO_DESCRIPTION(22, "Invalid argument") // EINVAL
STONE_ERRNO_DESCRIPTION(23, "File table overflow") // ENFILE
STONE_ERRNO_DESCRIPTION(24, "Too many open files") // EMFILE
STONE_ERRNO_DESCRIPTION(25, "Inappropriate ioctl for device") // ENOTTY
STONE_ERRNO_DESCRIPTION(26, "Text file busy") // ETXTBSY
STONE_ERRNO_DESCRIPTION(27, "File too large") // EFBIG
STONE_ERRNO_DESCRIPTION(28, "No space left on device") // ENOSPC
STONE_ERRNO_DESCRIPTION(29, "Illegal seek") // ESPIPE
STONE_ERRNO_DESCRIPTION(30, "Read-only file system") // EROFS
STONE_ERRNO_DESCRIPTION(31, "Too many links") // EMLINK
STONE_ERRNO_DESCRIPTION(32, "Broken pipe") // EPIPE
STONE_ERRNO_DESCRIPTION(33, "Math argument out of domain of func") // EDOM
STONE_ERRNO_DESCRIPTION(34, "Math result not representable") // ERANGE
STONE_ERRNO_DESCRIPTION(35, "Resource deadlock would occur") // EDEADLK
STONE_ERRNO_DESCRIPTION(36, "File name too long") // ENAMETOOLONG
STONE_ERRNO_DESCRIPTION(37, "No record locks available") // ENOLCK
STONE_ERRNO_DESCRIPTION(38, "Function not implemented") // ENOSYS
STONE_ERRNO_DESCRIPTION(39, "Directory not empty") // ENOTEMPTY
STONE_ERRNO_DESCRIPTION(40, "Too many symbolic links encountered") // ELOOP
STONE_ERRNO_DESCRIPTION(42, "No message of desired type") // ENOMSG
STONE_ERRNO_DESCRIPTION(43, "Identifier removed") // EIDRM
STONE_ERRNO_DESCRIPTION(44, "Channel number out of range") // ECHRNG
STONE_ERRNO_DESCRIPTION(45, "Level 2 not synchronized") // EL2NSYNC
STONE_ERRNO_DESCRIPTION(46, "Level 3 halted") // EL3HLT
STONE_ERRNO_DESCRIPTION(47, "Level 3 reset") // EL3RST
STONE_ERRNO_DESCRIPTION(48, "Link number out of range") // ELNRNG
STONE_ERRNO_DESCRIPTION(49, "Protocol driver not attached") // EUNATCH
STONE_ERRNO_DESCRIPTION(50, "No CSI structure available") // ENOCSI
STONE_ERRNO_DESCRIPTION(51, "Level 2 halted") // EL2HLT
STONE_ERRNO_DESCRIPTION(52, "Invalid exchange") // EBADE
STONE_ERRNO_DESCRIPTION(53, "Invalid request descriptor") // EBADR
STONE_ERRNO_DESCRIPTION(54, "Exchange full") // EXFULL
STONE_ERRNO_DESCRIPTION(55, "No anode") // ENOANO
STONE_ERRNO_DESCRIPTION(56, "Invalid request code") // EBADRQC
STONE_ERRNO_DESCRIPTION(57, "Invalid slot") // EBADSLT
STONE_ERRNO_DESCRIPTION(59, "Bad font file format") // EBFONT
STONE_ERRNO_DESCRIPTION(60, "Device not a stream") // ENOSTR
STONE_ERRNO_DESCRIPTION(61, "No data available") // ENODATA
STONE_ERRNO_DESCRIPTION(62, "Timer expired") // ETIME
STONE_ERRNO_DESCRIPTION(63, "Out of streams resources") // ENOSR
STONE_ERRNO_DESCRIPTION(64, "Machine is not on the network") // ENONET
STONE_ERRNO_DESCRIPTION(65, "Package not installed") // ENOPKG
STONE_ERRNO_DESCRIPTION(66, "Object is remote") // EREMOTE
STONE_ERRNO_DESCRIPTION(67, "Link has been severed") // ENOLINK
STONE_ERRNO_DESCRIPTION(68, "Advertise error") // EADV
STONE_ERRNO_DESCRIPTION(69, "Srmount error") // ESRMNT
STONE_ERRNO_DESCRIPTION(70, "Communication error on send") // ECOMM
STONE_ERRNO_DESCRIPTION(71, "Protocol error") // EPROTO
STONE_ERRNO_DESCRIPTION(72, "Multihop attempted") // EMULTIHOP
STONE_ERRNO_DESCRIPTION(73, "RFS specific error") // EDOTDOT
STONE_ERRNO_DESCRIPTION(74, "Not a data message") // EBADMSG
STONE_ERRNO_DESCRIPTION(75, "Value too large for defined data type") // EOVERFLOW
STONE_ERRNO_DESCRIPTION(76, "Name not unique on network") // ENOTUNIQ
STONE_ERRNO_DESCRIPTION(77, "File descriptor in bad state") // EBADFD
STONE_ERRNO_DESCRIPTION(78, "Remote address changed") // EREMCHG
STONE_ERRNO_DESCRIPTION(79, "Can not access a needed shared library") // ELIBACC
STONE_ERRNO_DESCRIPTION(80, "Accessing a corrupted shared library") // ELIBBAD
STONE_ERRNO_DESCRIPTION(81, ".lib section in a.out corrupted") // ELIBSCN
STONE_ERRNO_DESCRIPTION(82, "Attempting to link in too many shared libraries") // ELIBMAX
STONE_ERRNO_DESCRIPTION(83, "Cannot exec a shared library directly") // ELIBEXEC
STONE_ERRNO_DESCRIPTION(84, "Illegal byte sequence") // EILSEQ
STONE_ERRNO_DESCRIPTION(85, "Interrupted system call should be restarted") // ERESTART
STONE_ERRNO_DESCRIPTION(86, "Streams pipe error") // ESTRPIPE
STONE_ERRNO_DESCRIPTION(87, "Too many users") // EUSERS
STONE_ERRNO_DESCRIPTION(88, "Socket operation on non-socket") // ENOTSOCK
STONE_ERRNO_DESCRIPTION(89, "Destination address required") // EDESTADDRREQ
STONE_ERRNO_DESCRIPTION(90, "Message too long") // EMSGSIZE
STONE_ERRNO_DESCRIPTION(91, "Protocol wrong type for socket") // EPROTOTYPE
STONE_ERRNO_DESCRIPTION(92, "Protocol not available") // ENOPROTOOPT
STONE_ERRNO_DESCRIPTION(93, "Protocol not supported") // EPROTONOSUPPORT
STONE_ERRNO_DESCRIPTION(94, "Socket type not supported") // ESOCKTNOSUPPORT
STONE_ERRNO_DESCRIPTION(95, "Operation not supported on transport endpoint") // EOPNOTSUPP
STONE_ERRNO_DESCRIPTION(96, "Protocol family not supported") // EPFNOSUPPORT
STONE_ERRNO_DESCRIPTION(97, "Address family not supported by protocol") // EAFNOSUPPORT
STONE_ERRNO_DESCRIPTION(98, "Address already in use") // EADDRINUSE
STONE_ERRNO_DESCRIPTION(99, "Cannot assign requested address") // EADDRNOTAVAIL
STONE_ERRNO_DESCRIPTION(100, "Network is down") // ENETDOWN
STONE_ERRNO_DESCRIPTION(101, "Network is unreachable") // ENETUNREACH
STONE_ERRNO_DESCRIPTION(102, "Network dropped connection because of reset") // ENETRESET
STONE_ERRNO_DESCRIPTION(103, "Software caused connection abort") // ECONNABORTED
STONE_ERRNO_DESCRIPTION(104, "Connection reset by peer") // ECONNRESET
STONE_ERRNO_DESCRIPTION(105, "No buffer space available") // ENOBUFS
STONE_ERRNO_DESCRIPTION(106, "Transport endpoint is already connected") // EISCONN
STONE_ERRNO_DESCRIPTION(107, "Transport endpoint is not connected") // ENOTCONN
STONE_ERRNO_DESCRIPTION(108, "Cannot send after transport endpoint shutdown") // ESHUTDOWN
STONE_ERRNO_DESCRIPTION(109, "Too many references: cannot splice") // ETOOMANYREFS
STONE_ERRNO_DESCRIPTION(110, "Connection timed out") // ETIMEDOUT
STONE_ERRNO_DESCRIPTION(111, "Connection refused") // ECONNREFUSED
STONE_ERRNO_DESCRIPTION(112, "Host is down") // EHOSTDOWN
STONE_ERRNO_DESCRIPTION(113, "No route to host") // EHOSTUNREACH
STONE_ERRNO_DESCRIPTION(114, "Operation already in progress") // EALREADY
STONE_ERRNO_DESCRIPTION(115, "Operation now in progress") // EINPROGRESS
STONE_ERRNO_DESCRIPTION(116, "Stale NFS file handle") // ESTALE
STONE_ERRNO_DESCRIPTION(117, "Structure needs cleaning") // EUCLEAN
STONE_ERRNO_DESCRIPTION(118, "Not a XENIX named type file") // ENOTNAM
STONE_ERRNO_DESCRIPTION(119, "No XENIX semaphores available") // ENAVAIL
STONE_ERRNO_DESCRIPTION(120, "Is a named type file") // EISNAM
STONE_ERRNO_DESCRIPTION(121, "Remote I/O error") // EREMOTEIO
STONE_ERRNO_DESCRIPTION(122, "Quota exceeded") // EDQUOT
STONE_ERRNO_DESCRIPTION(123, "No medium found") // ENOMEDIUM
STONE_ERRNO_DESCRIPTION(124, "Wrong medium type") // EMEDIUMTYPE
STONE_ERRNO_DESCRIPTION(125, "Operation Canceled") // ECANCELED
STONE_ERRNO_DESCRIPTION(126, "Required key not available") // ENOKEY
STONE_ERRNO_DESCRIPTION(127, "Key has expired") // EKEYEXPIRED
STONE_ERRNO_DESCRIPTION(128, "Key has been revoked") // EKEYREVOKED
STONE_ERRNO_DESCRIPTION(129, "Key was rejected by service") // EKEYREJECTED
STONE_ERRNO_DESCRIPTION(130, "Owner died") // EOWNERDEAD
STONE_ERRNO_DESCRIPTION(131, "State not recoverable") // ENOTRECOVERABLE
STONE_ERRNO_DESCRIPTION(132, "Operation not possible due to RF-kill") // ERFKILL
STONE_ERRNO_DESCRIPTION(133, "Memory page has hardware error") // EHWPOISON
