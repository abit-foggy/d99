#ifndef D99_ELF_H
#define D99_ELF_H

#include "d99_util.h"

int d99_elf_is_elf(const char *path);
/* returns 0 and appends DT_NEEDED sonames to out; 1 if not ELF;
 * -1 on I/O or parse error */
int d99_elf_needed(const char *path, d99_strvec *out);

#endif /* D99_ELF_H */
