#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define SEEK_SET 0
#define SEEK_END 2

typedef struct {} FILE;

extern FILE* stderr;
extern FILE* stdout;

int fprintf(FILE * restrict stream, const char * restrict format, ...);
int fflush(FILE *stream);
int vsnprintf(char * restrict str, size_t size, const char * restrict format, va_list ap);
int snprintf(char * restrict str, size_t size, const char * restrict format, ...);
int fputc(int c, FILE *stream);
size_t fwrite(const void *restrict ptr, size_t size, size_t nitems, FILE *restrict stream);
int sscanf(const char *restrict s, const char *restrict format, ...);
FILE *fopen(const char *restrict filename, const char *restrict mode);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
size_t fread(void *restrict ptr, size_t size, size_t nitems, FILE *restrict stream);
int	fclose(FILE *stream);
int sprintf(char * restrict str, const char * restrict format, ...);
int printf(const char * restrict format, ...);

#endif