/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│vi: set net ft=c ts=2 sts=2 sw=2 fenc=utf-8                                :vi│
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2020 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ This program is free software; you can redistribute it and/or modify         │
│ it under the terms of the GNU General Public License as published by         │
│ the Free Software Foundation; version 2 of the License.                      │
│                                                                              │
│ This program is distributed in the hope that it will be useful, but          │
│ WITHOUT ANY WARRANTY; without even the implied warranty of                   │
│ MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU             │
│ General Public License for more details.                                     │
│                                                                              │
│ You should have received a copy of the GNU General Public License            │
│ along with this program; if not, write to the Free Software                  │
│ Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA                │
│ 02110-1301 USA                                                               │
╚─────────────────────────────────────────────────────────────────────────────*/
#include "libc/assert.h"
#include "libc/log/check.h"
#include "libc/macros.h"
#include "libc/mem/mem.h"
#include "libc/str/str.h"
#include "libc/x/x.h"
#include "tool/build/lib/endian.h"
#include "tool/build/lib/machine.h"
#include "tool/build/lib/memory.h"
#include "tool/build/lib/pml4t.h"
#include "tool/build/lib/stats.h"
#include "tool/build/lib/throw.h"

void SetReadAddr(struct Machine *m, int64_t addr, uint32_t size) {
  m->readaddr = addr;
  m->readsize = size;
}

void SetWriteAddr(struct Machine *m, int64_t addr, uint32_t size) {
  m->writeaddr = addr;
  m->writesize = size;
}

void *FindReal(struct Machine *m, int64_t v) {
  uint64_t *p;
  unsigned skew;
  unsigned char i;
  skew = v & 0xfff;
  v &= -0x1000;
  for (i = 0; i < ARRAYLEN(m->tlb); ++i) {
    if (m->tlb[i].v == v && m->tlb[i].r) {
      return (char *)m->tlb[i].r + skew;
    }
  }
  for (p = m->cr3, i = 39; i >= 12; i -= 9) {
    if (IsValidPage(p[(v >> i) & 511])) {
      p = UnmaskPageAddr(p[(v >> i) & 511]);
    } else {
      return NULL;
    }
  }
  m->tlbindex = (m->tlbindex + 1) & (ARRAYLEN(m->tlb) - 1);
  m->tlb[m->tlbindex] = m->tlb[0];
  m->tlb[0].r = p;
  m->tlb[0].v = ROUNDDOWN(v, 0x1000);
  DCHECK_NOTNULL(p);
  return (char *)p + skew;
}

void *ResolveAddress(struct Machine *m, int64_t v) {
  void *r;
  if ((r = FindReal(m, v))) return r;
  ThrowSegmentationFault(m, v);
}

void VirtualSet(struct Machine *m, int64_t v, char c, uint64_t n) {
  char *p;
  uint64_t k;
  k = 0x1000 - (v & 0xfff);
  while (n) {
    k = MIN(k, n);
    p = ResolveAddress(m, v);
    memset(p, c, k);
    n -= k;
    v += k;
    k = 0x1000;
  }
}

void VirtualCopy(struct Machine *m, int64_t v, char *r, uint64_t n, bool d) {
  char *p;
  uint64_t k;
  k = 0x1000 - (v & 0xfff);
  while (n) {
    k = MIN(k, n);
    p = ResolveAddress(m, v);
    if (d) {
      memcpy(r, p, k);
    } else {
      memcpy(p, r, k);
    }
    n -= k;
    r += k;
    v += k;
    k = 0x1000;
  }
}

void *VirtualSend(struct Machine *m, void *dst, int64_t src, uint64_t n) {
  VirtualCopy(m, src, dst, n, true);
  return dst;
}

void VirtualRecv(struct Machine *m, int64_t dst, void *src, uint64_t n) {
  VirtualCopy(m, dst, src, n, false);
}

void *ReserveAddress(struct Machine *m, int64_t v, size_t n) {
  void *r;
  DCHECK_LE(n, sizeof(m->stash));
  if ((v & 0xfff) + n <= 0x1000) return ResolveAddress(m, v);
  m->stashaddr = v;
  m->stashsize = n;
  r = m->stash;
  VirtualSend(m, r, v, n);
  return r;
}

