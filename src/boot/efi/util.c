/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <efi.h>
#include <efilib.h>

#include "util.h"

#ifdef __x86_64__
UINT64 ticks_read(void) {
        UINT64 a, d;
        __asm__ volatile ("rdtsc" : "=a" (a), "=d" (d));
        return (d << 32) | a;
}
#elif defined(__i386__)
UINT64 ticks_read(void) {
        UINT64 val;
        __asm__ volatile ("rdtsc" : "=A" (val));
        return val;
}
#elif defined(__aarch64__)
UINT64 ticks_read(void) {
        UINT64 val;
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (val));
        return val;
}
#else
UINT64 ticks_read(void) {
        UINT64 val = 1;
        return val;
}
#endif

#if defined(__aarch64__)
UINT64 ticks_freq(void) {
        UINT64 freq;
        __asm__ volatile ("mrs %0, cntfrq_el0": "=r" (freq));
        return freq;
}
#else
/* count TSC ticks during a millisecond delay */
UINT64 ticks_freq(void) {
        UINT64 ticks_start, ticks_end;

        ticks_start = ticks_read();
        BS->Stall(1000);
        ticks_end = ticks_read();

        return (ticks_end - ticks_start) * 1000UL;
}
#endif

UINT64 time_usec(void) {
        UINT64 ticks;
        static UINT64 freq;

        ticks = ticks_read();
        if (ticks == 0)
                return 0;

        if (freq == 0) {
                freq = ticks_freq();
                if (freq == 0)
                        return 0;
        }

        return 1000UL * 1000UL * ticks / freq;
}

EFI_STATUS parse_boolean(const CHAR8 *v, BOOLEAN *b) {
        assert(b);

        if (!v)
                return EFI_INVALID_PARAMETER;

        if (strcmpa(v, (CHAR8 *)"1") == 0 ||
            strcmpa(v, (CHAR8 *)"yes") == 0 ||
            strcmpa(v, (CHAR8 *)"y") == 0 ||
            strcmpa(v, (CHAR8 *)"true") == 0 ||
            strcmpa(v, (CHAR8 *)"t") == 0 ||
            strcmpa(v, (CHAR8 *)"on") == 0) {
                *b = TRUE;
                return EFI_SUCCESS;
        }

        if (strcmpa(v, (CHAR8 *)"0") == 0 ||
            strcmpa(v, (CHAR8 *)"no") == 0 ||
            strcmpa(v, (CHAR8 *)"n") == 0 ||
            strcmpa(v, (CHAR8 *)"false") == 0 ||
            strcmpa(v, (CHAR8 *)"f") == 0 ||
            strcmpa(v, (CHAR8 *)"off") == 0) {
                *b = FALSE;
                return EFI_SUCCESS;
        }

        return EFI_INVALID_PARAMETER;
}

EFI_STATUS efivar_set_raw(const EFI_GUID *vendor, const CHAR16 *name, const void *buf, UINTN size, UINT32 flags) {
        assert(vendor);
        assert(name);
        assert(buf || size == 0);

        flags |= EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
        return RT->SetVariable((CHAR16 *) name, (EFI_GUID *) vendor, flags, size, (void *) buf);
}

EFI_STATUS efivar_set(const EFI_GUID *vendor, const CHAR16 *name, const CHAR16 *value, UINT32 flags) {
        assert(vendor);
        assert(name);

        return efivar_set_raw(vendor, name, value, value ? StrSize(value) : 0, flags);
}

EFI_STATUS efivar_set_uint_string(const EFI_GUID *vendor, const CHAR16 *name, UINTN i, UINT32 flags) {
        CHAR16 str[32];

        assert(vendor);
        assert(name);

        SPrint(str, ELEMENTSOF(str), L"%u", i);
        return efivar_set(vendor, name, str, flags);
}

EFI_STATUS efivar_set_uint32_le(const EFI_GUID *vendor, const CHAR16 *name, UINT32 value, UINT32 flags) {
        UINT8 buf[4];

        assert(vendor);
        assert(name);

        buf[0] = (UINT8)(value >> 0U & 0xFF);
        buf[1] = (UINT8)(value >> 8U & 0xFF);
        buf[2] = (UINT8)(value >> 16U & 0xFF);
        buf[3] = (UINT8)(value >> 24U & 0xFF);

        return efivar_set_raw(vendor, name, buf, sizeof(buf), flags);
}

