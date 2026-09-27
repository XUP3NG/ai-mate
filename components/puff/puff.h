/*
 * puff.h — inflate (DEFLATE 解压) 接口
 *
 * 来自 zlib 的 contrib/puff (Mark Adler, 公共领域)。
 * 仅取接口声明, 实现见 puff.c。
 */

#ifndef PUFF_H
#define PUFF_H

#define NIL ((unsigned char *)0)        /* for no output option */

int puff(unsigned char *dest,           /* pointer to destination pointer */
         unsigned long *destlen,        /* amount of output space */
         const unsigned char *source,   /* pointer to source data pointer */
         unsigned long *sourcelen);     /* amount of input available */

#endif
