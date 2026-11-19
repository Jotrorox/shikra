/* embed.h -- interface of the generated src/embed.c (see tools/mkembed.c). */
#ifndef SHIKRA_EMBED_H
#define SHIKRA_EMBED_H

/* One embedded file: its repo-relative name and its text as a NULL-terminated array of
 * string-literal chunks (each < 4000 chars, the C99 limit is 4095) to concatenate. */
typedef struct {
    const char *name;
    const char *const *chunks;
} embed_file;

extern const embed_file embed_files[]; /* sorted by name, terminated by {0, 0} */

#endif