EFI_STATUS efivar_set_uint64_le(const EFI_GUID *vendor, const CHAR16 *name, UINT64 value, UINT32 flags) {
        UINT8 buf[8];

        assert(vendor);
        assert(name);

        buf[0] = (UINT8)(value >> 0U & 0xFF);
        buf[1] = (UINT8)(value >> 8U & 0xFF);
        buf[2] = (UINT8)(value >> 16U & 0xFF);
        buf[3] = (UINT8)(value >> 24U & 0xFF);
        buf[4] = (UINT8)(value >> 32U & 0xFF);
        buf[5] = (UINT8)(value >> 40U & 0xFF);
        buf[6] = (UINT8)(value >> 48U & 0xFF);
        buf[7] = (UINT8)(value >> 56U & 0xFF);

        return efivar_set_raw(vendor, name, buf, sizeof(buf), flags);
}

EFI_STATUS efivar_get(const EFI_GUID *vendor, const CHAR16 *name, CHAR16 **value) {
        _cleanup_freepool_ CHAR16 *buf = NULL;
        EFI_STATUS err;
        CHAR16 *val;
        UINTN size;

        assert(vendor);
        assert(name);

        err = efivar_get_raw(vendor, name, (CHAR8**)&buf, &size);
        if (EFI_ERROR(err))
                return err;

        /* Make sure there are no incomplete characters in the buffer */
        if ((size % sizeof(CHAR16)) != 0)
                return EFI_INVALID_PARAMETER;

        if (!value)
                return EFI_SUCCESS;

        /* Return buffer directly if it happens to be NUL terminated already */
        if (size >= sizeof(CHAR16) && buf[size/sizeof(CHAR16)] == 0) {
                *value = TAKE_PTR(buf);
                return EFI_SUCCESS;
        }

        /* Make sure a terminating NUL is available at the end */
        val = AllocatePool(size + sizeof(CHAR16));
        if (!val)
                return EFI_OUT_OF_RESOURCES;

        CopyMem(val, buf, size);
        val[size / sizeof(CHAR16)] = 0; /* NUL terminate */

        *value = val;
        return EFI_SUCCESS;
}

EFI_STATUS efivar_get_uint_string(const EFI_GUID *vendor, const CHAR16 *name, UINTN *i) {
        _cleanup_freepool_ CHAR16 *val = NULL;
        EFI_STATUS err;

        assert(vendor);
        assert(name);
        assert(i);

        err = efivar_get(vendor, name, &val);
        if (!EFI_ERROR(err))
                *i = Atoi(val);

        return err;
}

EFI_STATUS efivar_get_uint32_le(const EFI_GUID *vendor, const CHAR16 *name, UINT32 *ret) {
        _cleanup_freepool_ CHAR8 *buf = NULL;
        UINTN size;
        EFI_STATUS err;

        assert(vendor);
        assert(name);

        err = efivar_get_raw(vendor, name, &buf, &size);
        if (!EFI_ERROR(err) && ret) {
                if (size != sizeof(UINT32))
                        return EFI_BUFFER_TOO_SMALL;

                *ret = (UINT32) buf[0] << 0U | (UINT32) buf[1] << 8U | (UINT32) buf[2] << 16U |
                        (UINT32) buf[3] << 24U;
        }

        return err;
}

EFI_STATUS efivar_get_uint64_le(const EFI_GUID *vendor, const CHAR16 *name, UINT64 *ret) {
        _cleanup_freepool_ CHAR8 *buf = NULL;
        UINTN size;
        EFI_STATUS err;

        assert(vendor);
        assert(name);

        err = efivar_get_raw(vendor, name, &buf, &size);
        if (!EFI_ERROR(err) && ret) {
                if (size != sizeof(UINT64))
                        return EFI_BUFFER_TOO_SMALL;

                *ret = (UINT64) buf[0] << 0U | (UINT64) buf[1] << 8U | (UINT64) buf[2] << 16U |
                        (UINT64) buf[3] << 24U | (UINT64) buf[4] << 32U | (UINT64) buf[5] << 40U |
                        (UINT64) buf[6] << 48U | (UINT64) buf[7] << 56U;
        }

        return err;
}

EFI_STATUS efivar_get_raw(const EFI_GUID *vendor, const CHAR16 *name, CHAR8 **buffer, UINTN *size) {
        _cleanup_freepool_ CHAR8 *buf = NULL;
        UINTN l;
        EFI_STATUS err;

        assert(vendor);
        assert(name);

        l = sizeof(CHAR16 *) * EFI_MAXIMUM_VARIABLE_SIZE;
        buf = AllocatePool(l);
        if (!buf)
                return EFI_OUT_OF_RESOURCES;

        err = RT->GetVariable((CHAR16 *) name, (EFI_GUID *) vendor, NULL, &l, buf);
        if (!EFI_ERROR(err)) {

                if (buffer)
                        *buffer = TAKE_PTR(buf);

                if (size)
                        *size = l;
        }

        return err;
}