void *AccessRam(struct Machine *m, int64_t v, size_t n, void *p[2],
                uint8_t tmp[n], bool copy) {
  unsigned k;
  uint8_t *a, *b;
  DCHECK_LE(n, 0x1000);
  if ((v & 0xfff) + n <= 0x1000) return ResolveAddress(m, v);
  k = 0x1000;
  k -= v & 0xfff;
  DCHECK_LE(k, 0x1000);
  a = ResolveAddress(m, v);
  b = ResolveAddress(m, v + k);
  if (copy) {
    memcpy(tmp, a, k);
    memcpy(tmp + k, b, n - k);
  }
  p[0] = a;
  p[1] = b;
  return tmp;
}

void *Load(struct Machine *m, int64_t v, size_t n, uint8_t b[n]) {
  void *p[2];
  SetReadAddr(m, v, n);
  return AccessRam(m, v, n, p, b, true);
}

void *BeginStore(struct Machine *m, int64_t v, size_t n, void *p[2],
                 uint8_t b[n]) {
  SetWriteAddr(m, v, n);
  return AccessRam(m, v, n, p, b, false);
}

void *BeginStoreNp(struct Machine *m, int64_t v, size_t n, void *p[2],
                   uint8_t b[n]) {
  if (!v) return NULL;
  return BeginStore(m, v, n, p, b);
}

void *BeginLoadStore(struct Machine *m, int64_t v, size_t n, void *p[2],
                     uint8_t b[n]) {
  SetWriteAddr(m, v, n);
  return AccessRam(m, v, n, p, b, true);
}

void EndStore(struct Machine *m, int64_t v, size_t n, void *p[2],
              uint8_t b[n]) {
  uint8_t *a;
  unsigned k;
  DCHECK_LE(n, 0x1000);
  if ((v & 0xfff) + n <= 0x1000) return;
  k = 0x1000;
  k -= v & 0xfff;
  DCHECK_GT(k, n);
  DCHECK_NOTNULL(p[0]);
  DCHECK_NOTNULL(p[1]);
  memcpy(p[0], b, k);
  memcpy(p[1], b + k, n - k);
}

void EndStoreNp(struct Machine *m, int64_t v, size_t n, void *p[2],
                uint8_t b[n]) {
  if (v) EndStore(m, v, n, p, b);
}

void *LoadStr(struct Machine *m, int64_t addr) {
  size_t have;
  char *copy, *page, *p;
  have = 0x1000 - (addr & 0xfff);
  if (!addr) return NULL;
  if (!(page = FindReal(m, addr))) return NULL;
  if ((p = memchr(page, '\0', have))) {
    SetReadAddr(m, addr, p - page);
    return page;
  }
  CHECK_LT(m->freelist.i, ARRAYLEN(m->freelist.p));
  if (!(copy = malloc(have + 0x1000))) return NULL;
  memcpy(copy, page, have);
  for (;;) {
    if (!(page = FindReal(m, addr + have))) break;
    if ((p = memccpy(copy + have, page, '\0', 0x1000))) {
      SetReadAddr(m, addr, have + (p - (copy + have)));
      return (m->freelist.p[m->freelist.i++] = copy);
    }
    have += 0x1000;
    if (!(p = realloc(copy, have + 0x1000))) break;
    copy = p;
  }
  free(copy);
  return NULL;
}

void *LoadBuf(struct Machine *m, int64_t addr, size_t size) {
  char *buf, *copy;
  size_t have, need;
  have = 0x1000 - (addr & 0xfff);
  if (!addr) return NULL;
  if (!(buf = FindReal(m, addr))) return NULL;
  if (size > have) {
    CHECK_LT(m->freelist.i, ARRAYLEN(m->freelist.p));
    if (!(copy = malloc(size))) return NULL;
    memcpy(copy, buf, have);
    do {
      need = MIN(0x1000, size - have);
      if ((buf = FindReal(m, addr + have))) {
        memcpy(copy + have, buf, need);
        have += need;
      } else {
        free(copy);
        return NULL;
      }
    } while (have < size);
  }
  SetReadAddr(m, addr, size);
  return buf;
}