EFI_STATUS efivar_get_boolean_u8(const EFI_GUID *vendor, const CHAR16 *name, BOOLEAN *ret) {
        _cleanup_freepool_ CHAR8 *b = NULL;
        UINTN size;
        EFI_STATUS err;

        assert(vendor);
        assert(name);
        assert(ret);

        err = efivar_get_raw(vendor, name, &b, &size);
        if (!EFI_ERROR(err))
                *ret = *b > 0;

        return err;
}

void efivar_set_time_usec(const EFI_GUID *vendor, const CHAR16 *name, UINT64 usec) {
        CHAR16 str[32];

        assert(vendor);
        assert(name);

        if (usec == 0)
                usec = time_usec();
        if (usec == 0)
                return;

        SPrint(str, ELEMENTSOF(str), L"%ld", usec);
        efivar_set(vendor, name, str, 0);
}

static INTN utf8_to_16(const CHAR8 *stra, CHAR16 *c) {
        CHAR16 unichar;
        UINTN len;

        assert(stra);
        assert(c);

        if (!(stra[0] & 0x80))
                len = 1;
        else if ((stra[0] & 0xe0) == 0xc0)
                len = 2;
        else if ((stra[0] & 0xf0) == 0xe0)
                len = 3;
        else if ((stra[0] & 0xf8) == 0xf0)
                len = 4;
        else if ((stra[0] & 0xfc) == 0xf8)
                len = 5;
        else if ((stra[0] & 0xfe) == 0xfc)
                len = 6;
        else
                return -1;

        switch (len) {
        case 1:
                unichar = stra[0];
                break;
        case 2:
                unichar = stra[0] & 0x1f;
                break;
        case 3:
                unichar = stra[0] & 0x0f;
                break;
        case 4:
                unichar = stra[0] & 0x07;
                break;
        case 5:
                unichar = stra[0] & 0x03;
                break;
        case 6:
                unichar = stra[0] & 0x01;
                break;
        }

        for (UINTN i = 1; i < len; i++) {
                if ((stra[i] & 0xc0) != 0x80)
                        return -1;
                unichar <<= 6;
                unichar |= stra[i] & 0x3f;
        }

        *c = unichar;
        return len;
}

CHAR16 *stra_to_str(const CHAR8 *stra) {
        UINTN strlen;
        UINTN len;
        UINTN i;
        CHAR16 *str;

        assert(stra);

        len = strlena(stra);
        str = AllocatePool((len + 1) * sizeof(CHAR16));
        if (!str)
                return NULL;

        strlen = 0;
        i = 0;
        while (i < len) {
                INTN utf8len;

                utf8len = utf8_to_16(stra + i, str + strlen);
                if (utf8len <= 0) {
                        /* invalid utf8 sequence, skip the garbage */
                        i++;
                        continue;
                }

                strlen++;
                i += utf8len;
        }
        str[strlen] = '\0';
        return str;
}

CHAR16 *stra_to_path(const CHAR8 *stra) {
        CHAR16 *str;
        UINTN strlen;
        UINTN len;
        UINTN i;

        assert(stra);

        len = strlena(stra);
        str = AllocatePool((len + 2) * sizeof(CHAR16));
        if (!str)
                return NULL;

        str[0] = '\\';
        strlen = 1;
        i = 0;
        while (i < len) {
                INTN utf8len;

                utf8len = utf8_to_16(stra + i, str + strlen);
                if (utf8len <= 0) {
                        /* invalid utf8 sequence, skip the garbage */
                        i++;
                        continue;
                }

                if (str[strlen] == '/')
                        str[strlen] = '\\';
                if (str[strlen] == '\\' && str[strlen-1] == '\\') {
                        /* skip double slashes */
                        i += utf8len;
                        continue;
                }

                strlen++;
                i += utf8len;
        }
        str[strlen] = '\0';
        return str;
}

CHAR8 *strchra(const CHAR8 *s, CHAR8 c) {
        if (!s)
                return NULL;

        do {
                if (*s == c)
                        return (CHAR8*) s;
        } while (*s++);

        return NULL;
}

EFI_STATUS file_read(EFI_FILE_HANDLE dir, const CHAR16 *name, UINTN off, UINTN size, CHAR8 **ret, UINTN *ret_size) {
        _cleanup_(FileHandleClosep) EFI_FILE_HANDLE handle = NULL;
        _cleanup_freepool_ CHAR8 *buf = NULL;
        EFI_STATUS err;

        assert(name);
        assert(ret);

        err = dir->Open(dir, &handle, (CHAR16*) name, EFI_FILE_MODE_READ, 0ULL);
        if (EFI_ERROR(err))
                return err;

        if (size == 0) {
                _cleanup_freepool_ EFI_FILE_INFO *info = NULL;

                err = get_file_info_harder(handle, &info, NULL);
                if (EFI_ERROR(err))
                        return err;

                size = info->FileSize+1;
        }

        if (off > 0) {
                err = handle->SetPosition(handle, off);
                if (EFI_ERROR(err))
                        return err;
        }

        buf = AllocatePool(size + 1);
        if (!buf)
                return EFI_OUT_OF_RESOURCES;

        err = handle->Read(handle, &size, buf);
        if (EFI_ERROR(err))
                return err;

        buf[size] = '\0';

        *ret = TAKE_PTR(buf);
        if (ret_size)
                *ret_size = size;

        return err;
}

void log_error_stall(const CHAR16 *fmt, ...) {
        va_list args;

        assert(fmt);

        INT32 attr = ST->ConOut->Mode->Attribute;
        ST->ConOut->SetAttribute(ST->ConOut, EFI_LIGHTRED|EFI_BACKGROUND_BLACK);

        if (ST->ConOut->Mode->CursorColumn > 0)
                Print(L"\n");

        va_start(args, fmt);
        VPrint(fmt, args);
        va_end(args);

        Print(L"\n");

        ST->ConOut->SetAttribute(ST->ConOut, attr);

        /* Give the user a chance to see the message. */
        BS->Stall(3 * 1000 * 1000);
}

EFI_STATUS log_oom(void) {
        log_error_stall(L"Out of memory.");
        return EFI_OUT_OF_RESOURCES;
}

void *memmem_safe(const void *haystack, UINTN haystack_len, const void *needle, UINTN needle_len) {
        assert(haystack || haystack_len == 0);
        assert(needle || needle_len == 0);

        if (needle_len == 0)
                return (void*)haystack;

        for (const CHAR8 *h = haystack, *n = needle; haystack_len >= needle_len; h++, haystack_len--)
                if (*h == *n && CompareMem(h + 1, n + 1, needle_len - 1) == 0)
                        return (void*)h;

        return NULL;
}

void print_at(UINTN x, UINTN y, UINTN attr, const CHAR16 *str) {
        assert(str);
        ST->ConOut->SetCursorPosition(ST->ConOut, x, y);
        ST->ConOut->SetAttribute(ST->ConOut, attr);
        ST->ConOut->OutputString(ST->ConOut, (CHAR16*)str);
}

void clear_screen(UINTN attr) {
        ST->ConOut->SetAttribute(ST->ConOut, attr);
        ST->ConOut->ClearScreen(ST->ConOut);
}

void sort_pointer_array(
                void **array,
                UINTN n_members,
                compare_pointer_func_t compare) {

        assert(array || n_members == 0);
        assert(compare);

        if (n_members <= 1)
                return;

        for (UINTN i = 1; i < n_members; i++) {
                BOOLEAN more = FALSE;

                for (UINTN k = 0; k < n_members - i; k++) {
                        void *entry;

                        if (compare(array[k], array[k+1]) <= 0)
                                continue;

                        entry = array[k];
                        array[k] = array[k+1];
                        array[k+1] = entry;
                        more = TRUE;
                }
                if (!more)
                        break;
        }
}

EFI_STATUS get_file_info_harder(
                EFI_FILE_HANDLE handle,
                EFI_FILE_INFO **ret,
                UINTN *ret_size) {

        UINTN size = OFFSETOF(EFI_FILE_INFO, FileName) + 256;
        _cleanup_freepool_ EFI_FILE_INFO *fi = NULL;
        EFI_STATUS err;

        assert(handle);
        assert(ret);

        /* A lot like LibFileInfo() but with useful error propagation */

        fi = AllocatePool(size);
        if (!fi)
                return EFI_OUT_OF_RESOURCES;

        err = handle->GetInfo(handle, &GenericFileInfo, &size, fi);
        if (err == EFI_BUFFER_TOO_SMALL) {
                FreePool(fi);
                fi = AllocatePool(size);  /* GetInfo tells us the required size, let's use that now */
                if (!fi)
                        return EFI_OUT_OF_RESOURCES;

                err = handle->GetInfo(handle, &GenericFileInfo, &size, fi);
        }

        if (EFI_ERROR(err))
                return err;

        *ret = TAKE_PTR(fi);

        if (ret_size)
                *ret_size = size;

        return EFI_SUCCESS;
}

EFI_STATUS readdir_harder(
                EFI_FILE_HANDLE handle,
                EFI_FILE_INFO **buffer,
                UINTN *buffer_size) {

        EFI_STATUS err;
        UINTN sz;

        assert(handle);
        assert(buffer);
        assert(buffer_size);

        /* buffer/buffer_size are both in and output parameters. Should be zero-initialized initially, and
         * the specified buffer needs to be freed by caller, after final use. */

        if (!*buffer) {
                sz = OFFSETOF(EFI_FILE_INFO, FileName) /* + 256 */;

                *buffer = AllocatePool(sz);
                if (!*buffer)
                        return EFI_OUT_OF_RESOURCES;

                *buffer_size = sz;
        } else
                sz = *buffer_size;

        err = handle->Read(handle, &sz, *buffer);
        if (err == EFI_BUFFER_TOO_SMALL) {
                FreePool(*buffer);

                *buffer = AllocatePool(sz);
                if (!*buffer) {
                        *buffer_size = 0;
                        return EFI_OUT_OF_RESOURCES;
                }

                *buffer_size = sz;

                err = handle->Read(handle, &sz, *buffer);
        }
        if (EFI_ERROR(err))
                return err;

        if (sz == 0) {
                /* End of directory */
                FreePool(*buffer);
                *buffer = NULL;
                *buffer_size = 0;
        }

        return EFI_SUCCESS;
}

UINTN strnlena(const CHAR8 *p, UINTN maxlen) {
        UINTN c;

        if (!p)
                return 0;

        for (c = 0; c < maxlen; c++)
                if (p[c] == 0)
                        break;

        return c;
}

CHAR8 *strndup8(const CHAR8 *p, UINTN sz) {
        CHAR8 *n;

        /* Following efilib's naming scheme this function would be called strndupa(), but we already have a
         * function named like this in userspace, and it does something different there, hence to minimize
         * confusion, let's pick a different name here */

        assert(p || sz == 0);

        sz = strnlena(p, sz);

        n = AllocatePool(sz + 1);
        if (!n)
                return NULL;

        if (sz > 0)
                CopyMem(n, p, sz);
        n[sz] = 0;

        return n;
}

BOOLEAN is_ascii(const CHAR16 *f) {
        if (!f)
                return FALSE;

        for (; *f != 0; f++)
                if (*f > 127)
                        return FALSE;

        return TRUE;
}

CHAR16 **strv_free(CHAR16 **v) {
        if (!v)
                return NULL;

        for (CHAR16 **i = v; *i; i++)
                FreePool(*i);

        FreePool(v);
        return NULL;
}

EFI_STATUS open_directory(
                EFI_FILE_HANDLE root,
                const CHAR16 *path,
                EFI_FILE_HANDLE *ret) {

        _cleanup_(FileHandleClosep) EFI_FILE_HANDLE dir = NULL;
        _cleanup_freepool_ EFI_FILE_INFO *file_info = NULL;
        EFI_STATUS err;

        assert(root);

        /* Opens a file, and then verifies it is actually a directory */

        err = root->Open(root, &dir, (CHAR16*) path, EFI_FILE_MODE_READ, 0ULL);
        if (EFI_ERROR(err))
                return err;

        err = get_file_info_harder(dir, &file_info, NULL);
        if (EFI_ERROR(err))
                return err;
        if (!FLAGS_SET(file_info->Attribute, EFI_FILE_DIRECTORY))
                return EFI_LOAD_ERROR;

        *ret = TAKE_PTR(dir);
        return EFI_SUCCESS;
}

UINT64 get_os_indications_supported(void) {
        UINT64 osind;
        EFI_STATUS err;

        /* Returns the supported OS indications. If we can't acquire it, returns a zeroed out mask, i.e. no
         * supported features. */

        err = efivar_get_uint64_le(EFI_GLOBAL_GUID, L"OsIndicationsSupported", &osind);
        if (EFI_ERROR(err))
                return 0;

        return osind;
}
